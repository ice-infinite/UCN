//! One raw ingress and one global budget for a static node's independent client/server halves.
//! 静态节点的独立客户端/服务端共用一个原始入口和全局预算。
use crate::runtime_common::CLASSES;
use crate::{
    DurableExecutor, JournalPeer, JournalRuntime, JournalRuntimeStats, JournalStepBudget,
    JournalStepReport, JournalWorkClass, SourcePhase, SourceResult, SourceRuntime,
};
use ucn_adapter::{AdapterOwner, RxToken, TxDriver};
use ucn_owner::CallbackClaim;
use ucn_persistence::{PersistenceOwner, PersistenceProvider};
use ucn_security::{C0TransactionOwner, CryptoProvider, OriginSequenceOwner, SecurityOwner};
use ucn_service::{OperationKey, OperationOwner};
use ucn_types::{Error, HeaderContract, InteractionRole, PayloadKind, ProtocolOpcode, Result};
use ucn_wire::CommonHeader;

/// Fixed one-Peer duplex composition, not dynamic routing or a complete universal Node.
/// 固定单Peer双向装配，非动态路由或完整通用Node。
pub struct DuplexRuntime<
    const COMMANDS: usize,
    const FRAME: usize,
    const ARGUMENTS: usize,
    const RESULT_BYTES: usize,
    const CLIENT_BYTES: usize,
