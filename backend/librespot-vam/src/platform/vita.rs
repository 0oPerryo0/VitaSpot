//! PS Vita platform glue: network bring-up, file logging and the
//! `sceAudioOut` based librespot sink.

use std::collections::VecDeque;
use std::os::raw::{c_int, c_void};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, Once, OnceLock};
use std::thread::JoinHandle;

use librespot_playback::audio_backend::{Sink, SinkError, SinkResult};
use librespot_playback::config::AudioFormat;
use librespot_playback::convert::Converter;
use librespot_playback::decoder::AudioPacket;

use super::NetStatus;

const SCE_SYSMODULE_NET: u32 = 0x0001;

/// Number of PCM frames per `sceAudioOut` buffer. Must be a multiple of 64.
const GRAIN_FRAMES: i32 = 1024;
/// Stereo signed 16 bit.
const BYTES_PER_FRAME: usize = 4;
/// One second of audio buffered between librespot's decoder and the output thread.
const RING_BYTES: usize = 44100 * BYTES_PER_FRAME;

#[repr(C)]
struct SceNetInitParam {
    memory: *mut c_void,
    size: c_int,
    flags: c_int,
}

extern "C" {
    fn sceSysmoduleLoadModule(id: u32) -> c_int;
    fn sceNetInit(param: *mut SceNetInitParam) -> c_int;
    fn sceNetTerm() -> c_int;
    fn sceNetCtlInit() -> c_int;
    fn sceNetCtlTerm();
    fn sceNetCtlInetGetState(state: *mut c_int) -> c_int;
}

extern "C" {
    fn sceAVConfigGetSystemVol(volume: *mut c_int) -> c_int;
    fn sceAVConfigSetSystemVol(volume: c_int) -> c_int;
}

/// The console's output volume (what the volume buttons change), 0..=30.
pub fn system_volume_step() -> Option<u32> {
    let mut volume: c_int = 0;
    let rc = unsafe { sceAVConfigGetSystemVol(&mut volume) };
    if rc < 0 {
        log::warn!("sceAVConfigGetSystemVol failed: {rc:#x}");
        return None;
    }
    Some(volume.max(0) as u32)
}

/// Set the console's output volume (0..=30). False if the console refused.
pub fn set_system_volume_step(step: u32) -> bool {
    let rc = unsafe { sceAVConfigSetSystemVol(step as c_int) };
    if rc < 0 {
        log::warn!("sceAVConfigSetSystemVol({step}) failed: {rc:#x}");
        return false;
    }
    true
}

extern "C" {
    fn sceKernelGetProcessTimeWide() -> u64;
    fn sceKernelChangeThreadPriority(thid: c_int, priority: c_int) -> c_int;
}

/// Thread priorities (lower number runs first; user range 64..=191).
/// pthreads default to 159, just above the UI thread's 160, so busy backend
/// work (TLS, protobuf parsing) used to preempt rendering mid-frame. The UI
/// raises itself to 150 in platform_vita.c; audio output must beat it.
const PRIORITY_AUDIO: c_int = 80;
const PRIORITY_BACKGROUND: c_int = 170;

fn set_current_thread_priority(priority: c_int, what: &str) {
    // Thread id 0 means the calling thread.
    let rc = unsafe { sceKernelChangeThreadPriority(0, priority) };
    if rc < 0 {
        log::warn!("{what}: sceKernelChangeThreadPriority({priority}) = {rc:#x}");
    }
}

/// Network and metadata threads: runnable work here must not stall the UI.
pub fn lower_current_thread_priority() {
    set_current_thread_priority(PRIORITY_BACKGROUND, "background thread");
}

extern "C" {
    fn sceAudioOutOpenPort(port_type: c_int, len: c_int, freq: c_int, mode: c_int) -> c_int;
    fn sceAudioOutReleasePort(port: c_int) -> c_int;
    fn sceAudioOutOutput(port: c_int, buf: *const c_void) -> c_int;
    fn sceAudioOutSetVolume(port: c_int, ch: c_int, vol: *mut c_int) -> c_int;
}

/// Keep the 1 MiB `sceNetInit` pool alive for the whole process.
static NET_POOL: Mutex<Option<Vec<u64>>> = Mutex::new(None);

pub fn now_ms() -> u64 {
    unsafe { sceKernelGetProcessTimeWide() / 1000 }
}

