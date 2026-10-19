//! Synthetic library items and small formatting helpers.

use crate::abi::*;
use crate::model::Item;

/// The synthetic "Liked Songs" entry shown at the top of the library.
pub fn collection_item(username: &str, total: u32) -> Item {
    Item {
        kind: VAM_KIND_COLLECTION,
        uri: format!("spotify:user:{username}:collection"),
        title: "Liked Songs".to_string(),
        subtitle: format!("Playlist • {total} songs"),
        image_url: String::new(),
        album_uri: String::new(),
        artist_uri: String::new(),
        duration_ms: 0,
        is_explicit: false,
        is_playable: true,
        is_saved: true,
    }
}

/// `12 songs`, with singular handling.
pub fn songs_count(n: u64) -> String {
    if n == 1 {
        "1 song".to_string()
    } else {
        format!("{n} songs")
    }
}
