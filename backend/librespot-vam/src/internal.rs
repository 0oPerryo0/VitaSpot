//! Spotify's internal ("spclient") endpoints, used instead of the public Web
//! API for browsing.
//!
//! librespot's access tokens belong to Spotify's shared desktop client id, and
//! the public Web API rate-limits that id so hard that every call returned
//! HTTP 429. The endpoints here are the ones the official clients use:
//!
//! * batched extended metadata (tracks, albums, artists, episodes),
//! * collection v2 (Liked Songs, saved albums, followed artists),
//! * playlist v2 (rootlist and playlist contents),
//! * recently played and the user profile (best effort).
//!
//! None of them are documented. Every failure is logged with its endpoint so
//! the Vita log shows which one broke.

use std::collections::HashMap;
use std::sync::Mutex;

use futures_util::stream::{self, StreamExt};

use http::{
    header::{ACCEPT, CONTENT_TYPE},
    HeaderMap, HeaderValue, Method,
};
use librespot_core::{error::ErrorKind, Error, Session, SpotifyId};
use librespot_protocol::{
    extended_metadata::{BatchedEntityRequest, EntityRequest, ExtensionQuery},
    extension_kind::ExtensionKind,
    metadata::{self, image::Size},
    playlist4_external::{ListAttributes, SelectedListContent},
};
use protobuf::{EnumOrUnknown, Message};
use serde_json::Value;

use crate::abi::*;
use crate::model::Item;
use crate::webapi::{ApiError, WebApi};

/// Extended metadata accepts large batches; stay well below any limit.
const METADATA_BATCH: usize = 100;
/// Items per collection page request.
const COLLECTION_PAGE: i32 = 300;
/// Hard cap on collection pages, so a huge library cannot stall a page load.
const COLLECTION_MAX_PAGES: usize = 40;
const COLLECTION_CONTENT_TYPE: &str = "application/vnd.collection-v2.spotify.proto";

pub const SET_COLLECTION: &str = "collection";
pub const SET_ARTIST: &str = "artist";

// ---------------------------------------------------------------------------
// Errors and ids
// ---------------------------------------------------------------------------

pub(crate) fn api_error(endpoint: &str, error: Error) -> ApiError {
    log::warn!("spclient {endpoint} failed: {error}");
    match error.kind {
        ErrorKind::ResourceExhausted => {
            ApiError::rate_limited("Spotify is rate limiting requests. Try again soon.")
        }
        ErrorKind::NotFound => ApiError::not_found(),
        ErrorKind::Unauthenticated | ErrorKind::PermissionDenied => {
            ApiError::auth("Spotify refused the request. Try logging in again.")
        }
        _ => ApiError::network(format!("Spotify request failed: {error}")),
    }
}

