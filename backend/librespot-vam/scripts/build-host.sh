#!/usr/bin/env bash
# Build the VitaSpot backend static library for the host (desktop testing).
#
# Requirements: Rust (stable, x86_64-pc-windows-gnu on Windows) and a
# MinGW-w64 GCC available on PATH (MSYS2 UCRT64, or w64devkit + a libgcc_eh.a
# shim as described in README.md).
set -euo pipefail
cd "$(dirname "$0")/.."

profile="${PROFILE:-release}"
cargo build --lib --"$profile"

lib="target/$profile/libvam_backend.a"
if [[ ! -f "$lib" ]]; then
    echo "expected static library was not produced: $lib" >&2
    exit 1
fi
echo
echo "backend static library: $(pwd)/$lib"
echo "link the desktop UI with:"
echo "  cmake -B build -DVAM_BACKEND=external \\"
echo "        -DVAM_BACKEND_LIB=$(pwd)/$lib \\"
echo '        -DVAM_BACKEND_LINK_LIBS="<see README.md>"'
