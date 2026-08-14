use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=CARGO_FEATURE_NANO3S");
    if std::env::var("CARGO_FEATURE_NANO3S").is_err() {
        return;
    }

    // rtos_core lives two levels up from this crate
    // (mujina-upstream/mujina-miner -> mujina-upstream -> nano3s -> rtos_core).
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let rtos_core = manifest_dir
        .parent()
        .expect("mujina-miner has a parent dir")
        .parent()
        .expect("mujina-upstream has a parent dir")
        .join("rtos_core");

    let tools = rtos_core.join("tools");
    let include = rtos_core.join("include");
    let sdk_libs = rtos_core.join("vendor").join("sdk_libs");

    println!("cargo:rerun-if-changed={}", tools.join("nano3s_ipc_shim.c").display());
    println!("cargo:rerun-if-changed={}", tools.join("nano3s_ipc_shim.h").display());
    println!("cargo:rerun-if-changed={}", rtos_core.join("src").join("ipc_protocol.c").display());
    println!("cargo:rerun-if-changed={}", include.join("ipc_protocol.h").display());

    cc::Build::new()
        .file(tools.join("nano3s_ipc_shim.c"))
        .file(rtos_core.join("src").join("ipc_protocol.c"))
        .include(&include)
        .warnings(true)
        .compile("nano3s_ipc_shim");

    println!("cargo:rustc-link-search=native={}", sdk_libs.display());
    println!("cargo:rustc-link-lib=static=ipcmsg");
}