/// `spotify:<kind>:<base62>` from a 16 byte gid.
pub fn gid_uri(kind: &str, gid: &[u8]) -> String {
    SpotifyId::from_raw(gid)
        .and_then(|id| id.to_base62())
        .map(|b62| format!("spotify:{kind}:{b62}"))
        .unwrap_or_default()
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn image_url(file_id: &[u8]) -> String {
    if file_id.is_empty() {
        String::new()
    } else {
        format!("https://i.scdn.co/image/{}", hex(file_id))
    }
}

/// Pick an image: `large` for page headers, the default (~300 px) otherwise.
fn pick_image(images: &[metadata::Image], large: bool) -> String {
    let wanted: &[Size] = if large {
        &[Size::LARGE, Size::XLARGE, Size::DEFAULT, Size::SMALL]
    } else {
        &[Size::DEFAULT, Size::LARGE, Size::SMALL, Size::XLARGE]
    };
    for size in wanted {
        if let Some(image) = images.iter().find(|i| i.size() == *size) {
            return image_url(image.file_id());
        }
    }
    images
        .first()
        .map(|i| image_url(i.file_id()))
        .unwrap_or_default()
}

fn group_image(group: &protobuf::MessageField<metadata::ImageGroup>, fallback: &[metadata::Image], large: bool) -> String {
    let from_group = group
        .as_ref()
        .map(|g| pick_image(&g.image, large))
        .unwrap_or_default();
    if from_group.is_empty() {
        pick_image(fallback, large)
    } else {
        from_group
    }
}

// ---------------------------------------------------------------------------
// Extended metadata
// ---------------------------------------------------------------------------

async fn batch<M: Message>(
    web: &WebApi,
    kind: ExtensionKind,
    uris: &[String],
) -> HashMap<String, M> {
    let mut out = HashMap::new();
    let Ok(session) = web.session() else {
        return out;
    };
    let mut unique: Vec<&String> = Vec::new();
    for uri in uris {
        if !uri.is_empty() && !unique.contains(&uri) {
            unique.push(uri);
        }
    }
    for chunk in unique.chunks(METADATA_BATCH) {
        let request = BatchedEntityRequest {
            entity_request: chunk
                .iter()
                .map(|uri| EntityRequest {
                    entity_uri: (*uri).clone(),
                    query: vec![ExtensionQuery {
                        extension_kind: EnumOrUnknown::new(kind),
                        ..Default::default()
                    }],
                    ..Default::default()
                })
                .collect(),
            ..Default::default()
        };
        let response = match session.spclient().get_extended_metadata(request).await {
            Ok(response) => response,
            Err(e) => {
                let _ = api_error("/extended-metadata", e);
                continue;
            }
        };
        for array in response.extended_metadata {
            for data in array.extension_data {
                let Some(any) = data.extension_data.as_ref() else {
                    continue;
                };
                if any.value.is_empty() {
                    continue;
                }
                match M::parse_from_bytes(&any.value) {
                    Ok(message) => {
                        out.insert(data.entity_uri.clone(), message);
                    }
                    Err(e) => log::warn!("bad metadata for {}: {e}", data.entity_uri),
                }
            }
        }
    }
    out
}

pub async fn tracks(web: &WebApi, uris: &[String]) -> HashMap<String, metadata::Track> {
    batch(web, ExtensionKind::TRACK_V4, uris).await
}

pub async fn episodes(web: &WebApi, uris: &[String]) -> HashMap<String, metadata::Episode> {
    batch(web, ExtensionKind::EPISODE_V4, uris).await
}

pub async fn albums(web: &WebApi, uris: &[String]) -> HashMap<String, metadata::Album> {
    batch(web, ExtensionKind::ALBUM_V4, uris).await
}

pub async fn artists(web: &WebApi, uris: &[String]) -> HashMap<String, metadata::Artist> {
    batch(web, ExtensionKind::ARTIST_V4, uris).await
}

pub async fn album(web: &WebApi, uri: &str) -> Result<metadata::Album, ApiError> {
    albums(web, &[uri.to_string()])
        .await
        .remove(uri)
        .ok_or_else(ApiError::not_found)
}

pub async fn artist(web: &WebApi, uri: &str) -> Result<metadata::Artist, ApiError> {
    artists(web, &[uri.to_string()])
        .await
        .remove(uri)
        .ok_or_else(ApiError::not_found)
}

/// Resolve URIs to items in the given order, dropping the ones Spotify does
/// not know. Tracks, episodes, albums and artists are fetched in batches.
pub async fn resolve_items(web: &WebApi, uris: &[String]) -> Vec<Item> {
    let of_kind = |prefix: &str| -> Vec<String> {
        uris.iter().filter(|u| u.starts_with(prefix)).cloned().collect()
    };
    let (track_uris, episode_uris, album_uris, artist_uris) = (
        of_kind("spotify:track:"),
        of_kind("spotify:episode:"),
        of_kind("spotify:album:"),
        of_kind("spotify:artist:"),
    );
    let (tracks, episodes, albums, artists) = tokio::join!(
        tracks(web, &track_uris),
        episodes(web, &episode_uris),
        albums(web, &album_uris),
        artists(web, &artist_uris),
    );
    uris.iter()
        .filter_map(|uri| {
            if let Some(t) = tracks.get(uri) {
                track_item(uri, t)
            } else if let Some(e) = episodes.get(uri) {
                episode_item(uri, e)
            } else if let Some(a) = albums.get(uri) {
                album_item(uri, a)
            } else {
                artists.get(uri).and_then(|a| artist_item(uri, a))
            }
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Metadata to items
// ---------------------------------------------------------------------------

pub fn track_item(uri: &str, track: &metadata::Track) -> Option<Item> {
    if track.name().is_empty() {
        return None;
    }
    let album = track.album.as_ref();
    Some(Item {
        kind: VAM_KIND_TRACK,
        uri: uri.to_string(),
        title: track.name().to_string(),
        subtitle: track
            .artist
            .iter()
            .map(|a| a.name())
            .filter(|n| !n.is_empty())
            .collect::<Vec<_>>()
            .join(", "),
        image_url: album
            .map(|a| group_image(&a.cover_group, &a.cover, false))
            .unwrap_or_default(),
        album_uri: album.map(|a| gid_uri("album", a.gid())).unwrap_or_default(),
        artist_uri: track
            .artist
            .first()
            .map(|a| gid_uri("artist", a.gid()))
            .unwrap_or_default(),
        duration_ms: track.duration().max(0) as u32,
        is_explicit: track.explicit(),
        is_playable: true,
        is_saved: false,
    })
}

pub fn episode_item(uri: &str, episode: &metadata::Episode) -> Option<Item> {
    if episode.name().is_empty() {
        return None;
    }
    let show = episode.show.as_ref();
    let mut image = group_image(&episode.cover_image, &[], false);
    if image.is_empty() {
        image = show
            .map(|s| group_image(&s.cover_image, &[], false))
            .unwrap_or_default();
    }
    Some(Item {
        kind: VAM_KIND_EPISODE,
        uri: uri.to_string(),
        title: episode.name().to_string(),
        subtitle: show.map(|s| s.name().to_string()).unwrap_or_default(),
        image_url: image,
        album_uri: String::new(),
        artist_uri: show.map(|s| gid_uri("show", s.gid())).unwrap_or_default(),
        duration_ms: episode.duration().max(0) as u32,
        is_explicit: episode.explicit(),
        is_playable: true,
        is_saved: false,
    })
}

pub fn album_item(uri: &str, album: &metadata::Album) -> Option<Item> {
    if album.name().is_empty() {
        return None;
    }
    let artist = album.artist.first();
    Some(Item {
        kind: VAM_KIND_ALBUM,
        uri: uri.to_string(),
        title: album.name().to_string(),
        subtitle: format!("Album • {}", artist.map(|a| a.name()).unwrap_or("")),
        image_url: group_image(&album.cover_group, &album.cover, false),
        album_uri: String::new(),
        artist_uri: artist.map(|a| gid_uri("artist", a.gid())).unwrap_or_default(),
        duration_ms: 0,
        is_explicit: false,
        is_playable: true,
        is_saved: false,
    })
}

pub fn artist_item(uri: &str, artist: &metadata::Artist) -> Option<Item> {
    if artist.name().is_empty() {
        return None;
    }
    Some(Item {
        kind: VAM_KIND_ARTIST,
        uri: uri.to_string(),
        title: artist.name().to_string(),
        subtitle: "Artist".to_string(),
        image_url: artist_image(artist, false),
        album_uri: String::new(),
        artist_uri: String::new(),
        duration_ms: 0,
        is_explicit: false,
        is_playable: true,
        is_saved: false,
    })
}

pub fn artist_image(artist: &metadata::Artist, large: bool) -> String {
    group_image(&artist.portrait_group, &artist.portrait, large)
}

pub fn album_image(album: &metadata::Album, large: bool) -> String {
    group_image(&album.cover_group, &album.cover, large)
}

/// Track URIs of an album, disc by disc.
pub fn album_track_uris(album: &metadata::Album) -> Vec<String> {
    album
        .disc
        .iter()
        .flat_map(|d| d.track.iter())
        .map(|t| gid_uri("track", t.gid()))
        .filter(|u| !u.is_empty())
        .collect()
}

pub fn album_year(album: &metadata::Album) -> String {
    album
        .date
        .as_ref()
        .map(|d| d.year())
        .filter(|y| *y > 0)
        .map(|y| y.to_string())
        .unwrap_or_default()
}

/// Album URIs of one of an artist's album groups (albums, singles, ...).
pub fn group_album_uris(groups: &[metadata::AlbumGroup]) -> Vec<String> {
    groups
        .iter()
        .filter_map(|g| g.album.first())
        .map(|a| gid_uri("album", a.gid()))
        .filter(|u| !u.is_empty())
        .collect()
}

/// The artist's top tracks for `country`, else for any country.
pub fn top_track_uris(artist: &metadata::Artist, country: &str) -> Vec<String> {
    let top = artist
        .top_track
        .iter()
        .find(|t| t.country() == country)
        .or_else(|| artist.top_track.first());
    top.map(|t| {
        t.track
            .iter()
            .map(|t| gid_uri("track", t.gid()))
            .filter(|u| !u.is_empty())
            .collect()
    })
    .unwrap_or_default()
}

// ---------------------------------------------------------------------------
// Collection v2 (hand-encoded: librespot does not compile collection2v2.proto)
// ---------------------------------------------------------------------------

fn pb_varint(buf: &mut Vec<u8>, mut value: u64) {
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        if value == 0 {
            buf.push(byte);
            return;
        }
        buf.push(byte | 0x80);
    }
}

fn pb_bytes(buf: &mut Vec<u8>, field: u32, data: &[u8]) {
    pb_varint(buf, ((field as u64) << 3) | 2);
    pb_varint(buf, data.len() as u64);
    buf.extend_from_slice(data);
}

fn pb_uint(buf: &mut Vec<u8>, field: u32, value: u64) {
    pb_varint(buf, (field as u64) << 3);
    pb_varint(buf, value);
}

enum PbValue<'a> {
    Varint(u64),
    Bytes(&'a [u8]),
}

/// Minimal protobuf field iterator: varint and length-delimited fields are
/// returned, fixed-width fields are skipped.
struct PbReader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> PbReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        PbReader { data, pos: 0 }
    }

    fn varint(&mut self) -> Option<u64> {
        let mut value = 0u64;
        for shift in (0..64).step_by(7) {
            let byte = *self.data.get(self.pos)?;
            self.pos += 1;
            value |= ((byte & 0x7f) as u64) << shift;
            if byte & 0x80 == 0 {
                return Some(value);
            }
        }
        None
    }
}

impl<'a> Iterator for PbReader<'a> {
    type Item = (u32, PbValue<'a>);

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            if self.pos >= self.data.len() {
                return None;
            }
            let key = self.varint()?;
            let field = (key >> 3) as u32;
            match key & 7 {
                0 => return Some((field, PbValue::Varint(self.varint()?))),
                2 => {
                    let len = self.varint()? as usize;
                    let end = self.pos.checked_add(len)?;
                    let bytes = self.data.get(self.pos..end)?;
                    self.pos = end;
                    return Some((field, PbValue::Bytes(bytes)));
                }
                1 => self.pos += 8,
                5 => self.pos += 4,
                _ => return None,
            }
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct CollectionItem {
    pub uri: String,
    pub added_at: i64,
    pub is_removed: bool,
}

fn decode_collection_item(data: &[u8]) -> CollectionItem {
    let mut item = CollectionItem::default();
    for (field, value) in PbReader::new(data) {
        match (field, value) {
            (1, PbValue::Bytes(b)) => item.uri = String::from_utf8_lossy(b).into_owned(),
            (2, PbValue::Varint(v)) => item.added_at = v as i32 as i64,
            (3, PbValue::Varint(v)) => item.is_removed = v != 0,
            _ => {}
        }
    }
    item
}

fn collection_headers() -> HeaderMap {
    let mut headers = HeaderMap::new();
    headers.insert(CONTENT_TYPE, HeaderValue::from_static(COLLECTION_CONTENT_TYPE));
    headers.insert(ACCEPT, HeaderValue::from_static(COLLECTION_CONTENT_TYPE));
    headers
}

async fn collection_page(
    session: &Session,
    set: &str,
    token: &str,
) -> Result<(Vec<CollectionItem>, String), ApiError> {
    let mut body = Vec::new();
    pb_bytes(&mut body, 1, session.username().as_bytes());
    pb_bytes(&mut body, 2, set.as_bytes());
    if !token.is_empty() {
        pb_bytes(&mut body, 3, token.as_bytes());
    }
    pb_uint(&mut body, 4, COLLECTION_PAGE as u64);

    let endpoint = "/collection/v2/paging";
    let response = session
        .spclient()
        .request(&Method::POST, endpoint, Some(collection_headers()), Some(&body))
        .await
        .map_err(|e| api_error(endpoint, e))?;

    let mut items = Vec::new();
    let mut next = String::new();
    for (field, value) in PbReader::new(&response) {
        match (field, value) {
            (1, PbValue::Bytes(b)) => items.push(decode_collection_item(b)),
            (2, PbValue::Bytes(b)) => next = String::from_utf8_lossy(b).into_owned(),
            _ => {}
        }
    }
    Ok((items, next))
}

/// Every live item of a collection set, newest first.
pub async fn collection_all(web: &WebApi, set: &str) -> Result<Vec<CollectionItem>, ApiError> {
    let session = web.session()?;
    let mut all = Vec::new();
    let mut token = String::new();
    for _ in 0..COLLECTION_MAX_PAGES {
        let (items, next) = collection_page(&session, set, &token).await?;
        let got = items.len();
        all.extend(items.into_iter().filter(|i| !i.is_removed && !i.uri.is_empty()));
        if next.is_empty() || got == 0 {
            break;
        }
        token = next;
    }
    all.sort_by(|a, b| b.added_at.cmp(&a.added_at));
    Ok(all)
}

/// Add (`saved`) or remove one URI from a collection set.
pub async fn collection_write(
    web: &WebApi,
    set: &str,
    uri: &str,
    saved: bool,
) -> Result<(), ApiError> {
    let session = web.session()?;
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);

    let mut item = Vec::new();
    pb_bytes(&mut item, 1, uri.as_bytes());
    pb_uint(&mut item, 2, now);
    if !saved {
        pb_uint(&mut item, 3, 1);
    }

    let mut body = Vec::new();
    pb_bytes(&mut body, 1, session.username().as_bytes());
    pb_bytes(&mut body, 2, set.as_bytes());
    pb_bytes(&mut body, 3, &item);
    pb_bytes(&mut body, 4, format!("{now:x}{:x}", rand_u32()).as_bytes());

    let endpoint = "/collection/v2/write";
    session
        .spclient()
        .request(&Method::POST, endpoint, Some(collection_headers()), Some(&body))
        .await
        .map(|_| ())
        .map_err(|e| api_error(endpoint, e))
}

fn rand_u32() -> u32 {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.subsec_nanos())
        .unwrap_or(0);
    nanos ^ 0x9e37_79b9
}

// ---------------------------------------------------------------------------
// Playlists
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default)]
pub struct PlaylistInfo {
    pub uri: String,
    pub name: String,
    pub description: String,
    pub image_url: String,
    /// Owner's username (an opaque id for most accounts).
    pub owner: String,
    /// Owner's profile name, or the username when Spotify has none.
    pub owner_name: String,
    pub length: u32,
    pub track_uris: Vec<String>,
}

