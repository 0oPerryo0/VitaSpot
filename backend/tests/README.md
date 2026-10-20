# Backend contract test

`contract_test.c` drives `include/vam_backend.h` headlessly: init, wait for
login, request every virtual page, page through a long playlist, play, seek,
pause/resume, shuffle/repeat, queue, like/unlike, image download, lyrics
(reply shape, line offsets, synced start times in order, NOT_FOUND for an
episode) and cancellation.

It is written so it can run against the mock backend **and** the real
librespot backend:

* against the mock, all checks run;
* against the real backend without Spotify credentials the content and
  playback checks are reported as `SKIP` (the backend stays logged out), while
  auth, request-id, cancellation and string/event invariants are still
  verified.

## Mock backend

From the repository root, with MSYS2 UCRT64 (SDL2 required):

```bash
cmake -B build-contract-mock -S backend/tests -DVAM_TEST_BACKEND=mock
cmake --build build-contract-mock
VAM_MOCK_INSTANT=1 ./build-contract-mock/contract_test.exe ./vam-contract-data/
```

(Drop `VAM_MOCK_INSTANT=1` to also exercise the login transition, or set
`VAM_MOCK_FREE=1` to check the Premium-required path.)

## Real backend

Build the backend first (`backend/librespot-vam/scripts/build-host.sh`). On
Windows/MinGW the link line is:

```bash
cmake -B build-contract-real -S backend/tests \
  -DVAM_TEST_BACKEND=librespot \
  -DVAM_BACKEND_LIB=$PWD/backend/librespot-vam/target/release/libvam_backend.a \
  -DVAM_BACKEND_LINK_LIBS="-lws2_32 -lbcrypt -lntdll -luserenv -lole32 -loleaut32 -luuid -lwinmm -luser32 -lkernel32 -ladvapi32 -lgdi32 -ldbghelp -lsynchronization -lpropsys -liphlpapi -lpthread -lm"
cmake --build build-contract-real
./build-contract-real/contract_test.exe ./vam-contract-data/
```

On Linux use `-DVAM_BACKEND_LINK_LIBS="-lpthread -ldl -lm"`.

With a `credentials.json` in `./vam-contract-data/` the full playback test
runs (and needs a Premium account). Without it the test prints the login
screen state, checks the auth/event/request-id/cancellation invariants and
skips content.
