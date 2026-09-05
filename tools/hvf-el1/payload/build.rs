fn main() {
    let dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    println!("cargo:rustc-link-arg=-T{dir}/link.ld");
    println!("cargo:rustc-link-arg=--no-pie");
    println!("cargo:rerun-if-changed=link.ld");
    println!("cargo:rerun-if-changed=src/start.S");
    println!("cargo:rerun-if-changed=../bench.S");
    println!("cargo:rerun-if-changed=../benchlib.rs");
}
