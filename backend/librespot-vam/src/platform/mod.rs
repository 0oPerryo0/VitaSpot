//! Platform abstraction: logging, networking, wall clocks and the audio sink.

#[cfg(not(target_os = "vita"))]
mod host;
#[cfg(target_os = "vita")]
mod vita;

#[cfg(not(target_os = "vita"))]
pub use host::*;
#[cfg(target_os = "vita")]
pub use vita::*;

/// Result of bringing up the platform layer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(dead_code)] // WifiOff/Unavailable are only produced by the Vita build
pub enum NetStatus {
    /// Network stack is up and the console reports a Wi-Fi connection.
    Connected,
    /// Network stack is up but Wi-Fi is off / not associated.
    WifiOff,
    /// The network stack itself could not be started.
    Unavailable,
}
