# Vendored crates

`Cargo.toml` patches ten crates.io crates with the copies in this directory:

```toml
[patch.crates-io]
librespot-audio = { path = "vendor/librespot-audio" }
librespot-core = { path = "vendor/librespot-core" }
librespot-connect = { path = "vendor/librespot-connect" }
librespot-discovery = { path = "vendor/librespot-discovery" }
librespot-oauth = { path = "vendor/librespot-oauth" }
librespot-playback = { path = "vendor/librespot-playback" }
rustix = { path = "vendor/rustix" }
if-addrs = { path = "vendor/if-addrs" }
tempfile = { path = "vendor/tempfile" }
ring = { path = "vendor/ring" }
```

The librespot crates are upstream **librespot 0.8.0** (MIT, see each crate's
`Cargo.toml`); rustix is 1.1.5, if-addrs 0.14.0 and tempfile 3.27.0 (all MIT/Apache-2.0). The
patches are as small as possible so they are easy to rebase onto a newer
release.

## librespot-audio

librespot streams each track into a temp file. One handle writes the
downloaded ranges, and a second handle (`NamedTempFile::reopen`) reads them
for the decoder. This patch was added while chasing "probe reach EOF at 0
bytes" on the Vita, which turned out to be the condvar deadline bug fixed in
`vita-shim/vita_shim.c`. It is kept as a precaution: whether the Vita shows
writes from one handle through a second handle has not been tested on its
own. Vita-only changes:

* `src/fetch/mod.rs` - the reader gets a `dup()` of the writer's descriptor,
  so both use the same SceUID. Reads use `pread` (`FileExt::read_at`), and
  `Seek` only updates the stored position instead of the shared file offset.
* `src/fetch/receive.rs` - downloaded ranges are written with `pwrite`
  (`FileExt::write_all_at`).
* `Cargo.toml` - `libc` dependency for `dup()` on the Vita.

## librespot-playback

`src/player.rs` - `load_track` no longer calls `decoder.seek(0)` when a track
starts from the beginning. Symphonia's Ogg seek bisects the whole file even
for position 0, and each probe is a blocking CDN range request: four extra
round trips per track, about 2-3 s of the load time on the Vita. After
probing, the demuxer already sits at the first audio packet. Starts at a
non-zero position (resume, seek-on-load) still seek as before.

`src/config.rs` / `src/player.rs` - new `PlayerConfig::live_bitrate`
(`Option<Arc<AtomicU32>>`). When set to 96, 160 or 320, `load_track` picks
formats for that bitrate instead of `bitrate`, so the audio quality setting
applies from the next track without recreating the player (which would mean
signing in again). Preloads remember the `live_bitrate` they were started
with and are dropped when it no longer matches, so a next track fetched
before the change is loaded again at the new quality.

`src/audio_backend/mod.rs` / `src/player.rs` - seeks respond faster:

* `Sink` gains a `flush()` method (default no-op), which
  `handle_command_seek` calls once the decoder has seeked. The Vita sink
  empties its 1 s ring buffer, which otherwise played out pre-seek audio
  first.
* After a seek, `preload_data_before_playback` blocks only until
  `read_ahead_before_playback` (1 s) of audio is downloaded instead of
  `read_ahead_during_playback` (5 s). It still requests the full 5 s.
* Each seek logs how long the decoder seek (Ogg bisection, one CDN round
  trip per probe of data not yet downloaded) and the read-ahead wait took.

## librespot-core

* `src/session.rs` - `check_catalogue()` upstream calls `std::process::exit(1)`
  as soon as Spotify reports a non-Premium account. A library cannot terminate
  its host process: the VitaSpot UI has a dedicated "Premium required" screen, so
  the account type is now only logged and the frontend is told through
  `vam_auth_state.is_premium = 0`.
* `src/error.rs`, `Cargo.toml` - the `librespot-oauth` dependency (and the
  `From<OAuthError>` conversion) were removed. librespot-core only used it for
  that conversion, but it dragged `reqwest`, `oauth2` and the desktop-only
  `open` crate into the Vita build. VitaSpot does OAuth on a PC (`vam-login`) or
  on the phone via Zeroconf.
* `build.rs` - upstream generates version strings through `vergen-gitcl`. Its
  crates.io dependency graph can resolve two incompatible `vergen-lib`
  versions (observed with vergen-gitcl 1.0.8) and git metadata is meaningless
  for a vendored crate, so the script sets the four `cargo:rustc-env` values
  that `src/version.rs` reads. The now unused build dependencies were removed
  from `Cargo.toml`.
* `src/cache.rs` - `Cache::new` with a size limit listed every file in the
  audio cache (one stat each, ~25 ms on the Vita) before returning, and the
  engine opens the cache before it starts Zeroconf, so a full cache held up
  the login screen and auto-login for seconds. `FsSizeLimiter::new` now runs
  that scan on a background thread (`audio-cache-scan`, 128 KiB stack) and
  merges the result; files saved or opened meanwhile are tracked as usual,
  and `touch()` adds a file the scan hasn't listed yet instead of logging
  "limiter could not touch". Pruning starts once the scan is done.

No protocol, authentication or playback behaviour was changed.

