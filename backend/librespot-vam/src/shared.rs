//! State shared between the engine task and the page/image workers.

use std::sync::{Arc, Mutex};

use crate::images::ByteCache;
use crate::metadata::MetaCache;
use crate::model::Item;
use crate::saved::SavedCache;
use crate::state::Backend;
use crate::webapi::WebApi;

/// What the player is currently doing, mirrored for the queue page.
#[derive(Debug, Clone, Default)]
pub struct PlayerMeta {
    pub track_uri: String,
    pub track: Option<Item>,
    pub context_uri: String,
    pub context_title: String,
    pub prev_tracks: Vec<String>,
    pub next_tracks: Vec<String>,
    pub is_active: bool,
}

pub struct Shared {
    pub backend: Arc<Backend>,
    pub web: WebApi,
    pub meta: MetaCache,
    pub saved: SavedCache,
    pub images: ByteCache,
    pub player: Mutex<PlayerMeta>,
}

impl Shared {
    pub fn new(backend: Arc<Backend>, web: WebApi) -> Arc<Self> {
        let images = ByteCache::new(backend.config.cache_img_dir(), 30 * 1024 * 1024);
        Arc::new(Shared {
            backend,
            web,
            meta: MetaCache::new(),
            saved: SavedCache::new(),
            images,
            player: Mutex::new(PlayerMeta::default()),
        })
    }

    pub fn player_meta(&self) -> PlayerMeta {
        self.player
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }

    pub fn update_player(&self, update: impl FnOnce(&mut PlayerMeta)) {
        let mut guard = self.player.lock().unwrap_or_else(|e| e.into_inner());
        update(&mut guard);
    }
}
