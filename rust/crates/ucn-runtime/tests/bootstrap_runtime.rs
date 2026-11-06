#![cfg(feature = "bootstrap-runtime")]
//! Raw atomic Adapter and stateless Host HMAC Cookie, never a fake admitted DTO.
//! 真实原子 Adapter 与无状态 Host HMAC Cookie，不伪造已准入 DTO。
use ucn_adapter::*;
use ucn_admission::*;
use ucn_digest::{DigestWorkspace, sha256};
use ucn_identity::Principal;
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_runtime::*;
use ucn_types::*;
#[cfg(feature = "bootstrap-join")]
#[path = "bootstrap_runtime/join_tests.rs"]
mod join_tests;
type Ingress = AdapterEventIngress<2, 1, 1, 128>;
type Adapter<'a> = AdapterOwner<'a, 2, 1, 1, 128>;
struct Driver {
    #[cfg(feature = "route-data-runtime")]
    peer_locators: Vec<u32>,
    #[cfg(feature = "route-data-runtime")]
    data_submit: Option<DriverSubmit>,
    frames: Vec<Vec<u8>>,
    tokens: Vec<TxToken>,
    submit: DriverSubmit,
    cancel: DriverCancel,
    fragment_submit: Option<DriverSubmit>,
    last_submit: DriverSubmit,
}
impl Driver {
    fn new(submit: DriverSubmit) -> Self {
        Self {
            #[cfg(feature = "route-data-runtime")]
            peer_locators: vec![],
            #[cfg(feature = "route-data-runtime")]
            data_submit: None,
            frames: vec![],
            tokens: vec![],
            submit,
            cancel: DriverCancel::NotCancelled,
            fragment_submit: None,
            last_submit: submit,
        }
    }
}
impl TxDriver for Driver {
    fn submit(&mut self, _: LinkHandle, bytes: &[u8], token: TxToken) -> DriverSubmit {
        self.frames.push(bytes.to_vec());
        self.tokens.push(token);
        self.last_submit = if bytes.len() > 34
            && bytes[27..29] == [1, 8]
            && bytes[29..31] == [1, 3]
            && bytes[33] == 1
        {
            self.fragment_submit.take().unwrap_or(self.submit)
        } else {
            self.submit
        };
        self.last_submit
    }
    fn cancel(&mut self, _: TxToken) -> DriverCancel {
        self.cancel
    }
}
#[cfg(feature = "route-data-runtime")]
impl PeerTxDriver for Driver {
    fn submit_peer(
        &mut self,
        link: LinkHandle,
        peer: u32,
        bytes: &[u8],
        token: TxToken,
    ) -> DriverSubmit {
        self.peer_locators.push(peer);
        let ordinary = self.submit;
        if ucn_wire::CommonHeader::decode(bytes).is_ok_and(|h| h.contract == HeaderContract::C1) {
            self.submit = self.data_submit.unwrap_or(ordinary);
        }
        let result = self.submit(link, bytes, token);
        self.submit = ordinary;
        result
    }
}
struct Cookies {
    issued: usize,
    verified: usize,
    events: usize,
}
impl Cookies {
    fn new() -> Self {
        Self {
            issued: 0,
            verified: 0,
            events: 0,
        }
    }
    fn mac(hello: Hello, link: LinkIdentity, bucket: u32) -> [u8; 12] {
        let mut bytes = [0; 160];
        let mut h = [0; 40];
        encode_hello(hello, &mut h).unwrap();
        bytes[..64].fill(0x41 ^ 0x36);
        bytes[64..104].copy_from_slice(&h);
        bytes[104..108].copy_from_slice(&1u32.to_be_bytes());
        bytes[108] = 1;
        bytes[109..111].copy_from_slice(&link.link_id.to_be_bytes());
        bytes[111..115].copy_from_slice(&link.link_generation.to_be_bytes());
        bytes[115..119].copy_from_slice(&bucket.to_be_bytes());
        let inner = sha256(&bytes[..119], &mut DigestWorkspace::new());
        bytes[..64].fill(0x41 ^ 0x5c);
        bytes[64..96].copy_from_slice(&inner);
        sha256(&bytes[..96], &mut DigestWorkspace::new())[..12]
            .try_into()
            .unwrap()
    }
}
impl CookieProvider for Cookies {
    fn issue_cookie(
        &mut self,
        h: &Hello,
        l: LinkIdentity,
        b: u32,
        out: &mut [u8; 16],
    ) -> Result<u8> {
        self.issued += 1;
        out[..4].copy_from_slice(&b.to_be_bytes());
        out[4..].copy_from_slice(&Self::mac(*h, l, b));
        Ok(16)
    }
    fn verify_cookie(&mut self, v: &HelloCookie, key: &AdmissionKey, now: u64) -> Result<()> {
        self.verified += 1;
        let c = v.cookie_evidence.bytes();
        if c.len() != 16
            || key.link.link_id != v.selected_link_instance_id
            || key.link.link_generation != v.selected_link_instance_generation
        {
            return Err(Error::Security);
        }
        let bucket = u32::from_be_bytes(c[..4].try_into().unwrap());
        let current = now / 1_000_000 + 1;
        if u64::from(bucket) > current || current - u64::from(bucket) > 1 {
            return Err(Error::Timeout);
        }
        let hello = Hello {
            flow: BootstrapFlow::Join,
            identity_digest: v.identity_digest,
            device_nonce: v.device_nonce,
            transaction_id: v.transaction_id,
        };
        if c[4..] != Self::mac(hello, key.link, bucket) {
            return Err(Error::Security);
        }
        let challenge = CookieChallenge {
            flow: BootstrapFlow::Join,
            transaction_id: v.transaction_id,
            cookie_time_bucket: bucket,
            issuer_link_id: key.link.link_id,
            issuer_link_generation: key.link.link_generation,
            cookie_length: 16,
            cookie: c.try_into().unwrap(),
        };
        if bootstrap_prior_hash(
            hello,
            challenge,
            RealmId::new(1)?,
            AddressWidth::A1,
            &mut DigestWorkspace::new(),
        )? != v.prior_messages_hash
        {
            return Err(Error::Security);
        }
        Ok(())
    }
    fn authorize_event(
        &mut self,
        _: BootstrapEvent,
        _: &AdmissionKey,
        _: &BootstrapTranscript,
        _: u64,
        _: &Evidence,
    ) -> Result<()> {
        self.events += 1;
        Err(Error::Unsupported)
    }
}
fn hello() -> Hello {
    Hello {
        flow: BootstrapFlow::Join,
        identity_digest: Principal::new([1; 16]).unwrap(),
        device_nonce: 2,
        transaction_id: 3,
    }
}
fn config(gate: &CallbackGate, link: LinkHandle, owner: u32) -> BootstrapConfig<'_> {
    BootstrapConfig {
        owner,
        realm: RealmId::new(1).unwrap(),
        width: AddressWidth::A1,
        link,
        gate,
        lifetime_us: 1000,
        retry_us: 10,
        max_attempts: 20,
    }
}
fn admission(gate: &CallbackGate) -> AdmissionConfig<'_> {
    AdmissionConfig {
        runtime_instance: 1,
        owner_instance: 4,
        identity_owner_instance: 5,
        provider_gate: gate,
        max_pending_per_link: 1,
        token_burst: 8,
        tokens_per_second: 8,
        pending_timeout_us: 2000,
    }
}
fn publish<const B: usize>(
    i: &AdapterEventIngress<2, 1, 1, B>,
    l: LinkHandle,
    bytes: &[u8],
    now: u64,
    disc: u32,
) {
    i.rx_publish(
        l,
        bytes,
        RxMeta {
            timestamp_us: now,
            sender_discriminator: disc,
        },
    )
    .unwrap();
}

