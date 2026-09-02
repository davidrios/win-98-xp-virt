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
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=dylib=qemu-embed-i386");
    // Exported to dependents as DEP_QEMU_EMBED_I386_LIBDIR (via `links`), so
    // binaries can bake an rpath — link-args here would not propagate.
    println!("cargo:libdir={}", dir.display());
}
