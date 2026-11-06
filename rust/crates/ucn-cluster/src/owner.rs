use crate::{
    ClusterAuthority, ClusterConfig, ClusterEpoch, ClusterMember, ClusterPhase, ClusterView,
    HandoverCommit, HandoverReady, MemberFact, SnapshotProof, TransitionKind, VoteEvidence,
};
use ucn_durable::{DurableBase, DurableContract, DurableUpdate};
use ucn_persistence::{
    CodecWorkspace, DomainKind, PersistenceHandle, PersistenceProof, PersistenceRequest,
};
use ucn_types::{Error, Result};

#[derive(Clone, Copy)]
struct State<const V: usize> {
    epoch: ClusterEpoch,
    stable: ClusterConfig<V>,
    target_epoch: Option<ClusterEpoch>,
    target_config: Option<ClusterConfig<V>>,
    phase: ClusterPhase,
    kind: Option<TransitionKind>,
    transaction: u64,
    high_water: u64,
    deadline: u64,
    fenced: bool,
    joint: bool,
    old_votes: [Option<VoteEvidence>; V],
    new_votes: [Option<VoteEvidence>; V],
    backup_ready: bool,
    backup_generation: u32,
    backup_digest: [u8; 16],
}
struct Pending<const V: usize, const B: usize> {
    update: DurableUpdate<B>,
    next: State<V>,
    invalidated: [Option<[u8; 16]>; 32],
}
/// Fixed independent Cluster management Owner; `固定独立Cluster管理Owner`。
pub struct ClusterOwner<const VOTERS: usize, const MEMBERS: usize, const RECORD_BYTES: usize> {
    owner: u32,
    contract: DurableContract,
    durable_generation: u64,
    local: ClusterMember,
    state: Option<State<VOTERS>>,
    members: [Option<MemberFact>; MEMBERS],
    pending: Option<Pending<VOTERS, RECORD_BYTES>>,
    authority_until: u64,
    quorum: bool,
}
impl<const V: usize, const M: usize, const B: usize> ClusterOwner<V, M, B> {
    /// Bind canonical v2 Cluster record and current local Principal.
    /// 绑定规范v2 `ClusterRecord及当前本地身份`。
    /// # Errors
    /// Invalid profile/record/schema/dependency configuration rejects.
    pub fn new(owner: u32, contract: DurableContract, local: ClusterMember) -> Result<Self> {
        local.validate()?;
        if owner == 0
            || V == 0
            || V > 32
            || M < V
            || M > 32
            || B < 144 + 112 * V
            || contract.domain.kind != DomainKind::Cluster
            || contract.schema != 0x0803
            || contract.version != 2
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            contract,
            durable_generation: 0,
            local,
            state: None,
            members: [None; M],
            pending: None,
            authority_until: 0,
            quorum: false,
        })
    }
    /// Prepare factory Cluster only from empty durable domain and Config=1,Term=1.
    /// 仅空耐久域、Config1/Term1准备FactoryCluster。
    /// # Errors
    /// No local configured voter/Head, invalid lease/tx or nonempty domain rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn create(
        &mut self,
        base: DurableBase,
        epoch: ClusterEpoch,
        config: ClusterConfig<V>,
        transaction: u64,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        epoch.validate()?;
        config.validate()?;
        if self.state.is_some()
            || self.pending.is_some()
            || base.generation() != 0
            || base.contract() != self.contract
            || epoch.term != 1
            || config.id != 1
            || config.generation != 1
            || epoch.head != self.local.principal
            || epoch.binding != self.local.binding
            || config.find(epoch.head) != Some(self.local)
            || !self.local.voter
            || transaction == 0
            || now == 0
            || now >= until
        {
            return Err(Error::State);
        }
        let next = State {
            epoch,
            stable: config,
            target_epoch: None,
            target_config: None,
            phase: ClusterPhase::Stable,
            kind: None,
            transaction: 0,
            high_water: transaction,
            deadline: 0,
            fenced: false,
            joint: false,
            old_votes: [None; V],
            new_votes: [None; V],
            backup_ready: false,
            backup_generation: 0,
            backup_digest: [0; 16],
        };
        self.prepare(base, 0x0830, next, now, until, continuation, scratch)
    }
    /// Import durable state with no recovered live leases or usable historical votes.
    /// 导入耐久状态，不恢复Live租约或可用历史票。
    /// # Errors
    /// Bad canonical record/schema rejects.
    pub fn restore(
        &mut self,
        base: DurableBase,
        body: &[u8],
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if self.state.is_some()
            || self.pending.is_some()
            || base.contract() != self.contract
            || base.generation() == 0
        {
            return Err(Error::State);
        }
        base.verify_body(body, scratch)?;
        let mut state = decode_state::<V>(body)?;
        state.old_votes.fill(None);
        state.new_votes.fill(None);
        state.backup_ready = false;
        if matches!(state.phase, ClusterPhase::Collecting | ClusterPhase::Quorum) {
            state.phase = ClusterPhase::Aborted;
            state.deadline = 0;
        }
        self.state = Some(state);
        self.durable_generation = base.generation();
        self.authority_until = 0;
        self.quorum = false;
        Ok(())
    }
    /// Observe fresh authenticated membership. Expiry/re-admission atomically invalidates old votes.
    /// 观察新鲜认证成员；过期/重新准入原子使旧票失效。
    /// # Errors
    /// Non-configured/expired/capability-incomplete inputs cannot allocate member state.
    pub fn observe_member(&mut self, fact: MemberFact, now: u64, readmitted: bool) -> Result<()> {
        fact.member.validate()?;
        let state = self.state.as_ref().ok_or(Error::State)?;
        let allowed = state.stable.find(fact.member.principal) == Some(fact.member)
            || state
                .target_config
                .is_some_and(|config| config.find(fact.member.principal) == Some(fact.member));
        if !allowed || !fact.live(now) {
            return Err(Error::Access);
        }
        let slot = self.members.iter().position(|old| {
            old.is_some_and(|value| value.member.principal == fact.member.principal)
        });
        let clear = slot.is_some_and(|slot| {
            self.members[slot]
                .is_some_and(|old| readmitted || !old.live(now) || !old.same_parent(fact))
        });
        let index = slot
            .or_else(|| self.members.iter().position(Option::is_none))
            .ok_or(Error::NoSpace)?;
        if clear {
            self.clear_votes(fact.member.principal);
        }
        self.members[index] = Some(fact);
        self.quorum = self.live_quorum(now);
        Ok(())
    }
    fn clear_votes(&mut self, principal: [u8; 16]) {
        if let Some(pending) = self.pending.as_mut() {
            if !pending.invalidated.contains(&Some(principal)) {
                if let Some(slot) = pending.invalidated.iter_mut().find(|slot| slot.is_none()) {
                    *slot = Some(principal);
                }
            }
        }
        if let Some(state) = self.state.as_mut() {
            for vote in state.old_votes.iter_mut().chain(state.new_votes.iter_mut()) {
                if vote.is_some_and(|v| v.voter.principal == principal) {
                    *vote = None;
                }
            }
        }
    }
    fn live_config(&self, config: ClusterConfig<V>, now: u64) -> bool {
        config.members[..config.count]
            .iter()
            .filter(|member| {
                member.voter
                    && self
                        .members
                        .iter()
                        .flatten()
                        .any(|fact| fact.member == **member && fact.live(now))
            })
            .count()
            >= config.quorum()
    }
    fn live_quorum(&self, now: u64) -> bool {
        self.state.as_ref().is_some_and(|state| {
            self.live_config(state.stable, now)
                && (!state.joint
                    || state
                        .target_config
                        .is_some_and(|config| self.live_config(config, now)))
        })
    }
    /// Establish a local Authority lease only under current Head/live quorum.
    /// 仅当前Head/LiveQuorum下建立本地Authority租约。
    /// # Errors
    /// Fenced/wrong Head/no quorum rejects.
    pub fn renew_authority(&mut self, now: u64, until: u64) -> Result<()> {
        let state = self.state.as_ref().ok_or(Error::State)?;
        if state.fenced
            || self
                .pending
                .as_ref()
                .is_some_and(|pending| pending.next.fenced)
            || state.epoch.head != self.local.principal
            || state.epoch.binding != self.local.binding
            || now == 0
            || now >= until
            || !self.live_quorum(now)
        {
            return Err(Error::Access);
        }
        self.authority_until = until;
        Ok(())
    }
    /// Every Authority use rechecks now, live Stable/Joint quorum and local Fence.
    /// 每次Authority使用复验now、Live Stable/JointQuorum及本地Fence。
    /// # Errors
    /// Any loss immediately clears active permission.
    pub fn authority_preflight(&mut self, now: u64) -> Result<ClusterAuthority> {
        let state = self.state.as_ref().ok_or(Error::State)?;
        self.quorum = self.live_quorum(now);
        if state.fenced
            || self
                .pending
                .as_ref()
                .is_some_and(|pending| pending.next.fenced)
            || state.epoch.head != self.local.principal
            || state.epoch.binding != self.local.binding
            || now == 0
            || now >= self.authority_until
            || !self.quorum
        {
            self.authority_until = 0;
            return Err(Error::Access);
        }
        Ok(ClusterAuthority {
            owner: self.owner,
            epoch: state.epoch,
            config_id: state.stable.id,
            config_generation: state.stable.generation,
            checked_at: now,
            deadline: self.authority_until,
        })
    }
    /// Prepare exact checked-next Config; 准备精确checked-next Config。
    /// # Errors
    /// Missing current Authority, tx reuse, invalid next generation or pending rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn config_prepare(
        &mut self,
        base: DurableBase,
        target: ClusterConfig<V>,
        transaction: u64,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        self.authority_preflight(now)?;
        target.validate()?;
        let mut next = self.state.ok_or(Error::State)?;
        if self.pending.is_some()
            || next.joint
            || !matches!(
                next.phase,
                ClusterPhase::Stable | ClusterPhase::EpochDurable | ClusterPhase::Aborted
            )
            || target.id != next.stable.id.checked_add(1).ok_or(Error::Exhausted)?
            || target.generation
                != next
                    .stable
                    .generation
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?
            || transaction <= next.high_water
        {
            return Err(Error::State);
        }
        next.target_config = Some(target);
        next.phase = ClusterPhase::Prepared;
        next.kind = Some(TransitionKind::Config);
        next.transaction = transaction;
        next.high_water = transaction;
        next.deadline = until;
        self.prepare(base, 0x0831, next, now, until, continuation, scratch)
    }
    /// Durable Joint is mandatory before new Stable; `新Stable前必须耐久Joint`。
    /// # Errors
    /// PREPARED mismatch or no current old Authority rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn config_joint(
        &mut self,
        base: DurableBase,
        transaction: u64,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        self.authority_preflight(now)?;
        let mut next = self.state.ok_or(Error::State)?;
        if next.phase != ClusterPhase::Prepared
            || next.kind != Some(TransitionKind::Config)
            || next.transaction != transaction
            || next.target_config.is_none()
        {
            return Err(Error::State);
        }
        next.joint = true;
        next.phase = ClusterPhase::Joint;
        self.prepare(base, 0x0832, next, now, until, continuation, scratch)
    }
    /// Commit Config only from exact durable Joint and both live quorums.
    /// `仅精确耐久Joint及双方LiveQuorum下提交Config`。
    /// # Errors
    /// Missing Backup evidence, wrong transaction or no double quorum rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn config_commit(
        &mut self,
        base: DurableBase,
        transaction: u64,
        backup_proof: Option<SnapshotProof>,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        self.authority_preflight(now)?;
        let mut next = self.state.ok_or(Error::State)?;
        let target = next.target_config.ok_or(Error::State)?;
        if next.phase != ClusterPhase::Joint
            || !next.joint
            || next.transaction != transaction
            || !self.live_config(target, now)
        {
            return Err(Error::Access);
        }
        if let Some(backup) = target.members[..target.count]
            .iter()
            .find(|member| member.backup)
        {
            if backup_proof.is_none_or(|proof| {
                proof.transaction != transaction
                    || proof.backup != *backup
                    || proof.checked_at != now
                    || proof.epoch != next.epoch
                    || proof.config != next.stable.id
                    || proof.config_generation != next.stable.generation
                    || proof.joint != Some((target.id, target.generation))
                    || !self
                        .members
                        .iter()
                        .flatten()
                        .any(|fact| fact.member == *backup && fact.live(now))
            }) {
                return Err(Error::Access);
            }
        }
        next.stable = target;
        next.target_config = None;
        next.phase = ClusterPhase::Stable;
        next.joint = false;
        next.kind = None;
        next.old_votes.fill(None);
        next.new_votes.fill(None);
        next.backup_ready = false;
        next.backup_generation = 0;
        next.backup_digest = [0; 16];
        self.prepare(base, 0x0833, next, now, until, continuation, scratch)
    }
    /// Prepare designated Backup takeover/recovery with complete snapshot proof.
    /// 用完整Snapshot证明准备指定Backup接管/恢复。
    /// # Errors
    /// Wrong Backup, missing coverage, old tx or non-next Term rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn transition_begin(
        &mut self,
        base: DurableBase,
        kind: TransitionKind,
        target: ClusterEpoch,
        transaction: u64,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        target.validate()?;
        let mut next = self.state.ok_or(Error::State)?;
        if !matches!(kind, TransitionKind::Takeover | TransitionKind::Recovery)
            || matches!(
                next.phase,
                ClusterPhase::Collecting | ClusterPhase::Quorum | ClusterPhase::Prepared
            )
            || next.fenced
            || next
                .stable
                .find(self.local.principal)
                .is_none_or(|member| member != self.local || !member.backup)
            || !next.backup_ready
            || target.cluster != next.epoch.cluster
            || target.term != next.epoch.term.checked_add(1).ok_or(Error::Exhausted)?
            || target.head != self.local.principal
            || target.binding != self.local.binding
            || transaction <= next.high_water
        {
            return Err(Error::Access);
        }
        next.target_epoch = Some(target);
        next.kind = Some(kind);
        next.phase = ClusterPhase::Collecting;
        next.transaction = transaction;
        next.high_water = transaction;
        next.deadline = until;
        next.old_votes.fill(None);
        next.new_votes.fill(None);
        self.prepare(base, 0x0834, next, now, until, continuation, scratch)
    }
    /// Persist complete live voter evidence before publishing vote ACK.
    /// `发布VoteACK前持久化完整Live投票证据`。
    /// # Errors
    /// Wrong tx/terminal/deadline/session/capability/voter or conflicting vote rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn vote(
        &mut self,
        base: DurableBase,
        transaction: u64,
        vote: VoteEvidence,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        let mut next = self.state.ok_or(Error::State)?;
        if !matches!(next.phase, ClusterPhase::Collecting | ClusterPhase::Quorum)
            || next.transaction != transaction
            || now >= next.deadline
            || vote.vote_id == 0
            || vote.digest == [0; 16]
            || !self.vote_current(vote, now)
        {
            return Err(Error::Access);
        }
        let mut matched = false;
        for (config, votes) in [
            (next.stable, &mut next.old_votes),
            (
                next.target_config.unwrap_or(next.stable),
                &mut next.new_votes,
            ),
        ] {
            if let Some(index) = config.members[..config.count]
                .iter()
                .position(|member| *member == vote.voter && member.voter)
            {
                if votes[index].is_some_and(|old| old != vote) {
                    return Err(Error::Replay);
                }
                votes[index] = Some(vote);
                matched = true;
            }
        }
        if !matched {
            return Err(Error::Access);
        }
        if self.vote_quorum(&next, now) {
            next.phase = ClusterPhase::Quorum;
        }
        self.prepare(base, 0x0834, next, now, until, continuation, scratch)
    }
    fn vote_current(&self, vote: VoteEvidence, now: u64) -> bool {
        self.members.iter().flatten().any(|fact| {
            fact.member == vote.voter
                && fact.live(now)
                && fact.session == vote.session
                && fact.capability == vote.capability
        })
    }
    fn vote_quorum(&self, state: &State<V>, now: u64) -> bool {
        let old = state
            .old_votes
            .iter()
            .flatten()
            .filter(|vote| self.vote_current(**vote, now))
            .count()
            >= state.stable.quorum();
        old && (!state.joint
            || state.target_config.is_some_and(|config| {
                state
                    .new_votes
                    .iter()
                    .flatten()
                    .filter(|vote| self.vote_current(**vote, now))
                    .count()
                    >= config.quorum()
            }))
    }
    /// Exact certified Epoch commit; no generic bypass exists.
    /// 精确证书EpochCommit，不提供通用旁路。
    /// # Errors
    /// Historical/expired/readmitted votes or wrong successor rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn transition_commit(
        &mut self,
        base: DurableBase,
        transaction: u64,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        let mut next = self.state.ok_or(Error::State)?;
        if next.phase != ClusterPhase::Quorum
            || next.transaction != transaction
            || now >= next.deadline
            || !self.vote_quorum(&next, now)
            || !next.backup_ready
        {
            return Err(Error::Access);
        }
        let target = next.target_epoch.ok_or(Error::State)?;
        if target.head != self.local.principal || target.binding != self.local.binding {
            return Err(Error::Access);
        }
        next.epoch = target;
        next.phase = ClusterPhase::EpochDurable;
        self.prepare(base, 0x0835, next, now, until, continuation, scratch)
    }
    #[allow(clippy::too_many_arguments)]
    fn prepare(
        &mut self,
        base: DurableBase,
        operation: u16,
        next: State<V>,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if self.pending.is_some()
            || base.contract() != self.contract
            || base.generation() != self.durable_generation
        {
            return Err(Error::State);
        }
        let mut body = [0; B];
        let size = encode_state(next, self.local, &mut body)?;
        let update = DurableUpdate::prepare(
            base.for_operation(operation)?,
            &body[..size],
            now,
            until,
            continuation,
            scratch,
        )?;
        self.pending = Some(Pending {
            update,
            next,
            invalidated: [None; 32],
        });
        Ok(())
    }

    /// Record exact complete Backup snapshot before publishing READY.
    /// `发布READY前记录精确完整Backup快照`。
    /// # Errors
    /// Wrong assignment/config/epoch/source/time or stale snapshot rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn backup_ready(
        &mut self,
        base: DurableBase,
        proof: SnapshotProof,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        let mut next = self.state.ok_or(Error::State)?;
        if proof.epoch != next.epoch
            || proof.config != next.stable.id
            || proof.config_generation != next.stable.generation
            || proof.joint
                != next
                    .target_config
                    .filter(|_| next.joint)
                    .map(|config| (config.id, config.generation))
            || proof.backup != self.local
            || proof.checked_at != now
            || proof.generation <= next.backup_generation
            || next.fenced
        {
            return Err(Error::Access);
        }
        next.backup_ready = true;
        next.backup_generation = proof.generation;
        next.backup_digest = proof.digest;
        self.prepare(base, 0x0830, next, now, until, continuation, scratch)
    }
    /// Fresh target readiness binds current quorum, admission and complete snapshot.
    /// 新鲜目标就绪绑定当前Quorum、准入及完整Snapshot。
    /// # Errors
    /// Non-designated/expired/incomplete target cannot issue readiness.
    pub fn handover_target_ready(&self, now: u64) -> Result<HandoverReady> {
        let state = self.state.ok_or(Error::State)?;
        let fact = self
            .members
            .iter()
            .flatten()
            .find(|fact| fact.member == self.local && fact.live(now))
            .ok_or(Error::Access)?;
        if self.pending.is_some()
            || state.fenced
            || state.joint
            || state.target_config.is_some()
            || !matches!(
                state.phase,
                ClusterPhase::Stable | ClusterPhase::EpochDurable | ClusterPhase::Aborted
            )
            || !state.backup_ready
            || !self.local.backup
            || state.stable.find(self.local.principal) != Some(self.local)
            || !self.live_quorum(now)
        {
            return Err(Error::Access);
        }
        Ok(HandoverReady {
            epoch: state.epoch,
            target: self.local,
            config: state.stable.id,
            generation: state.stable.generation,
            session: fact.session,
            capability: fact.capability,
            capability_digest: fact.capability_digest,
            flow_generation: fact.flow_generation,
            link_generation: fact.link_generation,
            snapshot: state.backup_digest,
            checked_at: now,
            until: fact.lease_until.min(fact.capability_until),
        })
    }
    /// Prepare irreversible handover Fence only after exact current target readiness.
    /// 精确当前目标就绪后才准备不可逆移交Fence。
    /// # Errors
    /// Wrong target/current capability/quorum/epoch or expired proof rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn handover_commit(
        &mut self,
        base: DurableBase,
        ready: HandoverReady,
        transaction: u64,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        self.authority_preflight(now)?;
        let mut next = self.state.ok_or(Error::State)?;
        let valid = self.members.iter().flatten().any(|fact| {
            fact.member == ready.target
                && fact.live(now)
                && fact.session == ready.session
                && fact.capability == ready.capability
                && fact.capability_digest == ready.capability_digest
                && fact.flow_generation == ready.flow_generation
                && fact.link_generation == ready.link_generation
        });
        if ready.epoch != next.epoch
            || ready.config != next.stable.id
            || ready.generation != next.stable.generation
            || ready.checked_at != now
            || now >= ready.until
            || !valid
            || !ready.target.backup
            || ready.target.principal == self.local.principal
            || ready.snapshot == [0; 16]
            || transaction <= next.high_water
        {
            return Err(Error::Access);
        }
        next.target_epoch = Some(ClusterEpoch {
            cluster: next.epoch.cluster,
            term: next.epoch.term.checked_add(1).ok_or(Error::Exhausted)?,
            head: ready.target.principal,
            binding: ready.target.binding,
        });
        next.fenced = true;
        next.phase = ClusterPhase::EpochDurable;
        next.kind = Some(TransitionKind::Handover);
        next.transaction = transaction;
        next.high_water = transaction;
        next.deadline = until;
        self.prepare(base, 0x0836, next, now, until, continuation, scratch)?;
        self.authority_until = 0;
        Ok(())
    }
    /// Exact handover commit is unavailable before old Head Fence reload.
    /// `旧HeadFence回读前不能取得精确移交Commit`。
    /// # Errors
    /// Pending/non-fenced/non-handover transaction rejects.
    pub fn handover_proof(&self) -> Result<HandoverCommit> {
        let s = self.state.ok_or(Error::State)?;
        if self.pending.is_some()
            || !s.fenced
            || s.kind != Some(TransitionKind::Handover)
            || s.phase != ClusterPhase::EpochDurable
        {
            return Err(Error::State);
        }
        Ok(HandoverCommit {
            old: s.epoch,
            next: s.target_epoch.ok_or(Error::State)?,
            config: s.stable.id,
            config_generation: s.stable.generation,
            transaction: s.transaction,
        })
    }
    /// Accept only an old Head's sealed durable Fence and fresh own readiness.
    /// `仅接纳旧Head的封闭耐久Fence及本地新鲜就绪`。
    /// # Errors
    /// Stale target, wrong lineage/config or missing current readiness rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn accept_handover(
        &mut self,
        base: DurableBase,
        commit: HandoverCommit,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        self.handover_target_ready(now)?;
        let mut next = self.state.ok_or(Error::State)?;
        if commit.old != next.epoch
            || commit.config != next.stable.id
            || commit.config_generation != next.stable.generation
            || commit.next.cluster != next.epoch.cluster
            || commit.next.term != next.epoch.term.checked_add(1).ok_or(Error::Exhausted)?
            || commit.next.head != self.local.principal
            || commit.next.binding != self.local.binding
            || commit.transaction <= next.high_water
        {
            return Err(Error::Access);
        }
        next.epoch = commit.next;
        next.target_epoch = Some(commit.next);
        next.phase = ClusterPhase::EpochDurable;
        next.kind = Some(TransitionKind::Handover);
        next.transaction = commit.transaction;
        next.high_water = commit.transaction;
        next.deadline = until;
        self.prepare(base, 0x0835, next, now, until, continuation, scratch)
    }
    /// Abort is permitted only before durable Joint; `仅耐久Joint前允许Abort`。
    /// # Errors
    /// Joint, terminal or wrong transaction rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn abort_prepared(
        &mut self,
        base: DurableBase,
        transaction: u64,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        let mut next = self.state.ok_or(Error::State)?;
        if next.phase != ClusterPhase::Prepared || next.joint || next.transaction != transaction {
            return Err(Error::State);
        }
        next.target_config = None;
        next.phase = ClusterPhase::Aborted;
        next.kind = None;
        self.prepare(base, 0x0837, next, now, until, continuation, scratch)
    }
    /// Coordinator exact requirement; Coordinator精确要求。
    /// # Errors
    /// No pending transition rejects.
    pub fn requirement(&self) -> Result<PersistenceRequest<'_>> {
        Ok(self.pending.as_ref().ok_or(Error::State)?.update.request())
    }
    /// Bind Foundation handle; `绑定FoundationHandle`。
    /// # Errors
    /// Wrong pending state rejects.
    pub fn bind(&mut self, h: PersistenceHandle) -> Result<()> {
        self.pending.as_mut().ok_or(Error::State)?.update.bind(h)
    }
    /// Install exact durable truth, then recompute current live permission.
    /// 安装精确耐久事实后重算当前Live权限。
    /// # Errors
    /// Wrong proof preserves all pending/state facts.
    pub fn confirm(
        &mut self,
        h: PersistenceHandle,
        proof: PersistenceProof,
        now: u64,
    ) -> Result<()> {
        if now == 0 {
            return Err(Error::Argument);
        }
        self.pending
            .as_mut()
            .ok_or(Error::State)?
            .update
            .confirm(h, proof)?;
        let pending = self.pending.take().ok_or(Error::State)?;
        self.state = Some(pending.next);
        self.durable_generation = proof.record_generation();
        for principal in pending.invalidated.into_iter().flatten() {
            self.clear_votes(principal);
        }
        self.quorum = self.live_quorum(now);
        if !self.quorum {
            self.authority_until = 0;
        }
        Ok(())
    }
    /// Read current view without implicitly renewing a lease; 读取当前视图，不隐式续租。
    /// # Errors
    /// No committed state rejects.
    pub fn view(&self) -> Result<ClusterView> {
        let s = self.state.ok_or(Error::State)?;
        Ok(ClusterView {
            epoch: s.epoch,
            config_id: s.stable.id,
            phase: s.phase,
            fenced: s.fenced,
            quorum: self.quorum,
        })
    }
    /// Expire facts and votes before next use; 下次使用前清除过期事实及票。
    /// # Errors
    /// Unknown time rejects; `EpochDurable` never moves backwards.
    pub fn step(&mut self, now: u64) -> Result<()> {
        if now == 0 {
            return Err(Error::Argument);
        }
        for index in 0..M {
            if self.members[index].is_some_and(|fact| !fact.live(now)) {
                let principal = self.members[index].ok_or(Error::State)?.member.principal;
                self.clear_votes(principal);
                self.members[index] = None;
            }
        }
        self.quorum = self.live_quorum(now);
        if !self.quorum {
            self.authority_until = 0;
        }
        Ok(())
    }
}

