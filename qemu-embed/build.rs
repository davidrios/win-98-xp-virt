//! Links libqemu-embed-i386 from the QEMU build dir (override with
//! QEMU_EMBED_LIB_DIR) and bakes an rpath so `cargo run` finds it.
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let default = manifest.join("../build/qemu");
    let dir = std::env::var("QEMU_EMBED_LIB_DIR")
        .map(PathBuf::from)
        .unwrap_or(default);
    let dir = dir.canonicalize().unwrap_or(dir);
    println!("cargo:rerun-if-env-changed=QEMU_EMBED_LIB_DIR");
    warn_if_overlay_stale(&manifest);
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=dylib=qemu-embed-i386");
    // Exported to dependents as DEP_QEMU_EMBED_I386_LIBDIR (via `links`), so
    // binaries can bake an rpath — link-args here would not propagate.
    println!("cargo:libdir={}", dir.display());
}

/// `qemu/embed/` is an rsync copy of `embed/` made by `scripts/prepare-qemu.sh`;
/// after a `git pull` that touches the embed API the copy (and the library
/// built from it) lags behind the bindings, which surfaces as an undefined
/// `qemu_embed_*` symbol at link time. Say so up front.
fn warn_if_overlay_stale(manifest: &std::path::Path) {
    let root = manifest.join("..");
    for f in ["libqemu_embed.h", "libqemu_embed.c", "embedaudio.c"] {
        let src = root.join("embed").join(f);
        let copy = root.join("qemu/embed").join(f);
        println!("cargo:rerun-if-changed={}", src.display());
        println!("cargo:rerun-if-changed={}", copy.display());
        let (Ok(a), Ok(b)) = (std::fs::read(&src), std::fs::read(&copy)) else {
            continue;
        };
        if a != b {
            println!(
                "cargo:warning=qemu/embed/{f} is stale vs embed/{f}: run \
                 scripts/prepare-qemu.sh && scripts/configure-qemu.sh, then rebuild \
                 libqemu-embed-<target> (ninja) before linking the player"
            );
            return;
        }
    }
}