fn playlist_image(attributes: Option<&ListAttributes>) -> String {
    let Some(attributes) = attributes else {
        return String::new();
    };
    for wanted in ["default", "large", "small"] {
        if let Some(p) = attributes
            .picture_size
            .iter()
            .find(|p| p.target_name() == wanted && !p.url().is_empty())
        {
            return p.url().to_string();
        }
    }
    if let Some(p) = attributes.picture_size.iter().find(|p| !p.url().is_empty()) {
        return p.url().to_string();
    }
    image_url(attributes.picture())
}

/// Playlist descriptions arrive HTML-escaped (`&#x2F;`, `&amp;`, ...).
fn unescape_html(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut rest = text;
    while let Some(start) = rest.find('&') {
        out.push_str(&rest[..start]);
        rest = &rest[start..];
        let decoded = rest.find(';').filter(|&end| end <= 10).and_then(|end| {
            let entity = &rest[1..end];
            let ch = match entity {
                "amp" => Some('&'),
                "lt" => Some('<'),
                "gt" => Some('>'),
                "quot" => Some('"'),
                "apos" => Some('\''),
                _ => {
                    let code = if let Some(hex) =
                        entity.strip_prefix("#x").or_else(|| entity.strip_prefix("#X"))
                    {
                        u32::from_str_radix(hex, 16).ok()
                    } else {
                        entity.strip_prefix('#').and_then(|dec| dec.parse().ok())
                    };
                    code.and_then(char::from_u32)
                }
            };
            ch.map(|c| (c, end))
        });
        match decoded {
            Some((c, end)) => {
                out.push(c);
                rest = &rest[end + 1..];
            }
            None => {
                out.push('&');
                rest = &rest[1..];
            }
        }
    }
    out.push_str(rest);
    out
}

