//! Iris Native Shell application.

mod iris;
mod screens;

use nsh_core::prelude::*;

pub use iris::{Account, Call, IrisClient, IrisEvent, MediaStats, Snapshot, UiState};

#[native_module("iris/native")]
fn native(registry: &mut Registry) {
    registry.register::<App>();
}

#[cfg(windows)]
#[native_components(windows)]
fn components(components: &nsh_platform_windows::Components) {
    iris::video::register(components);
}

/// The application's root component.
#[component]
pub fn App() -> impl IntoElement {
    screens::app()
}
