//! Shared bounded Coordinator schedule and callback gate, not a business Owner dependency.
//! 共用有界Coordinator调度与回调门，不建立业务Owner依赖。
use ucn_adapter::{DriverCancel, DriverSubmit, LinkHandle, TxDriver, TxToken};
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_security::{CurrentFacts, Fingerprint, SessionHandle};
use ucn_types::{Error, Result};

/// Seven guaranteed scheduling classes; 七种保证调度机会的工作类别。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalWorkClass {
    /// One raw RX fact; 一个原始RX事实。
    Ingress = 0,
    /// One actual Foundation work unit; 一个真实Foundation工作单元。
    Foundation = 1,
    /// One authenticated C0 command; 一个认证C0命令。
    Control = 2,
    /// One durable continuation edge; 一个耐久续接状态边。
    Request = 3,
    /// Bounded timer/retention maintenance; 有界计时/保留维护。
    Timer = 4,
    /// One protected reply or Driver submission; 一次保护或Driver提交。
    Transmit = 5,
    /// One exact obligation inspection/retirement; 一个精确义务检查/退休。
    Retire = 6,
}
#[cfg(any(feature = "journal-runtime", feature = "source-runtime"))]
pub(crate) const CLASSES: [JournalWorkClass; 7] = [
    JournalWorkClass::Ingress,
    JournalWorkClass::Foundation,
    JournalWorkClass::Control,
    JournalWorkClass::Request,
    JournalWorkClass::Timer,
    JournalWorkClass::Transmit,
    JournalWorkClass::Retire,
];
pub(crate) struct GatedDriver<'a, D> {
    pub(crate) driver: &'a mut D,
    pub(crate) gate: &'a CallbackGate,
    pub(crate) owner: u32,
}
impl<D: TxDriver> TxDriver for GatedDriver<'_, D> {
    fn submit(&mut self, link: LinkHandle, frame: &[u8], token: TxToken) -> DriverSubmit {
        let claim = CallbackClaim::new(self.owner, token.generation(), 1, 0x0702);
        let Ok(lease) = claim.and_then(|c| self.gate.try_enter(c)) else {
            return DriverSubmit::NotSubmitted;
        };
        let result = self.driver.submit(link, frame, token);
        if self.gate.leave(lease).is_err() {
            DriverSubmit::Unknown
        } else {
            result
        }
    }
    fn cancel(&mut self, token: TxToken) -> DriverCancel {
        let claim = CallbackClaim::new(self.owner, token.generation(), 1, 0x0703);
        let Ok(lease) = claim.and_then(|c| self.gate.try_enter(c)) else {
            return DriverCancel::NotCancelled;
        };
        let result = self.driver.cancel(token);
        if self.gate.leave(lease).is_err() {
            DriverCancel::Unknown
        } else {
            result
        }
    }
}
/// Validated plan, no strict-priority bypass or unbounded spare-budget borrowing.
/// 已校验预算，不允许严格优先级旁路或无限借用。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct JournalStepBudget {
    pub(crate) total: u16,
    pub(crate) caps: [u16; 7],
}
impl JournalStepBudget {
    #[cfg(feature = "duplex-runtime")]
    pub(crate) const fn one_class(class: JournalWorkClass) -> Self {
        let mut caps = [0; 7];
        caps[class as usize] = 1;
        Self { total: 1, caps }
    }
    /// 1..64 total visits, each class 1..16; cursor survives even single-visit steps.
    /// 总访问1..64，每类1..16，单次预算1时游标也跨调用保留。
    /// # Errors
    /// Missing class/capacity or excess total rejects atomically.
    pub const fn new(total: u16, caps: [u16; 7]) -> Result<Self> {
        let mut sum = 0;
        let mut i = 0;
        while i < 7 {
            if caps[i] == 0 || caps[i] > 16 {
                return Err(Error::Config);
            }
            sum += caps[i];
            i += 1;
        }
        if total == 0 || total > 64 || total > sum {
            return Err(Error::Config);
        }
        Ok(Self { total, caps })
    }
}
/// Product-owned fixed policy, not peer-supplied timing/permission.
/// 产品持有的固定策略，不接受远端传入计时/权限。
#[derive(Clone, Copy)]
pub struct JournalRuntimeConfig {
    /// Coordinator instance, six derived non-reused sub-owner IDs; `Coordinator实例及六个派生子Owner编号`。
    pub owner: u32,
    /// Original Data request lifetime; 原Data请求生命周期。
    pub request_lifetime_us: u64,
    /// C0 admission-relative reply lifetime; C0接纳相对回复生命周期。
    pub control_lifetime_us: u64,
    /// Journal I/O deadline duration; Journal I/O时限。
    pub io_timeout_us: u64,
    /// Retry spacing, no deadline renewal; 重试间隔，不续截止。
    pub retry_us: u64,
    /// Maximum Driver submissions; 最大Driver提交次数。
    pub max_attempts: u16,
    /// Minimum ACK-relative retention; 最小ACK相对保留期。
    pub retention_us: u64,
}
/// Explicit current peer facts; they are still revalidated by actual Security at use.
/// 显式当前Peer事实，使用时仍由实际Security复验。
#[derive(Clone, Copy)]
pub struct JournalPeer {
    /// Actual session handle; 实际会话Handle。
    pub session: SessionHandle,
    /// Trusted local time/bindings, not RX timestamp authorization; 可信本地时间/绑定，非RX打点授权。
    pub facts: CurrentFacts,
    /// Product-selected security context; 产品选择的安全域。
    pub fingerprint: Fingerprint,
    /// Exact dedicated Adapter link; 专属Adapter的精确链路。
    pub link: LinkHandle,
}
/// One step's attempted visits and actual progress, not time/WCET proof.
/// 单次尝试访问及实际进展，不是时间/WCET证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct JournalStepReport {
    /// Visits including empty/pending classes; 含空/等待类别的预算访问。
    pub visits: [u16; 7],
    /// Bounded successful progression edges; 有界成功推进边数。
    pub progress: u16,
    /// First observable error, other classes still receive budget; 首个可观察错误，其他类别继续获预算。
    pub error: Option<Error>,
    /// Stopped and all volatile/physical obligations retired; 停止且易失/物理义务均退休。
    pub quiescent: bool,
}
/// Monotonic saturating diagnostics, never authorization; 饱和单调诊断，绝非授权。
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct JournalRuntimeStats {
    /// Per-class visits; 各类访问。
    pub visits: [u64; 7],
    /// Claimed raw frames; 已claim原始帧。
    pub rx: u64,
    /// Definitively rejected facts/commands; 明确拒绝事实/命令。
    pub rejected: u64,
    /// Executor handoffs; 执行器交接。
    pub executions: u64,
    /// Actual exact retirements; 实际精确退休。
    pub retired: u64,
}
