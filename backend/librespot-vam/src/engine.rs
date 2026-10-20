//! The backend engine: owns the librespot session, Spirc, player and all
//! asynchronous work. Everything in this module runs inside the dedicated
//! tokio runtime, never on the UI thread.

use std::sync::Arc;
use std::time::Duration;

use futures_util::StreamExt;
use librespot_connect::{
    ConnectConfig, ConnectSnapshot, LoadContextOptions, LoadRequest, LoadRequestOptions,
    Options as LoadOptions, PlayingTrack, Spirc,
};
use librespot_core::{
    authentication::Credentials, cache::Cache, config::DeviceType, Session, SessionConfig,
};
use librespot_discovery::Discovery;
use librespot_playback::{
    config::{Bitrate, PlayerConfig, VolumeCtrl},
    mixer::{softmixer::SoftMixer, Mixer, MixerConfig},
    player::{Player, PlayerEvent, PlayerEventChannel},
};
use tokio::sync::{mpsc::UnboundedReceiver, oneshot};

use crate::abi::*;
use crate::images;
use crate::internal;
use crate::metadata;
use crate::model::Item;
use crate::pages;
use crate::platform;
use crate::saved;
use crate::shared::{PlayerMeta, Shared};
use crate::state::Backend;
use crate::util::{copy_str, now_ms_inner, sha1_hex};

