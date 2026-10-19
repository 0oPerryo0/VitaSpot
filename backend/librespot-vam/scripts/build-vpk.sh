#!/usr/bin/env bash
# Build the VitaSpot VPK with the librespot backend linked in.
#
# Usage: VITASDK=/path/to/vitasdk ./scripts/build-vpk.sh [build-directory]
#
# This also compiles the small POSIX compatibility shim (vita-shim/) that the
# Rust standard library needs on the Vita, and locates ring's prebuilt
# assembly archive inside the cargo target directory.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="${1:-$repo/build-vita}"

: "${VITASDK:?set VITASDK to your VitaSDK installation}"
export PATH="$VITASDK/bin:$PATH"

lib="$here/target/armv7-sony-vita-newlibeabihf/release/libvam_backend.a"
if [[ ! -f "$lib" ]]; then
    echo "missing $lib; run scripts/build-vita.sh first" >&2
    exit 1
fi

# cmake needs native paths on Windows (C:/...), while this script may run in
# an MSYS shell with /c/... paths.
winpath() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

# The Rust standard library and rustix reference a few POSIX functions that
# newlib lacks; provide them as a small static library.
shim_src="$here/vita-shim/vita_shim.c"
shim_dir="$here/vita-shim"
(
    cd "$shim_dir"
    arm-vita-eabi-gcc -O2 -c vita_shim.c -o vita_shim.o
    arm-vita-eabi-ar rcs libvam_vita_shim.a vita_shim.o
)
shim="$(winpath "$shim_dir/libvam_vita_shim.a")"

# ring compiles its assembly into a native archive that is not part of the
# Rust staticlib; the final link has to pick it up explicitly.
ring_archive="$(find "$here/target/armv7-sony-vita-newlibeabihf" -name 'libring_core_0_17_14_.a' | head -1)"
if [[ -z "$ring_archive" ]]; then
    echo "could not find ring's assembly archive; run scripts/build-vita.sh first" >&2
    exit 1
fi
ring_dir="$(winpath "$(dirname "$ring_archive")")"
lib="$(winpath "$lib")"

# --wrap routes pthread_cond_timedwait through the shim, which fixes Rust's
# monotonic condvar deadlines for VitaSDK's pthread (see vita_shim.c).
vita_libs="-Wl,--wrap=pthread_cond_timedwait -L$ring_dir -lring_core_0_17_14_ $shim -lSceNet_stub -lSceNetCtl_stub -lpthread"
link_libs="${VAM_BACKEND_LINK_LIBS:-$vita_libs}"

cmake -B "$build" -S "$repo" \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DVAM_BACKEND=external \
    -DVAM_BACKEND_LIB="$lib" \
    -DVAM_BACKEND_LINK_LIBS="$link_libs"
cmake --build "$build"

echo
echo "VPK: $build/VitaSpot.vpk"
