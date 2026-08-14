use std::path::{Path, PathBuf};

fn main() {
    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    // Reuses the LVGL 8.3.5 C source tree + lv_conf.h already vendored for the
    // (now-unused) lvgl-sys crate -- same sources, just compiled directly by
    // `cc` instead of going through bindgen, so no Rust struct ever has to
    // match the C ABI.
    let lvgl_root = manifest_dir.join("vendor").join("lvgl-sys").join("vendor");
    let lvgl_src = lvgl_root.join("lvgl").join("src");
    let lv_config_dir = lvgl_root.join("include");
    let c_driver = manifest_dir.join("c_driver");

    println!("cargo:rerun-if-changed={}", c_driver.join("driver.c").display());
    println!("cargo:rerun-if-changed={}", c_driver.join("driver.h").display());

    let mut build = cc::Build::new();
    add_c_files(&mut build, &lvgl_src);
    build
        .file(c_driver.join("driver.c"))
        .define("LV_CONF_INCLUDE_SIMPLE", Some("1"))
        .include(&lvgl_src)
        .include(&lvgl_root)
        .include(&lv_config_dir)
        .include(&c_driver)
        .warnings(false)
        .compile("nano3s_ui_driver");
}

fn add_c_files(build: &mut cc::Build, dir: &Path) {
    for entry in dir.read_dir().unwrap() {
        let entry = entry.unwrap();
        let path = entry.path();
        if entry.file_type().unwrap().is_dir() {
            add_c_files(build, &path);
        } else if path.extension().and_then(|s| s.to_str()) == Some("c") {
            build.file(&path);
        }
    }
}