pub fn playlist_item(info: &PlaylistInfo) -> Item {
    Item {
        kind: VAM_KIND_PLAYLIST,
        uri: info.uri.clone(),
        title: info.name.clone(),
        subtitle: if info.owner_name.is_empty() {
            "Playlist".to_string()
        } else {
            format!("Playlist • {}", info.owner_name)
        },
        image_url: info.image_url.clone(),
        album_uri: String::new(),
        artist_uri: String::new(),
        duration_ms: 0,
        is_explicit: false,
        is_playable: true,
        is_saved: true,
    }
}

/// The user's playlists in library order (folders are flattened).
pub async fn rootlist(web: &WebApi) -> Result<Vec<PlaylistInfo>, ApiError> {
    let session = web.session()?;
    let endpoint = "/playlist/v2/user/<user>/rootlist";
    let bytes = session
        .spclient()
        .get_rootlist(0, Some(500))
        .await
        .map_err(|e| api_error(endpoint, e))?;
    let list = SelectedListContent::parse_from_bytes(&bytes)
        .map_err(|e| ApiError::network(format!("Bad playlist list from Spotify: {e}")))?;
    let Some(contents) = list.contents.as_ref() else {
        return Ok(Vec::new());
    };
    let mut out = Vec::new();
    for (index, item) in contents.items.iter().enumerate() {
        let uri = item.uri();
        if !uri.starts_with("spotify:playlist:") {
            continue;
        }
        let meta = contents.meta_items.get(index);
        let attributes = meta.and_then(|m| m.attributes.as_ref());
        out.push(PlaylistInfo {
            uri: uri.to_string(),
            name: attributes.map(|a| a.name().to_string()).unwrap_or_default(),
            description: String::new(),
            image_url: playlist_image(attributes),
            owner: meta.map(|m| m.owner_username().to_string()).unwrap_or_default(),
            owner_name: String::new(),
            length: meta.map(|m| m.length().max(0) as u32).unwrap_or(0),
            track_uris: Vec::new(),
        });
    }
    let names = profile_names(&session, out.iter().map(|p| p.owner.as_str())).await;
    for info in &mut out {
        info.owner_name = names.get(&info.owner).cloned().unwrap_or_default();
    }
    fill_mosaic_covers(web, &session, &mut out).await;
    Ok(out)
}