## librespot-connect

Upstream `Spirc` can only send commands; the connect state is private to its
event loop, which makes it impossible to implement the player snapshot,
`context_title`, queue and `is_active_device` fields of the VitaSpot contract.
Changes:

* `src/state.rs` - new public `ConnectSnapshot` and `ConnectState::snapshot()`
  which copy the current `PlayerState` / `DeviceInfo` without touching the
  network.
* `src/spirc.rs`
  * `Spirc` now holds an `Arc<Mutex<ConnectSnapshot>>` and exposes
    `Spirc::snapshot()`. `SpircTask::run()` refreshes it after every event loop
    iteration.
  * `Spirc` derives `Clone` (its only field is an mpsc sender) so command
    tasks can hold a handle.
  * new `SpircCommand::AddToQueue` / `Spirc::add_to_queue()`; upstream only
    handled the remote `AddToQueue` push, with no local API for "add to queue".
  * the delayed volume update fires `VOLUME_UPDATE_DELAY` after the first
    unsent change (`update_volume_due`). Upstream restarted the delay on every
    event loop iteration, so a steady stream of volume changes (holding the
    Vita's volume buttons, dragging the slider) kept `update_volume` set,
    and that blocks context resolving: tapping another song did nothing until
    the volume stopped changing.
  * playback stops after `UNAVAILABLE_STOP_AFTER` (3) tracks in a row fail to
    load (`unavailable_in_a_row`, reset when a track plays; failed preloads
    don't count). Upstream skipped to the next track on every failure, and
    when Spotify refuses every track (audio key errors, librespot#1649)
    autoplay kept refilling the queue, so it skipped forever.
* `src/state/context.rs` - `valid_resolve_uri` also rejects
  `spotify:web-api`, the placeholder uri of a context built from plain tracks
  (`LoadRequest::from_tracks`, used for a song played from search). Upstream
  asked Spotify for autoplay of that uri, got 400 Bad Request, and nothing
  followed the song, so skip stopped playback. Like a search context, it now
  falls back to the current track's uri, which gives a radio of that song.

Everything else (transfer, dealer handling, shuffle/repeat, the rest of
context resolving) is untouched upstream code.

## librespot-discovery

Without these two fixes the Vita never shows up in the Spotify device picker.

* `src/server.rs` - upstream binds the Zeroconf HTTP server to `[::]` (dual
  stack) on every non-Windows target. SceNet on the PS Vita is IPv4 only, so
  the bind failed, `Discovery::launch()` returned an error and nothing was
  advertised. The Vita now binds `0.0.0.0`, as Windows already did.
* `src/lib.rs` - libmdns takes the SRV target host name from `gethostname()`.
  VitaSDK's newlib returns the DHCP host name, which is normally empty, and
  then falls back to `"localhost"`. That gives `localhost.local`, which
  resolvers treat as loopback. On the Vita the responder now gets its own
  host name built from the device name and the discovery port, e.g.
  `ps-vita-51234.local`.

## librespot-oauth

* `Cargo.toml`, `src/lib.rs` - the `open` (browser launcher) dependency is
  only enabled for non-Vita targets and the `open::that_in_background` call is
  compiled out on the Vita. `open` has no Vita backend and OAuth login never
  runs on the console.

## rustix

* `src/backend/libc/fs/dir.rs` - the `libc` crate does not declare `dirfd()` for
  `armv7-sony-vita-newlibeabihf`, although newlib provides it. The patch
  declares the extern function for the Vita target. rustix is pulled in by
  `tempfile`, which librespot-audio uses for seek buffering.

## if-addrs

`if-addrs` only knows Linux/BSD/Apple/Windows and uses netlink/`getifaddrs`,
neither of which exists in the Vita's newlib. `patch-vita.py` (kept for
reference; the generated sources are committed) adds a `src/vita.rs` that
returns the console's IPv4 address and netmask from `SceNetCtl`, and excludes
the unsupported modules. libmdns needs this list for Zeroconf.

## tempfile

librespot-audio streams every track through a `NamedTempFile` in
`<data_dir>/tmp`. tempfile 3.27 makes the directory absolute with
`Path::is_absolute()` and otherwise prepends the current directory. A Vita path
such as `ux0:data/VitaSpot/tmp` has no leading `/`, so it became
`app0:/ux0:data/VitaSpot/tmp/.tmpXXXXXX`. The Vita rejects that with EINVAL, and
every track failed with "Unable to load encrypted file" and was skipped as
unavailable.

* `src/util.rs` - new `is_absolute()` that, on the Vita, also treats a first
  path component containing `:` (a device such as `ux0:`) as absolute.
* `src/util.rs`, `src/file/mod.rs`, `src/file/imp/unix.rs` - all four
  `is_absolute()` checks use it.

## ring

* `build.rs` - ring selects its assembly implementation from a hard-coded list
  of target OS names. `vita` was unknown, so ring compiled the Rust/NEON code
  paths but not the matching ARM assembly, producing undefined symbols at the
  final link. The Vita is added to the ARM entry that already covers the
  Nintendo Switch (`horizon`), i.e. the pregenerated ARM linux32 assembly is
  used unchanged.

