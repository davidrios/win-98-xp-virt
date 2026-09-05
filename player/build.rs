fn main() {
    let target = std::env::var("TARGET").unwrap_or_default();
    let unix = target.contains("apple") || target.contains("linux");
    // An installed player is `<prefix>/bin/win98-xp-virt-player` with the
    // embed library in `<prefix>/lib/win98-xp-virt` (M6 step 6, doc 07's
    // install layout). Origin-relative, so the packaged tree can be
    // extracted anywhere — and *first*, so the same binary copied out of a
    // developer's `target/` is genuinely self-contained once packaged
    // rather than quietly loading the library out of their build
    // directory. In a checkout that directory doesn't exist and the
    // loader simply moves on to the absolute one below.
    if unix {
        let relative = if target.contains("apple") {
            "@loader_path/../lib/win98-xp-virt"
        } else {
            "$ORIGIN/../lib/win98-xp-virt"
        };
        println!("cargo:rustc-link-arg-bins=-Wl,-rpath,{relative}");
    }
    // rpath to libqemu-embed so `cargo run` / target/*/player find it in place
    if let Ok(dir) = std::env::var("DEP_QEMU_EMBED_I386_LIBDIR") {
        if unix {
            println!("cargo:rustc-link-arg-bins=-Wl,-rpath,{dir}");
        }
    }
}
