//! The user's library (Liked Songs, saved albums, followed artists and
//! playlists), cached from Spotify's internal collection and rootlist
//! endpoints, plus like/save/follow.

use std::collections::HashSet;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use crate::abi::*;
use crate::internal::{self, PlaylistInfo, SET_ARTIST, SET_COLLECTION};
use crate::model::Item;
use crate::webapi::{ApiError, WebApi};

/// How long a library snapshot is reused before it is fetched again.
const LIBRARY_TTL: Duration = Duration::from_secs(120);

/// One snapshot of the library, newest first.
#[derive(Debug, Clone, Default)]
pub struct Library {
    pub liked: Vec<String>,
    pub albums: Vec<String>,
    pub artists: Vec<String>,
    pub playlists: Vec<PlaylistInfo>,
}

#[derive(Default)]
struct State {
    library: Library,
    loaded_at: Option<Instant>,
}

#[derive(Default)]
pub struct SavedCache {
    tracks: Mutex<HashSet<String>>,
    albums: Mutex<HashSet<String>>,
    playlists: Mutex<HashSet<String>>,
    artists: Mutex<HashSet<String>>,
    state: Mutex<State>,
    /// Serialises library loads so parallel page requests share one fetch.
    loading: tokio::sync::Mutex<()>,
}

fn kind_of(uri: &str) -> Option<&'static str> {
    let mut parts = uri.split(':');
    if parts.next()? != "spotify" {
        return None;
    }
    match parts.next()? {
        "track" => Some("track"),
        "album" => Some("album"),
        "playlist" => Some("playlist"),
        "artist" => Some("artist"),
        _ => None,
    }
}

impl SavedCache {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn clear(&self) {
        self.tracks.lock().unwrap_or_else(|e| e.into_inner()).clear();
        self.albums.lock().unwrap_or_else(|e| e.into_inner()).clear();
        self.playlists.lock().unwrap_or_else(|e| e.into_inner()).clear();
        self.artists.lock().unwrap_or_else(|e| e.into_inner()).clear();
        *self.state.lock().unwrap_or_else(|e| e.into_inner()) = State::default();
    }

    fn set_for(&self, kind: &str) -> Option<&Mutex<HashSet<String>>> {
        match kind {
            "track" => Some(&self.tracks),
            "album" => Some(&self.albums),
            "playlist" => Some(&self.playlists),
            "artist" => Some(&self.artists),
            _ => None,
        }
    }

    pub fn is_saved(&self, uri: &str) -> bool {
        match kind_of(uri).and_then(|k| self.set_for(k)) {
            Some(set) => set
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .contains(uri),
            None => false,
        }
    }

    pub fn set_saved(&self, uri: &str, saved: bool) {
        if let Some(set) = kind_of(uri).and_then(|k| self.set_for(k)) {
            let mut set = set.lock().unwrap_or_else(|e| e.into_inner());
            if saved {
                set.insert(uri.to_string());
            } else {
                set.remove(uri);
            }
        }
        // Keep the ordered snapshot in step, so Library and Liked Songs show
        // the change without waiting for the next fetch.
        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let list = match kind_of(uri) {
            Some("track") => &mut state.library.liked,
            Some("album") => &mut state.library.albums,
            Some("artist") => &mut state.library.artists,
            _ => return,
        };
        list.retain(|u| u != uri);
        if saved {
            list.insert(0, uri.to_string());
        }
    }

    /// The library snapshot, fetched when missing or older than the TTL.
    pub async fn library(&self, web: &WebApi) -> Library {
        let _guard = self.loading.lock().await;
        {
            let state = self.state.lock().unwrap_or_else(|e| e.into_inner());
            if state
                .loaded_at
                .map(|at| at.elapsed() < LIBRARY_TTL)
                .unwrap_or(false)
            {
                return state.library.clone();
            }
        }

        let (collection, artists, playlists) = tokio::join!(
            internal::collection_all(web, SET_COLLECTION),
            internal::collection_all(web, SET_ARTIST),
            internal::rootlist(web),
        );

        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let mut fresh = state.library.clone();
        if let Ok(items) = collection {
            fresh.liked = items
                .iter()
                .filter(|i| i.uri.starts_with("spotify:track:"))
                .map(|i| i.uri.clone())
                .collect();
            fresh.albums = items
                .iter()
                .filter(|i| i.uri.starts_with("spotify:album:"))
                .map(|i| i.uri.clone())
                .collect();
            replace(&self.tracks, &fresh.liked);
            replace(&self.albums, &fresh.albums);
        }
        if let Ok(items) = artists {
            fresh.artists = items
                .iter()
                .filter(|i| i.uri.starts_with("spotify:artist:"))
                .map(|i| i.uri.clone())
                .collect();
            replace(&self.artists, &fresh.artists);
        }
        if let Ok(list) = playlists {
            let uris: Vec<String> = list.iter().map(|p| p.uri.clone()).collect();
            replace(&self.playlists, &uris);
            fresh.playlists = list;
        }
        state.library = fresh.clone();
        state.loaded_at = Some(Instant::now());
        fresh
    }

    /// Fill in `is_saved` from the cached library.
    pub async fn annotate(&self, web: &WebApi, items: &mut [Item]) {
        if items.is_empty() {
            return;
        }
        let _ = self.library(web).await;
        for item in items.iter_mut() {
            match item.kind {
                VAM_KIND_TRACK | VAM_KIND_ALBUM | VAM_KIND_ARTIST | VAM_KIND_PLAYLIST => {
                    item.is_saved = self.is_saved(&item.uri);
                }
                _ => {}
            }
        }
    }
}

fn replace(set: &Mutex<HashSet<String>>, uris: &[String]) {
    let mut set = set.lock().unwrap_or_else(|e| e.into_inner());
    set.clear();
    set.extend(uris.iter().cloned());
}

/// Like/unlike a track, save/remove an album or follow/unfollow an artist via
/// the collection service, and update the local cache.
pub async fn set_saved(
    web: &WebApi,
    cache: &SavedCache,
    uri: &str,
    saved: bool,
) -> Result<(), ApiError> {
    let set = match kind_of(uri) {
        Some("track") | Some("album") => SET_COLLECTION,
        Some("artist") => SET_ARTIST,
        Some("playlist") => {
            return Err(ApiError::new(
                VAM_ERR_UNSUPPORTED,
                "Following playlists isn't supported yet.",
            ))
        }
        _ => return Err(ApiError::new(VAM_ERR_INVALID_ARG, "This item can't be saved.")),
    };
    internal::collection_write(web, set, uri, saved).await?;
    cache.set_saved(uri, saved);
    Ok(())
}