/// Bring up the Vita network stack and check that Wi-Fi is associated.
pub fn init_network(data_dir: &str) -> NetStatus {
    init_logging(data_dir);

    unsafe {
        let rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        log::info!("sceSysmoduleLoadModule(NET) = {rc:#x}");

        // 1 MiB pool, as recommended by the VitaSDK samples.
        let mut pool: Vec<u64> = vec![0; 1024 * 1024 / 8];
        let mut param = SceNetInitParam {
            memory: pool.as_mut_ptr() as *mut c_void,
            size: (pool.len() * 8) as c_int,
            flags: 0,
        };
        let rc = sceNetInit(&mut param);
        if rc < 0 {
            log::error!("sceNetInit failed: {rc:#x}");
            return NetStatus::Unavailable;
        }
        *NET_POOL.lock().unwrap_or_else(|e| e.into_inner()) = Some(pool);

        let rc = sceNetCtlInit();
        log::info!("sceNetCtlInit = {rc:#x}");

        if wifi_connected() {
            NetStatus::Connected
        } else {
            NetStatus::WifiOff
        }
    }
}

pub fn shutdown_network() {
    unsafe {
        sceNetCtlTerm();
        sceNetTerm();
    }
    *NET_POOL.lock().unwrap_or_else(|e| e.into_inner()) = None;
}

/// Ask the system whether the console is associated with a Wi-Fi network.
///
/// The VitaSDK header defines `SCE_NETCTL_STATE_CONNECTED = 3` for the value
/// returned by `sceNetCtlInetGetState`.
pub fn wifi_connected() -> bool {
    unsafe {
        let mut state: c_int = 0;
        let rc = sceNetCtlInetGetState(&mut state);
        if rc < 0 {
            log::warn!("sceNetCtlInetGetState failed: {rc:#x}");
            return false;
        }
        state == 3
    }
}

// ---------------------------------------------------------------------------
// Logging (writes to <data_dir>/vam.log so it can be inspected on hardware)
// ---------------------------------------------------------------------------

struct FileLogger {
    file: Mutex<Option<std::fs::File>>,
}

impl log::Log for FileLogger {
    fn enabled(&self, _metadata: &log::Metadata) -> bool {
        true
    }

    fn log(&self, record: &log::Record) {
        use std::io::Write;
        if let Ok(mut guard) = self.file.lock() {
            if let Some(file) = guard.as_mut() {
                let _ = writeln!(
                    file,
                    "[{}] {} {}: {}",
                    now_ms(),
                    record.level(),
                    record.target(),
                    record.args()
                );
            }
        }
    }

    fn flush(&self) {}
}

static LOGGER: OnceLock<FileLogger> = OnceLock::new();

pub fn init_logging(data_dir: &str) {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let path = format!("{}/vam.log", data_dir.trim_end_matches('/'));
        let file = std::fs::OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(&path)
            .ok();
        let logger = LOGGER.get_or_init(|| FileLogger {
            file: Mutex::new(file),
        });
        let _ = log::set_logger(logger);
        log::set_max_level(log::LevelFilter::Info);
        log::info!("VitaSpot backend starting (log: {path})");
    });
}

// ---------------------------------------------------------------------------
// Audio sink
// ---------------------------------------------------------------------------

struct RingState {
    bytes: VecDeque<u8>,
    closed: bool,
}

struct VitaSink {
    format: AudioFormat,
    ring: Arc<(Mutex<RingState>, Condvar)>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
    port: c_int,
}

impl VitaSink {
    fn new(format: AudioFormat) -> Self {
        VitaSink {
            format,
            ring: Arc::new((Mutex::new(RingState { bytes: VecDeque::new(), closed: false }), Condvar::new())),
            stop: Arc::new(AtomicBool::new(false)),
            thread: None,
            port: -1,
        }
    }

    fn push_bytes(&mut self, data: &[u8]) {
        let (lock, condvar) = &*self.ring;
        let mut ring = lock.lock().unwrap_or_else(|e| e.into_inner());
        let mut offset = 0usize;
        while offset < data.len() {
            while ring.bytes.len() >= RING_BYTES && !self.stop.load(Ordering::Relaxed) {
                ring = condvar
                    .wait_timeout(ring, std::time::Duration::from_millis(500))
                    .map(|(g, _)| g)
                    .unwrap_or_else(|e| e.into_inner().0);
            }
            if self.stop.load(Ordering::Relaxed) {
                return;
            }
            let space = RING_BYTES.saturating_sub(ring.bytes.len()).max(1);
            let take = space.min(data.len() - offset);
            ring.bytes.extend(&data[offset..offset + take]);
            offset += take;
        }
        condvar.notify_all();
    }
}

