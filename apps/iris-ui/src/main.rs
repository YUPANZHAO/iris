//! Runs the Rust application directly on the machine's native backend.

#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

#[cfg(target_os = "linux")]
use nsh_platform_linux as platform;
#[cfg(target_os = "macos")]
use nsh_platform_macos as platform;
#[cfg(target_os = "windows")]
use nsh_platform_windows as platform;

#[cfg(any(target_os = "linux", target_os = "macos", target_os = "windows"))]
fn main() -> impl std::process::Termination {
    env_logger::init();
    #[cfg(target_os = "windows")]
    platform::set_components(nsh_app_iris::__nsh_native_components_windows);

    let manifest: serde_json::Value =
        serde_json::from_str(include_str!("../nsh.json")).expect("invalid Iris NSH manifest");
    let window = nsh_core::desktop::MainWindowOptions::from_json(&manifest["window"])
        .expect("invalid Iris main window configuration");
    let root = platform::Root::of::<nsh_app_iris::App>().main_window(window);
    platform::run_app(
        platform::AppSource::Rust(root),
        platform::AppOptions {
            title: "Iris".to_owned(),
            #[cfg(any(target_os = "linux", target_os = "windows"))]
            app_id: "com.yupanzhao.iris".to_owned(),
            size: (960, 640),
            ..Default::default()
        },
    )
}

#[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
compile_error!(
    "Run this binary on macOS, Windows or Linux; use the TSX entry with nsh run for iOS and Android."
);
