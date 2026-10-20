//! The exported C API. Every function here is called from the UI thread and
//! must return quickly; all real work happens on the backend runtime.

use std::os::raw::c_char;

use crate::abi::*;
use crate::engine::Command;
use crate::platform;
use crate::state::{backend, release_event, Backend, Config, BACKEND};
use crate::util::{copy_str, cstr_to_string, now_ms_inner};

unsafe fn init_backend(
    config: *const vam_config,
) -> Result<std::sync::Arc<Backend>, i32> {
    if config.is_null() {
        return Err(VAM_ERR_INVALID_ARG);
    }
    let config = &*config;
    if config.api_version != VAM_API_VERSION {
        return Err(VAM_ERR_INVALID_ARG);
    }
    let mut device_name = cstr_to_string(config.device_name);
    if device_name.trim().is_empty() {
        device_name = "PS Vita".to_string();
    }
    let mut data_dir = cstr_to_string(config.data_dir);
    if data_dir.trim().is_empty() {
        return Err(VAM_ERR_INVALID_ARG);
    }
    if !data_dir.ends_with('/') && !data_dir.ends_with('\\') {
        data_dir.push('/');
    }
    // Make sure the data directory exists before logging/network bring-up.
    let _ = std::fs::create_dir_all(&data_dir);
    let bitrate = match config.bitrate {
        96 | 160 | 320 => config.bitrate,
        _ => 160,
    };

    match platform::init_network(&data_dir) {
        platform::NetStatus::Unavailable => {
            log::error!("platform network initialisation failed");
        }
        platform::NetStatus::WifiOff => {
            log::warn!("Wi-Fi is off at startup");
        }
        platform::NetStatus::Connected => {}
    }

    let cfg = Config {
        device_name,
        data_dir,
        bitrate,
        initial_volume: config.initial_volume,
        normalize_volume: config.normalize_volume != 0,
    };
    Ok(Backend::new(cfg))
}

/// Start backend threads, bring up networking, restore cached credentials
/// (auto-login) or start Zeroconf discovery.
#[no_mangle]
pub unsafe extern "C" fn vam_init(config: *const vam_config) -> i32 {
    if BACKEND.get().is_some() {
        return VAM_OK;
    }
    let backend = match init_backend(config) {
        Ok(backend) => backend,
        Err(code) => return code,
    };

    if BACKEND.set(backend.clone()).is_err() {
        return VAM_OK;
    }

    let runtime_thread = {
        let backend = backend.clone();
        std::thread::Builder::new()
            .name("vam-runtime".into())
            .stack_size(1024 * 1024)
            .spawn(move || {
                crate::platform::lower_current_thread_priority();
                let runtime = match tokio::runtime::Builder::new_multi_thread()
                    .worker_threads(2)
                    .thread_stack_size(512 * 1024)
                    .on_thread_start(crate::platform::lower_current_thread_priority)
                    .enable_all()
                    .build()
                {
                    Ok(runtime) => runtime,
                    Err(why) => {
                        log::error!("could not create backend runtime: {why}");
                        return;
                    }
                };
                let shared = crate::shared::Shared::new(
                    backend.clone(),
                    crate::webapi::WebApi::new(),
                );
                runtime.block_on(crate::engine::run(shared));
                // Dropping the runtime waits for the worker threads to notice
                // the shutdown and finish.
                drop(runtime);
                log::info!("backend runtime stopped");
            })
    };

    match runtime_thread {
        Ok(handle) => {
            *backend
                .runtime_thread
                .lock()
                .unwrap_or_else(|e| e.into_inner()) = Some(handle);
            VAM_OK
        }
        Err(why) => {
            log::error!("could not spawn backend thread: {why}");
            VAM_ERR_UNKNOWN
        }
    }
}

/// Stop playback, join threads, release everything.
#[no_mangle]
pub unsafe extern "C" fn vam_shutdown() {
    let Some(backend) = backend() else {
        return;
    };
    if backend.shutdown.swap(true, std::sync::atomic::Ordering::SeqCst) {
        return;
    }
    let _ = backend.send_command(Command::Shutdown);
    if let Some(handle) = backend
        .runtime_thread
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .take()
    {
        let _ = handle.join();
    }
    log::info!("backend thread joined");
    platform::shutdown_audio();
    platform::shutdown_network();
    backend.events.release_all();
}

