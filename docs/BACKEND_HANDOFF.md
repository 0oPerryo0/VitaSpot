# Backend handoff prompt

Copy everything below the line into the backend agent's session, started in this repository's root.

---

You are building the **backend** for VitaSpot, a homebrew Spotify client for the PS Vita. The SDL2 frontend is already done. Your job is a static library, built on **librespot** (Rust), that implements the C contract in `include/vam_backend.h` exactly. It must stream and play Spotify audio on the Vita itself and serve the browsing data the UI needs. It is for **Spotify Premium accounts only**.

This is a personal homebrew project.
## Read first

1. `include/vam_backend.h`: the contract. Every comment in it is a requirement.
2. `backend/mock/mock_backend.c`: a working reference implementation with fake data. Match its behaviour (event order, paging, virtual URIs, saved-state events). When the header and the mock disagree, the header wins; say so in your report.
3. `src/app.c`: how the UI consumes events (`handle_vam_event`), so you can see what it relies on.
4. `CMakeLists.txt`: how the frontend links an external backend (`VAM_BACKEND=external`, `VAM_BACKEND_LIB`, `VAM_BACKEND_LINK_LIBS`).

Do not change the frontend or the header. If the contract needs a change, stop and propose it with a reason instead of editing it.

## What to build

A Rust crate at `backend/librespot-vam/`:

- `crate-type = ["staticlib"]`, producing `libvam_backend.a`.
- Every function in the header exported as `#[no_mangle] pub extern "C"`, with `#[repr(C)]` mirrors of every struct. Enums are `int32_t` fields, not Rust enums, so unknown values can't cause UB.
- Two build targets:
  - **Host** (Windows via MinGW, or Linux) so it can be tested against the real UI on a PC first.
  - **PS Vita**: `armv7-sony-vita-newlibeabihf`. This is a tier-3 Rust target: nightly plus `build-std`. Use the vita-rust tooling (`cargo-vita`, `vitasdk-sys`) on top of an installed VitaSDK.
- `backend/librespot-vam/README.md` covering: toolchain setup, build commands for both targets, and the exact `cmake` command to build the VPK with your library, including `VAM_BACKEND_LINK_LIBS`.

### Suggested internal design

- One backend-owned async runtime (tokio) on its own threads. Spawn threads with explicit stack sizes (≥256 KB); Vita defaults are small.
- `vam_poll_event` pops a `Mutex<VecDeque<vam_event>>`. The `vam_get_*_state` functions copy a `Mutex`-guarded snapshot. **No exported function may block** on network or locks held across awaits.
- Allocate `vam_page` and its sections and items, plus image byte buffers, in Rust. Free them in `vam_event_release`, using `Box::into_raw` / `Box::from_raw` or `Vec` with a stored length. The UI never frees backend memory.
- Copy strings into the fixed `char[N]` buffers, truncated on a UTF-8 code point boundary and NUL-terminated.

## librespot pieces

Use a current librespot release (0.6 or newer, or `dev` if needed for login fixes). Pin the version in `Cargo.toml`.

- **Session / auth** (`librespot-core`): log in with stored reusable credentials when present (`<data_dir>/credentials.json`). Otherwise go to `VAM_AUTH_LOGGED_OUT`.
- **Zeroconf** (`librespot-discovery`): advertise `config.device_name` as a Spotify Connect device, so the user picks "PS Vita" in the phone app and the credentials blob is sent over. Set `zeroconf_active = 1` while advertising.
  - mDNS multicast on the Vita network stack is **unverified**. Test it early.
  - If it doesn't work, implement this fallback and report it: read a `credentials.json` the user created on a PC (with stock librespot or a small helper you add under `tools/`) and copied to `ux0:data/VitaSpot/`. Show instructions through `vam_auth_state.message`, and `login_url` / `login_code` if you add a web flow.
- **Premium check**: after login, read the account's product type. Not Premium means `is_premium = 0`, `status = LOGGED_IN`, and `vam_play` returns `VAM_ERR_NOT_PREMIUM`. The UI then shows its Premium-required screen.
- **Spotify Connect** (`librespot-connect`, Spirc): make the Vita a real Connect device, so the phone sees and controls playback. Implement `vam_play` / `pause` / `next` / etc. through Spirc so remote and local state stay consistent.
  - When another device takes over: `is_active_device = 0`, keep reporting state, and keep accepting commands.
- **Player** (`librespot-playback`):
  - Bitrate from `config.bitrate`: 96, 160 or 320 kbps Vorbis.
  - Loudness normalisation from `config.normalize_volume`.
  - Software volume driven by `vam_set_volume`. The UI calls it every frame while the slider is dragged, so coalesce updates.
  - Audio cache in `<data_dir>/cache/audio/`, size-capped at about 200 MB.
- **Vita audio sink**: write a custom librespot `Sink`.
  - `sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, 44100, SCE_AUDIO_OUT_MODE_STEREO)`, s16 output, with a dedicated output thread and a ring buffer large enough to survive UI hitches.
  - Release the port on stop.
  - On host builds use librespot's normal backend (rodio/cpal).
- **Metadata**. Fill pages from librespot's metadata, or from Spotify Web API calls made with an access token from the session (the approach ncspot uses), for whatever librespot doesn't cover: home shelves, search, library, follow and save.
  - Request only the scopes you need: `user-library-read user-library-modify user-follow-read user-follow-modify playlist-read-private user-read-recently-played user-top-read`.
  - Handle 429 rate limits (`Retry-After`) and report `VAM_ERR_RATE_LIMITED`.
