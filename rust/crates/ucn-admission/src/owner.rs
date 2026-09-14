use ucn_identity::{BindingView, Principal};
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_types::{Error, Result};

use crate::codec::{
    BootstrapEvent, BootstrapFlow, BootstrapPhase, BootstrapTranscript, COOKIE_MAX_BYTES,
    CookieChallenge, Evidence, Hello, HelloCookie, transcript_transition_valid,
    validate_transcript_phase,
};

const CALLBACK_ISSUE_COOKIE: u16 = 0x0610;
const CALLBACK_VERIFY_COOKIE: u16 = 0x0611;
const CALLBACK_AUTHORIZE_EVENT: u16 = 0x0612;

/// 一个入站 Link 的精确身份。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LinkIdentity {
    /// Manifest Link ID。
    pub link_id: u16,
    /// Link reopen 后严格递增的 Generation。
    pub link_generation: u32,
}

impl LinkIdentity {
    fn validate(self) -> Result<()> {
        if self.link_id == 0 || self.link_generation == 0 {
            return Err(Error::Argument);
        }
        Ok(())
    }
}

/// 一个 JOIN pending 的不可变主键。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AdmissionKey {
    /// 精确入站 Link。
    pub link: LinkIdentity,
    /// 本机 Driver/Peer 临时区分值。
    pub local_peer_discriminator: u32,
    /// HELLO 身份摘要。
    pub identity_digest: Principal,
    /// 事务 ID。
    pub transaction_id: u64,
}

impl AdmissionKey {
    fn validate(self) -> Result<()> {
        self.link.validate()?;
        if self.local_peer_discriminator == 0 || self.transaction_id == 0 {
            return Err(Error::Argument);
        }
        Ok(())
    }
}

/// Stateless Cookie 与 JOIN 事件的可信产品 Provider。
pub trait CookieProvider {
    /// 生成绑定请求、Link、时间桶和本地秘密的 Cookie。
    ///
    /// # Errors
    ///
    /// 产品密钥、随机源或输出合同不可用时返回错误。
    fn issue_cookie(
        &mut self,
        hello: &Hello,
        link: LinkIdentity,
        cookie_time_bucket: u32,
        output: &mut [u8; COOKIE_MAX_BYTES],
    ) -> Result<u8>;

    /// 在任何 pending 分配前验证 `HELLO_COOKIE`。
    ///
    /// # Errors
    ///
    /// Cookie 或其绑定的 JOIN 主键不成立时返回错误。
    fn verify_cookie(&mut self, hello_cookie: &HelloCookie, key: &AdmissionKey) -> Result<()>;

    /// 验证严格状态机中一个事件的完整 transcript 与 Evidence。
    ///
    /// # Errors
    ///
    /// 身份、签名、证据或 transcript 绑定不成立时返回错误。
    fn authorize_event(
        &mut self,
        event: BootstrapEvent,
        key: &AdmissionKey,
        transcript: &BootstrapTranscript,
        now_us: u64,
        evidence: &Evidence,
    ) -> Result<()>;
}

/// Admission Owner 静态配置。
pub struct AdmissionConfig<'a> {
    /// Runtime 实例。
    pub runtime_instance: u32,
    /// Owner instance。
    pub owner_instance: u32,
    /// 唯一允许签发最终 Binding View 的 Identity Owner 实例。
    pub identity_owner_instance: u32,
    /// 共享 Provider callback gate。
    pub provider_gate: &'a CallbackGate,
    /// 每条 Link 最大 pending 数。
    pub max_pending_per_link: usize,
    /// 每条 Link 初始 token 数。
    pub token_burst: u8,
    /// 每秒补充 token 数。
    pub tokens_per_second: u8,
    /// pending 固定半开寿命。
    pub pending_timeout_us: u64,
}

