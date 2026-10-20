//! On the Vita, the Spotify Connect volume is the console's own volume.
//!
//! [`SystemMixer`] turns Connect volume changes (the in-app slider, or a phone
//! controlling the Vita) into system volume steps and leaves the samples at
//! full scale, so the volume buttons and the slider are one control. The
//! engine polls the system volume to pick up button presses. If the console
//! refuses the change, the mixer falls back to software volume.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicU16, Ordering};
use std::sync::Mutex;

use librespot_core::Error;
use librespot_playback::mixer::{softmixer::SoftMixer, Mixer, MixerConfig, VolumeGetter};

use crate::platform;
use crate::util::now_ms_inner;

/// How long a volume read from the buttons can still come back from Spirc.
const ECHO_MS: u64 = 2000;

/// The system volume runs from 0 to this many steps.
pub const SYSTEM_STEPS: u32 = 30;

/// The system volume step closest to a Connect volume (0..=65535).
pub fn to_step(volume: u16) -> u32 {
    (volume as u32 * SYSTEM_STEPS + 32767) / 65535
}

/// The Connect volume for a system volume step.
pub fn from_step(step: u32) -> u16 {
    (step.min(SYSTEM_STEPS) * 65535 / SYSTEM_STEPS) as u16
}

/// The console volume as a Connect volume, if this platform has one.
pub fn system_volume() -> Option<u16> {
    platform::system_volume_step().map(from_step)
}

pub struct SystemMixer {
    soft: SoftMixer,
    volume: AtomicU16,
    system: AtomicBool,
    /// Volumes taken from the buttons, and when. Spirc hands each back to
    /// set_volume some time later; by then the buttons may have moved on, so
    /// writing it to the console would undo presses.
    from_system: Mutex<VecDeque<(u16, u64)>>,
}

impl SystemMixer {
    /// Adopt a volume read from the console (a button press) without writing
    /// it back, and remember it so Spirc's echo of it is ignored too.
    pub fn follow(&self, volume: u16) {
        self.volume.store(volume, Ordering::Relaxed);
        let now = now_ms_inner();
        let mut recent = self.from_system.lock().unwrap();
        recent.retain(|&(_, at)| now.saturating_sub(at) < ECHO_MS);
        recent.push_back((volume, now));
    }

    fn is_echo(&self, volume: u16) -> bool {
        let now = now_ms_inner();
        self.from_system
            .lock()
            .unwrap()
            .iter()
            .any(|&(v, at)| v == volume && now.saturating_sub(at) < ECHO_MS)
    }

    /// False once the console refused a change; volume is then in software.
    pub fn controls_system(&self) -> bool {
        self.system.load(Ordering::Relaxed)
    }
}

impl Mixer for SystemMixer {
    fn open(config: MixerConfig) -> Result<Self, Error> {
        Ok(Self {
            soft: SoftMixer::open(config)?,
            volume: AtomicU16::new(u16::MAX),
            system: AtomicBool::new(true),
            from_system: Mutex::new(VecDeque::new()),
        })
    }

    fn volume(&self) -> u16 {
        self.volume.load(Ordering::Relaxed)
    }

    fn set_volume(&self, volume: u16) {
        if self.controls_system() && self.is_echo(volume) {
            return;
        }
        self.volume.store(volume, Ordering::Relaxed);
        if self.controls_system() && platform::set_system_volume_step(to_step(volume)) {
            self.soft.set_volume(u16::MAX);
        } else {
            if self.system.swap(false, Ordering::Relaxed) {
                log::warn!("system volume can't be set; using software volume");
            }
            self.soft.set_volume(volume);
        }
    }

    fn get_soft_volume(&self) -> Box<dyn VolumeGetter + Send> {
        self.soft.get_soft_volume()
    }
}
