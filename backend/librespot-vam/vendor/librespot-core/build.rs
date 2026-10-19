// VitaSpot patch: replaces librespot-core's vergen based build script.
//
// The upstream script depends on `vergen-gitcl`, whose crates.io metadata can
// pick incompatible `vergen` / `vergen-lib` versions (observed with
// vergen-gitcl 1.0.8 in late 2026), and git metadata is meaningless for a
// vendored crate. The only contract is a handful of `cargo:rustc-env` values
// consumed by `src/version.rs`, so set them deterministically instead.

fn main() {
    println!("cargo:rustc-env=VERGEN_GIT_SHA=vitaspot");
    println!("cargo:rustc-env=VERGEN_GIT_COMMIT_DATE=2026-01-01T00:00:00Z");
    println!("cargo:rustc-env=VERGEN_BUILD_DATE=2026-01-01T00:00:00Z");
    println!("cargo:rustc-env=LIBRESPOT_BUILD_ID=vitaspot");
}