/// 认证前 HELLO/Cookie 签发的单次输入快照。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct InitialHelloContext {
    /// 精确入站 Link。
    pub link: LinkIdentity,
    /// Owner 读取的本地单调时间。
    pub now_us: u64,
    /// 完整 HELLO 请求字节数。
    pub request_bytes: usize,
    /// 拟发 Cookie Challenge 字节数。
    pub response_bytes: usize,
    /// Cookie 时间桶。
    pub cookie_time_bucket: u32,
}

#[derive(Clone, Copy)]
struct LinkBudget {
    occupied: bool,
    link: LinkIdentity,
    tokens: u8,
    last_refill_us: u64,
}

impl LinkBudget {
    const EMPTY: Self = Self {
        occupied: false,
        link: LinkIdentity {
            link_id: 0,
            link_generation: 0,
        },
        tokens: 0,
        last_refill_us: 0,
    };
}

#[derive(Clone, Copy)]
struct Pending {
    occupied: bool,
    slot_generation: u32,
    phase: BootstrapPhase,
    key: Option<AdmissionKey>,
    transcript: Option<BootstrapTranscript>,
    challenge_started_local_us: u64,
    deadline_us: u64,
    admitted: Option<AdmittedView>,
}

impl Pending {
    const EMPTY: Self = Self {
        occupied: false,
        slot_generation: 0,
        phase: BootstrapPhase::Aborted,
        key: None,
        transcript: None,
        challenge_started_local_us: 0,
        deadline_us: 0,
        admitted: None,
    };
}

/// Admission Owner 签发的精确 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AdmissionHandle {
    owner_instance: u32,
    slot: u16,
    slot_generation: u32,
    transaction_id: u64,
}

/// Pending 的只读快照。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PendingView {
    /// 当前阶段。
    pub phase: BootstrapPhase,
    /// 不可变主键。
    pub key: AdmissionKey,
    /// Owner 捕获的 challenge 起点。
    pub challenge_started_local_us: u64,
    /// 不会被重复消息刷新的半开 Deadline。
    pub deadline_us: u64,
}

/// 完成 Identity durable proof 后发布的准入结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AdmittedView {
    /// Identity Owner 形成的 live Binding view。
    pub binding: BindingView,
    /// JOIN transcript 的最终安全 Profile Generation。
    pub session_generation: u32,
    /// 精确入站 Link。
    pub link: LinkIdentity,
    /// 原 pending 的 slot generation。
    pub admission_generation: u32,
}

/// 固定容量 Dynamic Admission Owner。
pub struct AdmissionOwner<'a, const PENDING: usize, const LINKS: usize> {
    config: AdmissionConfig<'a>,
    next_operation_id: u32,
    budgets: [LinkBudget; LINKS],
    pending: [Pending; PENDING],
}

/// Nano Profile：2 pending、2 Link budget。
pub type NanoAdmissionOwner<'a> = AdmissionOwner<'a, 2, 2>;
/// Lite Profile：8 pending、8 Link budget。
pub type LiteAdmissionOwner<'a> = AdmissionOwner<'a, 8, 8>;
/// Full Profile：16 pending、16 Link budget。
pub type FullAdmissionOwner<'a> = AdmissionOwner<'a, 16, 16>;

impl<'a, const PENDING: usize, const LINKS: usize> AdmissionOwner<'a, PENDING, LINKS> {
    /// 建立固定容量 Admission Owner。
    ///
    /// # Errors
    ///
    /// Owner、容量、限流或 Deadline 配置非法时返回配置错误。
    pub fn new(config: AdmissionConfig<'a>) -> Result<Self> {
        if config.runtime_instance == 0
            || config.owner_instance == 0
            || config.identity_owner_instance == 0
            || PENDING == 0
            || PENDING > u16::MAX as usize
            || LINKS == 0
            || config.max_pending_per_link == 0
            || config.max_pending_per_link > PENDING
            || config.token_burst == 0
            || config.tokens_per_second == 0
            || config.pending_timeout_us == 0
        {
            return Err(Error::Config);
        }
        Ok(Self {
            config,
            next_operation_id: 1,
            budgets: [LinkBudget::EMPTY; LINKS],
            pending: [Pending::EMPTY; PENDING],
        })
    }

