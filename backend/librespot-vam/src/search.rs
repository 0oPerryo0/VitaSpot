//! Search through Spotify's GraphQL API ("pathfinder"), the one the official
//! clients use. The public Web API search answers HTTP 429 for librespot's
//! shared client id, and the old `searchview` endpoints are gone (404/400).
//!
//! Pathfinder only runs allow-listed ("persisted") queries identified by a
//! SHA-256 hash. If Spotify retires the hash below, searches fail with
//! `PersistedQueryNotFound` and the hash needs updating.

use bytes::Bytes;
use http::{Method, Request, StatusCode};
use http_body_util::BodyExt;
use serde_json::{json, Value};

use crate::abi::*;
use crate::model::Item;
use crate::webapi::{ApiError, WebApi};

const PATHFINDER_URL: &str = "https://api-partner.spotify.com/pathfinder/v2/query";
const SEARCH_OPERATION: &str = "searchDesktop";
const SEARCH_HASH: &str = "21969b655b795601fb2d2204a4243188e75fdc6d3520e7b9cd3f4db2aff9591e";

/// One page of search results, already converted to items.
#[derive(Debug, Default)]
pub struct Results {
    pub tracks: Vec<Item>,
    pub tracks_total: u32,
    pub artists: Vec<Item>,
    pub albums: Vec<Item>,
    pub playlists: Vec<Item>,
}

pub async fn search(web: &WebApi, query: &str, offset: u32, limit: u32) -> Result<Results, ApiError> {
    let variables = json!({
        "searchTerm": query,
        "offset": offset,
        "limit": limit,
        "numberOfTopResults": 5,
        "includeAudiobooks": false,
    });
    let data = pathfinder(web, SEARCH_OPERATION, SEARCH_HASH, variables).await?;
    let root = data.pointer("/data/searchV2").cloned().unwrap_or(Value::Null);

    let tracks_value = root.get("tracksV2").cloned().unwrap_or(Value::Null);
    Ok(Results {
        tracks: items(&tracks_value)
            .iter()
            .filter_map(|i| track_item(i.pointer("/item/data")?))
            .collect(),
        tracks_total: count(&tracks_value),
        artists: items(root.get("artists").unwrap_or(&Value::Null))
            .iter()
            .filter_map(|i| artist_item(i.get("data")?))
            .collect(),
        albums: items(root.get("albums").unwrap_or(&Value::Null))
            .iter()
            .filter_map(|i| album_item(i.get("data")?))
            .collect(),
        playlists: items(root.get("playlists").unwrap_or(&Value::Null))
            .iter()
            .filter_map(|i| playlist_item(i.get("data")?))
            .collect(),
    })
}

async fn pathfinder(web: &WebApi, operation: &str, hash: &str, variables: Value) -> Result<Value, ApiError> {
    let session = web.session()?;
    let token = session
        .login5()
        .auth_token()
        .await
        .map_err(|e| ApiError::network(format!("Couldn't sign the search request: {e}")))?;
    let client_token = session.spclient().client_token().await.unwrap_or_default();
    let body = json!({
        "variables": variables,
        "operationName": operation,
        "extensions": { "persistedQuery": { "version": 1, "sha256Hash": hash } },
    });
    let request = Request::builder()
        .method(Method::POST)
        .uri(PATHFINDER_URL)
        .header("authorization", format!("Bearer {}", token.access_token))
        .header("client-token", client_token)
        .header("accept", "application/json")
        .header("content-type", "application/json;charset=UTF-8")
        .header("app-platform", "WebPlayer")
        .body(Bytes::from(body.to_string()))
        .map_err(|e| ApiError::network(format!("Bad search request: {e}")))?;

    // request_fut hands back error statuses too, so their body can be logged.
    let response = session
        .http_client()
        .request_fut(request)
        .map_err(|e| ApiError::network(format!("Search failed: {e}")))?
        .await
        .map_err(|e| ApiError::network(format!("Search failed: {e}")))?;
    let status = response.status();
    let bytes = response
        .into_body()
        .collect()
        .await
        .map(|c| c.to_bytes())
        .unwrap_or_default();
    if status == StatusCode::TOO_MANY_REQUESTS {
        return Err(ApiError::rate_limited(
            "Spotify is rate limiting search right now. Try again in a minute.",
        ));
    }
    let value: Value = serde_json::from_slice(&bytes).unwrap_or(Value::Null);
    if !status.is_success() || value.get("data").map_or(true, Value::is_null) {
        let detail = value
            .pointer("/errors/0/message")
            .and_then(Value::as_str)
            .map(str::to_string)
            .unwrap_or_else(|| String::from_utf8_lossy(&bytes[..bytes.len().min(200)]).into_owned());
        log::warn!("pathfinder {operation} failed: HTTP {status}: {detail}");
        return Err(ApiError::network("Search isn't working right now."));
    }
    Ok(value)
}