struct SignedEvents(Cookies);
impl SignedEvents {
    fn sign(event: BootstrapEvent, t: &BootstrapTranscript) -> Evidence {
        let phase = match event {
            BootstrapEvent::AuthorityProof => BootstrapPhase::AuthorityVerified,
            BootstrapEvent::DeviceProof => BootstrapPhase::DeviceVerified,
            BootstrapEvent::AddressOffer => BootstrapPhase::AddressOffered,
            BootstrapEvent::DeviceCommit => BootstrapPhase::DeviceCommitted,
            BootstrapEvent::FinalDurable => BootstrapPhase::FinalDurable,
            _ => panic!(),
        };
        let key = if matches!(
            event,
            BootstrapEvent::AuthorityProof
                | BootstrapEvent::AddressOffer
                | BootstrapEvent::FinalDurable
        ) {
            0x71
        } else {
            0x72
        };
        let mut bytes = [0; 64 + 2 + TRANSCRIPT_BYTES];
        bytes[..64].fill(key ^ 0x36);
        bytes[64..66].copy_from_slice(&event.opcode().unwrap().to_be_bytes());
        encode_transcript(t, phase, &mut bytes[66..]).unwrap();
        let inner = sha256(&bytes, &mut DigestWorkspace::new());
        bytes[..64].fill(key ^ 0x5c);
        bytes[64..96].copy_from_slice(&inner);
        Evidence::new(&sha256(&bytes[..96], &mut DigestWorkspace::new())).unwrap()
    }
}
impl CookieProvider for SignedEvents {
    fn issue_cookie(
        &mut self,
        h: &Hello,
        l: LinkIdentity,
        b: u32,
        out: &mut [u8; 16],
    ) -> Result<u8> {
        self.0.issue_cookie(h, l, b, out)
    }
    fn verify_cookie(&mut self, v: &HelloCookie, k: &AdmissionKey, n: u64) -> Result<()> {
        self.0.verify_cookie(v, k, n)
    }
    fn authorize_event(
        &mut self,
        event: BootstrapEvent,
        key: &AdmissionKey,
        transcript: &BootstrapTranscript,
        now: u64,
        proof: &Evidence,
    ) -> Result<()> {
        self.0.events += 1;
        if now == 0
            || transcript.authority_principal != [0x41; 16]
            || key.identity_digest.bytes() != [1; 16]
            || (matches!(
                event,
                BootstrapEvent::DeviceProof | BootstrapEvent::DeviceCommit
            ) && transcript.joining_device_principal != [0x11; 16])
            || !matches!(
                event,
                BootstrapEvent::AuthorityProof
                    | BootstrapEvent::DeviceProof
                    | BootstrapEvent::AddressOffer
                    | BootstrapEvent::DeviceCommit
                    | BootstrapEvent::FinalDurable
            )
            || *proof != Self::sign(event, transcript)
        {
            return Err(Error::Security);
        }
        Ok(())
    }
}
fn authority_transcript(mut t: BootstrapTranscript) -> BootstrapTranscript {
    t.authority_principal = [0x41; 16];
    t.authority_generation = 1;
    t.authority_nonce = 8;
    t.realm_id = 1;
    t.authority_address = 1;
    t.authority_binding_generation = 1;
    t.authority_lease_sequence = 1;
    t.authority_lease_duration_us = 10_000;
    t.freshness_max_remaining_lease_us = 8_000;
    t.durable_fence_token = [0x42; 16];
    t.allocation_high_water_digest = [0x43; 16];
    t.quorum_config_digest = [0x44; 32];
    t.signer_set_digest = [0x45; 32];
    t.threshold_proof_digest = [0x46; 32];
    t.freshness_proof_transcript_hash = [0x47; 32];
    t.authority_signer_count = 3;
    t.authority_quorum_threshold = 2;
    t
}
fn device_transcript(mut t: BootstrapTranscript) -> BootstrapTranscript {
    t.joining_device_principal = [0x11; 16];
    t.selected_hop_suite = 1;
    t.selected_hop_key_id = 1;
    t.selected_hop_key_generation = 1;
    t.selected_e2e_mode = 1;
    t.selected_e2e_suite = 1;
    t.selected_e2e_key_id = 1;
    t.selected_e2e_key_generation = 1;
    t.selected_session_generation = 1;
    t
}