    fn consume_initial_hello(
        &mut self,
        link: LinkIdentity,
        now_us: u64,
        request_bytes: usize,
        response_bytes: usize,
    ) -> Result<()> {
        link.validate()?;
        if request_bytes == 0 || response_bytes > request_bytes {
            return Err(Error::Argument);
        }
        let index = self
            .budgets
            .iter()
            .position(|budget| budget.occupied && budget.link == link)
            .or_else(|| self.budgets.iter().position(|budget| !budget.occupied))
            .ok_or(Error::NoSpace)?;
        let budget = &mut self.budgets[index];
        if !budget.occupied {
            *budget = LinkBudget {
                occupied: true,
                link,
                tokens: self.config.token_burst,
                last_refill_us: now_us,
            };
        } else if now_us < budget.last_refill_us {
            return Err(Error::State);
        } else {
            let seconds = (now_us - budget.last_refill_us) / 1_000_000;
            if seconds != 0 {
                let addition = seconds.saturating_mul(u64::from(self.config.tokens_per_second));
                budget.tokens = u8::try_from(
                    u64::from(budget.tokens)
                        .saturating_add(addition)
                        .min(u64::from(self.config.token_burst)),
                )
                .map_err(|_| Error::Exhausted)?;
                budget.last_refill_us = budget
                    .last_refill_us
                    .checked_add(seconds.saturating_mul(1_000_000))
                    .ok_or(Error::Exhausted)?;
            }
        }
        if budget.tokens == 0 {
            return Err(Error::Access);
        }
        budget.tokens -= 1;
        Ok(())
    }

    /// 限流后生成无状态、无放大的 Cookie Challenge；不分配 JOIN pending。
    ///
    /// 限流、无放大检查和 Provider 回调是同一公开入口，调用者不能绕过前两项直接签发 Cookie。
    ///
    /// # Errors
    ///
    /// HELLO、Link、时间、限流、无放大或 Provider 输出不成立时返回错误。
    pub fn issue_cookie<P: CookieProvider>(
        &mut self,
        provider: &mut P,
        hello: Hello,
        context: InitialHelloContext,
    ) -> Result<CookieChallenge> {
        context.link.validate()?;
        if hello.flow != BootstrapFlow::Join
            || hello.device_nonce == 0
            || hello.transaction_id == 0
            || context.cookie_time_bucket == 0
        {
            return Err(Error::Argument);
        }
        self.consume_initial_hello(
            context.link,
            context.now_us,
            context.request_bytes,
            context.response_bytes,
        )?;
        let mut cookie = [0; COOKIE_MAX_BYTES];
        let claim = self.next_claim(CALLBACK_ISSUE_COOKIE)?;
        let callback_lease = self.config.provider_gate.try_enter(claim)?;
        let result = provider.issue_cookie(
            &hello,
            context.link,
            context.cookie_time_bucket,
            &mut cookie,
        );
        let leave_result = self.config.provider_gate.leave(callback_lease);
        let length = result?;
        leave_result?;
        if length == 0
            || usize::from(length) > COOKIE_MAX_BYTES
            || cookie[..usize::from(length)].iter().all(|byte| *byte == 0)
            || cookie[usize::from(length)..].iter().any(|byte| *byte != 0)
        {
            return Err(Error::Security);
        }
        Ok(CookieChallenge {
            flow: BootstrapFlow::Join,
            transaction_id: hello.transaction_id,
            cookie_time_bucket: context.cookie_time_bucket,
            cookie_length: length,
            cookie,
        })
    }