// ---------------------------------------------------------------------------
// Generated playlist covers
// ---------------------------------------------------------------------------

/// Tracks read per playlist when building its cover: enough to usually find
/// four different albums.
const MOSAIC_TRACKS: usize = 20;

/// Generated covers by playlist URI for this session.
static MOSAIC_COVERS: Mutex<Option<HashMap<String, String>>> = Mutex::new(None);

fn cached_mosaic(uri: &str) -> Option<String> {
    let cache = MOSAIC_COVERS.lock().unwrap_or_else(|e| e.into_inner());
    cache.as_ref().and_then(|c| c.get(uri).cloned())
}

fn cache_mosaic(uri: &str, cover: &str) {
    let mut cache = MOSAIC_COVERS.lock().unwrap_or_else(|e| e.into_inner());
    cache
        .get_or_insert_with(HashMap::new)
        .insert(uri.to_string(), cover.to_string());
}

/// Hex file id of a track's album cover (largest available), if any.
fn track_cover_id(track: &metadata::Track) -> Option<String> {
    let album = track.album.as_ref()?;
    let images = album
        .cover_group
        .as_ref()
        .map(|g| g.image.as_slice())
        .filter(|i| !i.is_empty())
        .unwrap_or(album.cover.as_slice());
    [Size::LARGE, Size::DEFAULT, Size::XLARGE, Size::SMALL]
        .iter()
        .find_map(|size| images.iter().find(|i| i.size() == *size))
        .or_else(|| images.first())
        .map(|i| hex(i.file_id()))
        .filter(|id| !id.is_empty())
}

