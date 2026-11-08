//! TEST DOUBLE ONLY: tagged reversible transform to exercise the O2 Provider SPI.
//! Not AES/ChaCha, not a production cipher, and never exported from a library/example.
use super::security;
use ucn_security::{
    CryptoProvider, HandshakeCandidate, KeySelector, OpenOriginRequest, SealOriginRequest,
};
use ucn_types::{Error, Result};
pub struct TestCrypto(security::Crypto);
impl TestCrypto {
    pub const fn new(candidate: HandshakeCandidate) -> Self {
        Self(security::Crypto::for_candidate(candidate))
    }
    fn nonce(&mut self, selector: KeySelector, input: &[u8]) -> Result<[u8; 12]> {
        let mut tag = [0; 16];
        self.0.compute_origin_tag(selector, input, &[], &mut tag)?;
        let mut nonce = [0; 12];
        nonce.copy_from_slice(&tag[..12]);
        Ok(nonce)
    }
}
impl CryptoProvider for TestCrypto {
    fn verify_session_proof(&mut self, c: &HandshakeCandidate, p: &[u8]) -> Result<()> {
        self.0.verify_session_proof(c, p)
    }
    fn compute_origin_tag(
        &mut self,
        k: KeySelector,
        a: &[u8],
        p: &[u8],
        t: &mut [u8; 16],
    ) -> Result<()> {
        self.0.compute_origin_tag(k, a, p, t)
    }
    fn verify_origin_tag(
        &mut self,
        k: KeySelector,
        a: &[u8],
        p: &[u8],
        t: &[u8; 16],
    ) -> Result<()> {
        self.0.verify_origin_tag(k, a, p, t)
    }
    fn seal_origin(&mut self, r: SealOriginRequest<'_>) -> Result<()> {
        if r.plaintext.len() != r.ciphertext.len() {
            return Err(Error::Argument);
        }
        let nonce = self.nonce(r.selector, r.nonce_input)?;
        r.derived_nonce.copy_from_slice(&nonce);
        for (i, (p, c)) in r.plaintext.iter().zip(r.ciphertext.iter_mut()).enumerate() {
            *c = *p ^ nonce[i % 12] ^ 0xA5;
        }
        self.0
            .compute_origin_tag(r.selector, r.aad, r.ciphertext, r.tag)
    }
    fn open_origin(&mut self, r: OpenOriginRequest<'_>) -> Result<()> {
        if r.plaintext.len() != r.ciphertext.len() {
            return Err(Error::Argument);
        }
        self.0
            .verify_origin_tag(r.selector, r.aad, r.ciphertext, r.tag)?;
        let nonce = self.nonce(r.selector, r.nonce_input)?;
        r.derived_nonce.copy_from_slice(&nonce);
        for (i, (p, c)) in r.plaintext.iter_mut().zip(r.ciphertext.iter()).enumerate() {
            *p = *c ^ nonce[i % 12] ^ 0xA5;
        }
        Ok(())
    }
    fn compute_hop_tag(&mut self, k: KeySelector, a: &[u8], t: &mut [u8; 12]) -> Result<()> {
        self.0.compute_hop_tag(k, a, t)
    }
    fn verify_hop_tag(&mut self, k: KeySelector, a: &[u8], t: &[u8; 12]) -> Result<()> {
        self.0.verify_hop_tag(k, a, t)
    }
}