    /// Cookie 验证成功后才分配一个固定 pending；重复消息不刷新 Deadline。
    ///
    /// # Errors
    ///
    /// Cookie、主键、Transcript、Deadline 或固定容量不成立时失败关闭。
    pub fn open_after_cookie<P: CookieProvider>(
        &mut self,
        provider: &mut P,
        key: AdmissionKey,
        hello_cookie: HelloCookie,
        transcript: BootstrapTranscript,
        now_us: u64,
    ) -> Result<AdmissionHandle> {
        key.validate()?;
        validate_transcript_phase(&transcript, BootstrapPhase::CookieVerified)?;
        if hello_cookie.flow != BootstrapFlow::Join
            || transcript.flow != BootstrapFlow::Join
            || key.identity_digest != hello_cookie.identity_digest
            || key.identity_digest != transcript.joining_device_identity_digest
            || key.transaction_id != hello_cookie.transaction_id
            || key.transaction_id != transcript.transaction_id
            || key.link.link_id != hello_cookie.selected_link_instance_id
            || key.link.link_id != transcript.selected_link_instance_id
            || key.link.link_generation != hello_cookie.selected_link_instance_generation
            || key.link.link_generation != transcript.selected_link_instance_generation
            || hello_cookie.device_nonce != transcript.device_nonce
            || hello_cookie.lease_freshness_challenge_nonce
                != transcript.lease_freshness_challenge_nonce
            || hello_cookie.prior_messages_hash != transcript.prior_messages_hash
        {
            return Err(Error::Security);
        }
        self.call_verify_cookie(provider, &hello_cookie, &key)?;
        if let Some((index, existing)) = self
            .pending
            .iter()
            .enumerate()
            .find(|(_, slot)| slot.occupied && slot.key == Some(key))
        {
            if existing.transcript != Some(transcript) || now_us >= existing.deadline_us {
                return Err(Error::Replay);
            }
            return self.make_handle(index);
        }
        let link_count = self
            .pending
            .iter()
            .filter(|slot| slot.occupied && slot.key.is_some_and(|value| value.link == key.link))
            .count();
        if link_count >= self.config.max_pending_per_link {
            return Err(Error::NoSpace);
        }
        let index = self
            .pending
            .iter()
            .position(|slot| !slot.occupied)
            .ok_or(Error::NoSpace)?;
        let deadline_us = now_us
            .checked_add(self.config.pending_timeout_us)
            .ok_or(Error::Exhausted)?;
        let slot_generation = self.pending[index]
            .slot_generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.pending[index] = Pending {
            occupied: true,
            slot_generation,
            phase: BootstrapPhase::CookieVerified,
            key: Some(key),
            transcript: Some(transcript),
            challenge_started_local_us: now_us,
            deadline_us,
            admitted: None,
        };
        self.make_handle(index)
    }

    /// 验证并推进一个严格相邻事件；字段只可在所属阶段从零补齐一次。
    ///
    /// # Errors
    ///
    /// Handle、Deadline、阶段、事件证据或字段不变式不成立时返回错误。
    pub fn advance<P: CookieProvider>(
        &mut self,
        provider: &mut P,
        handle: AdmissionHandle,
        event: BootstrapEvent,
        transcript: BootstrapTranscript,
        evidence: Evidence,
        now_us: u64,
    ) -> Result<()> {
        let index = self.handle_index(handle)?;
        let slot = self.pending[index];
        if matches!(
            slot.phase,
            BootstrapPhase::FinalDurable | BootstrapPhase::Aborted
        ) {
            return Err(Error::State);
        }
        let expected_phase = match (slot.phase, event) {
            (BootstrapPhase::CookieVerified, BootstrapEvent::AuthorityProof) => {
                BootstrapPhase::AuthorityVerified
            }
            (BootstrapPhase::AuthorityVerified, BootstrapEvent::DeviceProof) => {
                BootstrapPhase::DeviceVerified
            }
            (BootstrapPhase::DeviceVerified, BootstrapEvent::AddressOffer) => {
                BootstrapPhase::AddressOffered
            }
            (BootstrapPhase::AddressOffered, BootstrapEvent::DeviceCommit) => {
                BootstrapPhase::DeviceCommitted
            }
            (_, BootstrapEvent::Abort) => BootstrapPhase::Aborted,
            _ => return Err(Error::State),
        };
        if now_us < slot.challenge_started_local_us || now_us >= slot.deadline_us {
            return Err(Error::Timeout);
        }
        let previous = slot.transcript.ok_or(Error::State)?;
        let key = slot.key.ok_or(Error::State)?;
        if event == BootstrapEvent::Abort {
            self.call_authorize(provider, event, &key, &previous, now_us, &evidence)?;
            self.pending[index].phase = BootstrapPhase::Aborted;
            return Ok(());
        }
        validate_transcript_phase(&transcript, expected_phase)?;
        if !transcript_transition_valid(&previous, &transcript, event) {
            return Err(Error::Replay);
        }
        self.call_authorize(provider, event, &key, &transcript, now_us, &evidence)?;
        self.pending[index].phase = expected_phase;
        self.pending[index].transcript = Some(transcript);
        Ok(())
    }

