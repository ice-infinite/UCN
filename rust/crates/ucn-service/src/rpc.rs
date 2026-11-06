use crate::ServiceKey;
use ucn_types::{Error, Result};

/// Logical RPC phase; RPC逻辑状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RequestPhase {
    /// Accepted but not sent; 已接纳未发送。
    WaitSend,
    /// Sent, waiting for application result; 已发送待业务结果。
    WaitResult,
    /// Exact result received; 收到精确结果。
    Complete,
    /// Deadline exceeded; 超时。
    Failed,
    /// Cancelled by caller; 调用方取消。
    Cancelled,
}
/// Exact private-field correlation handle; 精确关联Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RequestHandle {
    owner: u32,
    slot: usize,
    generation: u32,
    server: bool,
}
/// Request immutable view; 请求只读视图。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RequestView {
    /// Complete key; 完整键。
    pub key: ServiceKey,
    /// Phase; 状态。
    pub phase: RequestPhase,
    /// Non-renewable deadline; 不续期时限。
    pub deadline: u64,
    /// Application result; 业务结果。
    pub result: i32,
    /// Retained result bytes; 保留结果字节数。
    pub bytes: usize,
}
struct Slot<const BYTES: usize> {
    key: ServiceKey,
    digest: [u8; 16],
    phase: RequestPhase,
    deadline: u64,
    result: i32,
    length: usize,
    bytes: [u8; BYTES],
}
/// Exclusive zero-write incoming RPC preflight; drop cannot invoke or publish.
/// 独占零写RPC预检，丢弃不调用业务、不发布状态。
#[must_use]
pub struct RequestAdmission<'a, const BYTES: usize> {
    slot: &'a mut Option<Slot<BYTES>>,
    generation: &'a mut u32,
    key: ServiceKey,
    digest: [u8; 16],
    deadline: u64,
    action: RequestAction,
}
impl<const B: usize> RequestAdmission<'_, B> {
    /// Prospective action, not executable before commit; 预计动作，提交前不能执行。
    #[must_use]
    pub const fn action(&self) -> RequestAction {
        self.action
    }
    /// Frozen original retention; 原始不续期保留时限。
    #[must_use]
    pub const fn deadline(&self) -> u64 {
        self.deadline
    }
    /// Infallible publication after Security Replay commit; Replay之后无失败发布。
    #[must_use]
    pub fn commit(self) -> RequestAction {
        if let RequestAction::Invoke(h) = self.action {
            *self.slot = Some(Slot {
                key: self.key,
                digest: self.digest,
                phase: RequestPhase::WaitResult,
                deadline: self.deadline,
                result: 0,
                length: 0,
                bytes: [0; B],
            });
            *self.generation = h.generation;
        }
        self.action
    }
}
/// Exclusive result preflight; 独占结果预检。
#[must_use]
pub struct ResultAdmission<'a, 'p, const BYTES: usize> {
    slot: &'a mut Slot<BYTES>,
    result: i32,
    payload: &'p [u8],
    fresh: bool,
}
impl<const B: usize> ResultAdmission<'_, '_, B> {
    /// Fresh publication or unchanged duplicate; 新发布或不变重复。
    #[must_use]
    pub const fn fresh(&self) -> bool {
        self.fresh
    }
    /// Infallible result commit, no callback; 无失败结果提交，不调用回调。
    pub fn commit(self) {
        if self.fresh {
            self.slot.bytes[..self.payload.len()].copy_from_slice(self.payload);
            self.slot.length = self.payload.len();
            self.slot.result = self.result;
            self.slot.phase = RequestPhase::Complete;
        }
    }
}
/// Server request verdict; Server请求裁决。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RequestAction {
    /// New one-shot application invocation; 新一次性业务调用。
    Invoke(RequestHandle),
    /// Still pending, no second invocation; 待完成，不重调。
    Pending(RequestHandle),
    /// Stable result replay; 稳定结果重放。
    Replay(RequestHandle),
}
/// Independent fixed RPC client and server receipt tables.
/// `独立固定Client关联及Server` Receipt表。
pub struct ServiceOwner<const REQUESTS: usize, const RECEIPTS: usize, const BYTES: usize> {
    owner: u32,
    retention: u64,
    requests: [Option<Slot<BYTES>>; REQUESTS],
    request_gen: [u32; REQUESTS],
    receipts: [Option<Slot<BYTES>>; RECEIPTS],
    receipt_gen: [u32; RECEIPTS],
    cursor: usize,
}
impl<const N: usize, const R: usize, const B: usize> ServiceOwner<N, R, B> {
    /// Initialize volatile RPC, without a Persistence dependency.
    /// 初始化易失RPC，不要求Persistence。
    /// # Errors
    /// Invalid identity/capacity/retention rejects.
    pub const fn new(owner: u32, retention: u64) -> Result<Self> {
        if owner == 0 || N == 0 || R == 0 || B == 0 || retention == 0 || retention > i64::MAX as u64
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            retention,
            requests: [const { None }; N],
            request_gen: [0; N],
            receipts: [const { None }; R],
            receipt_gen: [0; R],
            cursor: 0,
        })
    }
    /// Reserve exact request correlation; 预留精确Request关联。
    /// # Errors
    /// Duplicate/conflict/capacity/deadline reject atomically.
    pub fn request(
        &mut self,
        key: ServiceKey,
        digest: [u8; 16],
        now: u64,
        until: u64,
    ) -> Result<RequestHandle> {
        key.validate()?;
        if digest == [0; 16] || now == 0 || now >= until {
            return Err(Error::Argument);
        }
        if self
            .requests
            .iter()
            .flatten()
            .any(|slot| slot.key.client == key.client && slot.key.operation == key.operation)
        {
            return Err(Error::Replay);
        }
        let slot = self
            .requests
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.request_gen[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.requests[slot] = Some(Slot {
            key,
            digest,
            phase: RequestPhase::WaitSend,
            deadline: until,
            result: 0,
            length: 0,
            bytes: [0; B],
        });
        self.request_gen[slot] = generation;
        Ok(RequestHandle {
            owner: self.owner,
            slot,
            generation,
            server: false,
        })
    }
    fn validate(&self, h: RequestHandle) -> Result<usize> {
        let capacity = if h.server { R } else { N };
        if h.owner != self.owner || h.slot >= capacity {
            return Err(Error::State);
        }
        let generation = if h.server {
            self.receipt_gen[h.slot]
        } else {
            self.request_gen[h.slot]
        };
        let occupied = if h.server {
            self.receipts[h.slot].is_some()
        } else {
            self.requests[h.slot].is_some()
        };
        if h.generation != generation || !occupied {
            return Err(Error::State);
        }
        Ok(h.slot)
    }
    /// Mark real request submission; 标记真实请求发送。
    /// # Errors
    /// Wrong phase/server handle or deadline rejects.
    pub fn sent(&mut self, h: RequestHandle, now: u64) -> Result<()> {
        let index = self.validate(h)?;
        if h.server {
            return Err(Error::State);
        }
        let slot = self.requests[index].as_mut().ok_or(Error::State)?;
        if slot.phase != RequestPhase::WaitSend || now == 0 || now >= slot.deadline {
            return Err(Error::State);
        }
        slot.phase = RequestPhase::WaitResult;
        Ok(())
    }
    /// Accept final server result with exact original correlation.
    /// 精确原始关联下接纳最终Server结果。
    /// # Errors
    /// Wrong principal/session/key/role or conflicting replay rejects.
    pub fn on_result(
        &mut self,
        h: RequestHandle,
        key: ServiceKey,
        result: i32,
        payload: &[u8],
        now: u64,
    ) -> Result<()> {
        self.prepare_result(h, key, result, payload, now)?.commit();
        Ok(())
    }
    /// Validate a result without publishing it before Replay; Replay前只预检结果。
    /// # Errors
    /// Wrong full key, phase, bytes or lifetime leaves the result unchanged.
    pub fn prepare_result<'a, 'p>(
        &'a mut self,
        h: RequestHandle,
        key: ServiceKey,
        result: i32,
        payload: &'p [u8],
        now: u64,
    ) -> Result<ResultAdmission<'a, 'p, B>> {
        let index = self.validate(h)?;
        if h.server || payload.len() > B {
            return Err(Error::Argument);
        }
        let slot = self.requests[index].as_mut().ok_or(Error::State)?;
        if slot.key != key || now == 0 {
            return Err(Error::Access);
        }
        if slot.phase == RequestPhase::Complete {
            if slot.result != result || slot.bytes[..slot.length] != *payload {
                return Err(Error::Replay);
            }
            return Ok(ResultAdmission {
                slot,
                result,
                payload,
                fresh: false,
            });
        }
        if slot.phase != RequestPhase::WaitResult || now >= slot.deadline {
            return Err(Error::State);
        }
        Ok(ResultAdmission {
            slot,
            result,
            payload,
            fresh: true,
        })
    }
    /// Reserve server dedup and response before app invocation.
    /// 业务调用前预留Server去重及回复资源。
    /// # Errors
    /// Conflicting ID/digest or unexpired-full table fails closed.
    pub fn on_request(
        &mut self,
        key: ServiceKey,
        digest: [u8; 16],
        now: u64,
    ) -> Result<RequestAction> {
        Ok(self.prepare_request(key, digest, now)?.commit())
    }
    /// Reserve no state until the caller commits Replay; 调用方提交Replay前不改状态。
    /// # Errors
    /// Conflict/capacity/deadline failure leaves all existing slots unchanged.
    pub fn prepare_request(
        &mut self,
        key: ServiceKey,
        digest: [u8; 16],
        now: u64,
    ) -> Result<RequestAdmission<'_, B>> {
        key.validate()?;
        if digest == [0; 16] || now == 0 {
            return Err(Error::Argument);
        }
        for (index, slot) in self.receipts.iter().enumerate() {
            if let Some(slot) = slot {
                if slot.key.client == key.client && slot.key.operation == key.operation {
                    if slot.key != key || slot.digest != digest || now >= slot.deadline {
                        return Err(Error::Replay);
                    }
                    let h = RequestHandle {
                        owner: self.owner,
                        slot: index,
                        generation: self.receipt_gen[index],
                        server: true,
                    };
                    let action = if slot.phase == RequestPhase::Complete {
                        RequestAction::Replay(h)
                    } else {
                        RequestAction::Pending(h)
                    };
                    let deadline = slot.deadline;
                    return Ok(RequestAdmission {
                        slot: &mut self.receipts[index],
                        generation: &mut self.receipt_gen[index],
                        key,
                        digest,
                        deadline,
                        action,
                    });
                }
            }
        }
        let index = self
            .receipts
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.receipt_gen[index]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let deadline = now.checked_add(self.retention).ok_or(Error::Argument)?;
        let action = RequestAction::Invoke(RequestHandle {
            owner: self.owner,
            slot: index,
            generation,
            server: true,
        });
        Ok(RequestAdmission {
            slot: &mut self.receipts[index],
            generation: &mut self.receipt_gen[index],
            key,
            digest,
            deadline,
            action,
        })
    }
    /// Exact explicit server retirement after its original lifetime; Server精确显式退休。
    /// An already retired private handle is idempotent, never erases a newer slot.
    /// 已退休的私有Handle幂等，不能清除更新代际的槽；Owner命名空间必须精确。
    /// # Errors
    /// Active/client/cross-owner or impossible future handles cannot erase another slot.
    pub fn retire_server(&mut self, h: RequestHandle, now: u64) -> Result<()> {
        if !h.server
            || h.owner != self.owner
            || h.slot >= R
            || h.generation == 0
            || h.generation > self.receipt_gen[h.slot]
        {
            return Err(Error::State);
        }
        if h.generation != self.receipt_gen[h.slot] || self.receipts[h.slot].is_none() {
            return Ok(());
        }
        if now < self.receipts[h.slot].as_ref().ok_or(Error::State)?.deadline {
            return Err(Error::State);
        }
        self.receipts[h.slot] = None;
        Ok(())
    }
    /// Respond once using pre-reserved result storage; 使用预留结果区回复一次。
    /// # Errors
    /// Client handle, repeated response, deadline or oversize rejects.
    pub fn respond(
        &mut self,
        h: RequestHandle,
        result: i32,
        payload: &[u8],
        now: u64,
    ) -> Result<()> {
        let index = self.validate(h)?;
        if !h.server || payload.len() > B {
            return Err(Error::Argument);
        }
        let slot = self.receipts[index].as_mut().ok_or(Error::State)?;
        if slot.phase != RequestPhase::WaitResult || now == 0 || now >= slot.deadline {
            return Err(Error::State);
        }
        slot.bytes[..payload.len()].copy_from_slice(payload);
        slot.length = payload.len();
        slot.result = result;
        slot.phase = RequestPhase::Complete;
        Ok(())
    }
    /// Read exact request view; 读取精确请求视图。
    /// # Errors
    /// Stale handle rejects.
    pub fn view(&self, h: RequestHandle) -> Result<RequestView> {
        let index = self.validate(h)?;
        let slot = if h.server {
            self.receipts[index].as_ref()
        } else {
            self.requests[index].as_ref()
        }
        .ok_or(Error::State)?;
        Ok(RequestView {
            key: slot.key,
            phase: slot.phase,
            deadline: slot.deadline,
            result: slot.result,
            bytes: slot.length,
        })
    }
    /// Copy retained result bytes; 复制保留结果。
    /// # Errors
    /// Nonterminal/capacity errors leave output unchanged.
    pub fn copy_result(&self, h: RequestHandle, out: &mut [u8]) -> Result<usize> {
        let index = self.validate(h)?;
        let slot = if h.server {
            self.receipts[index].as_ref()
        } else {
            self.requests[index].as_ref()
        }
        .ok_or(Error::State)?;
        if slot.phase != RequestPhase::Complete {
            return Err(Error::State);
        }
        if out.len() < slot.length {
            return Err(Error::NoSpace);
        }
        out[..slot.length].copy_from_slice(&slot.bytes[..slot.length]);
        Ok(slot.length)
    }
    /// Cancel logical client request; 取消Client逻辑请求。
    /// # Errors
    /// Server/terminal handle rejects.
    pub fn cancel(&mut self, h: RequestHandle) -> Result<()> {
        let index = self.validate(h)?;
        if h.server {
            return Err(Error::State);
        }
        let slot = self.requests[index].as_mut().ok_or(Error::State)?;
        if !matches!(
            slot.phase,
            RequestPhase::WaitSend | RequestPhase::WaitResult
        ) {
            return Err(Error::State);
        }
        slot.phase = RequestPhase::Cancelled;
        Ok(())
    }
    /// Client terminal retirement; Client终态退休。
    /// # Errors
    /// Active/server handle rejects.
    pub fn retire(&mut self, h: RequestHandle) -> Result<()> {
        let index = self.validate(h)?;
        if h.server
            || !matches!(
                self.requests[index].as_ref().ok_or(Error::State)?.phase,
                RequestPhase::Complete | RequestPhase::Failed | RequestPhase::Cancelled
            )
        {
            return Err(Error::State);
        }
        self.requests[index] = None;
        Ok(())
    }
    /// Bounded explicit expiry; 显式有界过期。
    /// # Errors
    /// Unknown time or zero budget rejects.
    pub fn step(&mut self, now: u64, budget: usize) -> Result<usize> {
        if now == 0 || budget == 0 {
            return Err(Error::Argument);
        }
        let count = budget.min(N + R);
        for _ in 0..count {
            let index = self.cursor;
            self.cursor = (self.cursor + 1) % (N + R);
            if index < N {
                if let Some(slot) = self.requests[index].as_mut() {
                    if now >= slot.deadline
                        && matches!(
                            slot.phase,
                            RequestPhase::WaitSend | RequestPhase::WaitResult
                        )
                    {
                        slot.phase = RequestPhase::Failed;
                    }
                }
            } else {
                let index = index - N;
                if self.receipts[index].as_ref().is_some_and(|slot| {
                    now >= slot.deadline && slot.phase == RequestPhase::Complete
                }) {
                    self.receipts[index] = None;
                }
            }
        }
        Ok(count)
    }
}
/// Nano fixed RPC; `Nano固定RPC`。
pub type NanoServiceOwner = ServiceOwner<2, 2, 32>;
/// Lite fixed RPC; `Lite固定RPC`。
pub type LiteServiceOwner = ServiceOwner<8, 8, 64>;
/// Full fixed RPC; `Full固定RPC`。
pub type FullServiceOwner = ServiceOwner<16, 24, 128>;
