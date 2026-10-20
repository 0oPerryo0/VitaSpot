//! VitaSpot backend library.
//!
//! A librespot based implementation of `include/vam_backend.h` for the PS Vita
//! (and for desktop testing). The C API lives in [`ffi`]; everything else is
//! private implementation detail.

#![allow(clippy::missing_safety_doc)]

mod abi;
mod engine;
mod ffi;
mod images;
mod internal;
mod items;
mod lyrics;
mod metadata;
mod model;
mod pages;
mod platform;
mod saved;
mod search;
mod shared;
mod state;
mod util;
mod volume;
mod webapi;

pub use abi::VAM_API_VERSION;
