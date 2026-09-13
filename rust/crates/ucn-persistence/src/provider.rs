use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use ucn_types::{Error, Result};

use crate::codec::{DomainKey, MARKER_BYTES};

/// Persistence Provider I/O 阶段。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum IoPhase {
    /// 加载完整原始槽。
    LoadSlot = 1,
    /// 写非活动槽，Marker 仍为擦除态。
    WriteInactive = 2,
    /// 回读刚写入的完整槽。
    Readback = 3,
    /// 原子发布 Commit Marker。
    PublishMarker = 4,
    /// 加载独立 Witness。
    LoadWitness = 5,
    /// CAS 推进独立 Witness。
    AdvanceWitness = 6,
}

/// Provider 返回的 Blob 状态。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BlobState {
    /// Blob 存在且完整填充调用方输出。
    Present = 1,
    /// 仅 Witness 可返回：介质中没有该 Witness。
    Empty = 2,
    /// 介质存在但无法证明完整。
    Fault = 3,
}

/// 独立 Witness 的只读介质视图。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct WitnessView {
    /// Witness 所属域。
    pub domain: DomainKey,
    /// 可能已经发布过的最高 Record Generation。
    pub highest_maybe_published_generation: u64,
    /// Blob 状态。
    pub state: BlobState,
}

/// 一次同步或异步 Provider I/O 的精确完成事实。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Completion {
    /// 全局、不回绕 I/O Token。
    pub io_token: u64,
    /// 成功或精确失败原因。
    pub result: Result<()>,
    /// 实际完整读写字节数。
    pub exact_bytes: u32,
    /// 完成阶段。
    pub phase: IoPhase,
    /// Blob 状态。
    pub blob_state: BlobState,
    /// 槽索引；Witness 操作为 `u8::MAX`。
    pub slot_index: u8,
}

/// `begin_*()` 或 `poll()` 的状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum IoStart {
    /// 当前调用同步完成。
    Completed(Completion),
    /// 当前调用没有完成；必须使用相同 Token/Phase 继续 poll。
    Pending,
    /// Provider 在形成可信 Completion 前失败。
    Failed(Error),
}

/// Provider 固定介质几何。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProviderGeometry {
    /// 最小写对齐，必须非零且为 2 的幂。
    pub minimum_write_alignment: u32,
    /// 最小擦除对齐，必须非零且为 2 的幂。
    pub minimum_erase_alignment: u32,
    /// 可承载的最大固定槽长度。
    pub maximum_slot_bytes: u32,
    /// Provider 能原子发布的 Marker 字节数。
    pub atomic_marker_bytes: u16,
    /// 介质擦除值。
    pub erased_value: u8,
}

impl ProviderGeometry {
    pub(crate) fn validate<const SLOT: usize>(&self) -> Result<()> {
        let slot_bytes = u32::try_from(SLOT).map_err(|_| Error::Config)?;
        if self.minimum_write_alignment == 0
            || !self.minimum_write_alignment.is_power_of_two()
            || self.minimum_erase_alignment == 0
            || !self.minimum_erase_alignment.is_power_of_two()
            || self.maximum_slot_bytes < slot_bytes
            || self.atomic_marker_bytes as usize != MARKER_BYTES
            || slot_bytes % self.minimum_write_alignment != 0
            || slot_bytes % self.minimum_erase_alignment != 0
        {
            return Err(Error::Config);
        }
        Ok(())
    }
}

/// 产品实现的持久化介质边界。
///
/// 所有方法必须非阻塞、有界；返回 [`IoStart::Pending`] 后，Provider 自己保存固定 continuation，
/// 下一次 [`PersistenceProvider::poll`] 使用相同 token/phase 并重新获得输出缓冲区。
pub trait PersistenceProvider {
    /// 返回固定几何；不得产生介质副作用。
    fn geometry(&self) -> ProviderGeometry;

    /// 加载固定长度原始槽。空槽也必须填充完整擦除镜像并返回 `Present`。
    fn begin_load_slot(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        output: &mut [u8],
        io_token: u64,
    ) -> IoStart;

