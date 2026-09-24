use ucn_types::{Error, Result};

/// 64-bit Replay Window 对输入序号的只读分类。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReplayClassification {
    /// 尚未提交且位于可接受窗口内。
    Fresh,
    /// 已经提交。
    Duplicate,
    /// 已经落在窗口左侧，不再接受。
    Stale,
    /// 相同序号已有尚未提交的精确 mutation reservation。
    InFlight,
}

/// Replay Owner 为一次待提交 mutation 签发的精确 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct ReplayMutationHandle {
    pub(crate) owner_instance: u32,
    pub(crate) session_slot: u16,
    pub(crate) session_slot_generation: u32,
    pub(crate) key_generation: u32,
    pub(crate) reservation_slot: u16,
    pub(crate) reservation_generation: u32,
    pub(crate) sequence: u64,
    pub(crate) reservation_deadline_us: u64,
}

/// 成功提交 Replay mutation 后的不可变证据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReplayEvidence {
    /// 已提交序号。
    pub sequence: u64,
    /// 提交该序号时的 Peer Session Generation。
    pub session_generation: u32,
    /// 提交该序号时的精确 Key Generation。
    pub key_generation: u32,
    /// 该 Replay Window 所属的 canonical Security Context fingerprint。
    pub context_fingerprint: [u8; 16],
    /// canonical Origin AAD 的固定摘要。
    pub aad_digest: [u8; 16],
    /// 认证后 Origin 明文 Payload 的固定摘要。
    pub payload_digest: [u8; 16],
}

#[derive(Clone, Copy)]
struct Reservation {
    occupied: bool,
    generation: u32,
    sequence: u64,
    session_generation: u32,
    key_generation: u32,
    context_fingerprint: [u8; 16],
    deadline_us: u64,
    aad_digest: [u8; 16],
    payload_digest: [u8; 16],
}

impl Reservation {
    const EMPTY: Self = Self {
        occupied: false,
        generation: 0,
        sequence: 0,
        session_generation: 0,
        key_generation: 0,
        context_fingerprint: [0; 16],
        deadline_us: 0,
        aad_digest: [0; 16],
        payload_digest: [0; 16],
    };
}

pub(crate) struct ReplayWindow<const SLOTS: usize> {
    initialized: bool,
    highest: u64,
    bitmap: u64,
    reservations: [Reservation; SLOTS],
}

impl<const SLOTS: usize> ReplayWindow<SLOTS> {
    pub(crate) const fn new() -> Self {
        Self {
            initialized: false,
            highest: 0,
            bitmap: 0,
            reservations: [Reservation::EMPTY; SLOTS],
        }
    }