fn encode_epoch(out: &mut [u8], epoch: Option<ClusterEpoch>) {
    if let Some(e) = epoch {
        out[..4].copy_from_slice(&e.cluster.to_be_bytes());
        out[4..8].copy_from_slice(&e.term.to_be_bytes());
        out[8..12].copy_from_slice(&e.binding.to_be_bytes());
        out[12..28].copy_from_slice(&e.head);
    }
}
fn decode_epoch(b: &[u8]) -> Result<Option<ClusterEpoch>> {
    if b.iter().all(|byte| *byte == 0) {
        return Ok(None);
    }
    let mut head = [0; 16];
    head.copy_from_slice(&b[12..28]);
    let e = ClusterEpoch {
        cluster: u32::from_be_bytes(b[..4].try_into().map_err(|_| Error::Malformed)?),
        term: u32::from_be_bytes(b[4..8].try_into().map_err(|_| Error::Malformed)?),
        binding: u32::from_be_bytes(b[8..12].try_into().map_err(|_| Error::Malformed)?),
        head,
    };
    e.validate().map_err(|_| Error::Malformed)?;
    Ok(Some(e))
}
fn encode_state<const V: usize>(
    s: State<V>,
    local: ClusterMember,
    out: &mut [u8],
) -> Result<usize> {
    let size = 144 + 112 * V;
    if out.len() < size {
        return Err(Error::NoSpace);
    }
    out[..size].fill(0);
    out[..4].copy_from_slice(b"CLS6");
    out[4..6].copy_from_slice(&2u16.to_be_bytes());
    out[6] = s.phase as u8;
    out[7] = if s.fenced {
        5
    } else if s.epoch.head == local.principal {
        4
    } else if local.backup {
        3
    } else {
        2
    };
    encode_epoch(&mut out[8..36], Some(s.epoch));
    encode_epoch(&mut out[36..64], s.target_epoch);
    out[64..68].copy_from_slice(&s.stable.id.to_be_bytes());
    out[68..72].copy_from_slice(&s.stable.generation.to_be_bytes());
    if let Some(c) = s.target_config {
        out[72..76].copy_from_slice(&c.id.to_be_bytes());
        out[76..80].copy_from_slice(&c.generation.to_be_bytes());
        out[137] = u8::try_from(c.count).map_err(|_| Error::Config)?;
    }
    out[80..88].copy_from_slice(&s.transaction.to_be_bytes());
    out[88..96].copy_from_slice(&s.deadline.to_be_bytes());
    out[96..104].copy_from_slice(&s.high_water.to_be_bytes());
    out[112..116].copy_from_slice(&s.backup_generation.to_be_bytes());
    if s.backup_ready {
        out[120..136].copy_from_slice(&s.backup_digest);
    }
    out[136] = u8::try_from(s.stable.count).map_err(|_| Error::Config)?;
    out[138] = s.kind.map_or(0, |kind| kind as u8);
    out[139] = s.phase as u8;
    out[140] = u8::from(s.joint);
    out[141] = u8::from(s.fenced);
    out[142] = u8::from(s.backup_ready);
    let mut cursor = 144;
    for config in [Some(s.stable), s.target_config] {
        for index in 0..V {
            if let Some(config) = config {
                if index < config.count {
                    let m = config.members[index];
                    out[cursor..cursor + 16].copy_from_slice(&m.principal);
                    out[cursor + 16..cursor + 20].copy_from_slice(&m.binding.to_be_bytes());
                    out[cursor + 20] = 1 | (u8::from(m.voter) * 2) | (u8::from(m.backup) * 4);
                }
            }
            cursor += 24;
        }
    }
    for votes in [s.old_votes, s.new_votes] {
        for vote in votes {
            if let Some(v) = vote {
                out[cursor..cursor + 4].copy_from_slice(&v.voter.binding.to_be_bytes());
                out[cursor + 4..cursor + 8].copy_from_slice(&v.session.to_be_bytes());
                out[cursor + 8..cursor + 12].copy_from_slice(&v.capability.to_be_bytes());
                out[cursor + 12..cursor + 16].copy_from_slice(&v.vote_id.to_be_bytes());
                out[cursor + 16..cursor + 32].copy_from_slice(&v.digest);
            }
            cursor += 32;
        }
    }
    Ok(size)
}
#[allow(clippy::too_many_lines)] // One canonical record validator, no business transitions or side effects.
fn decode_state<const V: usize>(b: &[u8]) -> Result<State<V>> {
    if b.len() != 144 + 112 * V
        || &b[..4] != b"CLS6"
        || b[4..6] != [0, 2]
        || b[140] > 1
        || b[141] > 1
        || b[142] > 1
        || b[143] != 0
    {
        return Err(Error::Malformed);
    }
    let phase = match b[6] {
        1 => ClusterPhase::Stable,
        2 => ClusterPhase::Prepared,
        3 => ClusterPhase::Joint,
        4 => ClusterPhase::Collecting,
        5 => ClusterPhase::Quorum,
        6 => ClusterPhase::EpochDurable,
        7 => ClusterPhase::Aborted,
        _ => return Err(Error::Malformed),
    };
    let kind = match b[138] {
        0 => None,
        1 => Some(TransitionKind::Config),
        2 => Some(TransitionKind::Takeover),
        3 => Some(TransitionKind::Recovery),
        4 => Some(TransitionKind::Handover),
        5 => Some(TransitionKind::Merge),
        _ => return Err(Error::Malformed),
    };
    let r32 = |i| u32::from_be_bytes(b[i..i + 4].try_into().expect("checked Cluster record"));
    let r64 = |i| u64::from_be_bytes(b[i..i + 8].try_into().expect("checked Cluster record"));
    let mut configs = [None; 2];
    let mut cursor = 144;
    for (which, config) in configs.iter_mut().enumerate() {
        let count = usize::from(b[136 + which]);
        if count > V {
            return Err(Error::Malformed);
        }
        let mut members = [ClusterMember::EMPTY; V];
        for (index, member) in members.iter_mut().enumerate() {
            let m = &b[cursor..cursor + 24];
            if index < count {
                if m[21..24] != [0; 3] || m[20] & !7 != 0 || m[20] & 1 == 0 {
                    return Err(Error::Malformed);
                }
                member.principal.copy_from_slice(&m[..16]);
                member.binding =
                    u32::from_be_bytes(m[16..20].try_into().map_err(|_| Error::Malformed)?);
                member.voter = m[20] & 2 != 0;
                member.backup = m[20] & 4 != 0;
            } else if m.iter().any(|byte| *byte != 0) {
                return Err(Error::Malformed);
            }
            cursor += 24;
        }
        if count != 0 {
            let value = ClusterConfig {
                id: r32(64 + which * 8),
                generation: r32(68 + which * 8),
                count,
                members,
            };
            value.validate().map_err(|_| Error::Malformed)?;
            *config = Some(value);
        }
    }
    let stable = configs[0].ok_or(Error::Malformed)?;
    let mut votes = [[None; V]; 2];
    for (which, set) in votes.iter_mut().enumerate() {
        for (index, vote) in set.iter_mut().enumerate() {
            let bytes = &b[cursor..cursor + 32];
            if bytes.iter().any(|byte| *byte != 0) {
                let config = configs[which].unwrap_or(stable);
                if index >= config.count {
                    return Err(Error::Malformed);
                }
                let mut digest = [0; 16];
                digest.copy_from_slice(&bytes[16..32]);
                let evidence = VoteEvidence {
                    voter: config.members[index],
                    session: u32::from_be_bytes(
                        bytes[4..8].try_into().map_err(|_| Error::Malformed)?,
                    ),
                    capability: u32::from_be_bytes(
                        bytes[8..12].try_into().map_err(|_| Error::Malformed)?,
                    ),
                    vote_id: u32::from_be_bytes(
                        bytes[12..16].try_into().map_err(|_| Error::Malformed)?,
                    ),
                    digest,
                };
                if evidence.voter.binding
                    != u32::from_be_bytes(bytes[..4].try_into().map_err(|_| Error::Malformed)?)
                    || evidence.session == 0
                    || evidence.capability == 0
                    || evidence.vote_id == 0
                    || digest == [0; 16]
                {
                    return Err(Error::Malformed);
                }
                *vote = Some(evidence);
            }
            cursor += 32;
        }
    }
    let mut backup_digest = [0; 16];
    backup_digest.copy_from_slice(&b[120..136]);
    let epoch = decode_epoch(&b[8..36])?.ok_or(Error::Malformed)?;
    let target_epoch = decode_epoch(&b[36..64])?;
    if b[140] == 1 && configs[1].is_none()
        || b[142] == 1 && (r32(112) == 0 || backup_digest == [0; 16])
        || r64(80) > r64(96)
    {
        return Err(Error::Malformed);
    }
    Ok(State {
        epoch,
        stable,
        target_epoch,
        target_config: configs[1],
        phase,
        kind,
        transaction: r64(80),
        high_water: r64(96),
        deadline: r64(88),
        fenced: b[141] != 0,
        joint: b[140] != 0,
        old_votes: votes[0],
        new_votes: votes[1],
        backup_ready: b[142] != 0,
        backup_generation: r32(112),
        backup_digest,
    })
}
