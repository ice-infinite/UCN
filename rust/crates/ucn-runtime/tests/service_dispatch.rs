#![cfg(feature = "service-rpc")]
#![cfg_attr(not(feature = "service-rpc"), allow(missing_docs))]
//! Actual C1 Data authentication, bounded RPC dedup and physical retirement.
//! 实际C1 Data认证、有界RPC去重和物理退休；全部Driver/Flash均为Host测试实现。
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
use fixture::{security, *};
use ucn_adapter::*;
use ucn_runtime::*;
use ucn_security::*;
use ucn_service::*;
use ucn_types::*;
use ucn_wire::CommonHeader;
#[cfg(feature = "durable-rpc")]
#[path = "service_dispatch/durable.rs"]
mod durable;

type Rpc = RpcBridge<1, 128>;
type Service = ServiceOwner<1, 1, 16>;
fn endpoint(peer: u8, request: bool, result: bool) -> RpcEndpoint {
    RpcEndpoint::new(
        ServiceId::new(0x40).unwrap(),
        [peer; 16],
        request,
        result,
        16,
        OriginSecurity::O1,
    )
    .unwrap()
}
fn bridge(peer: u8, server: bool) -> Rpc {
    Rpc::new(u32::from(peer) + 10, endpoint(peer, server, !server), 10, 4).unwrap()
}
fn packet(rpc: &Rpc, h: RpcHandle, facts: CurrentFacts) -> FragmentPacket {
    let mut p = FragmentPacket {
        bytes: [0; 128],
        length: 0,
        identity: [0; 46],
        sequence: 0,
    };
    p.length = rpc.copy_packet(h, &mut p.bytes).unwrap();
    p.sequence = u32::from_be_bytes(p.bytes[9..13].try_into().unwrap());
    p.identity[..16].copy_from_slice(&facts.local.principal);
    p.identity[16..20].copy_from_slice(&facts.local.generation.get().to_be_bytes());
    p.identity[20..36].copy_from_slice(&facts.peer.principal);
    p.identity[36..40].copy_from_slice(&facts.peer.generation.get().to_be_bytes());
    p.identity[40..46].copy_from_slice(&p.bytes[7..13]);
    p
}
#[allow(clippy::too_many_arguments)]
fn begin(
    rpc: &mut Rpc,
    service: &mut Service,
    sec: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    counter: &mut SequenceCounter,
    session: SessionHandle,
    facts: CurrentFacts,
    link: LinkHandle,
    adapter: &AdapterOwner<'_, 1, 1, 1, 128>,
) -> Result<RpcHandle> {
    rpc.begin_request(
        service,
        sec,
        crypto,
        counter,
        session,
        facts,
        Fingerprint::new([5; 16]).unwrap(),
        link,
        adapter,
        b"request",
        121,
        &mut SecurityPacketWorkspace::<128>::new(),
    )
}
#[allow(clippy::too_many_arguments)]
fn request(
    rpc: &mut Rpc,
    service: &mut Service,
    sec: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    p: &FragmentPacket,
    link: LinkHandle,
    adapter: &AdapterOwner<'_, 1, 1, 1, 128>,
    out: &mut [u8],
) -> Result<RpcReceived> {
    rpc.receive_request(
        service,
        sec,
        crypto,
        session,
        facts,
        p.plan(),
        link,
        adapter,
        out,
        &mut SecurityPacketWorkspace::<128>::new(),
    )
}
#[allow(clippy::too_many_arguments)]
fn result(
    rpc: &mut Rpc,
    h: RpcHandle,
    service: &mut Service,
    sec: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    p: &FragmentPacket,
    link: LinkHandle,
) -> Result<bool> {
    rpc.receive_result(
        h,
        service,
        sec,
        crypto,
        session,
        facts,
        p.plan(),
        link,
        &mut SecurityPacketWorkspace::<128>::new(),
    )
}
#[allow(clippy::too_many_arguments)]
fn seal(
    rpc: &mut Rpc,
    h: RpcHandle,
    service: &Service,
    sec: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    counter: &mut SequenceCounter,
    session: SessionHandle,
    facts: CurrentFacts,
) -> Result<()> {
    rpc.seal_response(
        h,
        service,
        sec,
        crypto,
        counter,
        session,
        facts,
        &mut SecurityPacketWorkspace::<128>::new(),
    )
}
#[allow(clippy::too_many_arguments)]
fn custom(
    sec: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    role: InteractionRole,
    sequence: u32,
    operation: u64,
    flags: u8,
    code: u16,
    business: &[u8],
) -> FragmentPacket {
    let offset = if role == InteractionRole::Request {
        9
    } else {
        11
    };
    let mut body = [0; 64];
    encode_operation_envelope(
        role,
        OperationEnvelope {
            operation,
            flags,
            result: code,
        },
        &mut body,
    )
    .unwrap();
    body[offset..offset + business.len()].copy_from_slice(business);
    let mut prefix = [0; 13];
    CommonHeader {
        contract: HeaderContract::C1,
        origin_security: OriginSecurity::O1,
        delivery: DeliveryGuarantee::BestEffort,
        interaction: role,
        payload_kind: PayloadKind::Data,
        traffic_class: TrafficClass::Q2,
        hop_limit: HopLimit::new(1).unwrap(),
    }
    .encode(&mut prefix);
    prefix[3..5].copy_from_slice(
        &u16::try_from(facts.local.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[5..7].copy_from_slice(
        &u16::try_from(facts.peer.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[7..9].copy_from_slice(&0x40u16.to_be_bytes());
    prefix[9..13].copy_from_slice(&sequence.to_be_bytes());
    let mut p = FragmentPacket {
        bytes: [0; 128],
        length: 0,
        identity: [0; 46],
        sequence,
    };
    p.identity[..16].copy_from_slice(&facts.local.principal);
    p.identity[16..20].copy_from_slice(&facts.local.generation.get().to_be_bytes());
    p.identity[20..36].copy_from_slice(&facts.peer.principal);
    p.identity[36..40].copy_from_slice(&facts.peer.generation.get().to_be_bytes());
    p.identity[40..46].copy_from_slice(&prefix[7..13]);
    p.length = sec
        .protect_packet(
            crypto,
            OriginCounterOwner::Sequence(&mut SequenceCounter(sequence)),
            session,
            facts,
            data_access(session, AccessDirection::Outbound),
            PacketPlan {
                prefix: &prefix,
                payload: &body[..offset + business.len()],
                canonical_identity: &p.identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: Fingerprint::new([5; 16]).unwrap(),
                    sequence: OriginSequence::new(sequence).unwrap(),
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut SecurityPacketWorkspace::<128>::new(),
            &mut p.bytes,
        )
        .unwrap();
    p
}

#[test]
#[allow(clippy::too_many_lines)] // Complete actual multi-Owner request/result and lost-reply lifecycle.
fn authenticated_raw_rpc_atomic_rx_lost_result_and_cached_retry_do_not_execute_twice() {
    security::exercise(1, 2, |client, ch, cf, cc| {
        security::exercise(2, 1, |server, sh, sf, sc| {
            let ci = AdapterEventIngress::<1, 1, 1, 128>::new(1).unwrap();
            let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
            let mut ca = AdapterOwner::new(1, &ci).unwrap();
            let mut sa = AdapterOwner::new(2, &si).unwrap();
            let cl = ca.open_link(0, 1, 128).unwrap();
            let sl = sa.open_link(0, 1, 128).unwrap();
            ca.try_set_rx_enabled(true).unwrap();
            sa.try_set_rx_enabled(true).unwrap();
            let mut c = bridge(2, false);
            let mut s = bridge(1, true);
            let mut cs = Service::new(1, 100).unwrap();
            let mut ss = Service::new(2, 100).unwrap();
            let mut cseq = SequenceCounter(10);
            let mut sseq = SequenceCounter(20);
            let h = begin(&mut c, &mut cs, client, cc, &mut cseq, ch, cf, cl, &ca).unwrap();
            assert_eq!(cseq.0, 11);
            let mut cd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            c.submit(h, &mut cs, client, ch, cf, &mut ca, &mut cd)
                .unwrap();
            c.retire_attempt(h, &mut ca).unwrap();
            let cv = c.view(h).unwrap();
            assert_eq!(cs.view(cv.service).unwrap().phase, RequestPhase::WaitResult);
            assert_eq!(cs.view(cv.service).unwrap().key.operation, 10);
            let p = packet(&c, h, cf);
            assert_eq!(p.length, 13 + 9 + 7 + 16);
            si.rx_publish(
                sl,
                &cd.frames[0],
                RxMeta {
                    timestamp_us: sf.now_us,
                    sender_discriminator: 1,
                },
            )
            .unwrap();
            let mut raw = [0; 128];
            let claim = sa.rx_claim(&mut raw).unwrap();
            assert_eq!(&raw[..claim.frame_bytes], &p.bytes[..p.length]);
            let incoming = FragmentPacket {
                bytes: raw,
                length: claim.frame_bytes,
                identity: p.identity,
                sequence: p.sequence,
            };
            let mut business = [0xa5; 16];
            let got = request(
                &mut s,
                &mut ss,
                server,
                sc,
                sh,
                sf,
                &incoming,
                claim.link,
                &sa,
                &mut business,
            )
            .unwrap();
            sa.rx_retire(claim.token).unwrap();
            assert_eq!(got.action, RpcAction::Invoke);
            assert_eq!(&business[..got.bytes], b"request");
            let before = s.view(got.handle).unwrap();
            business.fill(0xa5);
            let duplicate = request(
                &mut s,
                &mut ss,
                server,
                sc,
                sh,
                sf,
                &p,
                sl,
                &sa,
                &mut business,
            )
            .unwrap();
            assert_eq!(duplicate.action, RpcAction::Pending);
            assert_eq!(duplicate.bytes, 0);
            assert_eq!(business, [0xa5; 16]);
            assert_eq!(s.view(got.handle).unwrap(), before);
            s.respond(
                got.handle,
                &mut ss,
                server,
                sh,
                sf,
                InteractionRole::Result,
                7,
                b"answer",
            )
            .unwrap();
            assert_eq!(
                s.respond(
                    got.handle,
                    &mut ss,
                    server,
                    sh,
                    sf,
                    InteractionRole::Result,
                    8,
                    b"second"
                ),
                Err(Error::State)
            );
            seal(&mut s, got.handle, &ss, server, sc, &mut sseq, sh, sf).unwrap();
            assert_eq!(sseq.0, 21);
            let response = packet(&s, got.handle, sf);
            assert_eq!(response.length, 13 + 11 + 6 + 16);
            let mut sd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            s.submit(got.handle, &mut ss, server, sh, sf, &mut sa, &mut sd)
                .unwrap();
            s.retire_attempt(got.handle, &mut sa).unwrap(); // Network loses the first result.
            let later_c = CurrentFacts { now_us: 31, ..cf };
            let later_s = CurrentFacts { now_us: 31, ..sf };
            c.requeue(h, 31).unwrap();
            c.submit(h, &mut cs, client, ch, later_c, &mut ca, &mut cd)
                .unwrap();
            c.retire_attempt(h, &mut ca).unwrap();
            assert_eq!(cd.frames[0], cd.frames[1]);
            assert_eq!(cseq.0, 11);
            let again = request(
                &mut s,
                &mut ss,
                server,
                sc,
                sh,
                later_s,
                &p,
                sl,
                &sa,
                &mut business,
            )
            .unwrap();
            assert_eq!(again.action, RpcAction::Replay);
            s.requeue(got.handle, 31).unwrap();
            s.submit(got.handle, &mut ss, server, sh, later_s, &mut sa, &mut sd)
                .unwrap();
            s.retire_attempt(got.handle, &mut sa).unwrap();
            assert_eq!(sd.frames[0], sd.frames[1]);
            assert_eq!(sseq.0, 21);
            ci.rx_publish(
                cl,
                &sd.frames[1],
                RxMeta {
                    timestamp_us: 31,
                    sender_discriminator: 2,
                },
            )
            .unwrap();
            let received = ca.rx_claim(&mut raw).unwrap();
            let final_packet = FragmentPacket {
                bytes: raw,
                length: received.frame_bytes,
                identity: response.identity,
                sequence: response.sequence,
            };
            assert!(
                result(
                    &mut c,
                    h,
                    &mut cs,
                    client,
                    cc,
                    ch,
                    later_c,
                    &final_packet,
                    cl
                )
                .unwrap()
            );
            ca.rx_retire(received.token).unwrap();
            assert!(!result(&mut c, h, &mut cs, client, cc, ch, later_c, &response, cl).unwrap());
            assert_eq!(cs.view(cv.service).unwrap().phase, RequestPhase::Complete);
            let mut answer = [0; 16];
            assert_eq!(cs.copy_result(cv.service, &mut answer).unwrap(), 6);
            assert_eq!(&answer[..6], b"answer");
            assert_eq!(cs.view(cv.service).unwrap().result, 7);
            c.retire(h, &mut cs, 31).unwrap();
            assert_eq!(s.retire(got.handle, &mut ss, 31), Err(Error::State));
            let before = s.view(got.handle).unwrap();
            let mut wrong_owner = Service::new(9, 100).unwrap();
            assert_eq!(
                s.retire(got.handle, &mut wrong_owner, 121),
                Err(Error::State)
            );
            assert_eq!(s.view(got.handle).unwrap(), before);
            ss.step(121, 1).unwrap(); // Service has already evicted its expired receipt.
            s.retire(got.handle, &mut ss, 121).unwrap();
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Atomic reject then retry same sequence, retained history after cache retirement.
fn rejection_is_zero_publish_and_old_operation_never_revives_after_receipt_expires() {
    security::exercise(1, 2, |client, ch, cf, cc| {
        security::exercise(2, 1, |server, sh, sf, sc| {
            let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
            let mut sa = AdapterOwner::new(2, &si).unwrap();
            let sl = sa.open_link(0, 1, 128).unwrap();
            let mut s = bridge(1, true);
            let mut ss = Service::new(2, 100).unwrap();
            let original = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                10,
                10,
                1,
                0,
                b"request",
            );
            let mut bad = FragmentPacket {
                bytes: original.bytes,
                ..original
            };
            bad.bytes[bad.length - 1] ^= 1;
            let mut out = [0xa5; 16];
            assert!(request(&mut s, &mut ss, server, sc, sh, sf, &bad, sl, &sa, &mut out).is_err());
            assert_eq!(out, [0xa5; 16]);
            assert_eq!(
                request(
                    &mut s,
                    &mut ss,
                    server,
                    sc,
                    sh,
                    sf,
                    &original,
                    sl,
                    &sa,
                    &mut out[..1]
                ),
                Err(Error::NoSpace)
            );
            assert_eq!(out, [0xa5; 16]);
            let forbidden = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                11,
                11,
                0,
                0,
                b"request",
            );
            assert_eq!(
                request(
                    &mut s, &mut ss, server, sc, sh, sf, &forbidden, sl, &sa, &mut out
                ),
                Err(Error::Unsupported)
            );
            let unallocated = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                12,
                13,
                1,
                0,
                b"request",
            );
            assert_eq!(
                request(
                    &mut s,
                    &mut ss,
                    server,
                    sc,
                    sh,
                    sf,
                    &unallocated,
                    sl,
                    &sa,
                    &mut out
                ),
                Err(Error::Unsupported)
            );
            let got = request(
                &mut s, &mut ss, server, sc, sh, sf, &original, sl, &sa, &mut out,
            )
            .unwrap();
            assert_eq!(got.action, RpcAction::Invoke);
            let second = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                13,
                11,
                1,
                0,
                b"next",
            );
            out.fill(0xa5);
            assert_eq!(
                request(
                    &mut s, &mut ss, server, sc, sh, sf, &second, sl, &sa, &mut out
                ),
                Err(Error::NoSpace)
            );
            assert_eq!(out, [0xa5; 16]);
            s.retire(got.handle, &mut ss, 121).unwrap();
            let late = CurrentFacts { now_us: 122, ..sf };
            let old = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                14,
                10,
                1,
                0,
                b"request",
            );
            assert_eq!(
                request(
                    &mut s, &mut ss, server, sc, sh, late, &old, sl, &sa, &mut out
                ),
                Err(Error::Replay)
            );
            assert_eq!(out, [0xa5; 16]);
            let new = request(
                &mut s, &mut ss, server, sc, sh, late, &second, sl, &sa, &mut out,
            )
            .unwrap();
            assert_eq!(new.action, RpcAction::Invoke);
            assert_eq!(&out[..new.bytes], b"next");
            // Different plaintext with an already accepted operation is never an update.
            let conflict = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                15,
                11,
                1,
                0,
                b"changed",
            );
            assert_eq!(
                request(
                    &mut s, &mut ss, server, sc, sh, late, &conflict, sl, &sa, &mut out
                ),
                Err(Error::Replay)
            );
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // NOT_SUBMITTED, no-renewal deadline, Driver unknown and stop retirement.
fn backpressure_keeps_exact_request_until_deadline_and_unknown_driver_keeps_holder() {
    security::exercise(1, 2, |client, ch, cf, cc| {
        let ci = AdapterEventIngress::<1, 1, 1, 128>::new(1).unwrap();
        let mut ca = AdapterOwner::new(1, &ci).unwrap();
        let cl = ca.open_link(0, 1, 128).unwrap();
        let mut c = bridge(2, false);
        let mut cs = Service::new(1, 100).unwrap();
        let mut seq = SequenceCounter(10);
        let h = begin(&mut c, &mut cs, client, cc, &mut seq, ch, cf, cl, &ca).unwrap();
        let original = packet(&c, h, cf);
        let mut d = Driver::new(DriverSubmit::NotSubmitted);
        for now_us in [21, 31, 41] {
            c.submit(
                h,
                &mut cs,
                client,
                ch,
                CurrentFacts { now_us, ..cf },
                &mut ca,
                &mut d,
            )
            .unwrap();
            assert_eq!(c.view(h).unwrap().phase, RpcPacketPhase::Ready);
            assert_eq!(
                cs.view(c.view(h).unwrap().service).unwrap().phase,
                RequestPhase::WaitSend
            );
        }
        d.submit = DriverSubmit::Submitted;
        c.submit(
            h,
            &mut cs,
            client,
            ch,
            CurrentFacts { now_us: 51, ..cf },
            &mut ca,
            &mut d,
        )
        .unwrap();
        assert_eq!(
            cs.view(c.view(h).unwrap().service).unwrap().phase,
            RequestPhase::WaitResult
        );
        assert!(
            d.frames
                .iter()
                .all(|p| p == &original.bytes[..original.length])
        );
        assert_eq!(seq.0, 11);
        c.stop();
        d.cancel = DriverCancel::Unknown;
        assert_eq!(c.cancel_attempt(h, &mut ca, &mut d), Err(Error::InDoubt));
        assert!(c.view(h).unwrap().physical_pending);
        assert_eq!(c.retire(h, &mut cs, 122), Err(Error::State));
        ca.reopen_link(cl, 2, 128).unwrap();
        ci.tx_complete(d.tokens[3], TerminalOutcome::Success)
            .unwrap();
        c.retire_attempt(h, &mut ca).unwrap();
        cs.step(121, 1).unwrap();
        c.retire(h, &mut cs, 122).unwrap();

        let ci = AdapterEventIngress::<1, 1, 1, 128>::new(3).unwrap();
        let mut ca = AdapterOwner::new(3, &ci).unwrap();
        let cl = ca.open_link(0, 1, 128).unwrap();
        let mut c = bridge(2, false);
        let mut cs = Service::new(3, 100).unwrap();
        let h = begin(&mut c, &mut cs, client, cc, &mut seq, ch, cf, cl, &ca).unwrap();
        d.submit = DriverSubmit::NotSubmitted;
        c.submit(h, &mut cs, client, ch, cf, &mut ca, &mut d)
            .unwrap();
        assert_eq!(
            c.submit(
                h,
                &mut cs,
                client,
                ch,
                CurrentFacts { now_us: 121, ..cf },
                &mut ca,
                &mut d
            ),
            Err(Error::State)
        );
        assert_eq!(c.view(h).unwrap().deadline, 121);
        cs.step(121, 1).unwrap();
        c.retire(h, &mut cs, 121).unwrap();
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Role/status, full context, before-submit and immutable duplicate results.
fn error_result_is_exact_and_wrong_link_operation_role_or_content_never_completes() {
    security::exercise(1, 2, |client, ch, cf, cc| {
        security::exercise(2, 1, |server, sh, sf, sc| {
            let ci = AdapterEventIngress::<1, 1, 1, 128>::new(1).unwrap();
            let mut ca = AdapterOwner::new(1, &ci).unwrap();
            let cl = ca.open_link(0, 1, 128).unwrap();
            let mut c = bridge(2, false);
            let mut cs = Service::new(1, 100).unwrap();
            let mut seq = SequenceCounter(10);
            let h = begin(&mut c, &mut cs, client, cc, &mut seq, ch, cf, cl, &ca).unwrap();
            let good = custom(
                server,
                sc,
                sh,
                sf,
                InteractionRole::Error,
                20,
                10,
                5,
                65535,
                b"failed",
            );
            assert_eq!(
                result(&mut c, h, &mut cs, client, cc, ch, cf, &good, cl),
                Err(Error::State)
            );
            let mut d = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            c.submit(h, &mut cs, client, ch, cf, &mut ca, &mut d)
                .unwrap();
            c.retire_attempt(h, &mut ca).unwrap();
            let wrong = custom(
                server,
                sc,
                sh,
                sf,
                InteractionRole::Result,
                21,
                11,
                5,
                7,
                b"wrong",
            );
            assert_eq!(
                result(&mut c, h, &mut cs, client, cc, ch, cf, &wrong, cl),
                Err(Error::Access)
            );
            let other_i = AdapterEventIngress::<1, 1, 1, 128>::new(9).unwrap();
            let mut other_a = AdapterOwner::new(9, &other_i).unwrap();
            let other_link = other_a.open_link(0, 1, 128).unwrap();
            assert_eq!(
                result(&mut c, h, &mut cs, client, cc, ch, cf, &good, other_link),
                Err(Error::State)
            );
            let mut bad = FragmentPacket {
                bytes: good.bytes,
                ..good
            };
            bad.bytes[bad.length - 1] ^= 1;
            assert!(result(&mut c, h, &mut cs, client, cc, ch, cf, &bad, cl).is_err());
            assert!(result(&mut c, h, &mut cs, client, cc, ch, cf, &good, cl).unwrap());
            let view = cs.view(c.view(h).unwrap().service).unwrap();
            assert_eq!(view.result, -65536);
            let conflict = custom(
                server,
                sc,
                sh,
                sf,
                InteractionRole::Error,
                22,
                10,
                5,
                65535,
                b"changed",
            );
            assert_eq!(
                result(&mut c, h, &mut cs, client, cc, ch, cf, &conflict, cl),
                Err(Error::Replay)
            );
            assert_eq!(cs.view(c.view(h).unwrap().service).unwrap(), view);
            assert!(!result(&mut c, h, &mut cs, client, cc, ch, cf, &good, cl).unwrap());
        });
    });
}

#[test]
fn plain_data_acl_never_implies_rpc_permission_and_response_capacity_is_reserved_before_invoke() {
    security::exercise(1, 2, |client, ch, cf, cc| {
        security::exercise(2, 1, |server, sh, sf, sc| {
            let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
            let mut sa = AdapterOwner::new(2, &si).unwrap();
            let sl = sa.open_link(0, 1, 128).unwrap();
            let p = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                10,
                10,
                1,
                0,
                b"request",
            );
            let mut out = [0xa5; 16];
            for policy in [
                endpoint(1, false, true),
                endpoint(9, true, false),
                RpcEndpoint::new(
                    ServiceId::new(0x40).unwrap(),
                    [1; 16],
                    true,
                    false,
                    16,
                    OriginSecurity::O2,
                )
                .unwrap(),
            ] {
                let mut denied = Rpc::new(3, policy, 10, 4).unwrap();
                let mut ss = Service::new(2, 100).unwrap();
                assert_eq!(
                    request(
                        &mut denied,
                        &mut ss,
                        server,
                        sc,
                        sh,
                        sf,
                        &p,
                        sl,
                        &sa,
                        &mut out
                    ),
                    Err(Error::Access)
                );
                assert_eq!(out, [0xa5; 16]);
            }
            let mut s = bridge(1, true);
            let mut ss = Service::new(2, 100).unwrap();
            // Request is 45B, but the allowed 16B response requires 56B.
            let tiny_i = AdapterEventIngress::<1, 1, 1, 128>::new(3).unwrap();
            let mut tiny = AdapterOwner::new(3, &tiny_i).unwrap();
            let tiny_l = tiny.open_link(0, 1, 45).unwrap();
            assert_eq!(
                request(
                    &mut s, &mut ss, server, sc, sh, sf, &p, tiny_l, &tiny, &mut out
                ),
                Err(Error::NoSpace)
            );
            assert_eq!(out, [0xa5; 16]);
            // Same original Sequence is still admissible; failures have not consumed Replay.
            let got = request(&mut s, &mut ss, server, sc, sh, sf, &p, sl, &sa, &mut out).unwrap();
            assert_eq!(got.action, RpcAction::Invoke);
            let before = ss.view(s.view(got.handle).unwrap().service).unwrap();
            assert_eq!(
                s.respond(
                    got.handle,
                    &mut ss,
                    server,
                    sh,
                    CurrentFacts { now_us: 121, ..sf },
                    InteractionRole::Result,
                    0,
                    b"late"
                ),
                Err(Error::State)
            );
            assert_eq!(
                ss.view(s.view(got.handle).unwrap().service).unwrap(),
                before
            );
        });
    });
}
