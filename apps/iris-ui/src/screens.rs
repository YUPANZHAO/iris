use std::{
    sync::Arc,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use nsh_core::{desktop::WindowControl, prelude::*};
use serde_json::json;

use crate::iris::{Call, IrisClient, UiState};

const BG: u32 = 0x0b111b;
const PANEL: u32 = 0x121b29;
const PANEL_ALT: u32 = 0x182437;
const BORDER: u32 = 0x26364d;
const TEXT: u32 = 0xe8eef8;
const MUTED: u32 = 0x91a0b5;
const ACCENT: u32 = 0x3b82f6;
const DANGER: u32 = 0xdc3d4b;
const SUCCESS: u32 = 0x2fbf81;

fn button(label: &'static str, background: u32) -> View {
    view()
        .px(px(15.0))
        .py(px(10.0))
        .rounded(px(10.0))
        .bg(rgb(background))
        .active(|style| style.opacity(0.7))
        .child(
            text(label)
                .text_size(px(14.0))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(0xffffff)),
        )
}

fn reactive_button(
    label: impl Fn() -> &'static str + 'static,
    background: u32,
) -> View {
    view()
        .px(px(15.0))
        .py(px(10.0))
        .rounded(px(10.0))
        .bg(rgb(background))
        .active(|style| style.opacity(0.7))
        .child(
            text(label)
                .text_size(px(14.0))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(0xffffff)),
        )
}

fn field_label(value: &'static str) -> impl IntoElement {
    text(value)
        .text_size(px(12.0))
        .font_weight(FontWeight::MEDIUM)
        .text_color(rgb(MUTED))
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis() as u64)
        .unwrap_or_default()
}

fn call_duration(call: &Call) -> String {
    let started = if call.connected_at_ms == 0 {
        call.created_at_ms
    } else {
        call.connected_at_ms
    };
    if started == 0 {
        return "00:00".to_owned();
    }
    let seconds = now_ms().saturating_sub(started) / 1000;
    format!("{:02}:{:02}", seconds / 60, seconds % 60)
}

fn status_text(state: &UiState) -> &'static str {
    if !state.error.is_empty() {
        "Error"
    } else if !state.ready {
        "Starting"
    } else if state.first_account().is_some_and(|account| account.registered) {
        "Registered"
    } else {
        "Ready"
    }
}

fn status_color(state: &UiState) -> u32 {
    if !state.error.is_empty() {
        DANGER
    } else if !state.ready {
        0xd08a24
    } else if state.first_account().is_some_and(|account| account.registered) {
        SUCCESS
    } else {
        ACCENT
    }
}

