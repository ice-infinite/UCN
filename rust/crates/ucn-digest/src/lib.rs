#![no_std]
#![forbid(unsafe_code)]

//! Canonical Wire digest Suite 1; 线上 SHA-256 前16字节摘要，无动态分配。
//! Fingerprints establish equality, not authentication. 摘要不能代替认证。

use ucn_types::{Error, Result};

const IV: [u32; 8] = [
    0x6a09_e667,
    0xbb67_ae85,
    0x3c6e_f372,
    0xa54f_f53a,
    0x510e_527f,
    0x9b05_688c,
    0x1f83_d9ab,
    0x5be0_cd19,
];
const K: [u32; 64] = [
    0x428a_2f98,
    0x7137_4491,
    0xb5c0_fbcf,
    0xe9b5_dba5,
    0x3956_c25b,
    0x59f1_11f1,
    0x923f_82a4,
    0xab1c_5ed5,
    0xd807_aa98,
    0x1283_5b01,
    0x2431_85be,
    0x550c_7dc3,
    0x72be_5d74,
    0x80de_b1fe,
    0x9bdc_06a7,
    0xc19b_f174,
    0xe49b_69c1,
    0xefbe_4786,
    0x0fc1_9dc6,
    0x240c_a1cc,
    0x2de9_2c6f,
    0x4a74_84aa,
    0x5cb0_a9dc,
    0x76f9_88da,
    0x983e_5152,
    0xa831_c66d,
    0xb003_27c8,
    0xbf59_7fc7,
    0xc6e0_0bf3,
    0xd5a7_9147,
    0x06ca_6351,
    0x1429_2967,
    0x27b7_0a85,
    0x2e1b_2138,
    0x4d2c_6dfc,
    0x5338_0d13,
    0x650a_7354,
    0x766a_0abb,
    0x81c2_c92e,
    0x9272_2c85,
    0xa2bf_e8a1,
    0xa81a_664b,
    0xc24b_8b70,
    0xc76c_51a3,
    0xd192_e819,
    0xd699_0624,
    0xf40e_3585,
    0x106a_a070,
    0x19a4_c116,
    0x1e37_6c08,
    0x2748_774c,
    0x34b0_bcb5,
    0x391c_0cb3,
    0x4ed8_aa4a,
    0x5b9c_ca4f,
    0x682e_6ff3,
    0x748f_82ee,
    0x78a5_636f,
    0x84c8_7814,
    0x8cc7_0208,
    0x90be_fffa,
    0xa450_6ceb,
    0xbef9_a3f7,
    0xc671_78f2,
];

/// Caller-owned fixed SHA scratch; 调用方持有固定摘要暂存。
pub struct DigestWorkspace {
    state: [u32; 8],
    schedule: [u32; 64],
    block: [u8; 64],
    used: usize,
    bytes: u64,
}

impl Default for DigestWorkspace {
    fn default() -> Self {
        Self::new()
    }
}

impl DigestWorkspace {
    /// Static initialization; 可静态初始化。
    #[must_use]
    pub const fn new() -> Self {
        Self {
            state: IV,
            schedule: [0; 64],
            block: [0; 64],
            used: 0,
            bytes: 0,
        }
    }

    fn reset(&mut self) {
        self.state = IV;
        self.used = 0;
        self.bytes = 0;
        self.block.fill(0);
    }