#[test]
#[allow(clippy::too_many_lines)]
fn actual_mixed_mtu_mutual_auth_retries_one_lost_fragment_without_admission() {
    let gate = CallbackGate::new(1).unwrap();
    let si = AdapterEventIngress::<2, 1, 1, 256>::new(1).unwrap();
    let li = AdapterEventIngress::<2, 1, 1, 256>::new(2).unwrap();
    let mut sa = AdapterOwner::<2, 1, 1, 256>::new(1, &si).unwrap();
    let mut la = AdapterOwner::<2, 1, 1, 256>::new(2, &li).unwrap();
    let sl = sa.open_link(0, 3, 127).unwrap();
    let ll = la.open_link(1, 9, 256).unwrap();
    sa.try_set_rx_enabled(true).unwrap();
    la.try_set_rx_enabled(true).unwrap();
    let mut client = BootstrapClient::new(config(&gate, sl, 10), &sa, hello(), 7, 90, 21).unwrap();
    let mut listener =
        BootstrapListener::new(config(&gate, ll, 20), admission(&gate), &la, 1_000_000, 21)
            .unwrap();
    let mut provider = SignedEvents(Cookies::new());
    let mut sd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
    let mut ld = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
    client.start(&sa, 21).unwrap();
    client.drive(&mut sa, &mut sd, 21).unwrap();
    publish(&li, ll, &sd.frames[0], 21, 31);
    listener.receive(&mut la, &mut provider, 21).unwrap();
    listener.drive(&mut la, &mut ld, 22).unwrap();
    publish(&si, sl, &ld.frames[0], 22, 90);
    client.receive(&mut sa, 22).unwrap();
    client.retire(&mut sa, &mut sd, 22).unwrap();
    client.drive(&mut sa, &mut sd, 23).unwrap();
    publish(&li, ll, &sd.frames[1], 23, 31);
    listener.receive(&mut la, &mut provider, 23).unwrap();
    // Even synchronous completion must be explicitly retired before borrowing the phase.
    assert!(matches!(
        client.auth_exchange(&sa, [0x41; 16], 23),
        Err(Error::State)
    ));
    assert_eq!(client.phase(), BootstrapClientPhase::Echoing);
    client.retire(&mut sa, &mut sd, 24).unwrap();
    listener.retire(&mut la, &mut ld, 24).unwrap();
    let mut ca = client.auth_exchange(&sa, [0x41; 16], 25).unwrap();
    let mut aa = listener.auth_exchange(&la, [0x41; 16], 25).unwrap();
    let authority = authority_transcript(ca.transcript());
    let bad = Evidence::new(&[7; 32]).unwrap();
    assert_eq!(
        aa.queue(
            &la,
            &mut provider,
            BootstrapEvent::AuthorityProof,
            authority,
            bad,
            25
        ),
        Err(Error::Security)
    );
    assert_eq!(aa.phase(), BootstrapPhase::CookieVerified);
    let ap = SignedEvents::sign(BootstrapEvent::AuthorityProof, &authority);
    aa.queue(
        &la,
        &mut provider,
        BootstrapEvent::AuthorityProof,
        authority,
        ap,
        26,
    )
    .unwrap();
    let mut n = 27;
    let fake_key = AdmissionKey {
        link: LinkIdentity {
            link_id: 2,
            link_generation: 9,
        },
        local_peer_discriminator: 31,
        identity_digest: hello().identity_digest,
        transaction_id: hello().transaction_id,
    };
    // A completed but invalid event must not poison the next complete valid retransmission.
    for bad_nonce in [true, false] {
        let mut forged = authority;
        if bad_nonce {
            forged.device_nonce += 1;
        }
        let proof = if bad_nonce {
            SignedEvents::sign(BootstrapEvent::AuthorityProof, &forged)
        } else {
            bad
        };
        let mut logical = [0; 508];
        let length = encode_logical_event(
            BootstrapEvent::AuthorityProof,
            BootstrapPhase::AuthorityVerified,
            &forged,
            proof,
            &mut logical,
        )
        .unwrap();
        for index in 0..7 {
            let fragment = BootstrapFragment::from_logical_chunk(
                fake_key,
                BootstrapFlow::Join,
                BootstrapEvent::AuthorityProof,
                BootstrapPhase::AuthorityVerified,
                index,
                62,
                &logical[..length],
            )
            .unwrap();
            let mut packet = [0; 128];
            let size = encode_bootstrap_event_frame(
                BootstrapEvent::AuthorityProof,
                fragment,
                RealmId::new(1).unwrap(),
                AddressWidth::A1,
                &mut packet,
            )
            .unwrap();
            if index == 0 {
                publish(&si, sl, &packet[..size], n, 91);
                assert_eq!(ca.receive(&mut sa, &mut provider, n), Err(Error::Security));
                packet[3] ^= 1;
                publish(&si, sl, &packet[..size], n, 90);
                assert_eq!(ca.receive(&mut sa, &mut provider, n), Err(Error::Malformed));
                packet[3] ^= 1;
            }
            publish(&si, sl, &packet[..size], n, 90);
            let result = ca.receive(&mut sa, &mut provider, n);
            assert_eq!(
                result,
                if index == 6 {
                    Err(Error::Security)
                } else {
                    Ok(false)
                }
            );
            assert_eq!(ca.phase(), BootstrapPhase::CookieVerified);
            n += 1;
        }
    }
    let mut dropped = false;
    let mut auth_packets = 0;
    while ca.phase() == BootstrapPhase::CookieVerified && n < 300 {
        if aa.drive(&mut la, &mut ld, n).unwrap() {
            let packet = ld.frames.last().unwrap();
            assert!(packet.len() <= 128);
            let (_, f) =
                decode_bootstrap_event_frame(packet, RealmId::new(1).unwrap(), AddressWidth::A1)
                    .unwrap();
            assert_eq!(f.chunk_bytes, 62);
            auth_packets += 1;
            if f.fragment_index == 1 && !dropped {
                dropped = true;
            } else {
                publish(&si, sl, packet, n, 90);
                ca.receive(&mut sa, &mut provider, n).unwrap();
            }
        }
        aa.retire(&mut la, &mut ld, n).unwrap();
        n += 1;
    }
    assert!(dropped && auth_packets > 7);
    assert_eq!(ca.phase(), BootstrapPhase::AuthorityVerified);
    let device = device_transcript(ca.transcript());
    let dp = SignedEvents::sign(BootstrapEvent::DeviceProof, &device);
    // New phases are not accepted through a caller-provided bool or FinalDurable frame.
    assert_eq!(
        ca.queue(
            &sa,
            &mut provider,
            BootstrapEvent::FinalDurable,
            device,
            dp,
            n
        ),
        Err(Error::State)
    );
    ca.queue(
        &sa,
        &mut provider,
        BootstrapEvent::DeviceProof,
        device,
        dp,
        n,
    )
    .unwrap();
    while aa.phase() != BootstrapPhase::DeviceVerified && n < 600 {
        if ca.drive(&mut sa, &mut sd, n).unwrap() {
            let packet = sd.frames.last().unwrap();
            assert!(packet.len() <= 128);
            publish(&li, ll, packet, n, 31);
            aa.receive(&mut la, &mut provider, n).unwrap();
        }
        ca.retire(&mut sa, &mut sd, n).unwrap();
        n += 1;
    }
    assert_eq!(aa.phase(), BootstrapPhase::DeviceVerified);
    assert_eq!(aa.transcript(), ca.transcript());
    // Failed local proof + forged incoming MAC + four successes; exact replay is free of crypto.
    assert_eq!(provider.0.events, 6);
    let duplicate = sd.frames.last().unwrap().clone();
    publish(&li, ll, &duplicate, n, 31);
    assert!(!aa.receive(&mut la, &mut provider, n).unwrap());
    assert_eq!(provider.0.events, 6);
    ca.stop(&mut sa).unwrap();
    aa.stop(&mut la).unwrap();
    ca.retire(&mut sa, &mut sd, n).unwrap();
    aa.retire(&mut la, &mut ld, n).unwrap();
    assert_eq!(client.phase(), BootstrapClientPhase::AuthEvents);
    assert_eq!(client.drive(&mut sa, &mut sd, n), Err(Error::State));
    client.stop(&mut sa).unwrap();
    listener.stop(&mut la).unwrap();
    listener.retire(&mut la, &mut ld, n).unwrap();
    assert!(client.quiescent(&sa) && listener.quiescent(&la));
}
#[test]
#[allow(clippy::too_many_lines)]
fn different_link_ids_raw_loss_and_early_cookie_keep_original_holder_and_deadline() {
    let gate = CallbackGate::new(1).unwrap();
    let si = Ingress::new(1).unwrap();
    let li = Ingress::new(2).unwrap();
    let mut sa = Adapter::new(1, &si).unwrap();
    let mut la = Adapter::new(2, &li).unwrap();
    let sl = sa.open_link(0, 3, 128).unwrap();
    let ll = la.open_link(1, 9, 128).unwrap();
    sa.try_set_rx_enabled(true).unwrap();
    la.try_set_rx_enabled(true).unwrap();
    let start = 21;
    let mut client =
        BootstrapClient::new(config(&gate, sl, 10), &sa, hello(), 7, 90, start).unwrap();
    let mut listener = BootstrapListener::new(
        config(&gate, ll, 20),
        admission(&gate),
        &la,
        1_000_000,
        start,
    )
    .unwrap();
    let mut provider = Cookies::new();
    let mut sd = Driver::new(DriverSubmit::Submitted);
    let mut ld = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
    client.start(&sa, start).unwrap();
    client.drive(&mut sa, &mut sd, start).unwrap();
    let token = sd.tokens[0];
    let original = sd.frames[0].clone();
    publish(&li, ll, &original, start, 31);
    listener.receive(&mut la, &mut provider, start).unwrap();
    assert_eq!(listener.pending(start), Err(Error::NotFound));
    assert_eq!(provider.issued, 1);
    listener.drive(&mut la, &mut ld, start + 1).unwrap();
    let challenge = ld.frames[0].clone();
    // First challenge is lost; a duplicate HELLO arrives while its reply token is held.
    publish(&li, ll, &original, start + 2, 31);
    listener.receive(&mut la, &mut provider, start + 2).unwrap();
    listener.retire(&mut la, &mut ld, start + 2).unwrap();
    assert!(!listener.drive(&mut la, &mut ld, start + 3).unwrap());
    assert!(listener.drive(&mut la, &mut ld, start + 11).unwrap());
    assert_eq!(ld.frames[1], challenge);
    publish(&si, sl, &challenge, start + 11, 90);
    client.receive(&mut sa, start + 11).unwrap();
    assert_eq!(client.phase(), BootstrapClientPhase::AwaitCookie);
    assert!(!client.drive(&mut sa, &mut sd, start + 12).unwrap());
    assert_eq!(sa.tx_view(token).unwrap().state, TxState::Submitted);
    si.tx_complete(token, TerminalOutcome::Success).unwrap();
    client.retire(&mut sa, &mut sd, start + 13).unwrap();
    sd.submit = DriverSubmit::NotSubmitted;
    assert!(client.drive(&mut sa, &mut sd, start + 13).unwrap());
    assert_eq!(client.phase(), BootstrapClientPhase::Echoing);
    let echo = sd.frames[1].clone();
    assert_eq!(echo.len(), 127);
    let BootstrapMessage::Echo(v) =
        decode_bootstrap_frame(&echo, RealmId::new(1).unwrap(), AddressWidth::A1).unwrap()
    else {
        panic!()
    };
    assert_eq!(
        (
            v.selected_link_instance_id,
            v.selected_link_instance_generation
        ),
        (2, 9)
    );
    sd.submit = DriverSubmit::Complete(TerminalOutcome::Success);
    assert!(client.drive(&mut sa, &mut sd, start + 23).unwrap());
    assert_eq!(sd.frames[2], echo);
    publish(&li, ll, &echo, start + 23, 31);
    listener
        .receive(&mut la, &mut provider, start + 23)
        .unwrap();
    let pending = listener.pending(start + 23).unwrap();
    assert_eq!(pending.phase, BootstrapPhase::CookieVerified);
    assert_eq!(
        pending.key.link,
        LinkIdentity {
            link_id: 2,
            link_generation: 9
        }
    );
    publish(&li, ll, &echo, start + 24, 31);
    listener
        .receive(&mut la, &mut provider, start + 24)
        .unwrap();
    assert_eq!(
        listener.pending(start + 24).unwrap().deadline_us,
        pending.deadline_us
    );
    assert_eq!(provider.issued, 1);
    assert_eq!(provider.events, 0);
    assert_eq!(
        client.drive(&mut sa, &mut sd, start + 1000),
        Err(Error::Timeout)
    );
    listener.step(pending.deadline_us).unwrap();
    assert_eq!(listener.pending(pending.deadline_us), Err(Error::Timeout));
    client.stop(&mut sa).unwrap();
    listener.stop(&mut la).unwrap();
    client
        .retire(&mut sa, &mut sd, pending.deadline_us)
        .unwrap();
    listener
        .retire(&mut la, &mut ld, pending.deadline_us)
        .unwrap();
    assert!(client.quiescent(&sa) && listener.quiescent(&la));
}
#[test]
#[allow(clippy::too_many_lines)]
fn stateless_cookie_freshness_reopen_and_crypto_budget_are_not_admission() {
    let gate = CallbackGate::new(1).unwrap();
    let i = Ingress::new(1).unwrap();
    let mut a = Adapter::new(1, &i).unwrap();
    let l = a.open_link(1, 9, 128).unwrap();
    a.try_set_rx_enabled(true).unwrap();
    let now = 21;
    let mut listener =
        BootstrapListener::new(config(&gate, l, 10), admission(&gate), &a, 1_000_000, now).unwrap();
    let mut provider = Cookies::new();
    let mut cookie = [0; 16];
    provider
        .issue_cookie(
            &hello(),
            LinkIdentity {
                link_id: 2,
                link_generation: 9,
            },
            1,
            &mut cookie,
        )
        .unwrap();
    let challenge = CookieChallenge {
        flow: BootstrapFlow::Join,
        transaction_id: 3,
        cookie_time_bucket: 1,
        issuer_link_id: 2,
        issuer_link_generation: 9,
        cookie_length: 16,
        cookie,
    };
    let echo = HelloCookie {
        flow: BootstrapFlow::Join,
        identity_digest: hello().identity_digest,
        device_nonce: 2,
        transaction_id: 3,
        lease_freshness_challenge_nonce: 7,
        selected_link_instance_id: 2,
        selected_link_instance_generation: 9,
        prior_messages_hash: bootstrap_prior_hash(
            hello(),
            challenge,
            RealmId::new(1).unwrap(),
            AddressWidth::A1,
            &mut DigestWorkspace::new(),
        )
        .unwrap(),
        cookie_evidence: Cookie::new(&cookie).unwrap(),
    };
    let mut bytes = [0; 128];
    let mut bad = echo;
    bad.selected_link_instance_generation = 8;
    let n = encode_bootstrap_frame(
        BootstrapMessage::Echo(bad),
        RealmId::new(1).unwrap(),
        AddressWidth::A1,
        &mut bytes,
    )
    .unwrap();
    publish(&i, l, &bytes[..n], now, 31);
    assert_eq!(
        listener.receive(&mut a, &mut provider, now),
        Err(Error::Security)
    );
    assert_eq!(provider.verified, 0);
    bad = echo;
    bad.prior_messages_hash[0] ^= 1;
    let n = encode_bootstrap_frame(
        BootstrapMessage::Echo(bad),
        RealmId::new(1).unwrap(),
        AddressWidth::A1,
        &mut bytes,
    )
    .unwrap();
    publish(&i, l, &bytes[..n], now, 31);
    assert_eq!(
        listener.receive(&mut a, &mut provider, now),
        Err(Error::Security)
    );
    assert_eq!(listener.pending(now), Err(Error::NotFound));
    let n = encode_bootstrap_frame(
        BootstrapMessage::Echo(echo),
        RealmId::new(1).unwrap(),
        AddressWidth::A1,
        &mut bytes,
    )
    .unwrap();
    publish(&i, l, &bytes[..n], now, 31);
    listener.receive(&mut a, &mut provider, now).unwrap();
    // This listener never received HELLO: validity comes from the stateless Cookie verifier.
    assert_eq!(
        listener.pending(now).unwrap().phase,
        BootstrapPhase::CookieVerified
    );
    for _ in 0..6 {
        publish(&i, l, &bytes[..n], now, 31);
        listener.receive(&mut a, &mut provider, now).unwrap();
    }
    let calls = provider.verified;
    publish(&i, l, &bytes[..n], now, 31);
    assert_eq!(
        listener.receive(&mut a, &mut provider, now),
        Err(Error::Access)
    );
    assert_eq!(provider.verified, calls);
    assert_eq!(provider.events, 0);
    listener.stop(&mut a).unwrap();
    let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
    listener.retire(&mut a, &mut driver, now).unwrap();
    // A new listener has no cached HELLO; old Cookie expiry is checked with trusted now.
    let mut late = BootstrapListener::new(
        config(&gate, l, 11),
        admission(&gate),
        &a,
        1_000_000,
        3_000_001,
    )
    .unwrap();
    a.try_set_rx_enabled(true).unwrap();
    publish(&i, l, &bytes[..n], 3_000_001, 31);
    assert_eq!(
        late.receive(&mut a, &mut provider, 3_000_001),
        Err(Error::Timeout)
    );
    assert_eq!(late.pending(3_000_001), Err(Error::NotFound));
    let lease = gate
        .try_enter(CallbackClaim::new(99, 1, 1, 1).unwrap())
        .unwrap();
    assert_eq!(late.stop(&mut a), Err(Error::State));
    gate.leave(lease).unwrap();
    let new_link = a.reopen_link(l, 10, 128).unwrap();
    let calls = provider.verified;
    assert_eq!(
        late.receive(&mut a, &mut provider, 3_000_002),
        Err(Error::NotFound)
    );
    assert_eq!(late.stop(&mut a), Err(Error::NotFound));
    assert_eq!(provider.verified, calls);
    let mut reopened = BootstrapListener::new(
        config(&gate, new_link, 12),
        admission(&gate),
        &a,
        1_000_000,
        3_000_002,
    )
    .unwrap();
    publish(&i, new_link, &bytes[..n], 3_000_002, 31);
    assert_eq!(
        reopened.receive(&mut a, &mut provider, 3_000_002),
        Err(Error::Security)
    );
    assert_eq!(provider.verified, calls);
}
#[test]
fn challenge_before_actual_hello_submit_and_different_gate_do_not_progress() {
    let gate = CallbackGate::new(1).unwrap();
    let other = CallbackGate::new(2).unwrap();
    let ingress = Ingress::new(1).unwrap();
    let mut adapter = Adapter::new(1, &ingress).unwrap();
    let link = adapter.open_link(0, 1, 128).unwrap();
    adapter.try_set_rx_enabled(true).unwrap();
    assert!(matches!(
        BootstrapListener::new(
            config(&gate, link, 10),
            admission(&other),
            &adapter,
            1_000_000,
            21
        ),
        Err(Error::Config)
    ));
    let mut client =
        BootstrapClient::new(config(&gate, link, 10), &adapter, hello(), 7, 90, 21).unwrap();
    client.start(&adapter, 21).unwrap();
    let mut driver = Driver::new(DriverSubmit::NotSubmitted);
    client.drive(&mut adapter, &mut driver, 21).unwrap();
    let challenge = CookieChallenge {
        flow: BootstrapFlow::Join,
        transaction_id: 3,
        cookie_time_bucket: 1,
        issuer_link_id: 7,
        issuer_link_generation: 9,
        cookie_length: 16,
        cookie: [1; 16],
    };
    let mut packet = [0; 128];
    let n = encode_bootstrap_frame(
        BootstrapMessage::Challenge(challenge),
        RealmId::new(1).unwrap(),
        AddressWidth::A1,
        &mut packet,
    )
    .unwrap();
    publish(&ingress, link, &packet[..n], 22, 90);
    assert_eq!(client.receive(&mut adapter, 22), Err(Error::State));
    assert_eq!(client.phase(), BootstrapClientPhase::AwaitCookie);
    client.stop(&mut adapter).unwrap();
    client.retire(&mut adapter, &mut driver, 22).unwrap();
    assert!(client.quiescent(&adapter));
}
#[test]
fn unknown_and_failed_cancellation_never_erase_a_bootstrap_holder() {
    for submit in [DriverSubmit::Unknown, DriverSubmit::Submitted] {
        let gate = CallbackGate::new(1).unwrap();
        let i = Ingress::new(1).unwrap();
        let mut a = Adapter::new(1, &i).unwrap();
        let l = a.open_link(0, 3, 128).unwrap();
        let now = 1_000_001;
        let mut client =
            BootstrapClient::new(config(&gate, l, 10), &a, hello(), 7, 90, now).unwrap();
        let mut driver = Driver::new(submit);
        client.start(&a, now).unwrap();
        let sent = client.drive(&mut a, &mut driver, now);
        assert_eq!(sent.is_err(), submit == DriverSubmit::Unknown);
        let token = driver.tokens[0];
        client.stop(&mut a).unwrap();
        let r = client.retire(&mut a, &mut driver, now);
        assert_eq!(r.is_err(), submit == DriverSubmit::Unknown);
        assert!(!client.quiescent(&a));
        assert!(a.tx_view(token).is_ok());
        if submit == DriverSubmit::Submitted {
            driver.cancel = DriverCancel::Cancelled;
            client.retire(&mut a, &mut driver, now).unwrap();
            assert!(client.quiescent(&a));
        }
    }
}
#[test]
fn bootstrap_frame_and_cookie_codec_mutations_are_zero_write_or_zero_callback() {
    let realm = RealmId::new(1).unwrap();
    let message = BootstrapMessage::Hello(hello());
    let mut bytes = [0; 128];
    let n = encode_bootstrap_frame(message, realm, AddressWidth::A1, &mut bytes).unwrap();
    assert_eq!(n, 69);
    for offset in [0, 1, 2, 6, 8, 10, 11, 19, 28] {
        let mut bad = bytes;
        bad[offset] ^= 0x80;
        assert!(
            decode_bootstrap_frame(&bad[..n], realm, AddressWidth::A1).is_err(),
            "offset={offset}"
        );
    }
    let mut short = [0xa5; 68];
    assert_eq!(
        encode_bootstrap_frame(message, realm, AddressWidth::A1, &mut short),
        Err(Error::NoSpace)
    );
    assert_eq!(short, [0xa5; 68]);
    let challenge = CookieChallenge {
        flow: BootstrapFlow::Join,
        transaction_id: 3,
        cookie_time_bucket: 1,
        issuer_link_id: 2,
        issuer_link_generation: 9,
        cookie_length: 16,
        cookie: [1; 16],
    };
    let mut raw = [0; 40];
    encode_cookie_challenge(challenge, &mut raw).unwrap();
    assert_eq!(raw[32..38], [0, 2, 0, 0, 0, 9]);
    for offset in [32, 34, 38] {
        let mut bad = raw;
        if offset == 38 {
            bad[offset] = 1;
        } else if offset == 32 {
            bad[32..34].fill(0);
        } else {
            bad[34..38].fill(0);
        }
        assert!(decode_cookie_challenge(&bad).is_err());
    }
    let echo = HelloCookie {
        flow: BootstrapFlow::Join,
        identity_digest: hello().identity_digest,
        device_nonce: 2,
        transaction_id: 3,
        lease_freshness_challenge_nonce: 7,
        selected_link_instance_id: 2,
        selected_link_instance_generation: 9,
        prior_messages_hash: [1; 32],
        cookie_evidence: Cookie::new(&[1; 16]).unwrap(),
    };
    assert_eq!(Cookie::new(&[1; 17]), Err(Error::Argument));
    let mut out = [0xa5; 97];
    assert_eq!(encode_hello_cookie(echo, &mut out), Err(Error::NoSpace));
    assert_eq!(out, [0xa5; 97]);
}
