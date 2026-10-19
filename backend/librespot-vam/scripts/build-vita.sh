#!/usr/bin/env bash
# Build libvam_backend.a for the PS Vita (armv7-sony-vita-newlibeabihf).
#
# Requirements:
#   * VitaSDK installed, VITASDK pointing at it
#   * Rust nightly with the rust-src component (rustup toolchain install
#     nightly --component rust-src). Set VAM_RUST_TOOLCHAIN to use another
#     toolchain, e.g. a pinned nightly-YYYY-MM-DD (CI does this).
set -euo pipefail
cd "$(dirname "$0")/.."

: "${VITASDK:?set VITASDK to your VitaSDK installation}"

export PATH="$VITASDK/bin:$PATH"
export CC_armv7_sony_vita_newlibeabihf="${CC_armv7_sony_vita_newlibeabihf:-$VITASDK/bin/arm-vita-eabi-gcc}"
export AR_armv7_sony_vita_newlibeabihf="${AR_armv7_sony_vita_newlibeabihf:-$VITASDK/bin/arm-vita-eabi-ar}"

cargo +"${VAM_RUST_TOOLCHAIN:-nightly}" build --release --lib \
    --target armv7-sony-vita-newlibeabihf \
    -Z build-std=std,panic_abort

lib="target/armv7-sony-vita-newlibeabihf/release/libvam_backend.a"
if [[ ! -f "$lib" ]]; then
    echo "expected static library was not produced: $lib" >&2
    exit 1
fi
echo
echo "Vita backend static library: $(pwd)/$lib"
