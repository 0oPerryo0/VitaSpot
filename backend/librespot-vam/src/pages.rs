//! Builders for every page type the frontend can request.
//!
//! Everything except search comes from Spotify's internal endpoints (see
//! `internal.rs`); the public Web API rate-limits librespot's client id.

use std::collections::{HashMap, HashSet};

use futures_util::future::join_all;

use crate::abi::*;
use crate::internal;
use crate::items;
use crate::metadata::resolve_playables;
use crate::model::{Item, Page, Section};
use crate::search;
use crate::shared::Shared;
use crate::webapi::ApiError;

/// Request a page and normalise the paging arguments.
pub async fn build_page(
    shared: &Shared,
    uri: &str,
    offset: u32,
    limit: u32,
) -> Result<Page, ApiError> {
    let web = &shared.web;
    if !web.is_authenticated() {
        return Err(ApiError::auth("Not logged in to Spotify."));
    }
    let limit = if limit == 0 || limit > 200 { 50 } else { limit };

    if uri == "vam:home" {
        home(shared, limit).await
    } else if uri == "vam:library" {
        library(shared, offset, limit).await
    } else if let Some(query) = uri.strip_prefix("vam:search:") {
        search(shared, query, offset, limit).await
    } else if uri == "vam:queue" {
        queue(shared).await
    } else if uri.starts_with("spotify:album:") {
        album(shared, uri, offset, limit).await
    } else if uri.starts_with("spotify:playlist:") {
        playlist(shared, uri, offset, limit).await
    } else if uri.starts_with("spotify:artist:") {
        artist(shared, uri).await
    } else if uri.ends_with(":collection") && uri.starts_with("spotify:user:") {
        collection(shared, uri, offset, limit).await
    } else if uri.starts_with("spotify:show:") {
        Err(ApiError::unavailable("Podcasts are not supported yet."))
    } else {
        Err(ApiError::not_found())
    }
}

/// `offset..offset+limit` of `list`, clamped.
fn window<T: Clone>(list: &[T], offset: u32, limit: u32) -> Vec<T> {
    let start = (offset as usize).min(list.len());
    let end = (start + limit as usize).min(list.len());
    list[start..end].to_vec()
}

fn liked_item(shared: &Shared, count: usize) -> Item {
    items::collection_item(&shared.web.username(), count as u32)
}

// ---------------------------------------------------------------------------
// Home
// ---------------------------------------------------------------------------

