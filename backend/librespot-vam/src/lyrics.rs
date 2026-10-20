//! Track lyrics for `vam_request_lyrics`, from Spotify's color-lyrics endpoint.
//!
//! The reply is parsed loosely through `serde_json::Value` instead of
//! librespot-metadata's strict structs, so a field Spotify adds or drops
//! doesn't lose the whole song. Only what the UI draws is kept: one text
//! buffer plus a start time and offset per line.

use std::os::raw::c_char;

use librespot_core::SpotifyId;
use serde_json::Value;

use crate::abi::*;
use crate::internal::api_error;
use crate::shared::Shared;
use crate::webapi::ApiError;

/// A song keeps at most this many lines and bytes of text.
const MAX_LINES: usize = 300;
const MAX_TEXT: usize = 16 * 1024;
/// Longer lines are cut (on a code point boundary).
const MAX_LINE_BYTES: usize = 512;

pub struct Lyrics {
    synced: bool,
    lines: Vec<vam_lyrics_line>,
    text: Vec<u8>,
}

pub async fn fetch_lyrics(shared: &Shared, track_uri: &str) -> Result<Lyrics, ApiError> {
    let base62 = track_uri
        .strip_prefix("spotify:track:")
        .ok_or_else(ApiError::not_found)?;
    let id = SpotifyId::from_base62(base62).map_err(|_| ApiError::not_found())?;
    let session = shared.web.session()?;
    let bytes = session
        .spclient()
        .get_lyrics(&id)
        .await
        .map_err(|e| api_error("/color-lyrics/v2/track/<id>", e))?;
    // Spotify answers an empty body, not a 404, for some tracks without lyrics.
    let root: Value = serde_json::from_slice(&bytes).map_err(|_| ApiError::not_found())?;
    parse(&root).ok_or_else(ApiError::not_found)
}

fn parse(root: &Value) -> Option<Lyrics> {
    let lyrics = root.get("lyrics")?;
    let synced = lyrics.get("syncType").and_then(Value::as_str) == Some("LINE_SYNCED");
    let mut out = Lyrics {
        synced,
        lines: Vec::new(),
        text: Vec::new(),
    };
    let mut last_start = 0u32;
    for line in lyrics.get("lines")?.as_array()? {
        if out.lines.len() >= MAX_LINES {
            break;
        }
        let words = line.get("words").and_then(Value::as_str).unwrap_or("");
        let words = clean(words);
        if out.text.len() + words.len() + 1 > MAX_TEXT {
            break;
        }
        let start_ms = if synced {
            // startTimeMs is a string ("12345"); accept a number too.
            let start = match line.get("startTimeMs") {
                Some(Value::String(s)) => s.trim().parse::<u64>().ok(),
                Some(Value::Number(n)) => n.as_u64(),
                _ => None,
            }
            .unwrap_or(last_start as u64)
            .min(u32::MAX as u64) as u32;
            // The UI walks lines in order: keep the times from going backwards.
            last_start = start.max(last_start);
            last_start
        } else {
            0
        };
        out.lines.push(vam_lyrics_line {
            start_ms,
            text_offset: out.text.len() as u32,
        });
        out.text.extend_from_slice(words.as_bytes());
        out.text.push(0);
    }
    // A song made only of empty lines has nothing to show.
    if out.lines.is_empty() || out.text.iter().all(|&b| b == 0) {
        return None;
    }
    Some(out)
}

/// One display line: no control characters, trimmed, at most MAX_LINE_BYTES.
fn clean(words: &str) -> String {
    let mut s: String = words
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect::<String>()
        .trim()
        .to_string();
    if s.len() > MAX_LINE_BYTES {
        let mut n = MAX_LINE_BYTES;
        while !s.is_char_boundary(n) {
            n -= 1;
        }
        s.truncate(n);
    }
    s
}

impl Lyrics {
    pub fn into_raw(self) -> *mut vam_lyrics {
        let line_count = self.lines.len() as u32;
        let lines = Box::into_raw(self.lines.into_boxed_slice()) as *mut vam_lyrics_line;
        let text_size = self.text.len() as u32;
        let text = Box::into_raw(self.text.into_boxed_slice()) as *const c_char;
        Box::into_raw(Box::new(vam_lyrics {
            synced: self.synced as u8,
            reserved: [0; 3],
            line_count,
            lines,
            text_size,
            text,
        }))
    }
}

/// Free lyrics previously produced by [`Lyrics::into_raw`].
///
/// # Safety
/// `lyrics` must be null or a pointer returned by `Lyrics::into_raw` that has
/// not been freed yet.
pub unsafe fn free_lyrics(lyrics: *mut vam_lyrics) {
    if lyrics.is_null() {
        return;
    }
    let lyrics = Box::from_raw(lyrics);
    drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
        lyrics.lines,
        lyrics.line_count as usize,
    )));
    drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
        lyrics.text as *mut u8,
        lyrics.text_size as usize,
    )));
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lyrics(json: &str) -> Option<Lyrics> {
        parse(&serde_json::from_str(json).unwrap())
    }

    fn line(l: &Lyrics, i: usize) -> &str {
        let start = l.lines[i].text_offset as usize;
        let end = start + l.text[start..].iter().position(|&b| b == 0).unwrap();
        std::str::from_utf8(&l.text[start..end]).unwrap()
    }

    #[test]
    fn parses_synced_lines() {
        let l = lyrics(
            r#"{"lyrics":{"syncType":"LINE_SYNCED","lines":[
                {"startTimeMs":"1000","words":"夜空に咲いた"},
                {"startTimeMs":"2500","words":" second\nline "},
                {"startTimeMs":"2000","words":"♪"}]}}"#,
        )
        .unwrap();
        assert!(l.synced);
        assert_eq!(l.lines.len(), 3);
        assert_eq!(line(&l, 0), "夜空に咲いた");
        assert_eq!(line(&l, 1), "second line");
        assert_eq!(l.lines[1].start_ms, 2500);
        // Times never go backwards.
        assert_eq!(l.lines[2].start_ms, 2500);
    }

    #[test]
    fn unsynced_lines_have_no_times() {
        let l = lyrics(r#"{"lyrics":{"syncType":"UNSYNCED","lines":[{"startTimeMs":"5","words":"a"}]}}"#)
            .unwrap();
        assert!(!l.synced);
        assert_eq!(l.lines[0].start_ms, 0);
    }

    #[test]
    fn empty_or_broken_replies_are_not_found() {
        assert!(lyrics(r#"{}"#).is_none());
        assert!(lyrics(r#"{"lyrics":{"lines":[]}}"#).is_none());
        assert!(lyrics(r#"{"lyrics":{"lines":[{"words":""}]}}"#).is_none());
    }

    #[test]
    fn caps_line_count() {
        let lines: Vec<String> = (0..400).map(|i| format!(r#"{{"words":"l{i}"}}"#)).collect();
        let l = lyrics(&format!(r#"{{"lyrics":{{"lines":[{}]}}}}"#, lines.join(","))).unwrap();
        assert_eq!(l.lines.len(), MAX_LINES);
    }

    #[test]
    fn round_trips_through_raw() {
        let l = lyrics(r#"{"lyrics":{"lines":[{"words":"a"},{"words":"bc"}]}}"#).unwrap();
        let raw = l.into_raw();
        unsafe {
            assert_eq!((*raw).line_count, 2);
            assert_eq!((*raw).text_size, 5);
            free_lyrics(raw);
        }
    }
}
