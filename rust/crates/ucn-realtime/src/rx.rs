//! Atomic complete-frame + physical-stamp ownership; 原子完整帧与物理打点所有权。
use crate::{EventDirection, EventKey, RetirementProof, Timestamp};
use core::sync::atomic::{AtomicBool, AtomicU8, AtomicU32, Ordering};
use ucn_types::{Error, Result};
struct Item<const B: usize> {
    state: AtomicU8,
    generation: AtomicU32,
    length: AtomicU32,
    metadata: [AtomicU32; 9],
    bytes: [AtomicU8; B],
}
impl<const B: usize> Item<B> {
    const fn new() -> Self {
        Self {
            state: AtomicU8::new(0),
            generation: AtomicU32::new(0),
            length: AtomicU32::new(0),
            metadata: [const { AtomicU32::new(0) }; 9],
            bytes: [const { AtomicU8::new(0) }; B],
        }
    }
}
/// Opaque claimed physical RX item; 不可伪造物理RX领取项。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimedRxClaim {
    queue: u32,
    slot: usize,
    generation: u32,
    stamp: Timestamp,
    bytes: usize,
}
impl TimedRxClaim {
    /// Actual stamp paired with this complete frame; 此完整帧配对的真实打点。
    #[must_use]
    pub const fn stamp(self) -> Timestamp {
        self.stamp
    }
    /// Copied frame length; 已复制帧长度。
    #[must_use]
    pub const fn bytes(self) -> usize {
        self.bytes
    }
}
/// One caller-owned bounded atomic queue; 单个调用方持有的有界原子队列。
pub struct TimedRxQueue<const SLOTS: usize, const BYTES: usize> {
    id: u32,
    items: [Item<BYTES>; SLOTS],
    cursor: AtomicU32,
    publisher: AtomicBool,
    releaser: AtomicBool,
}
impl<const N: usize, const B: usize> TimedRxQueue<N, B> {
    /// Exact static queue identity; 精确静态队列身份。
    #[must_use]
    pub const fn identity(&self) -> u32 {
        self.id
    }
    /// Const queue storage, independent from ordinary Core RX items.
    /// Const队列存储，独立于普通Core RX项。
    /// # Errors
    /// Invalid identity/capacity rejects.
    pub const fn new(id: u32) -> Result<Self> {
        if id == 0 || N == 0 || N > u16::MAX as usize || B == 0 || B > u16::MAX as usize {
            return Err(Error::Config);
        }
        Ok(Self {
            id,
            items: [const { Item::new() }; N],
            cursor: AtomicU32::new(0),
            publisher: AtomicBool::new(false),
            releaser: AtomicBool::new(false),
        })
    }
    /// Driver publishes the complete owned frame and genuine RX proof in one item.
    /// `Driver在一个项中发布完整帧与真实RX证明`。
    /// # Errors
    /// Full/short/invalid-direction/duplicate token rejects without a half item.
    pub fn publish(&self, stamp: Timestamp, frame: &[u8]) -> Result<()> {
        self.publisher
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| Error::State)?;
        let result = self.publish_inner(stamp, frame);
        self.publisher.store(false, Ordering::Release);
        result
    }
    fn publish_inner(&self, stamp: Timestamp, frame: &[u8]) -> Result<()> {
        let key = stamp.key();
        if frame.is_empty() || frame.len() > B || key.direction != EventDirection::Rx {
            return Err(Error::Argument);
        }
        let event_slot = u32::try_from(key.slot).map_err(|_| Error::Argument)?;
        if self.items.iter().any(|item| {
            matches!(item.state.load(Ordering::Acquire), 2 | 3)
                && item.metadata[0].load(Ordering::Relaxed) == u32::from(key.link)
                && item.metadata[1].load(Ordering::Relaxed) == key.instance
                && item.metadata[2].load(Ordering::Relaxed) == key.generation
                && item.metadata[3].load(Ordering::Relaxed) == event_slot
                && item.metadata[4].load(Ordering::Relaxed) == key.token
        }) {
            return Err(Error::State);
        }
        for item in &self.items {
            if item
                .state
                .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
            {
                continue;
            }
            let Some(generation) = item.generation.load(Ordering::Relaxed).checked_add(1) else {
                item.state.store(4, Ordering::Release);
                continue;
            };
            let time = stamp.time_us().to_le_bytes();
            let values = [
                u32::from(key.link),
                key.instance,
                key.generation,
                event_slot,
                key.token,
                u32::from_le_bytes([time[0], time[1], time[2], time[3]]),
                u32::from_le_bytes([time[4], time[5], time[6], time[7]]),
                stamp.bound_us(),
                generation,
            ];
            for (atom, value) in item.metadata.iter().zip(values) {
                atom.store(value, Ordering::Relaxed);
            }
            for (atom, byte) in item.bytes.iter().zip(frame) {
                atom.store(*byte, Ordering::Relaxed);
            }
            item.length.store(
                u32::try_from(frame.len()).map_err(|_| Error::Argument)?,
                Ordering::Relaxed,
            );
            item.generation.store(generation, Ordering::Relaxed);
            item.state.store(2, Ordering::Release);
            return Ok(());
        }
        Err(Error::NoSpace)
    }
    /// Claim one atomic record; exactly one consumer owns its retirement.
    /// 领取一个原子记录，恰好一个消费者持有退休义务。
    /// # Errors
    /// Insufficient output leaves the ready item and output unchanged.
    pub fn claim(&self, out: &mut [u8]) -> Result<TimedRxClaim> {
        let begin =
            usize::try_from(self.cursor.load(Ordering::Relaxed)).map_err(|_| Error::State)? % N;
        for offset in 0..N {
            let index = (begin + offset) % N;
            let item = &self.items[index];
            if item
                .state
                .compare_exchange(2, 3, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
            {
                continue;
            }
            let n =
                usize::try_from(item.length.load(Ordering::Relaxed)).map_err(|_| Error::State)?;
            if out.len() < n {
                item.state.store(2, Ordering::Release);
                return Err(Error::NoSpace);
            }
            let m = core::array::from_fn::<_, 9, _>(|i| item.metadata[i].load(Ordering::Relaxed));
            let key = EventKey {
                link: u16::try_from(m[0]).map_err(|_| Error::State)?,
                instance: m[1],
                generation: m[2],
                slot: usize::try_from(m[3]).map_err(|_| Error::State)?,
                token: m[4],
                direction: EventDirection::Rx,
            };
            for (byte, atom) in out[..n].iter_mut().zip(&item.bytes) {
                *byte = atom.load(Ordering::Relaxed);
            }
            self.cursor.store(
                u32::try_from((index + 1) % N).map_err(|_| Error::State)?,
                Ordering::Relaxed,
            );
            return Ok(TimedRxClaim {
                queue: self.id,
                slot: index,
                generation: m[8],
                stamp: Timestamp {
                    key,
                    time: u64::from(m[5]) | (u64::from(m[6]) << 32),
                    bound: m[7],
                },
                bytes: n,
            });
        }
        Err(Error::NoSpace)
    }
    /// Recycle only the exact claimed frame after real Driver retirement.
    /// 真实Driver退休后才回收精确已领取帧。
    /// # Errors
    /// Wrong queue/key/generation or duplicate release has zero state writes.
    pub fn release(&self, h: TimedRxClaim, proof: RetirementProof) -> Result<()> {
        // Serialize validation with recycling: a copied stale claim must not free a
        // newly published/claimed item between the generation check and state CAS.
        // 验证与回收串行化，防止旧领取项在代际检查后回收新项。
        self.releaser
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| Error::State)?;
        let result = self.release_inner(h, proof);
        self.releaser.store(false, Ordering::Release);
        result
    }
    fn release_inner(&self, h: TimedRxClaim, proof: RetirementProof) -> Result<()> {
        if h.queue != self.id || h.stamp.key() != proof.key() {
            return Err(Error::State);
        }
        let item = self.items.get(h.slot).ok_or(Error::State)?;
        if item.generation.load(Ordering::Acquire) != h.generation {
            return Err(Error::State);
        }
        item.state
            .compare_exchange(3, 0, Ordering::AcqRel, Ordering::Relaxed)
            .map_err(|_| Error::State)?;
        Ok(())
    }
}