> {
    owner: u32,
    source: SourceRuntime<FRAME, CLIENT_BYTES>,
    server: JournalRuntime<COMMANDS, FRAME, ARGUMENTS, RESULT_BYTES>,
    raw: [u8; FRAME],
    rx: Option<RxToken>,
    cursor: usize,
    role_cursor: [bool; 7],
    stats: JournalRuntimeStats,
    last_now: u64,
    stopped: bool,
}
impl<const C: usize, const F: usize, const A: usize, const R: usize, const B: usize>
    DuplexRuntime<C, F, A, R, B>
{
    /// Move clean halves under a single Coordinator; never move live physical obligations.
    /// 干净两半移入单Coordinator，不搬移活物理义务。
    /// # Errors
    /// Owner ranges/Journal domains overlap, or active half, rejects before I/O.
    pub fn new(
        owner: u32,
        source: SourceRuntime<F, B>,
        server: JournalRuntime<C, F, A, R>,
    ) -> Result<Self> {
        let (s0, s1, ids) = source.duplex_binding()?;
        let (r0, r1, journal) = server.duplex_binding()?;
        if owner == 0
            || ids == journal
            || !(s1 < r0 || r1 < s0)
            || (s0..=s1).contains(&owner)
            || (r0..=r1).contains(&owner)
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            source,
            server,
            raw: [0; F],
            rx: None,
            cursor: 0,
            role_cursor: [false; 7],
            stats: JournalRuntimeStats::default(),
            last_now: 0,
            stopped: false,
        })
    }
    /// Queue a local durable request, not a direct Driver call; 排队本地耐久请求，非直接Driver调用。
    /// # Errors
    /// Capacity/clock/current work errors leave accepted work unchanged.
    pub fn request(&mut self, bytes: &[u8], now: u64, deadline: u64) -> Result<()> {
        self.source.request(bytes, now, deadline)
    }
    /// Recover an original correlation without new execution; 恢复原关联，不新执行。
    /// # Errors
    /// Wrong identity/lifetime or active source rejects.
    pub fn recover(&mut self, key: OperationKey, now: u64, deadline: u64) -> Result<()> {
        self.source.recover(key, now, deadline)
    }
    /// Fixed local source phase; 固定本地源端阶段。
    #[must_use]
    pub const fn source_phase(&self) -> SourcePhase {
        self.source.phase()
    }
    /// Original accepted key, not a mutation handle; 原接纳键，非可变Handle。
    #[must_use]
    pub const fn operation_key(&self) -> Option<OperationKey> {
        self.source.operation_key()
    }
    /// Copy authenticated result; 复制认证结果。
    /// # Errors
    /// Missing result/short output preserves output.
    pub fn copy_result(&self, output: &mut [u8]) -> Result<SourceResult> {
        self.source.copy_result(output)
    }
    /// Read-only confirmed server Journal; 只读已确认服务端Journal。
    #[must_use]
    pub const fn server_operation(&self) -> &OperationOwner<R> {
        self.server.operation()
    }
    /// Global visit diagnostics; 全局访问诊断。
    #[must_use]
    pub const fn stats(&self) -> JournalRuntimeStats {
        self.stats
    }
    /// Fence both halves; drain exact Provider/Driver/RX obligations in later steps.
    /// 围栏两半，后续step收口精确Provider/Driver/RX义务。
    pub fn stop(&mut self) {
        self.stopped = true;
        self.source.stop();
        self.server.stop();
    }
    /// One global budget; one Foundation step, not one per child.
    /// 单全局预算，Foundation每次只走一步而非每子模块各一步。
    /// # Errors
    /// Both environment checks occur before RX claim or child mutation.
    #[allow(clippy::too_many_arguments)]
    pub fn step<
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        budget: JournalStepBudget,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
        provider: &mut impl PersistenceProvider,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
        driver: &mut impl TxDriver,
        sequence: &mut impl OriginSequenceOwner,
        transaction: &mut impl C0TransactionOwner,
        executor: &mut impl DurableExecutor,
    ) -> Result<JournalStepReport> {
        if peer.facts.now_us < self.last_now {
            return Err(Error::State);
        }
        self.source.environment(peer, security, persistence)?;
        self.server
            .validate_environment(peer.facts.now_us, security, persistence)?;
        let gate = security.callback_gate();
        self.last_now = peer.facts.now_us;
        let mut report = JournalStepReport {
            visits: [0; 7],
            progress: 0,
            error: None,
            quiescent: false,
        };
        for _ in 0..budget.total {
            for _ in 0..7 {
                if report.visits[self.cursor] < budget.caps[self.cursor] {
                    break;
                }
                self.cursor = (self.cursor + 1) % 7;
            }
            let index = self.cursor;
            self.cursor = (index + 1) % 7;
            report.visits[index] += 1;
            self.stats.visits[index] = self.stats.visits[index].saturating_add(1);
            let class = CLASSES[index];
            let advanced = match class {
                JournalWorkClass::Ingress => self.ingress(peer, security, crypto, adapter),
                JournalWorkClass::Foundation => {
                    match gate.try_enter(CallbackClaim::new(self.owner, 1, 1, 0x0724)?) {
                        Ok(lease) => {
                            let r = persistence
                                .step(provider, peer.facts.now_us, 1)
                                .map(|r| r.made_progress);
                            gate.leave(lease)?;
                            r
                        }
                        Err(e) => Err(e),
                    }
                }
                _ => {
                    self.role_cursor[index] = !self.role_cursor[index];
                    let one = JournalStepBudget::one_class(class);
                    let r = if self.role_cursor[index] {
                        self.source.step(
                            one,
                            peer,
                            security,
                            crypto,
                            persistence,
                            provider,
                            adapter,
                            driver,
                            sequence,
                            transaction,
                        )
                    } else {
                        self.server.step(
                            one,
                            peer,
                            security,
                            crypto,
                            persistence,
                            provider,
                            adapter,
                            driver,
                            sequence,
                            transaction,
                            executor,
                        )
                    };
                    match r {
                        Ok(r) => {
                            if let Some(error) = r.error {
                                Err(error)
                            } else {
                                Ok(r.progress != 0)
                            }
                        }
                        Err(e) => Err(e),
                    }
                }
            };
            match advanced {
                Ok(true) => report.progress += 1,
                Ok(false) => {}
                Err(e) => {
                    if report.error.is_none() {
                        report.error = Some(e);
                    }
                }
            }
        }
        self.stats.executions = self.server.stats().executions;
        report.quiescent = self.stopped
            && self.rx.is_none()
            && adapter.rx_is_quiescent()
            && self.source.quiescent()
            && self.server.quiescent();
        Ok(report)
    }
    fn ingress<const S: usize, const Q: usize, const T: usize, const RX: usize>(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
    ) -> Result<bool> {
        if self.stopped {
            adapter.try_set_rx_enabled(false)?;
        }
        if let Some(token) = self.rx {
            adapter.rx_retire(token)?;
            self.rx = None;
            return Ok(true);
        }
        let item = match adapter.rx_claim(&mut self.raw) {
            Ok(v) => v,
            Err(Error::NotFound) => return Ok(false),
            Err(e) => return Err(e),
        };
        self.rx = Some(item.token);
        self.stats.rx = self.stats.rx.saturating_add(1);
        let selected = if self.stopped {
            Ok(())
        } else {
            Self::target(
                &self.raw[..item.frame_bytes],
                security.address_width().bytes(),
            )
            .and_then(|source| {
                if source {
                    self.source.receive_shared(
                        &self.raw[..item.frame_bytes],
                        item.link,
                        peer,
                        security,
                        crypto,
                    )
                } else {
                    self.server.receive_shared(
                        &self.raw[..item.frame_bytes],
                        item.link,
                        peer,
                        security,
                        crypto,
                        adapter,
                    )
                }
            })
        };
        if selected.is_err() {
            self.stats.rejected = self.stats.rejected.saturating_add(1);
        }
        adapter.rx_retire(item.token)?;
        self.rx = None;
        self.stats.retired = self.stats.retired.saturating_add(1);
        selected.map(|()| true)
    }
    fn target(raw: &[u8], width: usize) -> Result<bool> {
        // Unauthenticated header is only a dispatch hint. Both children perform actual authentication.
        // 未认证头仅选分派路径，两子管线仍做真实认证。
        let h = CommonHeader::decode(raw)?;
        match (h.contract, h.payload_kind, h.interaction) {
            (HeaderContract::C1, PayloadKind::Data, InteractionRole::Request) => Ok(false),
            (
                HeaderContract::C1,
                PayloadKind::Data,
                InteractionRole::Result | InteractionRole::Error,
            ) => Ok(true),
            (HeaderContract::C0, PayloadKind::Control, _) => {
                let prefix = 25 + 2 * width;
                if raw.len() < prefix + 16 {
                    return Err(Error::Malformed);
                }
                match ProtocolOpcode::try_from(u16::from_be_bytes(
                    raw[prefix - 2..prefix]
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                ))? {
                    ProtocolOpcode::OperationResult => Ok(true),
                    ProtocolOpcode::OperationQuery
                    | ProtocolOpcode::OperationCancel
                    | ProtocolOpcode::OperationResume
                    | ProtocolOpcode::OperationResultAck => Ok(false),
                    _ => Err(Error::Unsupported),
                }
            }
            _ => Err(Error::Unsupported),
        }
    }
}
