//! Small helpers shared by the whole backend.

use std::os::raw::c_char;

/// Copy a UTF-8 string into a fixed-size NUL-terminated C buffer, truncating
/// on a code point boundary.
pub fn copy_str<const N: usize>(dst: &mut [u8; N], s: &str) {
    let max = N.saturating_sub(1);
    let bytes = s.as_bytes();
    let mut n = bytes.len().min(max);
    while n > 0 && !s.is_char_boundary(n) {
        n -= 1;
    }
    dst[..n].copy_from_slice(&bytes[..n]);
    dst[n] = 0;
    for b in dst.iter_mut().skip(n + 1) {
        *b = 0;
    }
}

/// Copy a C string (possibly null) into a Rust `String`.
///
/// # Safety
/// `ptr` must be null or point to a NUL-terminated string that stays valid for
/// the duration of the call. The frontend guarantees this by contract.
pub unsafe fn cstr_to_string(ptr: *const c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let mut len = 0usize;
    // Bounded scan so a broken pointer cannot run away forever.
    const MAX: usize = 64 * 1024;
    while len < MAX && *ptr.add(len) != 0 {
        len += 1;
    }
    let slice = std::slice::from_raw_parts(ptr as *const u8, len);
    String::from_utf8_lossy(slice).into_owned()
}

/// Current time in milliseconds from a monotonic clock.
#[cfg(not(target_os = "vita"))]
pub fn now_ms_inner() -> u64 {
    use std::sync::OnceLock;
    use std::time::Instant;

    static START: OnceLock<Instant> = OnceLock::new();
    let start = START.get_or_init(Instant::now);
    start.elapsed().as_millis() as u64
}

/// Current time in milliseconds from a monotonic clock (Vita).
#[cfg(target_os = "vita")]
pub fn now_ms_inner() -> u64 {
    extern "C" {
        // psp2/kernel/processmgr.h: returns microseconds since process start.
        fn sceKernelGetProcessTimeWide() -> u64;
    }
    unsafe { sceKernelGetProcessTimeWide() / 1000 }
}

/// Lowercase hex SHA-1, used for the Connect device id and cache keys.
pub fn sha1_hex(data: &[u8]) -> String {
    use sha1::{Digest, Sha1};
    let digest = Sha1::digest(data);
    let mut out = String::with_capacity(40);
    for b in digest {
        out.push(char::from_digit((b >> 4) as u32, 16).unwrap());
        out.push(char::from_digit((b & 0x0f) as u32, 16).unwrap());
    }
    out
}