/// Spotify's cover for a playlist without a picture: a 2x2 mosaic of the
/// first four different album covers, or the first cover when there are
/// fewer. `mosaic.scdn.co` is the image service the official clients use.
fn mosaic_url(track_uris: &[String], tracks: &HashMap<String, metadata::Track>) -> String {
    let mut ids: Vec<String> = Vec::new();
    for uri in track_uris {
        if let Some(id) = tracks.get(uri).and_then(track_cover_id) {
            if !ids.contains(&id) {
                ids.push(id);
                if ids.len() == 4 {
                    break;
                }
            }
        }
    }
    match ids.len() {
        0 => String::new(),
        4 => format!("https://mosaic.scdn.co/300/{}", ids.concat()),
        _ => format!("https://i.scdn.co/image/{}", ids[0]),
    }
}

fn first_tracks(track_uris: &[String]) -> Vec<String> {
    track_uris
        .iter()
        .filter(|u| u.starts_with("spotify:track:"))
        .take(MOSAIC_TRACKS)
        .cloned()
        .collect()
}

/// Generated cover for one playlist whose items are already known.
async fn mosaic_cover(web: &WebApi, uri: &str, track_uris: &[String]) -> String {
    if let Some(cover) = cached_mosaic(uri) {
        return cover;
    }
    let first = first_tracks(track_uris);
    let cover = mosaic_url(&first, &tracks(web, &first).await);
    if !cover.is_empty() {
        cache_mosaic(uri, &cover);
    }
    cover
}

