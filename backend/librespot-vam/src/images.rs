//! Image download + LRU disk cache for `vam_request_image`.

use std::fs;
use std::path::PathBuf;
use std::sync::Mutex;

use crate::shared::Shared;
use crate::util::{now_ms_inner, sha1_hex};
use crate::webapi::ApiError;

/// Simple size-capped file cache keyed by SHA-1 of the URL.
pub struct ByteCache {
    dir: PathBuf,
    max_bytes: u64,
    index: Mutex<Vec<(String, u64, u64)>>, // (key, size, last_used_ms)
}

impl ByteCache {
    pub fn new(dir: String, max_bytes: u64) -> Self {
        let dir = PathBuf::from(dir);
        let _ = fs::create_dir_all(&dir);
        let mut entries = Vec::new();
        if let Ok(read) = fs::read_dir(&dir) {
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
        ByteCache {
            dir,
            max_bytes,
            index: Mutex::new(entries),
        }
    }

    fn path_for(&self, key: &str) -> PathBuf {
        self.dir.join(format!("{key}.img"))
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
        self.prune();
    }

    fn prune(&self) {
        let Ok(mut index) = self.index.lock() else {
            return;
        };
        let mut total: u64 = index.iter().map(|(_, size, _)| *size).sum();
        if total <= self.max_bytes {
            return;
        }
        index.sort_by_key(|(_, _, used)| *used);
        let mut remaining = Vec::new();
        for entry in index.drain(..) {
            if total > self.max_bytes {
                total = total.saturating_sub(entry.1);
                let _ = fs::remove_file(self.path_for(&entry.0));
            } else {
                remaining.push(entry);
            }
        }
        *index = remaining;
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