    /// 写入非活动槽的完整未发布镜像。
    fn begin_write_inactive(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        input: &[u8],
        io_token: u64,
    ) -> IoStart;

    /// 回读刚写入槽的完整镜像。
    fn begin_readback(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        output: &mut [u8],
        io_token: u64,
    ) -> IoStart;

    /// 原子发布 16 B Marker。
    fn begin_publish_marker(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        marker: &[u8; MARKER_BYTES],
        io_token: u64,
    ) -> IoStart;

    /// 加载独立 Witness。
    fn begin_load_witness(
        &mut self,
        domain: DomainKey,
        output: &mut WitnessView,
        io_token: u64,
    ) -> IoStart;

    /// 仅在 Witness 精确等于 `expected_old` 时推进到 `exact_new`。
    fn begin_advance_witness(
        &mut self,
        domain: DomainKey,
        expected_old: u64,
        exact_new: u64,
        io_token: u64,
    ) -> IoStart;

    /// 继续一个 PENDING 操作；按 Phase 重新提供其唯一输出。
    fn poll(
        &mut self,
        io_token: u64,
        expected_phase: IoPhase,
        slot_output: Option<&mut [u8]>,
        witness_output: Option<&mut WitnessView>,
    ) -> IoStart;
}

/// 调用方持有、可由多个 Persistence Owner 共享的 Provider 回调域门和 I/O Token 分配器。
pub struct ProviderGate {
    active: AtomicBool,
    token_low: AtomicU32,
    token_high: AtomicU32,
}

impl ProviderGate {
    /// 建立空闲回调门；可用于静态初始化。
    #[must_use]
    pub const fn new() -> Self {
        Self {
            active: AtomicBool::new(false),
            token_low: AtomicU32::new(0),
            token_high: AtomicU32::new(0),
        }
    }

    pub(crate) fn call_new<T>(&self, callback: impl FnOnce(u64) -> T) -> Result<(u64, T)> {
        self.enter()?;
        let token = match self.next_token() {
            Ok(token) => token,
            Err(error) => {
                self.active.store(false, Ordering::Release);
                return Err(error);
            }
        };
        let result = callback(token);
        self.active.store(false, Ordering::Release);
        Ok((token, result))
    }

    pub(crate) fn call_existing<T>(&self, callback: impl FnOnce() -> T) -> Result<T> {
        self.enter()?;
        let result = callback();
        self.active.store(false, Ordering::Release);
        Ok(result)
    }

    fn enter(&self) -> Result<()> {
        self.active
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .map(|_| ())
            .map_err(|_| Error::State)
    }

    fn next_token(&self) -> Result<u64> {
        let low = self.token_low.load(Ordering::Relaxed);
        let high = self.token_high.load(Ordering::Relaxed);
        if low == u32::MAX && high == u32::MAX {
            return Err(Error::Exhausted);
        }
        let (next_low, carry) = low.overflowing_add(1);
        let next_high = high.checked_add(u32::from(carry)).ok_or(Error::Exhausted)?;
        self.token_high.store(next_high, Ordering::Relaxed);
        self.token_low.store(next_low, Ordering::Relaxed);
        Ok((u64::from(next_high) << 32) | u64::from(next_low))
    }

    #[cfg(test)]
    pub(crate) fn seed_token_for_test(&self, token: u64) {
        self.token_high
            .store((token >> 32) as u32, Ordering::Relaxed);
        self.token_low.store(
            u32::try_from(token & u64::from(u32::MAX)).unwrap_or(u32::MAX),
            Ordering::Relaxed,
        );
    }
}

impl Default for ProviderGate {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shared_gate_rejects_reentry_and_never_wraps_tokens() {
        let gate = ProviderGate::new();
        let (_, nested) = gate
            .call_new(|_| gate.call_new(|_| ()))
            .expect("outer call enters");
        assert_eq!(nested, Err(Error::State));
        let (token, ()) = gate.call_new(|_| ()).expect("gate released");
        assert_eq!(token, 2);

        gate.seed_token_for_test(u64::MAX);
        assert_eq!(gate.call_new(|_| ()), Err(Error::Exhausted));
        assert!(!gate.active.load(Ordering::Acquire));
    }
}