    pub(crate) fn classify(&self, sequence: u64) -> Result<ReplayClassification> {
        if sequence == 0 {
            return Err(Error::Argument);
        }
        if self
            .reservations
            .iter()
            .any(|reservation| reservation.occupied && reservation.sequence == sequence)
        {
            return Ok(ReplayClassification::InFlight);
        }
        self.classification_without_reservations(sequence)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn reserve(
        &mut self,
        owner_instance: u32,
        session_slot: u16,
        session_slot_generation: u32,
        peer_session_generation: u32,
        key_generation: u32,
        context_fingerprint: [u8; 16],
        sequence: u64,
        now_us: u64,
        reservation_deadline_us: u64,
        aad_digest: [u8; 16],
        payload_digest: [u8; 16],
    ) -> Result<ReplayMutationHandle> {
        if owner_instance == 0
            || session_slot == 0
            || session_slot_generation == 0
            || peer_session_generation == 0
            || key_generation == 0
            || context_fingerprint == [0; 16]
            || now_us >= reservation_deadline_us
            || aad_digest == [0; 16]
            || payload_digest == [0; 16]
        {
            return Err(Error::Argument);
        }
        if self.classify(sequence)? != ReplayClassification::Fresh {
            return Err(Error::Replay);
        }
        let slot = self
            .reservations
            .iter()
            .position(|reservation| !reservation.occupied)
            .ok_or(Error::NoSpace)?;
        let reservation = &mut self.reservations[slot];
        let generation = reservation
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        *reservation = Reservation {
            occupied: true,
            generation,
            sequence,
            session_generation: peer_session_generation,
            key_generation,
            context_fingerprint,
            deadline_us: reservation_deadline_us,
            aad_digest,
            payload_digest,
        };
        Ok(ReplayMutationHandle {
            owner_instance,
            session_slot,
            session_slot_generation,
            key_generation,
            reservation_slot: u16::try_from(slot + 1).map_err(|_| Error::NoSpace)?,
            reservation_generation: generation,
            sequence,
            reservation_deadline_us,
        })
    }

    pub(crate) fn commit(
        &mut self,
        handle: ReplayMutationHandle,
        now_us: u64,
    ) -> Result<ReplayEvidence> {
        let index = self.match_reservation(handle, now_us)?;
        self.commit_at_index(index, handle.sequence)
    }

    pub(crate) fn preflight_commit(&self, handle: ReplayMutationHandle, now_us: u64) -> Result<()> {
        self.match_reservation(handle, now_us).map(|_| ())
    }

    fn commit_at_index(&mut self, index: usize, sequence: u64) -> Result<ReplayEvidence> {
        let reservation = self.reservations[index];
        if reservation.sequence != sequence {
            return Err(Error::NotFound);
        }
        if !self.initialized {
            self.initialized = true;
            self.highest = sequence;
            self.bitmap = 1;
        } else if sequence > self.highest {
            let shift = sequence - self.highest;
            self.bitmap = if shift >= 64 {
                1
            } else {
                (self.bitmap << shift) | 1
            };
            self.highest = sequence;
        } else {
            self.bitmap |= 1_u64 << (self.highest - sequence);
        }
        let evidence = ReplayEvidence {
            sequence,
            session_generation: reservation.session_generation,
            key_generation: reservation.key_generation,
            context_fingerprint: reservation.context_fingerprint,
            aad_digest: reservation.aad_digest,
            payload_digest: reservation.payload_digest,
        };
        self.reservations[index].occupied = false;
        Ok(evidence)
    }

    pub(crate) fn abort(&mut self, handle: ReplayMutationHandle) -> Result<()> {
        let index = self.match_reservation_without_time(handle)?;
        self.reservations[index].occupied = false;
        Ok(())
    }

    pub(crate) fn clear(&mut self) {
        self.initialized = false;
        self.highest = 0;
        self.bitmap = 0;
        for reservation in &mut self.reservations {
            reservation.occupied = false;
        }
    }

    pub(crate) fn expire_reservation_at(&mut self, index: usize, now_us: u64) -> bool {
        let Some(reservation) = self.reservations.get_mut(index) else {
            return false;
        };
        if reservation.occupied && now_us >= reservation.deadline_us {
            reservation.occupied = false;
            true
        } else {
            false
        }
    }

    pub(crate) fn evidence(
        &self,
        sequence: u64,
        session_generation: u32,
        key_generation: u32,
        context_fingerprint: [u8; 16],
        aad_digest: [u8; 16],
        payload_digest: [u8; 16],
    ) -> Result<ReplayEvidence> {
        if self.classify(sequence)? != ReplayClassification::Duplicate {
            return Err(Error::Replay);
        }
        Ok(ReplayEvidence {
            sequence,
            session_generation,
            key_generation,
            context_fingerprint,
            aad_digest,
            payload_digest,
        })
    }

    fn match_reservation(&self, handle: ReplayMutationHandle, now_us: u64) -> Result<usize> {
        let index = self.match_reservation_without_time(handle)?;
        if now_us >= handle.reservation_deadline_us {
            return Err(Error::Timeout);
        }
        if self.classification_without_reservations(handle.sequence)? != ReplayClassification::Fresh
        {
            return Err(Error::Replay);
        }
        Ok(index)
    }

    fn match_reservation_without_time(&self, handle: ReplayMutationHandle) -> Result<usize> {
        if handle.reservation_slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.reservation_slot - 1);
        let reservation = self.reservations.get(index).ok_or(Error::NotFound)?;
        if !reservation.occupied
            || reservation.generation != handle.reservation_generation
            || reservation.sequence != handle.sequence
            || reservation.deadline_us != handle.reservation_deadline_us
        {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn classification_without_reservations(&self, sequence: u64) -> Result<ReplayClassification> {
        if sequence == 0 {
            return Err(Error::Argument);
        }
        if !self.initialized || sequence > self.highest {
            return Ok(ReplayClassification::Fresh);
        }
        let distance = self.highest - sequence;
        if distance >= 64 {
            return Ok(ReplayClassification::Stale);
        }
        if self.bitmap & (1_u64 << distance) != 0 {
            Ok(ReplayClassification::Duplicate)
        } else {
            Ok(ReplayClassification::Fresh)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{ReplayClassification, ReplayWindow};
    use ucn_types::Error;

    #[test]
    fn two_phase_window_does_not_publish_before_commit() {
        let mut window = ReplayWindow::<2>::new();
        let handle = window
            .reserve(1, 1, 7, 2, 1, [3; 16], 70, 10, 20, [1; 16], [2; 16])
            .expect("fresh reservation");
        assert_eq!(window.classify(70), Ok(ReplayClassification::InFlight));
        let evidence = window.commit(handle, 10).expect("commit");
        assert_eq!(evidence.sequence, 70);
        assert_eq!(evidence.session_generation, 2);
        assert_eq!(evidence.key_generation, 1);
        assert_eq!(evidence.context_fingerprint, [3; 16]);
        assert_eq!(window.classify(70), Ok(ReplayClassification::Duplicate));
        assert_eq!(window.commit(handle, 10), Err(Error::NotFound));
    }

    #[test]
    fn stale_and_inflight_duplicates_fail_closed() {
        let mut window = ReplayWindow::<2>::new();
        let first = window
            .reserve(1, 1, 1, 1, 1, [3; 16], 100, 10, 20, [1; 16], [2; 16])
            .expect("reserve first");
        assert_eq!(
            window.reserve(1, 1, 1, 1, 1, [3; 16], 100, 10, 20, [2; 16], [3; 16]),
            Err(Error::Replay)
        );
        window.commit(first, 10).expect("commit first");
        let newer = window
            .reserve(1, 1, 1, 1, 1, [3; 16], 164, 10, 20, [2; 16], [3; 16])
            .expect("reserve newer");
        window.commit(newer, 10).expect("commit newer");
        assert_eq!(window.classify(100), Ok(ReplayClassification::Stale));
    }

    #[test]
    fn abort_preserves_committed_window() {
        let mut window = ReplayWindow::<1>::new();
        let handle = window
            .reserve(1, 1, 1, 1, 1, [3; 16], 5, 10, 20, [1; 16], [2; 16])
            .expect("reserve");
        window.abort(handle).expect("abort");
        assert_eq!(window.classify(5), Ok(ReplayClassification::Fresh));
    }

    #[test]
    fn deadline_maintenance_releases_only_the_target_reservation() {
        let mut window = ReplayWindow::<2>::new();
        let expired = window
            .reserve(1, 1, 1, 1, 1, [3; 16], 5, 10, 20, [1; 16], [2; 16])
            .expect("reserve expiring slot");
        let live = window
            .reserve(1, 1, 1, 1, 1, [3; 16], 6, 10, 30, [2; 16], [3; 16])
            .expect("reserve live slot");
        assert!(!window.expire_reservation_at(0, 19));
        assert!(window.expire_reservation_at(0, 20));
        assert_eq!(window.commit(expired, 20), Err(Error::NotFound));
        assert_eq!(window.classify(5), Ok(ReplayClassification::Fresh));
        assert_eq!(window.commit(live, 20).unwrap().sequence, 6);
    }
}