    #[allow(clippy::many_single_char_names)] // Names follow the SHA-256 round specification.
    fn compress(&mut self) {
        for (index, word) in self.schedule[..16].iter_mut().enumerate() {
            let start = index * 4;
            *word = u32::from_be_bytes(
                self.block[start..start + 4]
                    .try_into()
                    .expect("fixed SHA block"),
            );
        }
        for index in 16..64 {
            let a = self.schedule[index - 15];
            let b = self.schedule[index - 2];
            let s0 = a.rotate_right(7) ^ a.rotate_right(18) ^ (a >> 3);
            let s1 = b.rotate_right(17) ^ b.rotate_right(19) ^ (b >> 10);
            self.schedule[index] = self.schedule[index - 16]
                .wrapping_add(s0)
                .wrapping_add(self.schedule[index - 7])
                .wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = self.state;
        for (constant, word) in K.iter().zip(self.schedule.iter()) {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let temp1 = h
                .wrapping_add(s1)
                .wrapping_add((e & f) ^ (!e & g))
                .wrapping_add(*constant)
                .wrapping_add(*word);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let temp2 = s0.wrapping_add((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(temp1);
            d = c;
            c = b;
            b = a;
            a = temp1.wrapping_add(temp2);
        }
        for (state, value) in self.state.iter_mut().zip([a, b, c, d, e, f, g, h]) {
            *state = state.wrapping_add(value);
        }
    }

    fn update(&mut self, mut input: &[u8]) {
        self.bytes += input.len() as u64;
        while !input.is_empty() {
            let count = (64 - self.used).min(input.len());
            self.block[self.used..self.used + count].copy_from_slice(&input[..count]);
            self.used += count;
            input = &input[count..];
            if self.used == 64 {
                self.compress();
                self.used = 0;
            }
        }
    }

    fn finish(&mut self) -> [u8; 32] {
        let bits = self.bytes * 8;
        self.block[self.used] = 0x80;
        self.used += 1;
        if self.used > 56 {
            self.block[self.used..].fill(0);
            self.compress();
            self.used = 0;
        }
        self.block[self.used..56].fill(0);
        self.block[56..].copy_from_slice(&bits.to_be_bytes());
        self.compress();
        let mut output = [0; 32];
        for (chunk, word) in output.chunks_exact_mut(4).zip(self.state) {
            chunk.copy_from_slice(&word.to_be_bytes());
        }
        output
    }
}

/// SHA-256 of exact bytes; 精确字节的 SHA-256。
pub struct CanonicalHasher<'a> {
    workspace: &'a mut DigestWorkspace,
    remaining: u32,
}
impl CanonicalHasher<'_> {
    /// Append exact bytes without allocating a whole canonical message.
    /// 追加精确字节，不分配完整规范消息。
    /// # Errors
    /// More bytes than the frozen length rejects before updating.
    pub fn append(&mut self, bytes: &[u8]) -> Result<()> {
        let count = u32::try_from(bytes.len()).map_err(|_| Error::Argument)?;
        if count > self.remaining {
            return Err(Error::Argument);
        }
        self.workspace.update(bytes);
        self.remaining -= count;
        Ok(())
    }
    /// Finish only an exactly complete message; 仅结束精确完整消息。
    /// # Errors
    /// Incomplete input rejects.
    pub fn finish(self) -> Result<[u8; 16]> {
        if self.remaining != 0 {
            return Err(Error::State);
        }
        let mut output = [0; 16];
        output.copy_from_slice(&self.workspace.finish()[..16]);
        Ok(output)
    }
}
/// Start a bounded canonical streaming digest; 开始有界规范流式摘要。
/// # Errors
/// Invalid domain rejects before mutating the workspace.
pub fn canonical_begin<'a>(
    domain: &[u8],
    length: u32,
    workspace: &'a mut DigestWorkspace,
) -> Result<CanonicalHasher<'a>> {
    if domain.is_empty() || domain.len() > 64 {
        return Err(Error::Argument);
    }
    workspace.reset();
    workspace.update(domain);
    workspace.update(&length.to_be_bytes());
    Ok(CanonicalHasher {
        workspace,
        remaining: length,
    })
}

/// SHA-256 of exact bytes; 精确字节的 SHA-256。
#[must_use]
pub fn sha256(bytes: &[u8], workspace: &mut DigestWorkspace) -> [u8; 32] {
    workspace.reset();
    workspace.update(bytes);
    workspace.finish()
}

/// `domain || u32be(length) || bytes`, truncated to 16 octets.
/// 规范 Wire 域分隔及长度前缀，输出前16字节。
/// # Errors
/// Input exceeding the u32 length contract is rejected before hashing.
/// 超出32位长度合同在计算前拒绝。
pub fn canonical_digest(
    domain: &[u8],
    bytes: &[u8],
    workspace: &mut DigestWorkspace,
) -> Result<[u8; 16]> {
    let length = u32::try_from(bytes.len()).map_err(|_| Error::Argument)?;
    if domain.is_empty() || domain.len() > 64 {
        return Err(Error::Argument);
    }
    workspace.reset();
    workspace.update(domain);
    workspace.update(&length.to_be_bytes());
    workspace.update(bytes);
    let full = workspace.finish();
    let mut output = [0; 16];
    output.copy_from_slice(&full[..16]);
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn published_sha_vectors_and_block_boundaries() {
        let mut workspace = DigestWorkspace::new();
        assert_eq!(
            sha256(b"abc", &mut workspace),
            [
                0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae,
                0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61,
                0xf2, 0x00, 0x15, 0xad
            ]
        );
        assert_eq!(
            sha256(b"", &mut workspace),
            [
                0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f,
                0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b,
                0x78, 0x52, 0xb8, 0x55
            ]
        );
        assert_ne!(
            sha256(&[1; 55], &mut workspace),
            sha256(&[1; 56], &mut workspace)
        );
        assert_ne!(
            sha256(&[1; 64], &mut workspace),
            sha256(&[1; 65], &mut workspace)
        );
    }
}
