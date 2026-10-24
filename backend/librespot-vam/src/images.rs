//! Image download + LRU disk cache for `vam_request_image`.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use crate::shared::Shared;
use crate::util::{now_ms_inner, sha1_hex};
use crate::webapi::ApiError;

/// Simple size-capped file cache keyed by SHA-1 of the URL.
pub struct ByteCache {
    dir: PathBuf,
    max_bytes: u64,
    index: Arc<Mutex<Vec<(String, u64, u64)>>>, // (key, size, last_used_ms)
}

type Index = Mutex<Vec<(String, u64, u64)>>;

impl ByteCache {
    /// Lists the existing files on a background thread: on the Vita that
    /// takes about 25 ms per file (12 s for a full cache), and sign-in used
    /// to wait for it. Until it finishes, `get` still reads files directly
    /// and `put` only undercounts, so pruning just starts a little late.
    pub fn new(dir: String, max_bytes: u64) -> Self {
        let dir = PathBuf::from(dir);
        let _ = fs::create_dir_all(&dir);
        let index: Arc<Index> = Arc::new(Mutex::new(Vec::new()));
        let (scan_dir, scan_index) = (dir.clone(), index.clone());
        let spawned = std::thread::Builder::new()
            .name("img-cache-index".into())
            .stack_size(128 * 1024)
            .spawn(move || {
                crate::platform::lower_current_thread_priority();
                let scanned = scan(&scan_dir);
                if let Ok(mut index) = scan_index.lock() {
                    // Files touched meanwhile are already listed, fresher.
                    for entry in scanned {
                        if !index.iter().any(|(k, _, _)| *k == entry.0) {
                            index.push(entry);
                        }
                    }
                }
                prune(&scan_dir, max_bytes, &scan_index);
            });
        if spawned.is_err() {
            *index.lock().unwrap_or_else(|e| e.into_inner()) = scan(&dir);
        }
        ByteCache {
            dir,
            max_bytes,
            index,
        }
    }

    fn path_for(&self, key: &str) -> PathBuf {
        path_in(&self.dir, key)
    }

    pub fn get(&self, key: &str) -> Option<Vec<u8>> {
        let path = self.path_for(key);
        let data = fs::read(&path).ok()?;
        let now = now_ms_inner();
        if let Ok(mut index) = self.index.lock() {
            if let Some(entry) = index.iter_mut().find(|(k, _, _)| k == key) {
                entry.2 = now;
            } else {
                index.push((key.to_string(), data.len() as u64, now));
            }
        }
        Some(data)
    }

    pub fn put(&self, key: &str, data: &[u8]) {
        let path = self.path_for(key);
        if fs::create_dir_all(&self.dir).is_err() {
            return;
        }
        if fs::write(&path, data).is_err() {
            return;
        }
        let now = now_ms_inner();
        let mut total: u64 = 0;
        if let Ok(mut index) = self.index.lock() {
            if let Some(entry) = index.iter_mut().find(|(k, _, _)| k == key) {
                total = entry.1;
                entry.1 = data.len() as u64;
                entry.2 = now;
            } else {
                index.push((key.to_string(), data.len() as u64, now));
            }
        }
        let _ = total;
        prune(&self.dir, self.max_bytes, &self.index);
    }
}

fn path_in(dir: &Path, key: &str) -> PathBuf {
    dir.join(format!("{key}.img"))
}

/// (key, size, last_used_ms) of every cached file in dir.
fn scan(dir: &Path) -> Vec<(String, u64, u64)> {
    let mut entries = Vec::new();
    if let Ok(read) = fs::read_dir(dir) {
        for entry in read.flatten() {
            if let Ok(meta) = entry.metadata() {
                if meta.is_file() {
                    let name = entry.file_name().to_string_lossy().to_string();
                    let key = name.trim_end_matches(".img").to_string();
                    let used = meta
                        .modified()
                        .ok()
                        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                        .map(|d| d.as_millis() as u64)
                        .unwrap_or(0);
                    entries.push((key, meta.len(), used));
                }
            }
        }
    }
    entries
}

/// Once the cache is over max_bytes, delete the least recently used files
/// down to 90% of it, so the sort runs once per batch rather than on every
/// put of a full cache.
fn prune(dir: &Path, max_bytes: u64, index: &Index) {
    let doomed: Vec<_> = {
        let Ok(mut index) = index.lock() else {
            return;
        };
        let mut total: u64 = index.iter().map(|(_, size, _)| *size).sum();
        if total <= max_bytes {
            return;
        }
        let target = max_bytes - max_bytes / 10;
        index.sort_by_key(|(_, _, used)| *used);
        let mut n = 0;
        while n < index.len() && total > target {
            total = total.saturating_sub(index[n].1);
            n += 1;
        }
        index.drain(..n).collect()
    };
    // Outside the lock: each delete takes milliseconds on the Vita, and
    // image loads need the index meanwhile.
    for (key, _, _) in doomed {
        let _ = fs::remove_file(path_in(dir, &key));
    }
}

/// Fetch (or reuse) the encoded JPEG/PNG bytes for an `i.scdn.co` URL.
pub async fn fetch_image(shared: &Shared, url: &str) -> Result<Vec<u8>, ApiError> {
    if url.is_empty() || !url.starts_with("https://") {
        return Err(ApiError::new(
            crate::abi::VAM_ERR_NOT_FOUND,
            "That image has no download URL.",
        ));
    }
    let key = sha1_hex(url.as_bytes());
    if let Some(bytes) = shared.images.get(&key) {
        return Ok(bytes);
    }
    let bytes = shared.web.get_bytes(url).await?;
    if bytes.len() > 8 * 1024 * 1024 {
        return Err(ApiError::new(
            crate::abi::VAM_ERR_UNKNOWN,
            "Image is too large.",
        ));
    }
    shared.images.put(&key, &bytes);
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn prune_drops_oldest_down_to_90_percent() {
        let dir = std::env::temp_dir().join(format!("vam-img-prune-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        // Ten 10-byte files, used at times 0..10, against a 95-byte cap.
        let entries: Vec<_> = (0..10u64).map(|i| (format!("k{i}"), 10, i)).collect();
        for (key, _, _) in &entries {
            fs::write(path_in(&dir, key), [0u8; 10]).unwrap();
        }
        let index: Index = Mutex::new(entries);

        prune(&dir, 95, &index);

        // 100 > 95, so it shrinks to <= 86 (95 - 9): the two oldest go.
        let left: Vec<_> = index.lock().unwrap().iter().map(|e| e.0.clone()).collect();
        assert_eq!(left, (2..10).map(|i| format!("k{i}")).collect::<Vec<_>>());
        assert!(!path_in(&dir, "k0").exists() && !path_in(&dir, "k1").exists());
        assert!(path_in(&dir, "k2").exists());
        let _ = fs::remove_dir_all(&dir);
    }
}