/// Monotonic clock in milliseconds, shared with `vam_player_state`.
#[no_mangle]
pub extern "C" fn vam_now_ms() -> u64 {
    now_ms_inner()
}

/// Writes a UI message into the backend log.
#[no_mangle]
pub unsafe extern "C" fn vam_log(message: *const c_char) {
    let message = cstr_to_string(message);
    if !message.is_empty() {
        log::info!(target: "vam_ui", "{message}");
    }
}

/// Non-blocking. Returns 1 and fills `*out` when an event was queued.
#[no_mangle]
pub unsafe extern "C" fn vam_poll_event(out: *mut vam_event) -> std::os::raw::c_int {
    if out.is_null() {
        return 0;
    }
    let Some(backend) = backend() else {
        return 0;
    };
    match backend.events.poll() {
        Some(event) => {
            *out = event;
            1
        }
        None => 0,
    }
}

/// Frees `event->page` / `event->data` / `event->lyrics`. Safe on any event.
#[no_mangle]
pub unsafe extern "C" fn vam_event_release(event: *mut vam_event) {
    if event.is_null() {
        return;
    }
    release_event(&mut *event);
}

#[no_mangle]
pub unsafe extern "C" fn vam_get_auth_state(out: *mut vam_auth_state) {
    if out.is_null() {
        return;
    }
    match backend() {
        Some(backend) => *out = backend.auth_snapshot(),
        None => {
            let mut state = vam_auth_state::default();
            state.status = VAM_AUTH_LOGGED_OUT;
            copy_str(&mut state.message, "Backend is not running.");
            *out = state;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn vam_get_player_state(out: *mut vam_player_state) {
    if out.is_null() {
        return;
    }
    match backend() {
        Some(backend) => *out = backend.player_snapshot(),
        None => *out = vam_player_state::default(),
    }
}

#[no_mangle]
pub extern "C" fn vam_set_bitrate(kbps: i32) -> i32 {
    if !matches!(kbps, 96 | 160 | 320) {
        return VAM_ERR_INVALID_ARG;
    }
    match backend() {
        Some(backend) => {
            backend
                .bitrate
                .store(kbps as u32, std::sync::atomic::Ordering::Relaxed);
            log::info!("audio quality set to {kbps} kbps");
            VAM_OK
        }
        None => VAM_ERR_UNKNOWN,
    }
}

#[no_mangle]
pub extern "C" fn vam_logout() -> i32 {
    match backend() {
        Some(backend) => {
            if backend.send_command(Command::Logout) {
                VAM_OK
            } else {
                VAM_ERR_UNKNOWN
            }
        }
        None => VAM_ERR_UNKNOWN,
    }
}

#[no_mangle]
pub unsafe extern "C" fn vam_request_page(
    uri: *const c_char,
    offset: u32,
    limit: u32,
) -> u32 {
    let uri = cstr_to_string(uri);
    if uri.is_empty() {
        return 0;
    }
    let Some(backend) = backend() else {
        return 0;
    };
    let request_id = backend.next_request_id();
    let queued = backend.send_command(Command::RequestPage {
        request_id,
        uri,
        offset,
        limit,
    });
    if queued {
        request_id
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn vam_request_image(url: *const c_char) -> u32 {
    let url = cstr_to_string(url);
    if url.is_empty() {
        return 0;
    }
    let Some(backend) = backend() else {
        return 0;
    };
    let request_id = backend.next_request_id();
    let queued = backend.send_command(Command::RequestImage { request_id, url });
    if queued {
        request_id
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn vam_request_lyrics(track_uri: *const c_char) -> u32 {
    let track_uri = cstr_to_string(track_uri);
    if track_uri.is_empty() {
        return 0;
    }
    let Some(backend) = backend() else {
        return 0;
    };
    let request_id = backend.next_request_id();
    let queued = backend.send_command(Command::RequestLyrics {
        request_id,
        track_uri,
    });
    if queued {
        request_id
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn vam_cancel(request_id: u32) {
    if let Some(backend) = backend() {
        backend.events.cancel(request_id);
    }
}

fn logged_in() -> Option<std::sync::Arc<Backend>> {
    let backend = backend()?;
    let auth = backend.auth_snapshot();
    if auth.status == VAM_AUTH_LOGGED_IN {
        Some(backend)
    } else {
        None
    }
}

#[no_mangle]
pub unsafe extern "C" fn vam_play(
    context_uri: *const c_char,
    track_uri: *const c_char,
    index: u32,
) -> i32 {
    let Some(backend) = backend() else {
        return VAM_ERR_AUTH;
    };
    let auth = backend.auth_snapshot();
    if auth.status != VAM_AUTH_LOGGED_IN {
        return VAM_ERR_AUTH;
    }
    if auth.is_premium == 0 {
        return VAM_ERR_NOT_PREMIUM;
    }
    let context = cstr_to_string(context_uri);
    let track = cstr_to_string(track_uri);
    if context.is_empty() && track.is_empty() {
        return VAM_ERR_INVALID_ARG;
    }
    if backend.send_command(Command::Play {
        context,
        track,
        index,
    }) {
        VAM_OK
    } else {
        VAM_ERR_UNKNOWN
    }
}

#[no_mangle]
pub extern "C" fn vam_pause() -> i32 {
    playback_command(Command::Pause)
}

#[no_mangle]
pub extern "C" fn vam_resume() -> i32 {
    playback_command(Command::Resume)
}

#[no_mangle]
pub extern "C" fn vam_next() -> i32 {
    playback_command(Command::Next)
}

#[no_mangle]
pub extern "C" fn vam_prev() -> i32 {
    playback_command(Command::Prev)
}

#[no_mangle]
pub extern "C" fn vam_seek(position_ms: u32) -> i32 {
    playback_command(Command::Seek(position_ms))
}

fn playback_command(command: Command) -> i32 {
    let Some(backend) = logged_in() else {
        return VAM_ERR_AUTH;
    };
    if backend.send_command(command) {
        VAM_OK
    } else {
        VAM_ERR_UNKNOWN
    }
}

#[no_mangle]
pub extern "C" fn vam_set_shuffle(enabled: std::os::raw::c_int) -> i32 {
    if logged_in().is_none() {
        return VAM_ERR_AUTH;
    }
    match backend() {
        Some(backend) => {
            if backend.send_command(Command::SetShuffle(enabled != 0)) {
                VAM_OK
            } else {
                VAM_ERR_UNKNOWN
            }
        }
        None => VAM_ERR_AUTH,
    }
}

#[no_mangle]
pub extern "C" fn vam_set_repeat(mode: i32) -> i32 {
    if !(VAM_REPEAT_OFF..=VAM_REPEAT_TRACK).contains(&mode) {
        return VAM_ERR_INVALID_ARG;
    }
    if logged_in().is_none() {
        return VAM_ERR_AUTH;
    }
    match backend() {
        Some(backend) => {
            if backend.send_command(Command::SetRepeat(mode)) {
                VAM_OK
            } else {
                VAM_ERR_UNKNOWN
            }
        }
        None => VAM_ERR_AUTH,
    }
}

#[no_mangle]
pub extern "C" fn vam_set_volume(volume: u16) -> i32 {
    match backend() {
        Some(backend) => {
            if backend.send_command(Command::SetVolume(volume)) {
                VAM_OK
            } else {
                VAM_ERR_UNKNOWN
            }
        }
        None => VAM_ERR_UNKNOWN,
    }
}

#[no_mangle]
pub unsafe extern "C" fn vam_add_to_queue(track_uri: *const c_char) -> i32 {
    let track_uri = cstr_to_string(track_uri);
    if track_uri.is_empty() {
        return VAM_ERR_INVALID_ARG;
    }
    let Some(backend) = backend() else {
        return VAM_ERR_AUTH;
    };
    let auth = backend.auth_snapshot();
    if auth.status != VAM_AUTH_LOGGED_IN {
        return VAM_ERR_AUTH;
    }
    if auth.is_premium == 0 {
        return VAM_ERR_NOT_PREMIUM;
    }
    if backend.send_command(Command::AddToQueue(track_uri)) {
        VAM_OK
    } else {
        VAM_ERR_UNKNOWN
    }
}

#[no_mangle]
pub unsafe extern "C" fn vam_set_saved(uri: *const c_char, saved: std::os::raw::c_int) -> u32 {
    let uri = cstr_to_string(uri);
    if uri.is_empty() {
        return 0;
    }
    let Some(backend) = backend() else {
        return 0;
    };
    let request_id = backend.next_request_id();
    let queued = backend.send_command(Command::SetSaved {
        request_id,
        uri,
        saved: saved != 0,
    });
    if queued {
        request_id
    } else {
        0
    }
}
