# Backend implementation report

This is the engineering report for the librespot backend. The user-facing
build/login instructions live in [`README.md`](README.md).

## What was delivered

* `backend/librespot-vam/` - the Rust crate (`crate-type = ["staticlib"]`,
  `libvam_backend.a`) implementing every function of `include/vam_backend.h`
  with `#[repr(C)]` mirrors and `int32_t` enum fields. Nothing in the header or
  in `src/` was modified.
* `backend/librespot-vam/vendor/` - six patched crates, with rationale in
  `vendor/README.md`.
* `backend/librespot-vam/vita-shim/` - POSIX shim for the Vita final link.
* `backend/librespot-vam/scripts/` - host, Vita library and VPK builds; the VPK
  script also builds the shim and locates ring's assembly archive.
* `backend/librespot-vam/tools/vam-login.rs` - PC OAuth helper that writes a
  librespot `credentials.json` for the Zeroconf fallback.
* `backend/tests/contract_test.c` + `backend/tests/CMakeLists.txt` - headless
  contract test, runnable against the mock and the real backend.

## Verification performed (host)

All commands were run in this checkout. The desktop toolchain is w64devkit +
Rust `x86_64-pc-windows-gnu` (a `libgcc_eh.a` shim was needed because w64devkit
omits it; MSYS2 GCC does not need this).

| Check | Result |
|---|---|
| `cargo check --lib` / `cargo build --release --lib` | pass, no warnings from `src/` |
| `scripts/build-host.sh` from a clean `target/` dir | pass (library at the path the README documents) |
| `cargo build --release --bin vam-login` | pass |
| Link the real UI against the backend (`VAM_BACKEND=external`) | pass (`VitaSpot.exe`) |
| Start the UI with the backend | pass, login screen shows the backend's Zeroconf message |
| Clean `vam_shutdown()` | pass (all engine tasks exit, thread joins) |
| Zeroconf advertisement observed from a second process | pass (mDNS browse found `PS Vita`, port and IP properties) |
| Contract test vs mock (`VAM_MOCK_INSTANT=1`) | **499 assertions, 0 failures, 0 skipped** |
| Contract test vs this backend (no credentials) | **27 assertions, 0 failures**, content checks skipped |

Observed during the Zeroconf test:

```
FOUND PS Vita._spotify-connect._tcp.local.
  server: <hostname>.local. port: <random>
  addresses: [...]          # LAN IP + others
  properties: {'VERSION': b'1.0', 'CPath': b'/'}
```

Contract test coverage: init and config, increasing `vam_now_ms`, auth event,
all four virtual URIs, section/layout/total/offset invariants, NUL-terminated
every string, non-zero request ids matching replies, image reply + data,
request cancellation (no reply may arrive), first-section paging (`offset=5`
after `count=5`, only section 0), playback start, seek, pause (position frozen),
resume, shuffle/repeat flags, add-to-queue, next/prev, saved-state
`REQUEST_DONE` + `SAVED_CHANGED`, and the free-account `VAM_ERR_NOT_PREMIUM`
path (mock run only).

Approximate build times on this machine: first host build ~11 min,
incremental ~30 s; clean Vita library build ~4-5 min; VPK link ~1 min.

## Verification performed (PS Vita)

Cross-compilation succeeded end to end:

* target `armv7-sony-vita-newlibeabihf`, nightly + `-Z build-std=std,panic_abort`;
* full librespot dependency tree builds for the Vita: tokio (mio, sockets),
  rustls 0.23 + ring 0.17.14, hyper, symphonia decoders, libmdns,
  librespot-core/playback/connect/discovery/metadata;
* `libvam_backend.a` produced (~89 MB, unstripped);
* VPK linked and packaged: `build-vita-librespot/VitaSpot.vpk` = **24,925,319
  bytes**; `eboot.bin` = 11.33 MB; `arm-vita-eabi-nm VitaSpot` shows
  `vam_init`, `vam_shutdown`, `vam_play`, `vam_request_page`, `vam_poll_event`,
  `vam_get_player_state`, `vam_now_ms` defined;
* both `scripts/build-vita.sh` and `scripts/build-vpk.sh` succeed from a clean
  `target/` directory (reproducible from the README).

### TLS cross-compile result (the anticipated biggest risk)

`rustls` + `ring` **did** cross-compile, so the libcurl/mbedtls fallback was
not needed. Two Vita-specific problems had to be solved, both documented in
`vendor/README.md`:

1. `ring` selects its ARM assembly from a hard-coded OS list and did not know
   `vita`; the Rust side referenced NEON/asm symbols that were never built.
   The Vita is now mapped to the existing `horizon` (Nintendo Switch) ARM
   linux32 assembly. This is the only ring change.
2. Rust std/`rustix` reference POSIX functions absent from VitaSDK newlib
   (`getppid`, `readlinkat`, `readv`, `writev`, `dup2`, `ioctl`, `readlink`,
   `fchown`, `lchown`, `mkfifo`, `*at()` family). `vita-shim/vita_shim.c`
   provides link-only stubs/forwards. None of these run in VitaSpot's code paths.

librespot's own connections (AP TCP, HTTP client, websocket dealer) all use
the same rustls stack, so they work with the compiled library. VitaSDK's
`SceSsl`/`SceHttp` modules are not used; the backend loads only `SceNet` and
`SceNetCtl` itself (the frontend already links the other Sce stubs).

## Contract details implemented

* Request ids from an atomic counter, never 0; replies carry the same id.
  `vam_cancel` drops queued replies and frees them; workers check the
  cancellation set before queueing and free their result instead.
* Paging: page 0 uses `offset 0, limit 50`; later pages return **only section
  0** with `offset == requested offset` and a correct `total` (album, playlist,
  Liked Songs and search-songs all page through the Web API; the library list
  is sliced server-side after assembling the bounded list).
