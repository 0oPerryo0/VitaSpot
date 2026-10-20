//! C ABI mirrors of `include/vam_backend.h`.
//!
//! Every struct here must stay byte-for-byte compatible with the C header.
//! Enums are plain `i32` fields so that values from a newer/older frontend
//! cannot produce invalid Rust enum values (which would be UB).

#![allow(non_camel_case_types)]
#![allow(dead_code)] // full mirror of the C header, not every constant is used

use std::os::raw::{c_char, c_void};

pub const VAM_API_VERSION: u32 = 2;

pub const VAM_URI_MAX: usize = 96;
pub const VAM_TEXT_MAX: usize = 192;
pub const VAM_URL_MAX: usize = 256;
pub const VAM_MSG_MAX: usize = 256;
pub const VAM_DESC_MAX: usize = 512;

// vam_result
pub const VAM_OK: i32 = 0;
pub const VAM_ERR_UNKNOWN: i32 = 1;
pub const VAM_ERR_NETWORK: i32 = 2;
pub const VAM_ERR_AUTH: i32 = 3;
pub const VAM_ERR_NOT_PREMIUM: i32 = 4;
pub const VAM_ERR_NOT_FOUND: i32 = 5;
pub const VAM_ERR_RATE_LIMITED: i32 = 6;
pub const VAM_ERR_UNSUPPORTED: i32 = 7;
pub const VAM_ERR_INVALID_ARG: i32 = 8;
pub const VAM_ERR_UNAVAILABLE: i32 = 9;

// vam_kind
pub const VAM_KIND_UNKNOWN: i32 = 0;
pub const VAM_KIND_TRACK: i32 = 1;
pub const VAM_KIND_EPISODE: i32 = 2;
pub const VAM_KIND_ALBUM: i32 = 3;
pub const VAM_KIND_PLAYLIST: i32 = 4;
pub const VAM_KIND_ARTIST: i32 = 5;
pub const VAM_KIND_SHOW: i32 = 6;
pub const VAM_KIND_COLLECTION: i32 = 7;
pub const VAM_KIND_HOME: i32 = 8;
pub const VAM_KIND_SEARCH: i32 = 9;
pub const VAM_KIND_LIBRARY: i32 = 10;
pub const VAM_KIND_QUEUE: i32 = 11;

// vam_layout
pub const VAM_LAYOUT_LIST: i32 = 0;
pub const VAM_LAYOUT_SHELF: i32 = 1;
pub const VAM_LAYOUT_GRID: i32 = 2;

// vam_auth_status
pub const VAM_AUTH_STARTING: i32 = 0;
pub const VAM_AUTH_LOGGED_OUT: i32 = 1;
pub const VAM_AUTH_CONNECTING: i32 = 2;
pub const VAM_AUTH_LOGGED_IN: i32 = 3;
pub const VAM_AUTH_ERROR: i32 = 4;

// vam_playback
pub const VAM_PLAYBACK_STOPPED: i32 = 0;
pub const VAM_PLAYBACK_PLAYING: i32 = 1;
pub const VAM_PLAYBACK_PAUSED: i32 = 2;
pub const VAM_PLAYBACK_BUFFERING: i32 = 3;

// vam_repeat
pub const VAM_REPEAT_OFF: i32 = 0;
pub const VAM_REPEAT_CONTEXT: i32 = 1;
pub const VAM_REPEAT_TRACK: i32 = 2;

// vam_event_type
pub const VAM_EV_NONE: i32 = 0;
pub const VAM_EV_AUTH_CHANGED: i32 = 1;
pub const VAM_EV_PLAYER_CHANGED: i32 = 2;
pub const VAM_EV_PAGE: i32 = 3;
pub const VAM_EV_IMAGE: i32 = 4;
pub const VAM_EV_REQUEST_DONE: i32 = 5;
pub const VAM_EV_SAVED_CHANGED: i32 = 6;
pub const VAM_EV_ERROR: i32 = 7;
pub const VAM_EV_TOAST: i32 = 8;
pub const VAM_EV_LYRICS: i32 = 9;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct vam_item {
    pub kind: i32,
    pub uri: [u8; VAM_URI_MAX],
    pub title: [u8; VAM_TEXT_MAX],
    pub subtitle: [u8; VAM_TEXT_MAX],
    pub image_url: [u8; VAM_URL_MAX],
    pub album_uri: [u8; VAM_URI_MAX],
    pub artist_uri: [u8; VAM_URI_MAX],
    pub duration_ms: u32,
    pub is_explicit: u8,
    pub is_playable: u8,
    pub is_saved: u8,
    pub reserved: u8,
}