pub fn app() -> impl IntoElement {
    let client = IrisClient::start();
    let state = signal(client.state());
    {
        let client = client.clone();
        set_interval(Duration::from_millis(100), move || {
            state.set(client.state());
        });
    }

    let account_uri = signal("sip:alice@127.0.0.1:5060".to_owned());
    let account_uri_edited = signal(false);
    let sip_port_synced = signal(false);
    let registrar_uri = signal(String::new());
    let username = signal(String::new());
    let password = signal(String::new());
    let destination = signal(String::new());
    let microphone_muted = signal(false);
    let camera_muted = signal(false);
    let audio_menu_open = signal(false);
    let video_menu_open = signal(false);

    {
        let state = state.clone();
        let account_uri = account_uri.clone();
        set_interval(Duration::from_millis(100), move || {
            let snapshot = state.get().snapshot;
            if !sip_port_synced.get_untracked() &&
                snapshot.sip_port != 0
            {
                if !account_uri_edited.get_untracked() {
                    account_uri.set(format!(
                        "sip:alice@127.0.0.1:{}",
                        snapshot.sip_port
                    ));
                }
                sip_port_synced.set(true);
            }
        });
    }

    let add_account = {
        let client = client.clone();
        let account_uri = account_uri.clone();
        let registrar_uri = registrar_uri.clone();
        let username = username.clone();
        let password = password.clone();
        move || {
            client.add_account(
                "Iris",
                &account_uri.get(),
                &registrar_uri.get(),
                &username.get(),
                &password.get(),
            );
        }
    };
    let register_account = {
        let client = client.clone();
        let state = state.clone();
        move || {
            if let Some(account) = state.get().first_account() {
                client.register_account(&account.id);
            }
        }
    };
    let start_call = {
        let client = client.clone();
        let state = state.clone();
        let destination = destination.clone();
        move || {
            if let Some(account) = state.get().first_account() {
                let target = destination.get();
                if !target.trim().is_empty() {
                    client.start_call(&account.id, target.trim());
                }
            }
        }
    };

    view()
        .flex_1()
        .w_full()
        .h_full()
        .bg(rgb(BG))
        .text_color(rgb(TEXT))
        .child(
            view()
                .h(px(58.0))
                .flex_row()
                .items_center()
                .gap(px(12.0))
                .px(px(22.0))
                .border_b_1()
                .border_b_color(rgb(BORDER))
                .bg(rgb(PANEL))
                .window_drag(true)
                .child(
                    text("Iris")
                        .text_size(px(22.0))
                        .font_weight(FontWeight::BOLD)
                        .text_color(rgb(0xffffff)),
                )
                .child(
                    text("Secure SIP video")
                        .text_size(px(13.0))
                        .text_color(rgb(MUTED)),
                )
                .child(view().flex_1())
                .child({
                    let state = state.clone();
                    view()
                        .px(px(10.0))
                        .py(px(5.0))
                        .rounded(px(20.0))
                        .bg(move || rgb(status_color(&state.get())))
                        .child(
                            text({
                                let state = state.clone();
                                move || status_text(&state.get())
                            })
                            .text_size(px(12.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(0xffffff)),
                        )
                })
                .child(window_control_button(
                    "−",
                    "Minimize",
                    WindowControl::Minimize,
                ))
                .child(window_control_button(
                    "□",
                    "Maximize or restore",
                    WindowControl::Maximize,
                ))
                .child(window_control_button(
                    "×",
                    "Close",
                    WindowControl::Close,
                )),
        )
        .child(
            view()
                .flex_1()
                .flex_row()
                .child(sidebar(
                    client.clone(),
                    state.clone(),
                    account_uri,
                    account_uri_edited,
                    registrar_uri,
                    username,
                    password,
                    destination.clone(),
                    add_account,
                    register_account,
                    start_call,
                    audio_menu_open,
                    video_menu_open,
                ))
                .child(call_area(
                    client,
                    state.clone(),
                    destination,
                    microphone_muted,
                    camera_muted,
                )),
        )
        .child(
            view()
                .px(px(16.0))
                .py(px(8.0))
                .border_t_1()
                .border_t_color(rgb(BORDER))
                .child(
                    text({
                        let state = state.clone();
                        move || {
                            let state = state.get();
                            if !state.error.is_empty() {
                                format!("Iris: {}", state.error)
                            } else if state.pending {
                                "Iris: applying command...".to_owned()
                            } else {
                                format!(
                                    "Iris {} | PJPROJECT {}",
                                    state.snapshot.version,
                                    state.snapshot.pjproject_version
                                )
                            }
                        }
                    })
                    .text_size(px(12.0))
                    .text_color(move || {
                        if state.get().error.is_empty() {
                            rgb(MUTED)
                        } else {
                            rgb(0xff8a94)
                        }
                    }),
                ),
        )
}

fn window_control_button(
    glyph: &'static str,
    label: &'static str,
    control: WindowControl,
) -> impl IntoElement {
    view()
        .w(px(44.0))
        .h(px(58.0))
        .items_center()
        .justify_center()
        .window_drag(false)
        .window_control(control)
        .label(label)
        .child(
            text(glyph)
                .text_size(px(18.0))
                .text_color(rgb(0xd0d8e5)),
        )
}

/// Native text editing with a DirectWrite fallback for NSH 0.5.1.
///
/// Windows' windowless RichEdit path can paint an empty command stream when
/// its target is exactly 96 DPI. Keep RichEdit responsible for editing,
/// caret placement, IME and accessibility, and overlay its current value
/// with NSH's ordinary text renderer on 100% displays.
fn text_input_field(
    value: Signal<String>,
    placeholder: &'static str,
    masked: bool,
    on_change: impl Fn(&str) + 'static,
) -> impl IntoElement {
    let input_value = {
        let value = value.clone();
        move || value.get()
    };
    let display_value = {
        let value = value.clone();
        move || {
            let value = value.get();
            if masked {
                "●".repeat(value.chars().count())
            } else {
                value
            }
        }
    };
    let show_fallback = {
        let value = value.clone();
        move || pixel_ratio() <= 1.01 && !value.get().is_empty()
    };
    let hide_native_text = move || {
        if pixel_ratio() <= 1.01 {
            rgb(PANEL_ALT)
        } else {
            rgb(TEXT)
        }
    };

    view()
        .relative()
        .h(px(42.0))
        .child(
            nsh_core::element::input()
                .absolute()
                .left(px(0.0))
                .right(px(0.0))
                .top(px(0.0))
                .bottom(px(0.0))
                .value(input_value)
                .on_change(on_change)
                .placeholder(placeholder)
                .placeholder_color(rgb(MUTED))
                .text_color(hide_native_text)
                .caret_color(rgb(ACCENT))
                .selection_color(rgb(0x274a78))
                .masked(masked)
                .px(px(12.0))
                .rounded(px(9.0))
                .bg(rgb(PANEL_ALT))
                .border_1()
                .border_color(rgb(BORDER)),
        )
        .child(when(show_fallback, move || {
            view()
                .absolute()
                .left(px(12.0))
                .right(px(12.0))
                .top(px(0.0))
                .bottom(px(0.0))
                .items_start()
                .justify_center()
                .overflow_hidden()
                .pointer_events(PointerEvents::None)
                .child(
                    text(display_value)
                        .text_size(px(14.0))
                        .text_color(rgb(TEXT)),
                )
        }))
}

#[allow(clippy::too_many_arguments)]
fn sidebar(
    client: Arc<IrisClient>,
    state: Signal<UiState>,
    account_uri: Signal<String>,
    account_uri_edited: Signal<bool>,
    registrar_uri: Signal<String>,
    username: Signal<String>,
    password: Signal<String>,
    destination: Signal<String>,
    add_account: impl Fn() + 'static,
    register_account: impl Fn() + 'static,
    start_call: impl Fn() + 'static,
    audio_menu_open: Signal<bool>,
    video_menu_open: Signal<bool>,
) -> impl IntoElement {
    scroll()
        .w(px(360.0))
        .h_full()
        .p(px(18.0))
        .gap(px(16.0))
        .border_r_1()
        .border_r_color(rgb(BORDER))
        .bg(rgb(PANEL))
        .child(
            view()
                .gap(px(10.0))
                .child(
                    text("SIP account")
                        .text_size(px(16.0))
                        .font_weight(FontWeight::BOLD),
                )
                .child(field_label("Account URI"))
                .child(text_input_field(
                    account_uri.clone(),
                    "",
                    false,
                    move |value: &str| {
                        account_uri_edited.set(true);
                        account_uri.set(value.to_owned());
                    },
                ))
                .child(field_label("Registrar (optional)"))
                .child(text_input_field(
                    registrar_uri.clone(),
                    "sip:example.com",
                    false,
                    move |value: &str| registrar_uri.set(value.to_owned()),
                ))
                .child(
                    view()
                        .flex_row()
                        .gap(px(10.0))
                        .child(
                            view()
                                .flex_1()
                                .gap(px(6.0))
                                .child(field_label("Username"))
                                .child(text_input_field(
                                    username.clone(),
                                    "",
                                    false,
                                    move |value: &str| username.set(value.to_owned()),
                                )),
                        )
                        .child(
                            view()
                                .flex_1()
                                .gap(px(6.0))
                                .child(field_label("Password"))
                                .child(text_input_field(
                                    password.clone(),
                                    "",
                                    true,
                                    move |value: &str| password.set(value.to_owned()),
                                )),
                        ),
                )
                .child(
                    view()
                        .flex_row()
                        .gap(px(8.0))
                        .child(button("Add account", ACCENT).on_press(add_account))
                        .child(button("Register", PANEL_ALT).on_press(register_account)),
                ),
        )
        .child(
            view()
                .gap(px(8.0))
                .child(
                    text("Accounts")
                        .text_size(px(14.0))
                        .font_weight(FontWeight::BOLD),
                )
                .child(dynamic({
                    let state = state.clone();
                    move || {
                        state
                            .get()
                            .snapshot
                            .accounts
                            .into_iter()
                            .map(|account| {
                                view()
                                    .p(px(12.0))
                                    .rounded(px(10.0))
                                    .bg(rgb(PANEL_ALT))
                                    .gap(px(4.0))
                                    .child(
                                        text(account.id_uri)
                                            .text_size(px(13.0))
                                            .font_weight(FontWeight::MEDIUM),
                                    )
                                    .child(
                                        text(format!("{} | {}", account.id, account.state))
                                            .text_size(px(12.0))
                                            .text_color(rgb(MUTED)),
                                    )
                            })
                            .collect::<Vec<_>>()
                    }
                })),
        )
        .child(
            view()
                .gap(px(8.0))
                .child(
                    text("Quick call")
                        .text_size(px(14.0))
                        .font_weight(FontWeight::BOLD),
                )
                .child(
                    text_input_field(
                        destination.clone(),
                        "sip:bob@example.com",
                        false,
                        move |value: &str| destination.set(value.to_owned()),
                    ),
                )
                .child(button("Start call", ACCENT).on_press(start_call)),
        )
        .child(
            view()
                .gap(px(8.0))
                .child(
                    text("Media source")
                        .text_size(px(14.0))
                        .font_weight(FontWeight::BOLD),
                )
                .child(source_picker(
                    client.clone(),
                    state.clone(),
                    "audio",
                    "Audio input",
                    audio_menu_open,
                ))
                .child(source_picker(
                    client,
                    state,
                    "video",
                    "Video input",
                    video_menu_open,
                )),
        )
        .child(view().flex_1())
        .child(
            view()
                .p(px(12.0))
                .rounded(px(10.0))
                .bg(rgb(0x102236))
                .gap(px(3.0))
                .child(
                    text("Media path")
                        .text_size(px(12.0))
                        .text_color(rgb(MUTED)),
                )
                .child(
                    text("FFmpeg capture | Iris RTP | Direct2D")
                        .text_size(px(12.0))
                        .text_color(rgb(0x86b7ff)),
                ),
        )
}

fn call_area(
    client: Arc<IrisClient>,
    state: Signal<UiState>,
    destination: Signal<String>,
    microphone_muted: Signal<bool>,
    camera_muted: Signal<bool>,
) -> impl IntoElement {
    let call_id = signal(String::new());
    {
        let state = state.clone();
        set_interval(Duration::from_millis(100), move || {
            let id = state
                .get()
                .active_call()
                .map(|call| call.id.clone())
                .unwrap_or_default();
            if call_id.get_untracked() != id {
                call_id.set(id);
            }
        });
    }

    let answer: Arc<dyn Fn() + Send + Sync> = {
        let client = client.clone();
        let state = state.clone();
        Arc::new(move || {
            if let Some(call) = state.get().incoming_call() {
                client.answer_call(&call.id);
            }
        })
    };
    let reject: Arc<dyn Fn() + Send + Sync> = {
        let client = client.clone();
        let state = state.clone();
        Arc::new(move || {
            if let Some(call) = state.get().incoming_call() {
                client.reject_call(&call.id);
            }
        })
    };
    let hangup = {
        let client = client.clone();
        let state = state.clone();
        move || {
            if let Some(call) = state.get().active_call() {
                client.hangup_call(&call.id);
            }
        }
    };
    let hold = {
        let client = client.clone();
        let state = state.clone();
        move || {
            if let Some(call) = state.get().active_call() {
                if call.is_held() {
                    client.resume_call(&call.id);
                } else {
                    client.hold_call(&call.id);
                }
            }
        }
    };
    let toggle_microphone = {
        let client = client.clone();
        let state = state.clone();
        move || {
            let muted = !microphone_muted.get_untracked();
            microphone_muted.set(muted);
            if let Some(call) = state.get().active_call() {
                client.set_audio_muted(&call.id, muted);
            }
        }
    };
    let toggle_camera = {
        let client = client.clone();
        let state = state.clone();
        move || {
            let muted = !camera_muted.get_untracked();
            camera_muted.set(muted);
            if let Some(call) = state.get().active_call() {
                client.set_video_muted(&call.id, muted);
            }
        }
    };

    view()
        .flex_1()
        .h_full()
        .p(px(20.0))
        .gap(px(16.0))
        .child(
            view()
                .flex_1()
                .rounded(px(16.0))
                .overflow_hidden()
                .bg(rgb(0x070b12))
                .border_1()
                .border_color(rgb(BORDER))
                .child(
                    when(
                        {
                            let call_id = call_id.clone();
                            move || !call_id.get().is_empty()
                        },
                        {
                            let call_id = call_id.clone();
                            move || {
                                native("iris.remote-video")
                                    .prop("callId", {
                                        let call_id = call_id.clone();
                                        move || json!(call_id.get())
                                    })
                                    .flex_1()
                            }
                        },
                    )
                    .otherwise(move || {
                        view()
                            .flex_1()
                            .items_center()
                            .justify_center()
                            .gap(px(10.0))
                            .child(
                                text("SIP video conference")
                                    .text_size(px(30.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(0xffffff)),
                            )
                            .child(
                                text("Choose an account and start a call")
                                    .text_size(px(14.0))
                                    .text_color(rgb(MUTED)),
                            )
                    }),
                )
                .child(when(
                    {
                        let call_id = call_id.clone();
                        let camera_muted = camera_muted.clone();
                        move || !call_id.get().is_empty() && !camera_muted.get()
                    },
                    {
                        let call_id = call_id.clone();
                        move || {
                            view()
                                .absolute()
                                .right(px(18.0))
                                .bottom(px(18.0))
                                .w(px(260.0))
                                .h(px(150.0))
                                .rounded(px(12.0))
                                .overflow_hidden()
                                .border_1()
                                .border_color(rgb(0x3c4b62))
                                .child(
                                    native("iris.local-video")
                                        .prop("callId", {
                                            let call_id = call_id.clone();
                                            move || json!(call_id.get())
                                        })
                                        .flex_1(),
                                )
                        }
                    },
                ))
                .child(
                    view()
                        .absolute()
                        .top(px(18.0))
                        .left(px(18.0))
                        .right(px(18.0))
                        .flex_row()
                        .items_center()
                        .gap(px(12.0))
                        .child(
                            text({
                                let state = state.clone();
                                move || {
                                    state
                                        .get()
                                        .active_call()
                                        .map(|call| call.remote_uri.clone())
                                        .unwrap_or_else(|| "No active call".to_owned())
                                }
                            })
                            .text_size(px(16.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(0xffffff)),
                        )
                        .child(
                            text({
                                let state = state.clone();
                                move || {
                                    state
                                        .get()
                                        .active_call()
                                        .map(call_duration)
                                        .unwrap_or_else(|| "00:00".to_owned())
                                }
                            })
                            .text_size(px(13.0))
                            .text_color(rgb(0xd0d8e5)),
                        )
                        .child(view().flex_1())
                        .child(
                            text({
                                let state = state.clone();
                                move || {
                                    state
                                        .get()
                                        .active_call()
                                        .map(|call| call.state.clone())
                                        .unwrap_or_default()
                                }
                            })
                            .text_size(px(12.0))
                            .text_color(rgb(MUTED)),
                        ),
                ),
        )
        .child(when(
            {
                let state = state.clone();
                move || state.get().incoming_call().is_some()
            },
            {
                let state = state.clone();
                move || {
                    let caller = state
                        .get()
                        .incoming_call()
                        .map(|call| call.remote_uri.clone())
                        .unwrap_or_default();
                    view()
                        .p(px(14.0))
                        .rounded(px(12.0))
                        .bg(rgb(0x183351))
                        .flex_row()
                        .items_center()
                        .gap(px(12.0))
                        .child(
                            text(format!("Incoming call | {caller}"))
                                .flex_1()
                                .text_size(px(14.0))
                                .font_weight(FontWeight::SEMIBOLD),
                        )
                        .child(button("Reject", DANGER).on_press({
                            let reject = reject.clone();
                            move || reject()
                        }))
                        .child(button("Answer", SUCCESS).on_press({
                            let answer = answer.clone();
                            move || answer()
                        }))
                }
            },
        ))
        .child(
            view()
                .h(px(76.0))
                .flex_row()
                .items_center()
                .justify_center()
                .gap(px(12.0))
                .child(
                    reactive_button(
                        {
                            let microphone_muted = microphone_muted.clone();
                            move || {
                                if microphone_muted.get() {
                                    "Unmute"
                                } else {
                                    "Mute"
                                }
                            }
                        },
                        PANEL_ALT,
                    )
                    .on_press(toggle_microphone),
                )
                .child(
                    reactive_button(
                        {
                            let camera_muted = camera_muted.clone();
                            move || {
                                if camera_muted.get() {
                                    "Camera on"
                                } else {
                                    "Camera off"
                                }
                            }
                        },
                        PANEL_ALT,
                    )
                    .on_press(toggle_camera),
                )
                .child(
                    reactive_button(
                        {
                            let state = state.clone();
                            move || {
                                if state
                                    .get()
                                    .active_call()
                                    .is_some_and(Call::is_held)
                                {
                                    "Resume"
                                } else {
                                    "Hold"
                                }
                            }
                        },
                        PANEL_ALT,
                    )
                    .on_press(hold),
                )
                .child(button("Hang up", DANGER).on_press(hangup)),
        )
        .child(
            view()
                .p(px(14.0))
                .rounded(px(12.0))
                .bg(rgb(PANEL))
                .border_1()
                .border_color(rgb(BORDER))
                .flex_row()
                .gap(px(20.0))
                .child(
                    text({
                        let state = state.clone();
                        move || {
                            let state = state.get();
                            match state
                                .active_call()
                                .and_then(|call| state.stats_for(&call.id))
                            {
                                Some(stats) => format!(
                                    "RTP up {} down {} loss {} send errors {}",
                                    stats.packets_sent,
                                    stats.packets_received,
                                    stats.packets_lost,
                                    stats.send_errors
                                ),
                                None => {
                                    "RTP statistics will appear after media starts"
                                        .to_owned()
                                }
                            }
                        }
                    })
                    .text_size(px(12.0))
                    .text_color(rgb(MUTED)),
                )
                .child(view().flex_1())
                .child(
                    text(move || {
                        if destination.get().is_empty() {
                            "Windows x64 | NSH Rust".to_owned()
                        } else {
                            destination.get()
                        }
                    })
                    .text_size(px(12.0))
                    .text_color(rgb(MUTED)),
                ),
        )
}

fn source_picker(
    client: Arc<IrisClient>,
    state: Signal<UiState>,
    kind: &'static str,
    title: &'static str,
    open: Signal<bool>,
) -> View {
    view()
        .gap(px(5.0))
        .child(
            view()
                .h(px(40.0))
                .px(px(12.0))
                .rounded(px(9.0))
                .bg(rgb(PANEL_ALT))
                .border_1()
                .border_color(rgb(BORDER))
                .flex_row()
                .items_center()
                .gap(px(8.0))
                .on_press({
                    let open = open.clone();
                    move || open.update(|value| *value = !*value)
                })
                .child(
                    text(title)
                        .flex_1()
                        .text_size(px(12.0))
                        .text_color(rgb(MUTED)),
                )
                .child(
                    text({
                        let state = state.clone();
                        move || {
                            let state = state.get();
                            source_display_name(&state, kind)
                        }
                    })
                    .text_size(px(12.0))
                    .font_weight(FontWeight::MEDIUM)
                    .text_color(rgb(TEXT))
                    .number_of_lines(1),
                )
                .child(
                    text(move || if open.get() { "▲" } else { "▼" })
                        .text_size(px(11.0))
                        .text_color(rgb(MUTED)),
                ),
        )
        .child(when(
            {
                let open = open.clone();
                move || open.get()
            },
            {
            let client = client.clone();
            let state = state.clone();
            let open = open.clone();
            move || {
                view()
                    .rounded(px(9.0))
                    .border_1()
                    .border_color(rgb(BORDER))
                    .bg(rgb(0x0d1725))
                    .p(px(5.0))
                    .child(dynamic({
                        let client = client.clone();
                        let state = state.clone();
                        move || {
                            let current = source_value(&state.get(), kind);
                            let mut rows = vec![
                                source_row(
                                    client.clone(),
                                    kind,
                                    open.clone(),
                                    "auto",
                                    "Automatic (default device)",
                                    current == "auto",
                                ),
                                source_row(
                                    client.clone(),
                                    kind,
                                    open.clone(),
                                    "null",
                                    "Empty source (silent audio / gray identity card)",
                                    current == "null",
                                ),
                            ];
                            let devices = if kind == "audio" {
                                state.get().audio_devices
                            } else {
                                state.get().video_devices
                            };
                            rows.extend(devices.into_iter().map(|device| {
                                source_row(
                                    client.clone(),
                                    kind,
                                    open.clone(),
                                    &device.id,
                                    &device.name,
                                    current == device.id,
                                )
                            }));
                            rows
                        }
                    }))
                }
            },
        ))
}

fn source_row(
    client: Arc<IrisClient>,
    kind: &'static str,
    open: Signal<bool>,
    source: &str,
    label: &str,
    selected: bool,
) -> View {
    let source_value = source.to_owned();
    view()
        .min_h(px(34.0))
        .px(px(9.0))
        .rounded(px(7.0))
        .bg(if selected { rgb(0x1d3c61) } else { rgb(0x0d1725) })
        .flex_row()
        .items_center()
        .gap(px(7.0))
        .on_press(move || {
            client.set_media_input(kind, &source_value);
            open.set(false);
        })
        .child(
            text(if selected { "✓" } else { " " })
                .w(px(16.0))
                .text_size(px(12.0))
                .text_color(rgb(SUCCESS)),
        )
        .child(
            text(label.to_owned())
                .flex_1()
                .text_size(px(11.0))
                .text_color(rgb(if selected { TEXT } else { MUTED }))
                .number_of_lines(1),
        )
}

fn source_value(state: &UiState, kind: &str) -> String {
    if kind == "audio" {
        state.audio_input.clone()
    } else {
        state.video_input.clone()
    }
}

fn source_display_name(state: &UiState, kind: &str) -> String {
    let value = source_value(state, kind);
    match value.as_str() {
        "auto" => "Automatic".to_owned(),
        "null" => "Empty source".to_owned(),
        other => {
            let devices = if kind == "audio" {
                &state.audio_devices
            } else {
                &state.video_devices
            };
            devices
                .iter()
                .find(|device| device.id == other)
                .map(|device| device.name.clone())
                .unwrap_or_else(|| other.to_owned())
        }
    }
}