* Virtual URIs:
  * `vam:home` - GRID (max 6) + "Recently played", "Your playlists",
    "Saved albums", "Your top artists" SHELVES.
  * `vam:library` - one LIST; Liked Songs `VAM_KIND_COLLECTION` first with the
    `spotify:user:<name>:collection` URI, then followed playlists, saved
    albums, followed artists.
  * `vam:search:<utf-8>` - "Songs" LIST, "Artists"/"Albums"/"Playlists"
    SHELVES; paging hits only the Songs list.
  * `vam:queue` - "Now playing" (1 item) + "Next up" from the live Connect
    queue (queued tracks first, then the rest of the context).
* Items: tracks carry `album_uri`, `artist_uri`, `duration_ms`,
  `is_explicit`, `is_playable`, `is_saved`; subtitles use the header formats
  ("Artist A, Artist B", "Album • Artist", "Playlist • Owner",
  "Artist • 2021 • 12 songs", "Owner • 50 songs").
* Player state: `seq` is bumped and `VAM_EV_PLAYER_CHANGED` emitted for track,
  play/pause/buffering, seek, shuffle, repeat, volume, queue and active-device
  changes. `position_ms`/`position_timestamp_ms` are refreshed only on
  discontinuities. `context_title` comes from the Connect context metadata or a
  Web API lookup ("PLAYING FROM ...").
* Saving: Web API `PUT/DELETE /me/{tracks,albums,following}`; reply
  `VAM_EV_REQUEST_DONE` (with `uri`) and, on success, `VAM_EV_SAVED_CHANGED`.
  The player's current track heart is updated too.
* Errors go out as `VAM_EV_ERROR` with short sentences; 429 becomes
  `VAM_ERR_RATE_LIMITED` with `Retry-After` seconds in the event message.
* Auth: `VAM_EV_AUTH_CHANGED` on every transition; messages for Wi-Fi off,
  sign-in progress, Zeroconf instructions and the Premium requirement.
* Logout deletes `credentials.json`, stops playback, resets player state and
  returns to `LOGGED_OUT` with Zeroconf advertising again.
* Audio: bitrate 96/160/320, normalisation from config (dynamic limiter, no
  dithering to save CPU), volume coalesced at 50 ms, audio cache 200 MB in
  `cache/audio/`, image cache 30 MB in `cache/img/`.
* Vita sink: `sceAudioOutOpenPort(BGM, 1024, 44100, stereo)`, 1 s ring buffer,
  dedicated output thread, port released on stop; host uses rodio/cpal.
* Session: stable device id `sha1(device_name)` (like stock librespot), data in
  `<data_dir>`, suspend/resume reconnect with stored credentials, playback
  resumes paused.

## Header vs mock

Only one actual disagreement was found: the mock serves **BMP** bytes for
`VAM_EV_IMAGE`, while the header promises encoded **JPEG/PNG**. The header
wins - the real backend returns whatever `i.scdn.co` serves (JPEG/PNG) and
never generates images. No contract-affecting difference otherwise; event
order, paging, virtual URIs, saved-state events and the queue layout all match
the mock.

Where the header is silent, decisions were made to match the mock where
possible: collection item subtitle `"Playlist • N songs"`, artist subtitles
`"Artist"` in lists, `is_saved = 1` for the Liked Songs collection item,
album/playlist detail pages repeat their header fields on later pages (allowed
by the header), and an unknown/empty `vam_play` target returns
`VAM_ERR_INVALID_ARG`.

## Unverified on hardware (do not assume these work)

* **Any authenticated behaviour**: no Premium account/credentials were
  available. Login handshake, Web API browsing, streaming/decoding, seek,
  Connect control from the phone, saving and the free-account Premium screen
  are implemented and compile/link, but were not exercised against Spotify.
* **Vita Zeroconf**: implemented with libmdns plus a `SceNetCtl`-based
  interface list. It works on the host; multicast on the Vita stack is
  untested. The credentials-file fallback exists for that case.
* **Vita audio**: `sceAudioOut` BGM port, 1024-frame grains, 1 s ring buffer;
  latency, underrun behaviour and headphone/speaker routing were not tested.
* **Suspend/resume**: logic exists (session invalid or >30 s tick gap triggers
  a reconnect); actual Vita suspend behaviour was not observed.
* **Memory/CPU on hardware**: the VPK/eboot size is measured, runtime RSS and
  frame-time impact are not. The backend is expected to fit the ~64 MB budget
  (no eager playlist loading, 200 MB/30 MB disk caps, decoded audio is streamed)
  but this is an estimate.
* **Wi-Fi off detection**: uses `sceNetCtlInetGetState(...) == 3`
  (`SCE_NETCTL_STATE_CONNECTED` from the VitaSDK header); untested on hardware.
* Podcasts (`spotify:show:`) are not implemented (header permits this kind but
  the frontend has no podcast UI).
* `vam_login_url` / `vam_login_code` are empty: there is no OAuth web flow on
  the console. The `vam-login` PC helper is the fallback.

## How to finish verification on hardware

1. `VITASDK=... ./scripts/build-vpk.sh`, install the VPK, connect the Vita to
   Wi-Fi.
2. Launch with `ux0:data/VitaSpot/vam.log` visible (or over ftp) and confirm the
   auth message; try the phone device picker, else copy a `credentials.json`.
3. With a Premium account: measure launch-to-first-audio, play 30 minutes and
   watch the log for stalls/session invalidation, check `vam_get_player_state`
   position drift, then exercise phone-to-Vita control and suspend/resume.
4. Free account: confirm `is_premium = 0` and the Premium-required screen, and
   that `vam_play` returns `VAM_ERR_NOT_PREMIUM`.