impl Default for vam_item {
    fn default() -> Self {
        // SAFETY: all fields are plain integers / byte arrays.
        unsafe { std::mem::zeroed() }
    }
}

#[repr(C)]
pub struct vam_section {
    pub title: [u8; VAM_TEXT_MAX],
    pub layout: i32,
    pub offset: u32,
    pub total: u32,
    pub count: u32,
    pub items: *mut vam_item,
}

#[repr(C)]
pub struct vam_page {
    pub kind: i32,
    pub uri: [u8; VAM_URI_MAX],
    pub title: [u8; VAM_TEXT_MAX],
    pub subtitle: [u8; VAM_TEXT_MAX],
    pub description: [u8; VAM_DESC_MAX],
    pub image_url: [u8; VAM_URL_MAX],
    pub is_saved: u8,
    pub reserved: [u8; 3],
    pub section_count: u32,
    pub sections: *mut vam_section,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct vam_player_state {
    pub playback: i32,
    pub repeat: i32,
    pub shuffle: u8,
    pub has_track: u8,
    pub is_active_device: u8,
    pub reserved: u8,
    pub volume: u16,
    pub reserved2: u16,
    pub position_ms: u32,
    pub position_timestamp_ms: u64,
    pub track: vam_item,
    pub context_uri: [u8; VAM_URI_MAX],
    pub context_title: [u8; VAM_TEXT_MAX],
    pub seq: u32,
}

impl Default for vam_player_state {
    fn default() -> Self {
        // SAFETY: all fields are plain integers / byte arrays.
        unsafe { std::mem::zeroed() }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct vam_auth_state {
    pub status: i32,
    pub is_premium: u8,
    pub zeroconf_active: u8,
    pub reserved: [u8; 2],
    pub username: [u8; VAM_TEXT_MAX],
    pub display_name: [u8; VAM_TEXT_MAX],
    pub device_name: [u8; VAM_TEXT_MAX],
    pub message: [u8; VAM_MSG_MAX],
    pub login_url: [u8; VAM_URL_MAX],
    pub login_code: [u8; 32],
    pub image_url: [u8; VAM_URL_MAX],
}

impl Default for vam_auth_state {
    fn default() -> Self {
        // SAFETY: all fields are plain integers / byte arrays.
        unsafe { std::mem::zeroed() }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct vam_lyrics_line {
    pub start_ms: u32,
    pub text_offset: u32,
}

#[repr(C)]
pub struct vam_lyrics {
    pub synced: u8,
    pub reserved: [u8; 3],
    pub line_count: u32,
    pub lines: *mut vam_lyrics_line,
    pub text_size: u32,
    pub text: *const c_char,
}

#[repr(C)]
pub struct vam_event {
    pub type_: i32,
    pub request_id: u32,
    pub result: i32,
    pub uri: [u8; VAM_URI_MAX],
    pub message: [u8; VAM_MSG_MAX],
    pub saved: u8,
    pub reserved: [u8; 3],
    pub page: *mut vam_page,
    pub data: *const u8,
    pub data_size: u32,
    pub lyrics: *mut vam_lyrics,
}

impl Default for vam_event {
    fn default() -> Self {
        vam_event {
            type_: VAM_EV_NONE,
            request_id: 0,
            result: VAM_OK,
            uri: [0; VAM_URI_MAX],
            message: [0; VAM_MSG_MAX],
            saved: 0,
            reserved: [0; 3],
            page: std::ptr::null_mut(),
            data: std::ptr::null(),
            data_size: 0,
            lyrics: std::ptr::null_mut(),
        }
    }
}

// SAFETY: a `vam_event` is an owning handle. `page`, `data` and `lyrics` point
// at allocations made by this library which are never shared or aliased: the
// event queue hands the only reference to the UI thread, and
// `vam_event_release` frees them exactly once. Moving the event between the
// worker threads and the event queue mutex is therefore sound.
unsafe impl Send for vam_event {}

#[repr(C)]
pub struct vam_config {
    pub api_version: u32,
    pub device_name: *const c_char,
    pub data_dir: *const c_char,
    pub bitrate: i32,
    pub initial_volume: u16,
    pub normalize_volume: u8,
    pub reserved: u8,
}

/// Opaque marker used to keep `c_void` referenced on all targets.
pub type c_void_ptr = *mut c_void;