/// The first items of a playlist, without downloading the whole list.
async fn playlist_head(session: &Session, uri: &str) -> Vec<String> {
    let id = uri.rsplit(':').next().unwrap_or("");
    let endpoint = format!("/playlist/v2/playlist/{id}?from=0&length={MOSAIC_TRACKS}");
    let bytes = match session.spclient().request(&Method::GET, &endpoint, None, None).await {
        Ok(bytes) => bytes,
        Err(e) => {
            let _ = api_error("/playlist/v2/playlist/<id> (cover)", e);
            return Vec::new();
        }
    };
    SelectedListContent::parse_from_bytes(&bytes)
        .ok()
        .and_then(|list| list.contents.into_option())
        .map(|c| c.items.iter().map(|i| i.uri().to_string()).collect())
        .unwrap_or_default()
}

/// Generated covers for library playlists that have no picture of their own.
async fn fill_mosaic_covers(web: &WebApi, session: &Session, playlists: &mut [PlaylistInfo]) {
    let mut missing: Vec<String> = Vec::new();
    for info in playlists.iter_mut().filter(|p| p.image_url.is_empty()) {
        match cached_mosaic(&info.uri) {
            Some(cover) => info.image_url = cover,
            None => missing.push(info.uri.clone()),
        }
    }
    if missing.is_empty() {
        return;
    }
    let heads: Vec<(String, Vec<String>)> = stream::iter(missing)
        .map(|uri| async move {
            let items = first_tracks(&playlist_head(session, &uri).await);
            (uri, items)
        })
        .buffer_unordered(PROFILE_CONCURRENCY)
        .collect()
        .await;
    let all: Vec<String> = heads.iter().flat_map(|(_, items)| items.iter().cloned()).collect();
    let metadata = tracks(web, &all).await;
    for (uri, items) in heads {
        let cover = mosaic_url(&items, &metadata);
        if cover.is_empty() {
            continue;
        }
        cache_mosaic(&uri, &cover);
        if let Some(info) = playlists.iter_mut().find(|p| p.uri == uri) {
            info.image_url = cover;
        }
    }
}

/// A playlist's attributes and every item URI.
pub async fn playlist(web: &WebApi, uri: &str) -> Result<PlaylistInfo, ApiError> {
    let session = web.session()?;
    let id = uri.rsplit(':').next().unwrap_or("");
    let playlist_id = SpotifyId::from_base62(id).map_err(|_| ApiError::not_found())?;
    let endpoint = "/playlist/v2/playlist/<id>";
    let bytes = session
        .spclient()
        .get_playlist(&playlist_id)
        .await
        .map_err(|e| api_error(endpoint, e))?;
    let list = SelectedListContent::parse_from_bytes(&bytes)
        .map_err(|e| ApiError::network(format!("Bad playlist from Spotify: {e}")))?;
    let attributes = list.attributes.as_ref();
    let track_uris: Vec<String> = list
        .contents
        .as_ref()
        .map(|c| c.items.iter().map(|i| i.uri().to_string()).collect())
        .unwrap_or_default();
    let mut image_url = playlist_image(attributes);
    if image_url.is_empty() {
        image_url = mosaic_cover(web, uri, &track_uris).await;
    }
    let owner = list.owner_username().to_string();
    let owner_name = profile_names(&session, [owner.as_str()])
        .await
        .remove(&owner)
        .unwrap_or_default();
    Ok(PlaylistInfo {
        uri: uri.to_string(),
        name: attributes.map(|a| a.name().to_string()).unwrap_or_default(),
        description: attributes.map(|a| unescape_html(a.description())).unwrap_or_default(),
        image_url,
        owner,
        owner_name,
        length: if list.length() > 0 {
            list.length() as u32
        } else {
            track_uris.len() as u32
        },
        track_uris,
    })
}

// ---------------------------------------------------------------------------
// Recently played and profile (best effort)
// ---------------------------------------------------------------------------