- **Images**: `vam_request_image` downloads `i.scdn.co` URLs.
  - Choose `image_url` values near 300 px; the UI decodes JPEG and PNG.
  - Keep an LRU disk cache in `<data_dir>/cache/img/` (about 30 MB) and serve hits without network.

TLS on the Vita is the biggest build risk. `rustls` + `ring` may or may not cross-compile cleanly for `armv7-sony-vita-newlibeabihf`. Try it first. If it fails, use VitaSDK's `libcurl` + `mbedtls` through FFI for HTTP, and report what librespot's own connections needed.

## Contract details the UI depends on

- **Request IDs**: non-zero. The reply event carries the same `request_id`. `vam_cancel` drops the reply and frees its memory.
- **Paging**: the first page uses `offset 0, limit 50`. Later pages request `offset = items so far` and return only section 0, with `vam_section.offset == offset` and a correct `total`. Mismatched offsets are ignored by the UI.
- **Virtual URIs** (exact section layouts are in the header):
  - `vam:home`: a GRID of up to 6 items first, then SHELVES.
  - `vam:library`: one LIST. Liked Songs comes first as `VAM_KIND_COLLECTION`, with its URI usable as a play context and a page URI.
  - `vam:search:<raw utf-8 query>`: Songs (LIST), Artists, Albums, Playlists (SHELF).
  - `vam:queue`: "Now playing" (1 item), then "Next up". The UI re-requests it whenever `player.seq` changes.
- **Item fields**:
  - Tracks need `album_uri`, `artist_uri`, `duration_ms`, `is_explicit`, `is_playable` and `is_saved`. Grey-outs and the context menu depend on them.
  - `subtitle` formats: tracks `"Artist A, Artist B"`; albums `"Album • Artist"`; playlists `"Playlist • Owner"`. Pages: albums `"Artist • 2021 • 12 songs"`, playlists `"Owner • N songs"`.
- **Player state**:
  - Emit `VAM_EV_PLAYER_CHANGED` and bump `seq` on every change: track, play/pause, buffering, seek, shuffle, repeat, volume, queue, active device.
  - Update `position_ms` + `position_timestamp_ms` (from `vam_now_ms()`) only on discontinuities. The UI extrapolates position while `PLAYING`.
  - `context_title` is shown as "PLAYING FROM ...".
- **Saving**: `vam_set_saved` replies with `VAM_EV_REQUEST_DONE` (with `uri` set). On success it also emits `VAM_EV_SAVED_CHANGED`. The UI updates optimistically and reverts on failure.
- **Errors**: playback failures, lost connection and similar go out as `VAM_EV_ERROR` with a short human sentence. Use `VAM_EV_TOAST` for informational notes.
- **Auth transitions**: emit `VAM_EV_AUTH_CHANGED` on every status change. Use `message` for human-readable progress or errors, such as "No Wi-Fi connection".
- **Logout**: `vam_logout` deletes stored credentials, stops playback and returns to `LOGGED_OUT` with Zeroconf advertising again.

## Vita platform requirements

- Networking: the backend owns it.
  - `sceSysmoduleLoadModule(SCE_SYSMODULE_NET)`, `sceNetInit` (with about a 1 MB pool), `sceNetCtlInit`, plus SSL/HTTP modules only if you use them.
  - Check Wi-Fi state, and report "Wi-Fi is off" rather than hanging.
- Suspend/resume: sockets die when the Vita sleeps. Detect the dead session after resume and reconnect with the stored credentials, without UI involvement. Playback may resume paused.
- Memory: keep the backend under about 64 MB resident. The UI uses about 60 MB for textures. Don't load whole playlists eagerly.
- CPU: the ARM runs at 444 MHz (set by the frontend). Keep decoding and TLS off the UI thread; the frontend renders at 60 fps on one core.
- The frontend already handles auto-suspend while audio plays (`sceKernelPowerTick`). Don't duplicate it.
- Paths: `config.data_dir` ends with a slash (`ux0:data/VitaSpot/` on Vita).

## Testing and acceptance

1. **Host first**: build the crate for your PC and link it into the UI:
   `cmake -B build -DVAM_BACKEND=external -DVAM_BACKEND_LIB=<path>/libvam_backend.a -DVAM_BACKEND_LINK_LIBS="..."`.
   Go through Zeroconf login, browse every page type, play/pause/seek/skip, shuffle/repeat, like/unlike, add to queue, and control from the phone.
2. **Contract test**: add `backend/tests/contract_test.c`, a headless C program that drives the API: init, wait for login, request each virtual URI, page a long playlist, play, seek. It asserts event and field invariants (non-zero IDs, NUL-terminated strings, offsets, `seq` increments). Run it against the mock and your backend.
3. **Vita**: build the VPK and run it on hardware. Report:
   - whether Zeroconf worked or the fallback was needed,
   - time from launch to first audio,
   - stalls over 30 minutes of playback,
   - memory use,
   - behaviour after suspend/resume.

**Done means:**
- a Premium account plays music out of the Vita speakers and headphones;
- the phone app sees and controls the Vita;
- every UI screen fills with real data;
- a free account gets the Premium-required screen;
- the build is reproducible from the README.

Report anything you could not verify on real hardware as unverified. Don't claim it works.