impl Sink for VitaSink {
    fn start(&mut self) -> SinkResult<()> {
        self.stop.store(false, Ordering::SeqCst);
        {
            // `stop()` closes the ring; reopen it, or the next output thread
            // sees `closed` and exits at once (silent playback).
            let (lock, _) = &*self.ring;
            let mut ring = lock.lock().unwrap_or_else(|e| e.into_inner());
            ring.closed = false;
            ring.bytes.clear();
        }
        if self.port < 0 {
            // SCE_AUDIO_OUT_PORT_TYPE_BGM = 1, SCE_AUDIO_OUT_MODE_STEREO = 1,
            // 44100 Hz signed 16 bit.
            let port = unsafe { sceAudioOutOpenPort(1, GRAIN_FRAMES, 44100, 1) };
            if port < 0 {
                return Err(SinkError::NotConnected(format!(
                    "sceAudioOutOpenPort failed: {port:#x}"
                )));
            }
            let mut volume = [32768 as c_int; 2]; // SCE_AUDIO_VOLUME_0DB
            let rc = unsafe { sceAudioOutSetVolume(port, 3 /* L|R */, volume.as_mut_ptr()) };
            if rc < 0 {
                log::warn!("sceAudioOutSetVolume failed: {rc:#x}");
            }
            self.port = port;
            log::info!("audio port opened: {port}");
        }

        if self.thread.is_none() {
            let ring = Arc::clone(&self.ring);
            let stop = Arc::clone(&self.stop);
            let port = self.port;
            let handle = std::thread::Builder::new()
                .name("vam-audio-out".into())
                .stack_size(256 * 1024)
                .spawn(move || audio_thread(port, ring, stop))
                .map_err(|e| SinkError::NotConnected(e.to_string()))?;
            self.thread = Some(handle);
        }
        Ok(())
    }

    fn stop(&mut self) -> SinkResult<()> {
        self.stop.store(true, Ordering::SeqCst);
        {
            let (lock, condvar) = &*self.ring;
            let mut ring = lock.lock().unwrap_or_else(|e| e.into_inner());
            ring.closed = true;
            condvar.notify_all();
        }
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
        if self.port >= 0 {
            unsafe { sceAudioOutReleasePort(self.port) };
            log::info!("audio port released: {}", self.port);
            self.port = -1;
        }
        Ok(())
    }

    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        match packet {
            AudioPacket::Samples(samples) => match self.format {
                AudioFormat::S16 => {
                    let samples = converter.f64_to_s16(&samples);
                    let bytes = unsafe {
                        std::slice::from_raw_parts(
                            samples.as_ptr() as *const u8,
                            samples.len() * std::mem::size_of::<i16>(),
                        )
                    };
                    self.push_bytes(bytes);
                }
                other => {
                    return Err(SinkError::InvalidParams(format!(
                        "Vita sink only supports S16, got {other:?}"
                    )))
                }
            },
            AudioPacket::Raw(raw) => self.push_bytes(&raw),
        }
        Ok(())
    }
}

fn audio_thread(port: c_int, ring: Arc<(Mutex<RingState>, Condvar)>, stop: Arc<AtomicBool>) {
    set_current_thread_priority(PRIORITY_AUDIO, "audio thread");
    let need = GRAIN_FRAMES as usize * BYTES_PER_FRAME;
    let mut buffer = vec![0u8; need];
    // Underruns: the ring ran dry after playback had started, i.e. the
    // decoder could not keep up. Heard as dropouts or crackles.
    let mut started = false;
    let mut underruns: u32 = 0;
    loop {
        if stop.load(Ordering::SeqCst) {
            break;
        }
        {
            let (lock, condvar) = &*ring;
            let mut guard = lock.lock().unwrap_or_else(|e| e.into_inner());
            if started && guard.bytes.len() < need && !guard.closed {
                underruns += 1;
                if underruns.is_power_of_two() {
                    log::warn!("audio underrun #{underruns} (decoder too slow?)");
                }
            }
            while guard.bytes.len() < need && !guard.closed && !stop.load(Ordering::Relaxed) {
                guard = condvar
                    .wait_timeout(guard, std::time::Duration::from_millis(100))
                    .map(|(g, _)| g)
                    .unwrap_or_else(|e| e.into_inner().0);
            }
            if guard.closed || stop.load(Ordering::Relaxed) {
                break;
            }
            if guard.bytes.len() < need {
                continue;
            }
            for slot in buffer.iter_mut() {
                *slot = guard.bytes.pop_front().unwrap_or(0);
            }
            started = true;
            condvar.notify_all();
        }
        let rc = unsafe { sceAudioOutOutput(port, buffer.as_ptr() as *const c_void) };
        if rc < 0 {
            log::error!("sceAudioOutOutput failed: {rc:#x}");
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }
    log::info!("audio output thread stopped");
}

/// Create the Vita audio sink. librespot always requests S16 output here.
pub fn make_sink() -> Box<dyn Sink> {
    Box::new(VitaSink::new(AudioFormat::S16))
}

/// Release the audio port at shutdown (the sink owns it while playing).
pub fn shutdown_audio() {}