/// Commands sent from the C API (UI thread) into the engine task.
#[derive(Debug)]
pub enum Command {
    Shutdown,
    Logout,
    Play {
        context: String,
        track: String,
        index: u32,
    },
    Pause,
    Resume,
    Next,
    Prev,
    Seek(u32),
    SetShuffle(bool),
    SetRepeat(i32),
    SetVolume(u16),
    AddToQueue(String),
    SetSaved {
        request_id: u32,
        uri: String,
        saved: bool,
    },
    RequestPage {
        request_id: u32,
        uri: String,
        offset: u32,
        limit: u32,
    },
    RequestImage {
        request_id: u32,
        url: String,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Phase {
    Starting,
    WaitingForCredentials,
    LoggedIn,
}

enum Tick {
    Command(Option<Command>),
    Credentials(Option<Credentials>),
    PlayerEvent(Option<PlayerEvent>),
    SpircDone,
    Fast,
    Slow,
    Volume,
}

const AUDIO_CACHE_BYTES: u64 = 200 * 1024 * 1024;
const PLAYER_TICK: Duration = Duration::from_millis(100);
const VOLUME_TICK: Duration = Duration::from_millis(50);
const SLOW_TICK: Duration = Duration::from_secs(1);
const SUSPEND_GAP_MS: u64 = 30_000;
// Switching repeat takes two Spirc calls (clear one flag, set the other) and
// each reports its own state, so the button would blink through the
// intermediate modes. Show the requested mode for this long instead.
const REPEAT_SETTLE_MS: u64 = 1000;

pub async fn run(shared: Arc<Shared>) {
    let backend = shared.backend.clone();
    let (tx, rx) = tokio::sync::mpsc::unbounded_channel();
    *backend
        .commands
        .lock()
        .unwrap_or_else(|e| e.into_inner()) = Some(tx);

    let mut engine = Engine::new(shared, rx).await;
    engine.main_loop().await;
    if let Some(discovery) = engine.discovery.take() {
        // libmdns can take a while to unregister the service; never let
        // shutdown hang the host process.
        let _ = tokio::time::timeout(Duration::from_secs(2), discovery.shutdown()).await;
    }
    engine.teardown().await;
    *backend
        .commands
        .lock()
        .unwrap_or_else(|e| e.into_inner()) = None;
    log::info!("engine stopped");
}

struct Engine {
    shared: Arc<Shared>,
    backend: Arc<Backend>,
    commands: UnboundedReceiver<Command>,
    phase: Phase,
    cache: Option<Cache>,
    session: Option<Session>,
    spirc: Option<Spirc>,
    player: Option<Arc<Player>>,
    mixer: Option<Arc<dyn Mixer>>,
    player_events: Option<PlayerEventChannel>,
    spirc_done: Option<oneshot::Receiver<()>>,
    discovery: Option<Discovery>,
    credentials: Option<Credentials>,
    last_snapshot: ConnectSnapshot,
    last_credentials_marker: Option<String>,
    pending_volume: Option<u16>,
    /// Requested repeat mode and the time (ms) until which it overrides Spirc.
    pending_repeat: Option<(i32, u64)>,
    reconnect_at: Option<tokio::time::Instant>,
    discovery_retry_at: Option<tokio::time::Instant>,
    last_wifi: bool,
    last_tick_ms: u64,
    shutting_down: bool,
}

impl Engine {
    async fn new(shared: Arc<Shared>, commands: UnboundedReceiver<Command>) -> Self {
        let backend = shared.backend.clone();
        let config = backend.config.clone();

        // Make sure the directory tree exists before any worker touches it.
        let _ = std::fs::create_dir_all(&config.data_dir);
        let _ = std::fs::create_dir_all(config.cache_audio_dir());
        let _ = std::fs::create_dir_all(config.cache_img_dir());
        let _ = std::fs::create_dir_all(config.path("tmp"));

        let cache = Cache::new(
            Some(config.data_dir.clone()),
            Some(config.data_dir.clone()),
            Some(config.cache_audio_dir()),
            Some(AUDIO_CACHE_BYTES),
        )
        .map_err(|e| log::warn!("could not open librespot cache: {e}"))
        .ok();

        let wifi = platform::wifi_connected();
        if !wifi {
            backend.set_auth(|auth| {
                auth.status = VAM_AUTH_LOGGED_OUT;
                copy_str(&mut auth.message, "Wi-Fi is off. Connect to a network to sign in.");
            });
        }

        let mut engine = Engine {
            shared,
            backend,
            commands,
            phase: Phase::Starting,
            cache,
            session: None,
            spirc: None,
            player: None,
            mixer: None,
            player_events: None,
            spirc_done: None,
            discovery: None,
            credentials: None,
            last_snapshot: ConnectSnapshot::default(),
            last_credentials_marker: None,
            pending_volume: None,
            pending_repeat: None,
            reconnect_at: None,
            discovery_retry_at: None,
            last_wifi: wifi,
            last_tick_ms: now_ms_inner(),
            shutting_down: false,
        };

        // Auto-login with stored credentials, otherwise advertise over
        // Zeroconf and wait for the phone app to send credentials.
        let stored = engine.cache.as_ref().and_then(|c| c.credentials());
        match stored {
            Some(credentials) => {
                engine.last_credentials_marker = Some(marker(&credentials));
                engine.phase = Phase::WaitingForCredentials;
                engine.start_discovery();
                if let Err(why) = engine.login(credentials).await {
                    log::warn!("cached login failed: {why}");
                    engine.phase = Phase::WaitingForCredentials;
                    engine
                        .backend
                        .set_auth_status(VAM_AUTH_ERROR, &format!("Sign-in failed: {why}"));
                }
            }
            None => {
                engine.phase = Phase::WaitingForCredentials;
                engine.start_discovery();
                if engine.discovery.is_some() {
                    engine.backend.set_auth(|auth| {
                        auth.status = VAM_AUTH_LOGGED_OUT;
                        auth.zeroconf_active = 1;
                        copy_str(
                            &mut auth.message,
                            "Open Spotify on your phone, tap the device picker and choose this device.",
                        );
                    });
                }
            }
        }

        engine
    }

    async fn main_loop(&mut self) {
        let mut fast = tokio::time::interval(PLAYER_TICK);
        let mut slow = tokio::time::interval(SLOW_TICK);
        let mut volume = tokio::time::interval(VOLUME_TICK);
        fast.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        slow.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        volume.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);

        loop {
            let tick = {
                let commands = &mut self.commands;
                let discovery = &mut self.discovery;
                let player_events = &mut self.player_events;
                let spirc_done = &mut self.spirc_done;

                tokio::select! {
                    cmd = commands.recv() => Tick::Command(cmd),
                    cred = async {
                        match discovery.as_mut() {
                            Some(discovery) => discovery.next().await,
                            None => std::future::pending::<Option<Credentials>>().await,
                        }
                    } => Tick::Credentials(cred),
                    event = async {
                        match player_events.as_mut() {
                            Some(events) => events.recv().await,
                            None => std::future::pending::<Option<PlayerEvent>>().await,
                        }
                    } => Tick::PlayerEvent(event),
                    _ = async {
                        match spirc_done.as_mut() {
                            Some(done) => done.await.ok(),
                            None => std::future::pending::<Option<()>>().await,
                        }
                    } => Tick::SpircDone,
                    _ = fast.tick() => Tick::Fast,
                    _ = slow.tick() => Tick::Slow,
                    _ = volume.tick() => Tick::Volume,
                }
            };

            let keep_running = match tick {
                Tick::Command(Some(command)) => self.handle_command(command).await,
                Tick::Command(None) => false,
                Tick::Credentials(Some(credentials)) => {
                    self.handle_discovered_credentials(credentials).await;
                    true
                }
                Tick::Credentials(None) => {
                    log::warn!("Zeroconf discovery stopped");
                    self.discovery = None;
                    self.shared.backend.set_auth(|auth| {
                        auth.zeroconf_active = 0;
                    });
                    self.discovery_retry_at =
                        Some(tokio::time::Instant::now() + Duration::from_secs(10));
                    true
                }
                Tick::PlayerEvent(Some(event)) => {
                    self.handle_player_event(event);
                    true
                }
                Tick::PlayerEvent(None) => {
                    self.player_events = None;
                    true
                }
                Tick::SpircDone => {
                    log::warn!("connect session ended");
                    if self.phase == Phase::LoggedIn && !self.shutting_down {
                        self.reconnect("The connection to Spotify was lost.");
                    }
                    true
                }
                Tick::Fast => {
                    let settled = matches!(self.pending_repeat, Some((_, until)) if now_ms_inner() >= until);
                    if settled {
                        self.pending_repeat = None;
                    }
                    self.apply_snapshot(settled);
                    true
                }
                Tick::Slow => {
                    self.slow_tick().await;
                    true
                }
                Tick::Volume => {
                    self.flush_volume();
                    true
                }
            };

            if !keep_running {
                break;
            }
        }
    }

    // ------------------------------------------------------------------
    // Command dispatch
    // ------------------------------------------------------------------

    async fn handle_command(&mut self, command: Command) -> bool {
        match command {
            Command::Shutdown => {
                self.shutting_down = true;
                return false;
            }
            Command::Logout => self.logout().await,
            Command::RequestPage {
                request_id,
                uri,
                offset,
                limit,
            } => self.spawn_page(request_id, uri, offset, limit),
            Command::RequestImage { request_id, url } => self.spawn_image(request_id, url),
            Command::SetSaved {
                request_id,
                uri,
                saved,
            } => self.spawn_set_saved(request_id, uri, saved),
            Command::SetVolume(volume) => {
                self.pending_volume = Some(volume);
                self.optimistic_volume(volume);
            }
            Command::Play {
                context,
                track,
                index,
            } => {
                self.optimistic_play(&context);
                self.spawn_play(context, track, index);
            }
            Command::Pause => {
                self.optimistic_pause();
                if let Some(spirc) = self.spirc.clone() {
                    let backend = self.backend.clone();
                    tokio::spawn(async move {
                        if let Err(why) = spirc.pause() {
                            backend.emit_error(&format!("Couldn't pause: {why}"));
                        }
                    });
                }
            }
            Command::Resume => {
                self.optimistic_resume();
                self.spawn_simple_spirc(true, |spirc| spirc.play());
            }
            Command::Next => self.spawn_simple_spirc(true, |spirc| spirc.next()),
            Command::Prev => self.spawn_simple_spirc(true, |spirc| spirc.prev()),
            Command::Seek(position) => {
                log::info!(
                    "seek to {position} ms requested (active: {})",
                    self.last_snapshot.is_active
                );
                self.optimistic_seek(position);
                self.spawn_simple_spirc(true, move |spirc| spirc.set_position_ms(position));
            }
            Command::SetShuffle(enabled) => {
                self.backend.set_player(|state| {
                    state.shuffle = enabled as u8;
                    state.seq = state.seq.wrapping_add(1);
                });
                self.spawn_simple_spirc(false, move |spirc| spirc.shuffle(enabled));
            }
            Command::SetRepeat(mode) => {
                self.pending_repeat = Some((mode, now_ms_inner() + REPEAT_SETTLE_MS));
                self.backend.set_player(|state| {
                    state.repeat = mode;
                    state.seq = state.seq.wrapping_add(1);
                });
                self.spawn_simple_spirc(false, move |spirc| {
                    // Repeat modes are mutually exclusive; make sure the other
                    // flag is cleared when switching between them.
                    match mode {
                        VAM_REPEAT_TRACK => {
                            spirc.repeat(false)?;
                            spirc.repeat_track(true)
                        }
                        VAM_REPEAT_CONTEXT => {
                            spirc.repeat_track(false)?;
                            spirc.repeat(true)
                        }
                        _ => {
                            spirc.repeat_track(false)?;
                            spirc.repeat(false)
                        }
                    }
                });
            }
            Command::AddToQueue(track_uri) => self.spawn_add_to_queue(track_uri),
        }
        true
    }

    fn spawn_simple_spirc<F>(&self, transfer: bool, action: F)
    where
        F: FnOnce(&Spirc) -> Result<(), librespot_core::Error> + Send + 'static,
    {
        let Some(spirc) = self.spirc.clone() else {
            self.backend.emit_error("Not signed in to Spotify.");
            return;
        };
        let backend = self.backend.clone();
        let needs_transfer = transfer && !self.last_snapshot.is_active;
        tokio::spawn(async move {
            if needs_transfer && !ensure_active(&spirc).await {
                backend.emit_toast("Another Spotify device is controlling playback.");
            }
            if let Err(why) = action(&spirc) {
                backend.emit_error(&format!("Playback command failed: {why}"));
            }
        });
    }

    fn spawn_play(&self, context: String, track: String, index: u32) {
        let Some(spirc) = self.spirc.clone() else {
            self.backend.emit_error("Not signed in to Spotify.");
            return;
        };
        let backend = self.backend.clone();
        let shared = self.shared.clone();
        let snapshot = self.last_snapshot.clone();
        let options = LoadOptions {
            shuffle: snapshot.shuffle,
            repeat: snapshot.repeat_context,
            repeat_track: snapshot.repeat_track,
        };
        let seeking_track = if !track.is_empty() {
            Some(PlayingTrack::Uri(track.clone()))
        } else {
            Some(PlayingTrack::Index(index))
        };
        tokio::spawn(async move {
            if !activate_for_load(&spirc).await {
                backend.emit_toast("Another Spotify device is controlling playback.");
                backend.set_player(|state| {
                    state.playback = VAM_PLAYBACK_STOPPED;
                    state.has_track = 0;
                    state.seq = state.seq.wrapping_add(1);
                });
                return;
            }
            let request = if context.is_empty() {
                if track.is_empty() {
                    backend.emit_error("Nothing to play.");
                    return;
                }
                LoadRequest::from_tracks(
                    vec![track.clone()],
                    LoadRequestOptions {
                        start_playing: true,
                        seek_to: 0,
                        context_options: Some(LoadContextOptions::Options(options)),
                        playing_track: Some(PlayingTrack::Uri(track)),
                    },
                )
            } else {
                LoadRequest::from_context_uri(
                    context,
                    LoadRequestOptions {
                        start_playing: true,
                        seek_to: 0,
                        context_options: Some(LoadContextOptions::Options(options)),
                        playing_track: seeking_track,
                    },
                )
            };
            if let Err(why) = spirc.load(request) {
                backend.emit_error(&format!("Couldn't start playback: {why}"));
                backend.set_player(|state| {
                    state.playback = VAM_PLAYBACK_STOPPED;
                    state.has_track = 0;
                    state.seq = state.seq.wrapping_add(1);
                });
                return;
            }
            let _ = &shared;
        });
    }

    fn spawn_add_to_queue(&self, track_uri: String) {
        let Some(spirc) = self.spirc.clone() else {
            self.backend.emit_error("Not signed in to Spotify.");
            return;
        };
        let backend = self.backend.clone();
        tokio::spawn(async move {
            if !ensure_active(&spirc).await {
                backend.emit_toast("Another Spotify device is controlling playback.");
                return;
            }
            if let Err(why) = spirc.add_to_queue(track_uri) {
                backend.emit_error(&format!("Couldn't add to the queue: {why}"));
            }
        });
    }

    fn spawn_page(&self, request_id: u32, uri: String, offset: u32, limit: u32) {
        let shared = self.shared.clone();
        let backend = self.backend.clone();
        tokio::spawn(async move {
            if backend.events.is_cancelled(request_id) {
                backend.events.clear_cancelled(request_id);
                return;
            }
            let result = pages::build_page(&shared, &uri, offset, limit).await;
            let mut event = vam_event::default();
            event.type_ = VAM_EV_PAGE;
            event.request_id = request_id;
            copy_str(&mut event.uri, &uri);
            match result {
                Ok(page) => {
                    event.result = VAM_OK;
                    event.page = page.into_raw();
                }
                Err(why) => {
                    event.result = why.result;
                    copy_str(&mut event.message, &why.message);
                }
            }
            backend.events.push(event);
        });
    }

    fn spawn_image(&self, request_id: u32, url: String) {
        let shared = self.shared.clone();
        let backend = self.backend.clone();
        tokio::spawn(async move {
            if backend.events.is_cancelled(request_id) {
                backend.events.clear_cancelled(request_id);
                return;
            }
            let mut event = vam_event::default();
            event.type_ = VAM_EV_IMAGE;
            event.request_id = request_id;
            match images::fetch_image(&shared, &url).await {
                Ok(bytes) => {
                    let boxed = bytes.into_boxed_slice();
                    event.result = VAM_OK;
                    event.data = boxed.as_ptr();
                    event.data_size = boxed.len() as u32;
                    std::mem::forget(boxed);
                }
                Err(why) => {
                    event.result = why.result;
                    copy_str(&mut event.message, &why.message);
                }
            }
            backend.events.push(event);
        });
    }

    fn spawn_set_saved(&self, request_id: u32, uri: String, saved: bool) {
        let shared = self.shared.clone();
        let backend = self.backend.clone();
        tokio::spawn(async move {
            if backend.events.is_cancelled(request_id) {
                backend.events.clear_cancelled(request_id);
                return;
            }
            let result =
                saved::set_saved(&shared.web, &shared.saved, &uri, saved).await;
            let mut event = vam_event::default();
            event.type_ = VAM_EV_REQUEST_DONE;
            event.request_id = request_id;
            event.result = match &result {
                Ok(()) => VAM_OK,
                Err(why) => why.result,
            };
            copy_str(&mut event.uri, &uri);
            if let Err(why) = &result {
                copy_str(&mut event.message, &why.message);
            }
            backend.events.push(event);

            if result.is_ok() {
                let mut changed = vam_event::default();
                changed.type_ = VAM_EV_SAVED_CHANGED;
                changed.uri = [0; VAM_URI_MAX];
                copy_str(&mut changed.uri, &uri);
                changed.saved = saved as u8;
                backend.events.push(changed);
                // Keep the current player track's heart in sync.
                backend.set_player(|state| {
                    let current = cstr_lossy(&state.track.uri);
                    if current == uri {
                        state.track.is_saved = saved as u8;
                        state.seq = state.seq.wrapping_add(1);
                    }
                });
            }
        });
    }

    // ------------------------------------------------------------------
    // Optimistic local state updates
    // ------------------------------------------------------------------

    fn optimistic_volume(&self, volume: u16) {
        let backend = self.backend.clone();
        backend.set_player(move |state| {
            if state.volume != volume {
                state.volume = volume;
                state.seq = state.seq.wrapping_add(1);
            }
        });
    }

    fn optimistic_play(&self, context: &str) {
        let context = context.to_string();
        // `has_track` stays as is: there is no track metadata until the
        // player reports TrackChanged.
        self.backend.set_player(move |state| {
            state.playback = VAM_PLAYBACK_BUFFERING;
            state.position_ms = 0;
            state.position_timestamp_ms = now_ms_inner();
            copy_str(&mut state.context_uri, &context);
            state.seq = state.seq.wrapping_add(1);
        });
    }

    fn optimistic_pause(&self) {
        self.backend.set_player(|state| {
            if state.playback == VAM_PLAYBACK_PLAYING {
                state.position_ms = extrapolated_position(state);
                state.position_timestamp_ms = now_ms_inner();
            }
            state.playback = VAM_PLAYBACK_PAUSED;
            state.seq = state.seq.wrapping_add(1);
        });
    }

    fn optimistic_resume(&self) {
        self.backend.set_player(|state| {
            state.playback = VAM_PLAYBACK_PLAYING;
            state.position_timestamp_ms = now_ms_inner();
            state.seq = state.seq.wrapping_add(1);
        });
    }

    fn optimistic_seek(&self, position: u32) {
        self.backend.set_player(|state| {
            state.position_ms = position;
            state.position_timestamp_ms = now_ms_inner();
            state.seq = state.seq.wrapping_add(1);
        });
    }

    // ------------------------------------------------------------------
    // Player state
    // ------------------------------------------------------------------

    fn handle_player_event(&mut self, event: PlayerEvent) {
        match event {
            PlayerEvent::TrackChanged { audio_item } => {
                let item = metadata::item_from_audio(&audio_item);
                self.shared.meta.put(item.clone());
                let track_uri = item.uri.clone();
                self.shared.update_player(|meta| {
                    meta.track_uri = track_uri.clone();
                    meta.track = Some(item.clone());
                });
                if let Some(spirc) = &self.spirc {
                    let snapshot = spirc.snapshot();
                    self.shared.update_player(|meta| {
                        meta.context_uri = snapshot.context_uri.clone();
                        if meta.context_title.is_empty() {
                            meta.context_title = context_title_from(&snapshot);
                        }
                    });
                }
                self.backend.set_player(move |state| {
                    state.has_track = 1;
                    state.track = item.to_c();
                    state.position_timestamp_ms = now_ms_inner();
                    state.seq = state.seq.wrapping_add(1);
                });
            }
            PlayerEvent::Loading { position_ms, .. } => {
                self.backend.set_player(move |state| {
                    state.playback = VAM_PLAYBACK_BUFFERING;
                    state.position_ms = position_ms;
                    state.position_timestamp_ms = now_ms_inner();
                    state.seq = state.seq.wrapping_add(1);
                });
            }
            PlayerEvent::Playing { position_ms, .. }
            | PlayerEvent::PositionCorrection { position_ms, .. } => {
                self.backend.set_player(move |state| {
                    state.playback = VAM_PLAYBACK_PLAYING;
                    state.position_ms = position_ms;
                    state.position_timestamp_ms = now_ms_inner();
                    state.seq = state.seq.wrapping_add(1);
                });
            }
            PlayerEvent::Paused { position_ms, .. } => {
                self.backend.set_player(move |state| {
                    state.playback = VAM_PLAYBACK_PAUSED;
                    state.position_ms = position_ms;
                    state.position_timestamp_ms = now_ms_inner();
                    state.seq = state.seq.wrapping_add(1);
                });
            }
            PlayerEvent::Seeked { position_ms, .. } => {
                log::info!("player seeked to {position_ms} ms");
                self.backend.set_player(move |state| {
                    state.position_ms = position_ms;
                    state.position_timestamp_ms = now_ms_inner();
                    state.seq = state.seq.wrapping_add(1);
                });
            }
            PlayerEvent::Stopped { .. } => {
                self.backend.set_player(|state| {
                    state.playback = VAM_PLAYBACK_STOPPED;
                    state.seq = state.seq.wrapping_add(1);
                });
            }
            PlayerEvent::VolumeChanged { volume } => self.optimistic_volume(volume),
            PlayerEvent::ShuffleChanged { shuffle } => {
                self.backend.set_player(move |state| {
                    state.shuffle = shuffle as u8;
                    state.seq = state.seq.wrapping_add(1);
                });
            }
            PlayerEvent::RepeatChanged { context, track } => {
                let mode = self.shown_repeat(repeat_mode(context, track));
                self.backend.set_player(move |state| {
                    state.repeat = mode;
                    state.seq = state.seq.wrapping_add(1);
                });
            }
            PlayerEvent::Unavailable { track_id, .. } => {
                log::warn!("track unavailable: {}", track_id.to_uri().unwrap_or_default());
                self.backend
                    .emit_error("That track isn't available. Skipping to the next one.");
            }
            PlayerEvent::EndOfTrack { .. } => {}
            PlayerEvent::TimeToPreloadNextTrack { .. } => {}
            PlayerEvent::PlayRequestIdChanged { .. } => {}
            PlayerEvent::Preloading { .. } => {}
            PlayerEvent::SessionConnected { .. } => {
                log::info!("session connected");
            }
            PlayerEvent::SessionDisconnected { .. } => {
                log::warn!("session disconnected");
            }
            PlayerEvent::SessionClientChanged { client_name, .. } => {
                log::info!("remote client changed: {client_name}");
            }
            PlayerEvent::AutoPlayChanged { .. } => {}
            PlayerEvent::FilterExplicitContentChanged { .. } => {}
            PlayerEvent::PositionChanged { position_ms, .. } => {
                self.backend.set_player(move |state| {
                    state.position_ms = position_ms;
                    state.position_timestamp_ms = now_ms_inner();
                });
            }
        }
    }

    fn apply_snapshot(&mut self, force: bool) {
        let Some(spirc) = self.spirc.clone() else {
            return;
        };
        let snapshot = spirc.snapshot();
        if !force && !snapshot_changed(&snapshot, &self.last_snapshot) {
            return;
        }
        self.last_snapshot = snapshot.clone();
        self.update_from_snapshot(snapshot);
    }

    /// The repeat mode to show: a just-requested one while Spirc settles.
    fn shown_repeat(&self, reported: i32) -> i32 {
        match self.pending_repeat {
            Some((mode, until)) if now_ms_inner() < until => mode,
            _ => reported,
        }
    }

    fn update_from_snapshot(&self, snapshot: ConnectSnapshot) {
        let now = now_ms_inner();
        let current = self.backend.player_snapshot();
        let mut next = current;

        // Context.
        copy_str(&mut next.context_uri, &snapshot.context_uri);
        let title = context_title_from(&snapshot);
        if !title.is_empty() {
            copy_str(&mut next.context_title, &title);
        }

        // Playback status.
        let playback = if snapshot.track_uri.is_empty() && !snapshot.is_playing {
            VAM_PLAYBACK_STOPPED
        } else if snapshot.is_playing && !snapshot.is_paused {
            if snapshot.is_buffering {
                VAM_PLAYBACK_BUFFERING
            } else {
                VAM_PLAYBACK_PLAYING
            }
        } else if snapshot.is_paused {
            VAM_PLAYBACK_PAUSED
        } else {
            VAM_PLAYBACK_STOPPED
        };
        next.playback = playback;

        // Options.
        next.shuffle = snapshot.shuffle as u8;
        next.repeat = self.shown_repeat(repeat_mode(snapshot.repeat_context, snapshot.repeat_track));
        next.is_active_device = snapshot.is_active as u8;
        if snapshot.volume > 0 {
            next.volume = snapshot.volume;
        }

        // Track metadata: prefer what the player decoded, then the cache.
        let track_changed = !state_uri_is(&next.track.uri, &snapshot.track_uri);
        if track_changed {
            let meta = self.shared.player_meta();
            let item = meta
                .track
                .clone()
                .filter(|item| item.uri == snapshot.track_uri)
                .or_else(|| self.shared.meta.get(&snapshot.track_uri))
                .unwrap_or_else(|| item_from_snapshot(&snapshot));
            next.track = item.to_c();
            if !snapshot.track_uri.is_empty() {
                self.spawn_track_metadata(snapshot.track_uri.clone());
            }
            if !snapshot.context_uri.is_empty() {
                self.spawn_context_title(snapshot.context_uri.clone());
            }
        }
        next.has_track = (!snapshot.track_uri.is_empty()) as u8;
        if next.track.duration_ms == 0 && snapshot.duration_ms > 0 {
            next.track.duration_ms = snapshot.duration_ms;
        }

        // Position: only touch it on discontinuities.
        let expected = extrapolated_position(&next);
        let difference = (expected as i64 - snapshot.position_ms as i64).abs();
        if track_changed
            || playback != current.playback
            || difference > 1_200
            || snapshot.position_ms == 0
        {
            next.position_ms = snapshot.position_ms;
            next.position_timestamp_ms = now;
        }

        next.seq = current.seq.wrapping_add(1);
        self.backend.set_player(|state| *state = next);
        self.shared.update_player(|meta| {
            meta.track_uri = snapshot.track_uri.clone();
            meta.context_uri = snapshot.context_uri.clone();
            if !snapshot.context_metadata.is_empty() {
                let title = context_title_from(&snapshot);
                if !title.is_empty() {
                    meta.context_title = title;
                }
            }
            meta.next_tracks = snapshot.next_tracks.clone();
            meta.prev_tracks = snapshot.prev_tracks.clone();
            meta.is_active = snapshot.is_active;
        });
    }

    /// Resolve metadata for a newly loaded track from the Web API.
    fn spawn_track_metadata(&self, uri: String) {
        if self.shared.meta.get(&uri).is_some() {
            return;
        }
        let shared = self.shared.clone();
        tokio::spawn(async move {
            let items = metadata::resolve_playables(&shared.web, &shared.meta, &[uri.clone()]).await;
            if let Some(item) = items.into_iter().next() {
                shared.meta.put(item.clone());
                shared.update_player(|meta| {
                    if meta.track_uri.is_empty() || meta.track_uri == uri {
                        meta.track = Some(item.clone());
                    }
                });
                shared.backend.set_player(|state| {
                    if cstr_lossy(&state.track.uri) == uri {
                        state.track = item.to_c();
                        state.seq = state.seq.wrapping_add(1);
                    }
                });
            }
        });
    }

    fn spawn_context_title(&self, uri: String) {
        if uri.is_empty() {
            return;
        }
        let shared = self.shared.clone();
        tokio::spawn(async move {
            let name = if uri.starts_with("spotify:album:") {
                internal::album(&shared.web, &uri)
                    .await
                    .map(|a| a.name().to_string())
                    .unwrap_or_default()
            } else if uri.starts_with("spotify:playlist:") {
                internal::playlist(&shared.web, &uri)
                    .await
                    .map(|p| p.name)
                    .unwrap_or_default()
            } else if uri.starts_with("spotify:artist:") {
                internal::artist(&shared.web, &uri)
                    .await
                    .map(|a| a.name().to_string())
                    .unwrap_or_default()
            } else if uri.ends_with(":collection") {
                shared.update_player(|meta| {
                    if meta.context_uri == uri {
                        meta.context_title = "Liked Songs".to_string();
                    }
                });
                shared.backend.set_player(|state| {
                    if cstr_lossy(&state.context_uri) == uri {
                        copy_str(&mut state.context_title, "Liked Songs");
                        state.seq = state.seq.wrapping_add(1);
                    }
                });
                return;
            } else {
                return;
            };
            if name.is_empty() {
                return;
            }
            shared.update_player(|meta| {
                if meta.context_uri == uri {
                    meta.context_title = name.clone();
                }
            });
            shared.backend.set_player(|state| {
                if cstr_lossy(&state.context_uri) == uri {
                    copy_str(&mut state.context_title, &name);
                    state.seq = state.seq.wrapping_add(1);
                }
            });
        });
    }

    // ------------------------------------------------------------------
    // Volume coalescing
    // ------------------------------------------------------------------

    fn flush_volume(&mut self) {
        let Some(volume) = self.pending_volume.take() else {
            return;
        };
        if let Some(spirc) = &self.spirc {
            if let Err(why) = spirc.set_volume(volume) {
                log::warn!("could not set volume: {why}");
            }
        }
        if let Some(mixer) = &self.mixer {
            mixer.set_volume(volume);
        }
    }

    // ------------------------------------------------------------------
    // Slow maintenance
    // ------------------------------------------------------------------

    async fn slow_tick(&mut self) {
        let now = now_ms_inner();
        let gap = now.saturating_sub(self.last_tick_ms);
        self.last_tick_ms = now;

        let wifi = platform::wifi_connected();
        if wifi != self.last_wifi {
            self.last_wifi = wifi;
            if !wifi {
                self.backend.set_auth(|auth| {
                    copy_str(&mut auth.message, "Wi-Fi is off. Reconnect to continue.");
                });
            }
        }

        if self.phase == Phase::LoggedIn {
            // The Vita drops sockets when it sleeps; detect the jump and
            // re-establish the session without involving the UI.
            if gap > SUSPEND_GAP_MS {
                log::warn!("resume after {gap}ms gap, reconnecting");
                self.reconnect("Reconnecting after suspend...");
                return;
            }
            let invalid = self
                .session
                .as_ref()
                .map(|session| session.is_invalid())
                .unwrap_or(true);
            if invalid {
                self.reconnect("The connection to Spotify was lost.");
                return;
            }
        }

        if let Some(at) = self.reconnect_at {
            if tokio::time::Instant::now() >= at {
                if !wifi {
                    self.reconnect_at = Some(tokio::time::Instant::now() + Duration::from_secs(5));
                    return;
                }
                self.reconnect_at = None;
                let credentials = self
                    .credentials
                    .clone()
                    .or_else(|| self.cache.as_ref().and_then(|c| c.credentials()));
                if let Some(credentials) = credentials {
                    self.last_credentials_marker = Some(marker(&credentials));
                    if let Err(why) = self.login(credentials).await {
                        log::warn!("reconnect failed: {why}");
                        self.reconnect_at =
                            Some(tokio::time::Instant::now() + Duration::from_secs(5));
                    }
                }
            }
        }

        if self.phase == Phase::WaitingForCredentials {
            if self.discovery.is_none() && wifi {
                let retry = self
                    .discovery_retry_at
                    .map(|at| tokio::time::Instant::now() >= at)
                    .unwrap_or(true);
                if retry {
                    self.start_discovery();
                }
            }

            // Fallback login: a credentials.json copied onto the device.
            let pending = self.cache.as_ref().and_then(|cache| cache.credentials());
            if let Some(credentials) = pending {
                let credentials_marker = marker(&credentials);
                if self.last_credentials_marker.as_deref() != Some(credentials_marker.as_str()) {
                    self.last_credentials_marker = Some(credentials_marker);
                    if let Err(why) = self.login(credentials).await {
                        log::warn!("credentials.json login failed: {why}");
                        self.phase = Phase::WaitingForCredentials;
                        self.backend.set_auth_status(
                            VAM_AUTH_ERROR,
                            &format!("Saved credentials were rejected: {why}"),
                        );
                    }
                }
            }

            if wifi {
                let auth = self.backend.auth_snapshot();
                if auth.message.is_empty() || cstr_lossy(&auth.message).contains("Wi-Fi") {
                    self.backend.set_auth(|auth| {
                        auth.status = VAM_AUTH_LOGGED_OUT;
                        auth.zeroconf_active = self.discovery.is_some() as u8;
                        copy_str(
                            &mut auth.message,
                            "Open Spotify on your phone, tap the device picker and choose this device.",
                        );
                    });
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Session lifecycle
    // ------------------------------------------------------------------

    async fn handle_discovered_credentials(&mut self, credentials: Credentials) {
        self.backend.set_auth(|auth| {
            auth.status = VAM_AUTH_CONNECTING;
            copy_str(&mut auth.message, "Received Spotify credentials, signing in...");
        });
        self.last_credentials_marker = Some(marker(&credentials));
        if let Err(why) = self.login(credentials).await {
            log::warn!("zeroconf login failed: {why}");
            self.phase = Phase::WaitingForCredentials;
            self.backend
                .set_auth_status(VAM_AUTH_ERROR, &format!("Sign-in failed: {why}"));
        }
    }

    async fn login(&mut self, credentials: Credentials) -> Result<(), String> {
        self.teardown().await;

        self.backend.set_auth(|auth| {
            auth.status = VAM_AUTH_CONNECTING;
            auth.is_premium = 0;
            auth.zeroconf_active = self.discovery.is_some() as u8;
            copy_str(&mut auth.message, "Signing in to Spotify...");
        });

        let config = self.backend.config.clone();
        let device_id = sha1_hex(config.device_name.as_bytes());
        let session_config = SessionConfig {
            device_id,
            tmp_dir: config.path("tmp").into(),
            ..SessionConfig::default()
        };

        let session = Session::new(session_config, self.cache.clone());

        let mixer_config = MixerConfig {
            volume_ctrl: VolumeCtrl::Log(60.0),
            ..MixerConfig::default()
        };
        let mixer: Arc<dyn Mixer> = Arc::new(
            SoftMixer::open(mixer_config).map_err(|e| format!("audio mixer: {e}"))?,
        );
        mixer.set_volume(config.initial_volume);

        let mut player_config = PlayerConfig {
            bitrate: match config.bitrate {
                96 => Bitrate::Bitrate96,
                320 => Bitrate::Bitrate320,
                _ => Bitrate::Bitrate160,
            },
            normalisation: config.normalize_volume,
            // The ARM CPU is slow; skip dithering and use the cheaper basic
            // normalisation only when the UI asked for normalisation.
            ditherer: None,
            ..PlayerConfig::default()
        };
        if !config.normalize_volume {
            player_config.normalisation = false;
        }

        let player = Player::new(
            player_config,
            session.clone(),
            mixer.get_soft_volume(),
            || platform::make_sink(),
        );
        let player_events = player.get_player_event_channel();

        let connect_config = ConnectConfig {
            name: config.device_name.clone(),
            device_type: DeviceType::GameConsole,
            initial_volume: config.initial_volume,
            volume_steps: 64,
            ..ConnectConfig::default()
        };

        let (spirc, spirc_future) = Spirc::new(
            connect_config,
            session.clone(),
            credentials.clone(),
            player.clone(),
            mixer.clone(),
        )
        .await
        .map_err(|e| format!("{e}"))?;

        let (done_tx, done_rx) = oneshot::channel();
        tokio::spawn(async move {
            spirc_future.await;
            let _ = done_tx.send(());
        });

        self.session = Some(session.clone());
        self.spirc = Some(spirc);
        self.player = Some(player);
        self.mixer = Some(mixer);
        self.player_events = Some(player_events);
        self.spirc_done = Some(done_rx);
        self.credentials = Some(credentials);
        self.phase = Phase::LoggedIn;
        self.shared.web.set_session(session.clone());

        // The account type comes from the session's user attributes (the AP
        // sends them shortly after login); the display name from the internal
        // profile endpoint. The public Web API is not used: it rate-limits
        // librespot's client id.
        for _ in 0..10 {
            if session.user_data().attributes.contains_key("type") {
                break;
            }
            tokio::time::sleep(Duration::from_millis(300)).await;
        }
        let profile = internal::own_profile(&self.shared.web).await;
        let mut display_name = profile.name;

        let attributes = session.user_data().attributes;
        let product = attributes.get("type").cloned();
        let premium = product.as_deref() == Some("premium");
        let username = session.username();
        if display_name.is_empty() {
            display_name = attributes
                .get("display-name")
                .cloned()
                .unwrap_or_else(|| username.clone());
        }

        let message = if premium {
            String::new()
        } else {
            "Spotify Premium is required to play music.".to_string()
        };

        self.backend.set_auth(|auth| {
            auth.status = VAM_AUTH_LOGGED_IN;
            auth.is_premium = premium as u8;
            auth.zeroconf_active = 1;
            copy_str(&mut auth.username, &username);
            copy_str(&mut auth.display_name, &display_name);
            copy_str(&mut auth.image_url, &profile.image_url);
            copy_str(&mut auth.message, &message);
        });
        self.backend.set_player(|state| {
            state.playback = VAM_PLAYBACK_STOPPED;
            state.has_track = 0;
            state.position_ms = 0;
            state.position_timestamp_ms = now_ms_inner();
            state.seq = state.seq.wrapping_add(1);
        });

        log::info!(
            "logged in as {username:?}, display name {display_name:?} (premium: {premium}, product: {product:?})"
        );
        self.apply_snapshot(true);
        Ok(())
    }

    async fn logout(&mut self) {
        self.teardown().await;
        let _ = std::fs::remove_file(self.backend.config.credentials_path());
        self.credentials = None;
        self.last_credentials_marker = None;
        self.reconnect_at = None;
        self.phase = Phase::WaitingForCredentials;

        if self.discovery.is_none() {
            self.start_discovery();
        }
        self.backend.set_auth(|auth| {
            auth.status = VAM_AUTH_LOGGED_OUT;
            auth.is_premium = 0;
            auth.zeroconf_active = self.discovery.is_some() as u8;
            auth.username = [0; VAM_TEXT_MAX];
            auth.display_name = [0; VAM_TEXT_MAX];
            auth.login_url = [0; VAM_URL_MAX];
            auth.login_code = [0; 32];
            auth.image_url = [0; VAM_URL_MAX];
            copy_str(
                &mut auth.message,
                "Logged out. Open Spotify on your phone to sign in again.",
            );
        });
    }

    async fn teardown(&mut self) {
        log::info!("tearing down backend");
        if let Some(player) = self.player.take() {
            player.stop();
        }
        if let Some(spirc) = self.spirc.take() {
            let _ = spirc.shutdown();
        }
        if let Some(session) = self.session.take() {
            session.shutdown();
        }
        self.player_events = None;
        self.spirc_done = None;
        self.mixer = None;
        self.shared.web.clear_session();
        self.shared.saved.clear();
        self.shared.meta.clear();
        self.shared.update_player(|meta| *meta = PlayerMeta::default());
        self.last_snapshot = ConnectSnapshot::default();
        self.backend.set_player(|state| {
            let volume = state.volume;
            let seq = state.seq.wrapping_add(1);
            *state = vam_player_state::default();
            state.volume = volume;
            state.seq = seq;
            state.is_active_device = 0;
        });
        log::info!("teardown done");
    }

    fn reconnect(&mut self, message: &str) {
        if self.shutting_down {
            return;
        }
        self.phase = Phase::WaitingForCredentials;
        self.player = None;
        self.player_events = None;
        self.spirc_done = None;
        self.mixer = None;
        self.spirc = None;
        self.shared.web.clear_session();
        if let Some(session) = self.session.take() {
            session.shutdown();
        }
        self.reconnect_at = Some(tokio::time::Instant::now() + Duration::from_secs(2));
        self.backend.set_auth(|auth| {
            auth.status = VAM_AUTH_CONNECTING;
            auth.zeroconf_active = self.discovery.is_some() as u8;
            copy_str(&mut auth.message, message);
        });
    }

    fn start_discovery(&mut self) {
        if !platform::wifi_connected() {
            self.backend.set_auth(|auth| {
                auth.status = VAM_AUTH_LOGGED_OUT;
                auth.zeroconf_active = 0;
                copy_str(&mut auth.message, "Wi-Fi is off. Connect to a network to sign in.");
            });
            self.discovery_retry_at = Some(tokio::time::Instant::now() + Duration::from_secs(5));
            return;
        }

        let config = self.backend.config.clone();
        let device_id = sha1_hex(config.device_name.as_bytes());
        let client_id = SessionConfig::default().client_id;
        let result = Discovery::builder(device_id, client_id)
            .name(config.device_name.clone())
            .device_type(DeviceType::GameConsole)
            .port(0)
            .launch();
        match result {
            Ok(discovery) => {
                self.discovery = Some(discovery);
                self.discovery_retry_at = None;
                self.backend.set_auth(|auth| {
                    auth.zeroconf_active = 1;
                    if auth.status == VAM_AUTH_STARTING {
                        auth.status = VAM_AUTH_LOGGED_OUT;
                    }
                });
                log::info!("Zeroconf advertising as {:?}", config.device_name);
            }
            Err(why) => {
                log::warn!("Zeroconf discovery unavailable: {why}");
                self.discovery = None;
                self.discovery_retry_at =
                    Some(tokio::time::Instant::now() + Duration::from_secs(15));
                self.backend.set_auth(|auth| {
                    auth.status = VAM_AUTH_LOGGED_OUT;
                    auth.zeroconf_active = 0;
                    copy_str(
                        &mut auth.message,
                        "Zeroconf is unavailable. Copy a librespot credentials.json \
                         into the VitaSpot data folder to sign in.",
                    );
                });
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Becomes the active device for a load that replaces whatever was playing.
/// `Spirc::activate` is local and instant. A transfer (`ensure_active`) asks
/// Spotify for the old session first; on a fresh start that request hangs
/// for seconds (about 9 s on the Vita) before failing with 410 Gone, and
/// the load would discard the transferred state anyway.
async fn activate_for_load(spirc: &Spirc) -> bool {
    if spirc.snapshot().is_active {
        return true;
    }
    if let Err(why) = spirc.activate() {
        log::warn!("activate request failed: {why}");
        return false;
    }
    for _ in 0..100 {
        if spirc.snapshot().is_active {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    false
}

async fn ensure_active(spirc: &Spirc) -> bool {
    if spirc.snapshot().is_active {
        return true;
    }
    if let Err(why) = spirc.transfer(None) {
        log::warn!("transfer request failed: {why}");
        return false;
    }
    // A transfer takes several seconds (4 s observed on desktop, more on the
    // Vita); giving up early dropped the play request that triggered it.
    for _ in 0..100 {
        tokio::time::sleep(Duration::from_millis(100)).await;
        if spirc.snapshot().is_active {
            return true;
        }
    }
    false
}

fn snapshot_changed(a: &ConnectSnapshot, b: &ConnectSnapshot) -> bool {
    // `position_ms` is extrapolated on every snapshot and therefore drifts
    // monotonically; compare the raw anchor instead so that a playing device
    // does not emit a PLAYER_CHANGED event every tick.
    a.is_active != b.is_active
        || a.is_playing != b.is_playing
        || a.is_paused != b.is_paused
        || a.is_buffering != b.is_buffering
        || a.volume != b.volume
        || a.track_uri != b.track_uri
        || a.context_uri != b.context_uri
        || a.shuffle != b.shuffle
        || a.repeat_context != b.repeat_context
        || a.repeat_track != b.repeat_track
        || a.queue_revision != b.queue_revision
        || a.next_tracks.len() != b.next_tracks.len()
        || a.position_as_of_ms != b.position_as_of_ms
        || a.anchor_timestamp_ms != b.anchor_timestamp_ms
        || a.duration_ms != b.duration_ms
}

fn extrapolated_position(state: &vam_player_state) -> u32 {
    if state.playback != VAM_PLAYBACK_PLAYING {
        return state.position_ms;
    }
    let now = now_ms_inner();
    let extra = now.saturating_sub(state.position_timestamp_ms) as u32;
    let position = state.position_ms.saturating_add(extra);
    if state.track.duration_ms > 0 {
        position.min(state.track.duration_ms)
    } else {
        position
    }
}

fn item_from_snapshot(snapshot: &ConnectSnapshot) -> Item {
    let meta = &snapshot.track_metadata;
    let get = |key: &str| meta.get(key).cloned().unwrap_or_default();
    let title = if meta.contains_key("title") {
        get("title")
    } else if meta.contains_key("name") {
        get("name")
    } else {
        String::new()
    };
    let artist = {
        let artist = get("artist_name");
        if artist.is_empty() {
            get("artist")
        } else {
            artist
        }
    };
    Item {
        kind: if snapshot.track_uri.starts_with("spotify:episode:") {
            VAM_KIND_EPISODE
        } else {
            VAM_KIND_TRACK
        },
        uri: snapshot.track_uri.clone(),
        title,
        subtitle: artist,
        image_url: get("image_url"),
        album_uri: snapshot.track_album_uri.clone(),
        artist_uri: snapshot.track_artist_uri.clone(),
        duration_ms: snapshot.duration_ms,
        is_explicit: get("is_explicit") == "true",
        is_playable: true,
        is_saved: false,
    }
}

fn context_title_from(snapshot: &ConnectSnapshot) -> String {
    for key in ["context_description", "context_name", "context_title"] {
        if let Some(value) = snapshot.context_metadata.get(key) {
            if !value.is_empty() {
                return value.clone();
            }
        }
    }
    String::new()
}

fn state_uri_is(buffer: &[u8], uri: &str) -> bool {
    cstr_lossy(buffer) == uri
}

fn cstr_lossy(buffer: &[u8]) -> String {
    let end = buffer.iter().position(|b| *b == 0).unwrap_or(buffer.len());
    String::from_utf8_lossy(&buffer[..end]).into_owned()
}

fn marker(credentials: &Credentials) -> String {
    sha1_hex(&credentials.auth_data)
}

fn repeat_mode(context: bool, track: bool) -> i32 {
    if track {
        VAM_REPEAT_TRACK
    } else if context {
        VAM_REPEAT_CONTEXT
    } else {
        VAM_REPEAT_OFF
    }
}