fn items(section: &Value) -> Vec<Value> {
    section
        .get("items")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default()
}

fn count(section: &Value) -> u32 {
    section.get("totalCount").and_then(Value::as_u64).unwrap_or(0) as u32
}

fn text<'a>(value: &'a Value, pointer: &str) -> &'a str {
    value.pointer(pointer).and_then(Value::as_str).unwrap_or("")
}

/// The source closest to 300 px wide from an image's `sources` list.
fn image(value: Option<&Value>) -> String {
    let Some(sources) = value.and_then(|v| v.get("sources")).and_then(Value::as_array) else {
        return String::new();
    };
    sources
        .iter()
        .min_by_key(|s| (s.get("width").and_then(Value::as_i64).unwrap_or(0) - 300).abs())
        .and_then(|s| s.get("url").and_then(Value::as_str))
        .unwrap_or("")
        .to_string()
}

fn artist_names(value: &Value) -> String {
    value
        .pointer("/artists/items")
        .and_then(Value::as_array)
        .map(|artists| {
            artists
                .iter()
                .map(|a| text(a, "/profile/name"))
                .filter(|n| !n.is_empty())
                .collect::<Vec<_>>()
                .join(", ")
        })
        .unwrap_or_default()
}

fn track_item(data: &Value) -> Option<Item> {
    let uri = text(data, "/uri");
    if !uri.starts_with("spotify:track:") {
        return None;
    }
    Some(Item {
        kind: VAM_KIND_TRACK,
        uri: uri.to_string(),
        title: text(data, "/name").to_string(),
        subtitle: artist_names(data),
        image_url: image(data.pointer("/albumOfTrack/coverArt")),
        album_uri: text(data, "/albumOfTrack/uri").to_string(),
        artist_uri: text(data, "/artists/items/0/uri").to_string(),
        duration_ms: data
            .pointer("/duration/totalMilliseconds")
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        is_explicit: text(data, "/contentRating/label") == "EXPLICIT",
        is_playable: data
            .pointer("/playability/playable")
            .and_then(Value::as_bool)
            .unwrap_or(true),
        is_saved: false,
    })
}

fn artist_item(data: &Value) -> Option<Item> {
    let uri = text(data, "/uri");
    if !uri.starts_with("spotify:artist:") {
        return None;
    }
    Some(Item {
        kind: VAM_KIND_ARTIST,
        uri: uri.to_string(),
        title: text(data, "/profile/name").to_string(),
        subtitle: "Artist".to_string(),
        image_url: image(data.pointer("/visuals/avatarImage")),
        is_playable: true,
        ..Default::default()
    })
}

fn album_item(data: &Value) -> Option<Item> {
    let uri = text(data, "/uri");
    if !uri.starts_with("spotify:album:") {
        return None;
    }
    let artist = text(data, "/artists/items/0/profile/name");
    Some(Item {
        kind: VAM_KIND_ALBUM,
        uri: uri.to_string(),
        title: text(data, "/name").to_string(),
        subtitle: format!("Album • {artist}"),
        image_url: image(data.get("coverArt")),
        artist_uri: text(data, "/artists/items/0/uri").to_string(),
        is_playable: true,
        ..Default::default()
    })
}

fn playlist_item(data: &Value) -> Option<Item> {
    let uri = text(data, "/uri");
    if !uri.starts_with("spotify:playlist:") {
        return None;
    }
    let owner = text(data, "/ownerV2/data/name");
    Some(Item {
        kind: VAM_KIND_PLAYLIST,
        uri: uri.to_string(),
        title: text(data, "/name").to_string(),
        subtitle: if owner.is_empty() {
            "Playlist".to_string()
        } else {
            format!("Playlist • {owner}")
        },
        image_url: image(data.pointer("/images/items/0")),
        is_playable: true,
        ..Default::default()
    })
}
