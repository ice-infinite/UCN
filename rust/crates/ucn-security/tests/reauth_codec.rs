//! Independent structural and derivation negatives; not authentication evidence.
use ucn_security::{Binding, PeerReauthRecord, REAUTH_RECORD_BYTES};
use ucn_types::{AddressWidth, BindingGeneration, NodeAddress, RealmId};
fn record() -> PeerReauthRecord {
    let binding = |address, p| Binding {
        address: NodeAddress::new(address, AddressWidth::A1).unwrap(),
        generation: BindingGeneration::active(1).unwrap(),
        principal: [p; 16],
    };
    PeerReauthRecord {
        stage: 0,
        width: AddressWidth::A1,
        realm: RealmId::new(1).unwrap(),
        initiator: binding(1, 0x11),
        responder: binding(2, 0x22),
        previous_session: 1,
        next_session: 2,
        origin_suite: 1,
        origin_key: 1,
        hop_key: 3,
        origin_generation: 2,
        hop_generation: 2,
        initiator_link: (7, 9),
        responder_link: (0, 0),
        transaction: 3,
        initiator_nonce: 4,
        responder_nonce: 0,
        lifetime_us: 1000,
        previous_digest: [0x77; 32],
        initiator_keys: (1, 1),
        responder_history: ucn_security::PeerReauthHistory::EMPTY,
    }
}
#[test]
fn exact_layout_reserves_lengths_and_zero_write() {
    let mut raw = [0; REAUTH_RECORD_BYTES];
    record().encode(&mut raw).unwrap();
    assert_eq!(&raw[..8], &[2, 0, 2, 0, 0, 0, 0, 1]);
    assert_eq!(&raw[56..64], &[0, 0, 0, 1, 0, 0, 0, 2]);
    assert_eq!(PeerReauthRecord::decode(&raw).unwrap(), record());
    for offset in [0, 1, 2, 3, 65, 70, 71] {
        let mut b = raw;
        b[offset] = 255;
        assert!(PeerReauthRecord::decode(&b).is_err());
    }
    for length in [207, 209] {
        let mut b = [0xcc; 209];
        b[..208].copy_from_slice(&raw);
        assert!(PeerReauthRecord::decode(&b[..length]).is_err());
    }
    let mut out = [0xcc; 208];
    let bad = PeerReauthRecord {
        responder_nonce: 1,
        ..record()
    };
    assert!(bad.encode(&mut out).is_err());
    assert_eq!(out, [0xcc; 208]);
    assert!(record().encode(&mut out[..207]).is_err());
    assert_eq!(out, [0xcc; 208]);
}
#[test]
fn candidate_binding_directions_serial_exhaustion_and_deadline() {
    let r = PeerReauthRecord {
        stage: 2,
        responder_link: (8, 10),
        responder_nonce: 5,
        responder_history: ucn_security::PeerReauthHistory {
            session: 1,
            origin: 1,
            hop: 1,
            digest: [0x77; 32],
        },
        ..record()
    };
    let a = r.candidate(true, 1, 100, 500).unwrap();
    let b = r.candidate(false, 1, 100, 500).unwrap();
    assert_eq!(a.local, b.peer);
    assert_eq!(a.origin_tx, b.origin_rx);
    assert_eq!(a.hop_tx, b.hop_rx);
    assert_eq!(a.transcript_digest, b.transcript_digest);
    assert_eq!(a.expires_at_us, 500);
    assert!(r.candidate(true, 1, u64::MAX, u64::MAX).is_err());
    assert!(r.candidate(true, 0, 100, 500).is_err());
    assert!(
        PeerReauthRecord { stage: 255, ..r }
            .candidate(true, 1, 100, 500)
            .is_err()
    );
    assert!(
        PeerReauthRecord { stage: 0, ..r }
            .candidate(true, 1, 100, 500)
            .is_err()
    );
    let terminal = PeerReauthRecord {
        previous_session: u32::MAX - 1,
        next_session: u32::MAX,
        responder_history: ucn_security::PeerReauthHistory {
            session: u32::MAX - 1,
            ..r.responder_history
        },
        ..r
    };
    assert!(terminal.validate().is_ok());
    assert!(
        PeerReauthRecord {
            previous_session: u32::MAX,
            next_session: 1,
            ..r
        }
        .validate()
        .is_err()
    );
}
#[test]
fn fragment_geometry_reserved_fields_and_short_output_are_rejected() {
    use ucn_security::{
        PeerReauthFragment, decode_peer_reauth_fragment, encode_peer_reauth_fragment,
    };
    let data = [0x44; 1];
    let fragment = PeerReauthFragment {
        stage: 2,
        index: 3,
        total_bytes: 241,
        transaction: 3,
        data: &data,
    };
    let mut raw = [0; 98];
    let n = encode_peer_reauth_fragment(fragment, &mut raw).unwrap();
    assert_eq!(n, 19);
    assert_eq!(decode_peer_reauth_fragment(&raw[..n]).unwrap(), fragment);
    for index in [0, 1, 2, 3, 4, 6, 8, 9] {
        let mut bad = raw;
        bad[index] = 255;
        assert!(decode_peer_reauth_fragment(&bad[..n]).is_err());
    }
    let mut out = [0xcc; 98];
    assert!(encode_peer_reauth_fragment(fragment, &mut out[..18]).is_err());
    assert_eq!(out, [0xcc; 98]);
    assert!(
        encode_peer_reauth_fragment(
            PeerReauthFragment {
                index: 4,
                ..fragment
            },
            &mut out
        )
        .is_err()
    );
    assert_eq!(out, [0xcc; 98]);
}
#[test]
fn asymmetric_history_cannot_hide_equal_generation_fork_or_regression() {
    let mut r = record();
    r.stage = 2;
    r.responder_link = (8, 10);
    r.responder_nonce = 5;
    r.responder_history = ucn_security::PeerReauthHistory {
        session: 2,
        origin: 2,
        hop: 2,
        digest: [0x88; 32],
    };
    r.next_session = 3;
    r.origin_generation = 3;
    r.hop_generation = 3;
    assert!(r.validate().is_ok());
    let mut raw = [0; REAUTH_RECORD_BYTES];
    r.encode(&mut raw).unwrap();
    assert_eq!(PeerReauthRecord::decode(&raw).unwrap(), r);
    for h in [
        ucn_security::PeerReauthHistory {
            session: 1,
            origin: 1,
            hop: 1,
            digest: [0x88; 32],
        },
        ucn_security::PeerReauthHistory {
            origin: 1,
            ..r.responder_history
        },
        ucn_security::PeerReauthHistory {
            session: u32::MAX,
            ..r.responder_history
        },
    ] {
        assert!(
            PeerReauthRecord {
                responder_history: h,
                ..r
            }
            .validate()
            .is_err()
        );
    }
}