async fn home(shared: &Shared, limit: u32) -> Result<Page, ApiError> {
    let web = &shared.web;
    let shelf_len = limit.min(20) as usize;

    let (library, recent) = tokio::join!(
        shared.saved.library(web),
        internal::recently_played_contexts(web),
    );

    // Recently played contexts: albums and artists resolve in one batch,
    // playlists come from the rootlist or are fetched one by one.
    let recent: Vec<String> = recent
        .into_iter()
        .filter(|u| {
            u.starts_with("spotify:album:")
                || u.starts_with("spotify:artist:")
                || u.starts_with("spotify:playlist:")
        })
        .take(shelf_len)
        .collect();
    let known: HashMap<&str, &internal::PlaylistInfo> = library
        .playlists
        .iter()
        .map(|p| (p.uri.as_str(), p))
        .collect();
    let other_playlists: Vec<&String> = recent
        .iter()
        .filter(|u| u.starts_with("spotify:playlist:") && !known.contains_key(u.as_str()))
        .take(8)
        .collect();
    let (resolved, fetched) = tokio::join!(
        internal::resolve_items(web, &recent),
        join_all(other_playlists.iter().map(|u| internal::playlist(web, u))),
    );
    let mut by_uri: HashMap<String, Item> =
        resolved.into_iter().map(|i| (i.uri.clone(), i)).collect();
    for playlist in fetched.into_iter().flatten() {
        by_uri.insert(playlist.uri.clone(), internal::playlist_item(&playlist));
    }
    for (uri, playlist) in known.iter() {
        by_uri
            .entry(uri.to_string())
            .or_insert_with(|| internal::playlist_item(playlist));
    }
    let recent_items: Vec<Item> = recent.iter().filter_map(|u| by_uri.get(u).cloned()).collect();

    let saved_albums = window(&library.albums, 0, shelf_len as u32);
    let followed = window(&library.artists, 0, shelf_len as u32);
    let (albums, artists) = tokio::join!(
        internal::resolve_items(web, &saved_albums),
        internal::resolve_items(web, &followed),
    );
    let playlists: Vec<Item> = library
        .playlists
        .iter()
        .take(shelf_len)
        .map(internal::playlist_item)
        .collect();

    let mut page = Page::new(VAM_KIND_HOME, "vam:home");

    // GRID: up to six quick-access items, topped up with playlists.
    let mut grid = Section::grid("");
    let mut in_grid: HashSet<String> = HashSet::new();
    for item in recent_items.iter().chain(playlists.iter()) {
        if grid.items.len() >= 6 {
            break;
        }
        if in_grid.insert(item.uri.clone()) {
            grid.push(item.clone());
        }
    }
    grid.finish();
    page.sections.push(grid);

    for (title, list) in [
        ("Recently played", recent_items),
        ("Your playlists", playlists),
        ("Saved albums", albums),
        ("Your artists", artists),
    ] {
        let mut section = Section::shelf(title);
        section.items = list;
        section.finish();
        page.sections.push(section);
    }

    annotate_sections(shared, &mut page.sections).await;
    Ok(page)
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

/// One row of the flattened library list, resolved only when on screen.
#[derive(Clone)]
enum Row {
    Liked,
    Playlist(usize),
    Entity(String),
}

async fn library(shared: &Shared, offset: u32, limit: u32) -> Result<Page, ApiError> {
    let web = &shared.web;
    let library = shared.saved.library(web).await;

    let mut rows: Vec<Row> = vec![Row::Liked];
    rows.extend((0..library.playlists.len()).map(Row::Playlist));
    rows.extend(library.albums.iter().cloned().map(Row::Entity));
    rows.extend(library.artists.iter().cloned().map(Row::Entity));
    let total = rows.len() as u32;

    let visible = window(&rows, offset, limit);
    let entity_uris: Vec<String> = visible
        .iter()
        .filter_map(|r| match r {
            Row::Entity(uri) => Some(uri.clone()),
            _ => None,
        })
        .collect();
    let resolved: HashMap<String, Item> = internal::resolve_items(web, &entity_uris)
        .await
        .into_iter()
        .map(|i| (i.uri.clone(), i))
        .collect();

    let mut section = Section::list("");
    section.offset = offset;
    section.total = total;
    for row in visible {
        let item = match row {
            Row::Liked => Some(liked_item(shared, library.liked.len())),
            Row::Playlist(index) => library.playlists.get(index).map(internal::playlist_item),
            Row::Entity(uri) => resolved.get(&uri).cloned().map(|mut item| {
                item.is_saved = true;
                item
            }),
        };
        if let Some(item) = item {
            section.push(item);
        }
    }

    let mut page = Page::new(VAM_KIND_LIBRARY, "vam:library");
    page.sections.push(section);
    Ok(page)
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

async fn search(shared: &Shared, query: &str, offset: u32, limit: u32) -> Result<Page, ApiError> {
    let mark = |mut item: Item| {
        item.is_saved = shared.saved.is_saved(&item.uri);
        item
    };
    let mut page = Page::new(VAM_KIND_SEARCH, format!("vam:search:{query}"));

    if offset > 0 {
        // Only the "Songs" list is paged, exactly like the contract says.
        let results = search::search(&shared.web, query, offset, limit).await?;
        let mut section = Section::list("Songs");
        section.offset = offset;
        section.total = results.tracks_total;
        for item in results.tracks {
            section.push(mark(item));
        }
        page.sections.push(section);
        annotate_sections(shared, &mut page.sections).await;
        return Ok(page);
    }

    let results = search::search(&shared.web, query, 0, 10).await?;
    let mut songs = Section::list("Songs");
    songs.total = results.tracks_total;
    for item in results.tracks {
        songs.push(mark(item));
    }
    page.sections.push(songs);
    for (title, found) in [
        ("Artists", results.artists),
        ("Albums", results.albums),
        ("Playlists", results.playlists),
    ] {
        let mut section = Section::shelf(title);
        for item in found {
            section.push(mark(item));
        }
        section.finish();
        page.sections.push(section);
    }

    annotate_sections(shared, &mut page.sections).await;
    Ok(page)
}

// ---------------------------------------------------------------------------
// Album
// ---------------------------------------------------------------------------

async fn album(
    shared: &Shared,
    uri: &str,
    offset: u32,
    limit: u32,
) -> Result<Page, ApiError> {
    let web = &shared.web;
    let album = internal::album(web, uri).await?;
    let track_uris = internal::album_track_uris(&album);
    let total_tracks = track_uris.len() as u32;

    let mut page = Page::new(VAM_KIND_ALBUM, uri);
    page.title = album.name().to_string();
    let artist = album.artist.first().map(|a| a.name()).unwrap_or("");
    let year = internal::album_year(&album);
    page.subtitle = [artist.to_string(), year, items::songs_count(total_tracks as u64)]
        .into_iter()
        .filter(|p| !p.is_empty())
        .collect::<Vec<_>>()
        .join(" • ");
    page.image_url = internal::album_image(&album, true);
    let _ = shared.saved.library(web).await;
    page.is_saved = shared.saved.is_saved(uri);

    let small_image = internal::album_image(&album, false);
    let visible = window(&track_uris, offset, limit);
    let mut section = Section::list("");
    section.offset = offset;
    section.total = total_tracks;
    for mut item in resolve_playables(&shared.web, &shared.meta, &visible).await {
        if item.album_uri.is_empty() {
            item.album_uri = uri.to_string();
        }
        if item.image_url.is_empty() {
            item.image_url = small_image.clone();
        }
        section.push(item);
    }
    page.sections.push(section);
    annotate_sections(shared, &mut page.sections).await;
    Ok(page)
}

// ---------------------------------------------------------------------------
// Playlist
// ---------------------------------------------------------------------------

async fn playlist(
    shared: &Shared,
    uri: &str,
    offset: u32,
    limit: u32,
) -> Result<Page, ApiError> {
    let web = &shared.web;
    let info = internal::playlist(web, uri).await?;
    let total_tracks = info.track_uris.len() as u32;

    let mut page = Page::new(VAM_KIND_PLAYLIST, uri);
    page.title = info.name.clone();
    page.description = info.description.clone();
    page.image_url = info.image_url.clone();
    page.subtitle = format!(
        "{} • {}",
        info.owner_name,
        items::songs_count(info.length.max(total_tracks) as u64)
    );
    let _ = shared.saved.library(web).await;
    page.is_saved = info.owner == web.username() || shared.saved.is_saved(uri);

    let visible = window(&info.track_uris, offset, limit);
    let mut section = Section::list("");
    section.offset = offset;
    section.total = total_tracks;
    section.items = resolve_playables(&shared.web, &shared.meta, &visible).await;
    page.sections.push(section);
    annotate_sections(shared, &mut page.sections).await;
    Ok(page)
}

// ---------------------------------------------------------------------------
// Artist
// ---------------------------------------------------------------------------

/// Albums shown per artist shelf.
const ARTIST_SHELF: usize = 20;

async fn artist(shared: &Shared, uri: &str) -> Result<Page, ApiError> {
    let web = &shared.web;
    let artist = internal::artist(web, uri).await?;

    let mut page = Page::new(VAM_KIND_ARTIST, uri);
    page.title = artist.name().to_string();
    page.subtitle = "Artist".to_string();
    page.image_url = internal::artist_image(&artist, true);

    let top: Vec<String> = internal::top_track_uris(&artist, &web.country())
        .into_iter()
        .take(10)
        .collect();
    let groups = [
        ("Albums", internal::group_album_uris(&artist.album_group)),
        ("Singles and EPs", internal::group_album_uris(&artist.single_group)),
        ("Appears on", internal::group_album_uris(&artist.appears_on_group)),
    ];
    let mut album_uris: Vec<String> = Vec::new();
    for (_, uris) in groups.iter() {
        album_uris.extend(uris.iter().take(ARTIST_SHELF).cloned());
    }

    let (popular_items, album_items, _) = tokio::join!(
        resolve_playables(&shared.web, &shared.meta, &top),
        internal::resolve_items(web, &album_uris),
        shared.saved.library(web),
    );
    page.is_saved = shared.saved.is_saved(uri);
    let albums: HashMap<String, Item> =
        album_items.into_iter().map(|i| (i.uri.clone(), i)).collect();

    let mut popular = Section::list("Popular");
    popular.items = popular_items.into_iter().filter(|i| i.is_playable).collect();
    popular.finish();
    page.sections.push(popular);

    for (title, uris) in groups {
        let mut section = Section::shelf(title);
        section.items = uris
            .iter()
            .take(ARTIST_SHELF)
            .filter_map(|u| albums.get(u).cloned())
            .collect();
        section.finish();
        page.sections.push(section);
    }

    annotate_sections(shared, &mut page.sections).await;
    Ok(page)
}

// ---------------------------------------------------------------------------
// Liked Songs (collection)
// ---------------------------------------------------------------------------

async fn collection(
    shared: &Shared,
    uri: &str,
    offset: u32,
    limit: u32,
) -> Result<Page, ApiError> {
    let library = shared.saved.library(&shared.web).await;
    let total = library.liked.len() as u32;

    let mut page = Page::new(VAM_KIND_COLLECTION, uri);
    if offset == 0 {
        page.title = "Liked Songs".to_string();
        page.subtitle = format!("You • {}", items::songs_count(total as u64));
    }

    let visible = window(&library.liked, offset, limit);
    let mut section = Section::list("");
    section.offset = offset;
    section.total = total;
    for mut item in resolve_playables(&shared.web, &shared.meta, &visible).await {
        item.is_saved = true;
        section.push(item);
    }
    page.sections.push(section);
    Ok(page)
}

// ---------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------

async fn queue(shared: &Shared) -> Result<Page, ApiError> {
    let meta = shared.player_meta();
    let mut page = Page::new(VAM_KIND_QUEUE, "vam:queue");

    let mut now_playing = Section::list("Now playing");
    let current = if let Some(track) = meta.track.clone() {
        Some(track)
    } else if !meta.track_uri.is_empty() {
        resolve_playables(&shared.web, &shared.meta, std::slice::from_ref(&meta.track_uri))
            .await
            .into_iter()
            .next()
    } else {
        None
    };
    if let Some(track) = current {
        now_playing.push(track);
    }
    now_playing.finish();
    page.sections.push(now_playing);

    // "Next up": queued tracks, then the rest of the context.
    let mut up_next_uris: Vec<String> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();
    for uri in meta.next_tracks.iter() {
        if uri.is_empty()
            || uri.starts_with("spotify:delimiter")
            || uri.starts_with("spotify:local")
        {
            continue;
        }
        if seen.insert(uri.clone()) {
            up_next_uris.push(uri.clone());
        }
    }

    let resolved = resolve_playables(&shared.web, &shared.meta, &up_next_uris).await;
    let mut next_section = Section::list("Next up");
    next_section.items = resolved;
    next_section.finish();
    page.sections.push(next_section);

    annotate_sections(shared, &mut page.sections).await;
    Ok(page)
}

// ---------------------------------------------------------------------------
// Saved state enrichment
// ---------------------------------------------------------------------------

async fn annotate_sections(shared: &Shared, sections: &mut [Section]) {
    let all: Vec<Item> = sections
        .iter()
        .flat_map(|s| s.items.iter().cloned())
        .collect();
    if all.is_empty() {
        return;
    }
    let mut annotated = all;
    shared.saved.annotate(&shared.web, &mut annotated).await;
    let states: HashMap<String, bool> = annotated
        .iter()
        .map(|item| (item.uri.clone(), item.is_saved))
        .collect();
    for section in sections.iter_mut() {
        for item in section.items.iter_mut() {
            if let Some(saved) = states.get(&item.uri) {
                item.is_saved = *saved;
            }
        }
    }
}
