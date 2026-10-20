//! Host (Windows/Linux/macOS) platform glue.

use std::sync::Once;

use librespot_playback::audio_backend::{self, Sink};
use librespot_playback::config::AudioFormat;

use super::NetStatus;

/// Initialise logging for the host build.
pub fn init_logging() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let mut builder = env_logger::Builder::new();
        let spec = std::env::var("VAM_LOG")
            .or_else(|_| std::env::var("RUST_LOG"))
            .unwrap_or_else(|_| "librespot=warn,vam_backend=info".to_string());
        builder.parse_filters(&spec);
        builder.try_init().ok();
    });
}

/// The host always has a network stack available.
pub fn init_network(_data_dir: &str) -> NetStatus {
    init_logging();
    NetStatus::Connected
}

pub fn shutdown_network() {}

/// Host builds have no Wi-Fi state to query.
pub fn wifi_connected() -> bool {
    true
}

/// Create the default librespot audio backend (rodio/cpal).
pub fn make_sink() -> Box<dyn Sink> {
    let backend = audio_backend::find(None).expect("no librespot audio backend compiled in");
    backend(None, AudioFormat::S16)
}

/// No-op; the host's audio backend is managed by librespot.
pub fn shutdown_audio() {}

/// Only the Vita needs explicit thread priorities.
pub fn lower_current_thread_priority() {}

/// Hosts keep Spotify's volume in software; there is no system volume to follow.
pub fn system_volume_step() -> Option<u32> {
    None
}

pub fn set_system_volume_step(_step: u32) -> bool {
    false
}
