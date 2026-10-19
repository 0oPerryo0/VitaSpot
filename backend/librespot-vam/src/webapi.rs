//! Session holder shared by the metadata code, plus plain HTTP downloads
//! (image CDN) on the HTTP stack librespot already owns. Everything else
//! talks to Spotify's internal endpoints (`internal.rs`, `search.rs`); the
//! public Web API rate-limits librespot's shared client id.

use std::sync::{Arc, Mutex};

use bytes::Bytes;
use http::{header, Method, Request};
use http_body_util::BodyExt;
use librespot_core::Session;

use crate::abi::*;

#[derive(Debug, Clone)]
pub struct ApiError {
    pub result: i32,
    pub message: String,
}

impl ApiError {
    pub fn new(result: i32, message: impl Into<String>) -> Self {
        ApiError {
            result,
            message: message.into(),
        }
    }

    pub fn auth(message: impl Into<String>) -> Self {
        Self::new(VAM_ERR_AUTH, message)
    }

    pub fn network(message: impl Into<String>) -> Self {
        Self::new(VAM_ERR_NETWORK, message)
    }

    pub fn not_found() -> Self {
        Self::new(VAM_ERR_NOT_FOUND, "Spotify has no such item.")
    }

    pub fn unavailable(message: impl Into<String>) -> Self {
        Self::new(VAM_ERR_UNAVAILABLE, message)
    }

    pub fn rate_limited(message: impl Into<String>) -> Self {
        Self::new(VAM_ERR_RATE_LIMITED, message)
    }
}

/// Shared Web API client. Cheap to clone.
#[derive(Clone)]
pub struct WebApi {
    inner: Arc<Inner>,
}

struct Inner {
    session: Mutex<Option<Session>>,
}

impl WebApi {
    pub fn new() -> Self {
        WebApi {
            inner: Arc::new(Inner {
                session: Mutex::new(None),
            }),
        }
    }

    pub fn set_session(&self, session: Session) {
        *self.inner.session.lock().unwrap_or_else(|e| e.into_inner()) = Some(session);
    }

    pub fn clear_session(&self) {
        *self.inner.session.lock().unwrap_or_else(|e| e.into_inner()) = None;
    }

    pub fn is_authenticated(&self) -> bool {
        self.inner
            .session
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .is_some()
    }

    /// Canonical username of the logged in account (empty when logged out).
    pub fn username(&self) -> String {
        self.inner
            .session
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .map(|s| s.username())
            .unwrap_or_default()
    }

    /// Spotify market/country code reported by the session.
    pub fn country(&self) -> String {
        self.inner
            .session
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .map(|s| s.country())
            .unwrap_or_default()
    }

    pub(crate) fn session(&self) -> Result<Session, ApiError> {
        self.inner
            .session
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
            .ok_or_else(|| ApiError::auth("Not logged in."))
    }

    /// Download arbitrary bytes (image CDN). No auth required.
    pub async fn get_bytes(&self, url: &str) -> Result<Vec<u8>, ApiError> {
        let session = self.session()?;
        let request = Request::builder()
            .method(Method::GET)
            .uri(url)
            .header(header::USER_AGENT, "VitaSpot/1.0")
            .body(Bytes::new())
            .map_err(|e| ApiError::new(VAM_ERR_UNKNOWN, e.to_string()))?;
        let response = session
            .http_client()
            .request(request)
            .await
            .map_err(|e| ApiError::network(format!("Image download failed: {e}")))?;
        let status = response.status();
        let bytes = response
            .into_body()
            .collect()
            .await
            .map(|c| c.to_bytes().to_vec())
            .map_err(|e| ApiError::network(format!("Image download failed: {e}")))?;
        if !status.is_success() {
            return Err(ApiError::network(format!("Image download HTTP {status}")));
        }
        Ok(bytes)
    }
}

impl Default for WebApi {
    fn default() -> Self {
        Self::new()
    }
}
