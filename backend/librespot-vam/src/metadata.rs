//! Small in-memory metadata cache, plus batched internal metadata lookups.

use std::collections::HashMap;
use std::sync::Mutex;

use librespot_metadata::audio::{AudioItem, UniqueFields};

use crate::abi::*;
use crate::internal;
use crate::model::Item;
use crate::webapi::WebApi;

const MAX_ENTRIES: usize = 2048;

#[derive(Default)]
pub struct MetaCache {
    items: Mutex<HashMap<String, Item>>,
}

impl MetaCache {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn get(&self, uri: &str) -> Option<Item> {
        self.items
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .get(uri)
            .cloned()
    }

    pub fn put(&self, item: Item) {
        let mut guard = self.items.lock().unwrap_or_else(|e| e.into_inner());
        if guard.len() >= MAX_ENTRIES {
            guard.clear();
        }
        guard.insert(item.uri.clone(), item);
    }

    pub fn clear(&self) {
        self.items.lock().unwrap_or_else(|e| e.into_inner()).clear();
    }
}

/// Convert the metadata librespot decodes from the audio stream into an item.
pub fn item_from_audio(audio: &AudioItem) -> Item {
    let mut item = Item {
        uri: audio.uri.clone(),
        title: audio.name.clone(),
        duration_ms: audio.duration_ms,
        is_explicit: audio.is_explicit,
        is_playable: true,
        kind: if audio.uri.starts_with("spotify:episode:") {
            VAM_KIND_EPISODE
        } else {
            VAM_KIND_TRACK
        },
        ..Default::default()
    };

    if let Some(cover) = audio
        .covers
        .iter()
        .min_by_key(|c| (c.width as i64 - 300).abs())
    {
        item.image_url = cover.url.clone();
    }

    match &audio.unique_fields {
        UniqueFields::Track {
            artists,
            album,
            album_artists: _,
            ..
        } => {
            item.subtitle = artists
                .0
                .iter()
                .map(|a| a.name.clone())
                .collect::<Vec<_>>()
                .join(", ");
            item.artist_uri = artists
                .0
                .first()
                .and_then(|a| a.id.to_uri().ok())
                .unwrap_or_default();
            // librespot's AudioItem only carries the album name, so the album
            // URI is filled in later from extended metadata when available.
            let _ = album;
        }
        UniqueFields::Episode {
            show_name,
            description,
            ..
        } => {
            item.subtitle = show_name.clone();
            item.album_uri.clear();
            let _ = description;
        }
        UniqueFields::Local { .. } => {
            item.is_playable = false;
        }
    }

    item
}

/// Resolve a list of track/episode URIs to items, using the cache first and
/// batching extended-metadata requests for whatever is missing.
pub async fn resolve_playables(
    web: &WebApi,
    cache: &MetaCache,
    uris: &[String],
) -> Vec<Item> {
    let mut result: Vec<Option<Item>> = uris.iter().map(|u| cache.get(u)).collect();

    let missing: Vec<String> = uris
        .iter()
        .zip(result.iter())
        .filter(|(uri, cached)| {
            cached.is_none()
                && (uri.starts_with("spotify:track:") || uri.starts_with("spotify:episode:"))
        })
        .map(|(uri, _)| uri.clone())
        .collect();
    if !missing.is_empty() {
        for item in internal::resolve_items(web, &missing).await {
            cache.put(item.clone());
            for (index, uri) in uris.iter().enumerate() {
                if *uri == item.uri && result[index].is_none() {
                    result[index] = Some(item.clone());
                }
            }
        }
    }

    result
        .into_iter()
        .zip(uris.iter())
        .map(|(item, uri)| {
            item.unwrap_or_else(|| Item {
                kind: if uri.starts_with("spotify:episode:") {
                    VAM_KIND_EPISODE
                } else {
                    VAM_KIND_TRACK
                },
                uri: uri.clone(),
                title: "Unavailable track".to_string(),
                is_playable: false,
                ..Default::default()
            })
        })
        .collect()
}
