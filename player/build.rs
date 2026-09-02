fn main() {
    // rpath to libqemu-embed so `cargo run` / target/*/player find it in place
    if let Ok(dir) = std::env::var("DEP_QEMU_EMBED_I386_LIBDIR") {
        let target = std::env::var("TARGET").unwrap_or_default();
        if target.contains("apple") || target.contains("linux") {
            println!("cargo:rustc-link-arg-bins=-Wl,-rpath,{dir}");
        }
    }
}
