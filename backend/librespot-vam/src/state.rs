//! Process-wide backend state: configuration, event queue, snapshots.

use std::collections::{HashSet, VecDeque};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread::JoinHandle;

use tokio::sync::mpsc::UnboundedSender;

use crate::abi::*;
use crate::engine::Command;
use crate::model::{free_image, free_page};
use crate::util::copy_str;

#[derive(Debug, Clone)]
pub struct Config {
    pub device_name: String,
    pub data_dir: String,
    pub bitrate: i32,
    pub initial_volume: u16,
    pub normalize_volume: bool,
}

impl Config {
    /// Absolute-ish path below the data dir, with forward slashes.
    pub fn path(&self, rel: &str) -> String {
        let mut dir = self.data_dir.clone();
        if !dir.ends_with('/') && !dir.ends_with('\\') {
            dir.push('/');
        }
        format!("{dir}{rel}")
    }

    pub fn cache_audio_dir(&self) -> String {
        self.path("cache/audio")
    }

    pub fn cache_img_dir(&self) -> String {
        self.path("cache/img")
    }

    pub fn credentials_path(&self) -> String {
        self.path("credentials.json")
    }
}

/// Queue of events waiting to be polled by the UI.
///
/// Also tracks cancellation: `vam_cancel` may race with a worker that has
/// already produced a reply. Replies for cancelled ids are freed instead of
/// queued, and ids cancelled after queueing are dropped at poll time.
#[derive(Default)]
pub struct EventQueue {
    inner: Mutex<EventQueueInner>,
}

#[derive(Default)]
struct EventQueueInner {
    events: VecDeque<vam_event>,
    cancelled: HashSet<u32>,
}

impl EventQueue {
    pub fn push(&self, mut event: vam_event) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if event.request_id != 0 && inner.cancelled.remove(&event.request_id) {
            // SAFETY: event owns backend allocations.
            unsafe { release_event(&mut event) };
            return;
        }
        inner.events.push_back(event);
    }

    pub fn push_simple(&self, type_: i32, result: i32, message: &str, uri: &str) {
        let mut event = vam_event::default();
        event.type_ = type_;
        event.result = result;
        copy_str(&mut event.message, message);
        copy_str(&mut event.uri, uri);
        self.push(event);
    }

    pub fn poll(&self) -> Option<vam_event> {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        while let Some(mut event) = inner.events.pop_front() {
            if event.request_id != 0 && inner.cancelled.remove(&event.request_id) {
                // SAFETY: event owns backend allocations.
                unsafe { release_event(&mut event) };
                continue;
            }
            return Some(event);
        }
        None
    }

    pub fn cancel(&self, request_id: u32) {
        if request_id == 0 {
            return;
        }
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(pos) = inner
            .events
            .iter()
            .position(|e| e.request_id == request_id)
        {
            if let Some(mut event) = inner.events.remove(pos) {
                // SAFETY: event owns backend allocations.
                unsafe { release_event(&mut event) };
            }
        } else {
            inner.cancelled.insert(request_id);
        }
    }

    pub fn is_cancelled(&self, request_id: u32) -> bool {
        self.inner
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .cancelled
            .contains(&request_id)
    }

    /// Drop a cancellation marker once a worker has observed it.
    pub fn clear_cancelled(&self, request_id: u32) {
        self.inner
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .cancelled
            .remove(&request_id);
    }

    pub fn release_all(&self) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        for mut event in inner.events.drain(..) {
            // SAFETY: event owns backend allocations.
            unsafe { release_event(&mut event) };
        }
        inner.cancelled.clear();
    }
}

/// # Safety
/// `event` must be a valid event produced by this backend and not released yet.
pub unsafe fn release_event(event: &mut vam_event) {
    if !event.page.is_null() {
        free_page(event.page);
        event.page = std::ptr::null_mut();
    }
    if !event.data.is_null() {
        free_image(event.data, event.data_size);
        event.data = std::ptr::null();
        event.data_size = 0;
    }
}

pub struct Backend {
    pub config: Config,
    pub events: EventQueue,
    pub auth: Mutex<vam_auth_state>,
    pub player: Mutex<vam_player_state>,
    next_request_id: AtomicU32,
    pub commands: Mutex<Option<UnboundedSender<Command>>>,
    pub runtime_thread: Mutex<Option<JoinHandle<()>>>,
    pub shutdown: AtomicBool,
}

impl Backend {
    pub fn new(config: Config) -> Arc<Self> {
        let mut auth = vam_auth_state::default();
        auth.status = VAM_AUTH_STARTING;
        copy_str(&mut auth.device_name, &config.device_name);

        let mut player = vam_player_state::default();
        player.volume = config.initial_volume;
        player.is_active_device = 1;

        Arc::new(Backend {
            config,
            events: EventQueue::default(),
            auth: Mutex::new(auth),
            player: Mutex::new(player),
            next_request_id: AtomicU32::new(1),
            commands: Mutex::new(None),
            runtime_thread: Mutex::new(None),
            shutdown: AtomicBool::new(false),
        })
    }

    /// Allocate the next non-zero request id.
    pub fn next_request_id(&self) -> u32 {
        loop {
            let id = self.next_request_id.fetch_add(1, Ordering::Relaxed);
            if id != 0 {
                return id;
            }
        }
    }

    pub fn command_sender(&self) -> Option<UnboundedSender<Command>> {
        self.commands
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }

    pub fn send_command(&self, command: Command) -> bool {
        match self.command_sender() {
            Some(tx) => tx.send(command).is_ok(),
            None => false,
        }
    }

    pub fn auth_snapshot(&self) -> vam_auth_state {
        *self.auth.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn player_snapshot(&self) -> vam_player_state {
        *self.player.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn set_auth(&self, update: impl FnOnce(&mut vam_auth_state)) {
        {
            let mut auth = self.auth.lock().unwrap_or_else(|e| e.into_inner());
            update(&mut auth);
        }
        self.events.push_simple(VAM_EV_AUTH_CHANGED, VAM_OK, "", "");
    }

    pub fn set_auth_status(&self, status: i32, message: &str) {
        self.set_auth(|auth| {
            auth.status = status;
            copy_str(&mut auth.message, message);
        });
    }

    pub fn set_player(&self, update: impl FnOnce(&mut vam_player_state)) {
        {
            let mut player = self.player.lock().unwrap_or_else(|e| e.into_inner());
            update(&mut player);
        }
        self.events.push_simple(VAM_EV_PLAYER_CHANGED, VAM_OK, "", "");
    }

    pub fn emit_error(&self, message: &str) {
        self.events
            .push_simple(VAM_EV_ERROR, VAM_ERR_UNKNOWN, message, "");
    }

    pub fn emit_toast(&self, message: &str) {
        self.events
            .push_simple(VAM_EV_TOAST, VAM_OK, message, "");
    }
}

pub static BACKEND: OnceLock<Arc<Backend>> = OnceLock::new();

pub fn backend() -> Option<Arc<Backend>> {
    BACKEND.get().cloned()
}