/// Context URIs (albums, playlists, artists) the user played recently.
pub async fn recently_played_contexts(web: &WebApi) -> Vec<String> {
    let Ok(session) = web.session() else {
        return Vec::new();
    };
    let endpoint = format!(
        "/recently-played/v3/user/{}/recently-played?format=json&offset=0&limit=50&filter=default",
        session.username()
    );
    let bytes = match session
        .spclient()
        .request_as_json(&Method::GET, &endpoint, None, None)
        .await
    {
        Ok(bytes) => bytes,
        Err(e) => {
            let _ = api_error("/recently-played", e);
            return Vec::new();
        }
    };
    let Ok(value) = serde_json::from_slice::<Value>(&bytes) else {
        log::warn!("recently-played: unexpected response");
        return Vec::new();
    };
    let contexts = value
        .get("playContexts")
        .and_then(|v| v.as_array())
        .cloned()
        .unwrap_or_default();
    contexts
        .iter()
        .filter_map(|c| c.get("uri").and_then(|v| v.as_str()))
        .map(|s| s.to_string())
        .collect()
}

/// The signed-in account's profile: display name (empty if Spotify has
/// none) and picture URL (empty if none).
pub async fn own_profile(web: &WebApi) -> Profile {
    let Ok(session) = web.session() else {
        return Profile::default();
    };
    let username = session.username();
    let mut profile = fetch_profile(&session, &username).await.unwrap_or_default();
    if profile.name.is_empty() || profile.name == username {
        profile.name.clear();
    } else {
        let mut cache = PROFILE_NAMES.lock().unwrap_or_else(|e| e.into_inner());
        cache
            .get_or_insert_with(HashMap::new)
            .insert(username, profile.name.clone());
    }
    profile
}

#[derive(Debug, Clone, Default)]
pub struct Profile {
    pub name: String,
    pub image_url: String,
}

/// Profile names by username for this session. Playlists only carry the
/// owner's username, which for most accounts is an opaque id.
static PROFILE_NAMES: Mutex<Option<HashMap<String, String>>> = Mutex::new(None);

/// Lookups in flight at once when a library lists many owners.
const PROFILE_CONCURRENCY: usize = 6;

/// Profile name for each username, falling back to the username itself when
/// Spotify has none or the lookup fails. Results are cached for the session.
async fn profile_names<'a>(
    session: &Session,
    usernames: impl IntoIterator<Item = &'a str>,
) -> HashMap<String, String> {
    let mut wanted: Vec<String> = usernames
        .into_iter()
        .filter(|u| !u.is_empty())
        .map(|u| u.to_string())
        .collect();
    wanted.sort();
    wanted.dedup();

    let mut out = HashMap::new();
    let mut missing = Vec::new();
    {
        let cache = PROFILE_NAMES.lock().unwrap_or_else(|e| e.into_inner());
        for username in wanted {
            match cache.as_ref().and_then(|c| c.get(&username)) {
                Some(name) => {
                    out.insert(username, name.clone());
                }
                None => missing.push(username),
            }
        }
    }
    if missing.is_empty() {
        return out;
    }

    let fetched: Vec<(String, String)> = stream::iter(missing)
        .map(|username| async move {
            let name = fetch_profile(session, &username)
                .await
                .map(|p| p.name)
                .filter(|n| !n.is_empty())
                .unwrap_or_else(|| username.clone());
            (username, name)
        })
        .buffer_unordered(PROFILE_CONCURRENCY)
        .collect()
        .await;

    let mut cache = PROFILE_NAMES.lock().unwrap_or_else(|e| e.into_inner());
    let cache = cache.get_or_insert_with(HashMap::new);
    for (username, name) in fetched {
        cache.insert(username.clone(), name.clone());
        out.insert(username, name);
    }
    out
}

async fn fetch_profile(session: &Session, username: &str) -> Option<Profile> {
    // No limits: Spotify answers 400 Bad Request to playlist_limit=0.
    let bytes = match session.spclient().get_user_profile(username, None, None).await {
        Ok(bytes) => bytes,
        Err(e) => {
            let _ = api_error("/user-profile-view", e);
            return None;
        }
    };
    let value: Value = serde_json::from_slice(&bytes).ok()?;
    let text = |key: &str| value.get(key).and_then(|v| v.as_str()).unwrap_or("").to_string();
    Some(Profile {
        name: text("name"),
        image_url: text("image_url"),
    })
}

#[cfg(test)]
mod tests {
    use super::unescape_html;

    #[test]
    fn unescapes_playlist_descriptions() {
        assert_eq!(unescape_html("https:&#x2F;&#x2F;soundiiz.com"), "https://soundiiz.com");
        assert_eq!(unescape_html("R&amp;B &#39;hits&#39; &lt;3"), "R&B 'hits' <3");
        assert_eq!(unescape_html("A & B &bogus; &"), "A & B &bogus; &");
    }
}