    /// 消费 Identity Owner 发布的 live Binding proof 并最终发布 ADMITTED。
    ///
    /// # Errors
    ///
    /// Identity proof、Transcript、Deadline 或最终授权不精确匹配时失败关闭。
    pub fn admit_durable<P: CookieProvider>(
        &mut self,
        provider: &mut P,
        handle: AdmissionHandle,
        binding: BindingView,
        final_transcript: BootstrapTranscript,
        evidence: Evidence,
        now_us: u64,
    ) -> Result<AdmittedView> {
        let index = self.handle_index(handle)?;
        let slot = self.pending[index];
        let previous = slot.transcript.ok_or(Error::State)?;
        let key = slot.key.ok_or(Error::State)?;
        let certificate = binding.certificate();
        validate_transcript_phase(&final_transcript, BootstrapPhase::FinalDurable)?;
        if slot.phase != BootstrapPhase::DeviceCommitted
            || now_us < slot.challenge_started_local_us
            || now_us >= slot.deadline_us
            || binding.runtime_instance() != self.config.runtime_instance
            || binding.identity_owner_instance() != self.config.identity_owner_instance
            || now_us >= binding.local_deadline_us()
            || !transcript_transition_valid(
                &previous,
                &final_transcript,
                BootstrapEvent::FinalDurable,
            )
            || certificate.binding.principal.bytes() != final_transcript.joining_device_principal
            || certificate.binding.realm.get() != final_transcript.realm_id
            || certificate.binding.address.get() != final_transcript.proposed_address
            || certificate.binding.generation.get() != final_transcript.address_binding_generation
            || certificate.lease_id != final_transcript.binding_lease_id
            || certificate.authority_principal.bytes() != final_transcript.authority_principal
            || certificate.authority_generation.get() != final_transcript.authority_generation
            || certificate.authority_lease_sequence != final_transcript.authority_lease_sequence
        {
            return Err(Error::Security);
        }
        self.call_authorize(
            provider,
            BootstrapEvent::FinalDurable,
            &key,
            &final_transcript,
            now_us,
            &evidence,
        )?;
        let admitted = AdmittedView {
            binding,
            session_generation: final_transcript.selected_session_generation,
            link: key.link,
            admission_generation: slot.slot_generation,
        };
        self.pending[index].phase = BootstrapPhase::FinalDurable;
        self.pending[index].transcript = Some(final_transcript);
        self.pending[index].admitted = Some(admitted);
        Ok(admitted)
    }

    /// 查询 pending 即时快照。
    ///
    /// # Errors
    ///
    /// Handle 不属于当前 Owner 或已失效时返回错误。
    pub fn pending_get(&self, handle: AdmissionHandle) -> Result<PendingView> {
        let slot = &self.pending[self.handle_index(handle)?];
        Ok(PendingView {
            phase: slot.phase,
            key: slot.key.ok_or(Error::State)?,
            challenge_started_local_us: slot.challenge_started_local_us,
            deadline_us: slot.deadline_us,
        })
    }

    /// 取出最终 ADMITTED 快照。
    ///
    /// # Errors
    ///
    /// Handle、阶段或 Binding 租约不再有效时返回错误。
    pub fn admitted_get(&self, handle: AdmissionHandle, now_us: u64) -> Result<AdmittedView> {
        let slot = &self.pending[self.handle_index(handle)?];
        let admitted = slot.admitted.ok_or(Error::State)?;
        if slot.phase != BootstrapPhase::FinalDurable
            || now_us >= admitted.binding.local_deadline_us()
        {
            return Err(Error::Access);
        }
        Ok(admitted)
    }

    /// 显式按半开 Deadline 过期 pending；错误输入不会触发 lazy eviction。
    pub fn expire(&mut self, now_us: u64) -> usize {
        let mut expired = 0;
        for slot in &mut self.pending {
            if slot.occupied
                && !matches!(
                    slot.phase,
                    BootstrapPhase::FinalDurable | BootstrapPhase::Aborted
                )
                && now_us >= slot.deadline_us
            {
                slot.phase = BootstrapPhase::Aborted;
                expired += 1;
            }
        }
        expired
    }

    /// 退休已完成或已终止 pending；slot generation 不回退。
    ///
    /// # Errors
    ///
    /// Handle 无效或事务尚未进入终态时返回错误。
    pub fn retire(&mut self, handle: AdmissionHandle) -> Result<()> {
        let index = self.handle_index(handle)?;
        if !matches!(
            self.pending[index].phase,
            BootstrapPhase::FinalDurable | BootstrapPhase::Aborted
        ) {
            return Err(Error::State);
        }
        let generation = self.pending[index].slot_generation;
        self.pending[index] = Pending::EMPTY;
        self.pending[index].slot_generation = generation;
        Ok(())
    }

    fn make_handle(&self, index: usize) -> Result<AdmissionHandle> {
        let slot = self.pending.get(index).ok_or(Error::NotFound)?;
        Ok(AdmissionHandle {
            owner_instance: self.config.owner_instance,
            slot: u16::try_from(index + 1).map_err(|_| Error::NoSpace)?,
            slot_generation: slot.slot_generation,
            transaction_id: slot.key.ok_or(Error::State)?.transaction_id,
        })
    }

    fn handle_index(&self, handle: AdmissionHandle) -> Result<usize> {
        if handle.owner_instance != self.config.owner_instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        let slot = self.pending.get(index).ok_or(Error::NotFound)?;
        if !slot.occupied
            || slot.slot_generation != handle.slot_generation
            || slot
                .key
                .is_none_or(|key| key.transaction_id != handle.transaction_id)
        {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn call_verify_cookie<P: CookieProvider>(
        &mut self,
        provider: &mut P,
        hello_cookie: &HelloCookie,
        key: &AdmissionKey,
    ) -> Result<()> {
        let claim = self.next_claim(CALLBACK_VERIFY_COOKIE)?;
        let callback_lease = self.config.provider_gate.try_enter(claim)?;
        let result = provider.verify_cookie(hello_cookie, key);
        let leave_result = self.config.provider_gate.leave(callback_lease);
        result.and(leave_result)
    }

    fn call_authorize<P: CookieProvider>(
        &mut self,
        provider: &mut P,
        event: BootstrapEvent,
        key: &AdmissionKey,
        transcript: &BootstrapTranscript,
        now_us: u64,
        evidence: &Evidence,
    ) -> Result<()> {
        let claim = self.next_claim(CALLBACK_AUTHORIZE_EVENT)?;
        let callback_lease = self.config.provider_gate.try_enter(claim)?;
        let result = provider.authorize_event(event, key, transcript, now_us, evidence);
        let leave_result = self.config.provider_gate.leave(callback_lease);
        result.and(leave_result)
    }

    fn next_claim(&mut self, kind: u16) -> Result<CallbackClaim> {
        let operation_id = self.next_operation_id;
        self.next_operation_id = self
            .next_operation_id
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        CallbackClaim::new(self.config.owner_instance, operation_id, 1, kind)
    }
}
