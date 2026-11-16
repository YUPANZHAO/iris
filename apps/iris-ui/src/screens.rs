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
const ACCENT: u32 = 0x315fc5;
const DANGER: u32 = 0xdc3d4b;
const SUCCESS: u32 = 0x2fbf81;

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Dashboard,
    Dialer,
    Settings,
}

macro_rules! icon {
    ($name:literal) => {
        nsh_core::element::svg(nsh_core::element::ImageSrc::svg_markup(include_str!(
            concat!("../assets/icons/", $name, ".svg")
        )))
        .w(px(18.0))
        .h(px(18.0))
        .flex_shrink(0.0)
        .pointer_events(PointerEvents::None)
    };
}

fn button(icon: impl IntoElement, label: &'static str, background: u32) -> View {
    reactive_button(icon, move || label, background)
}

fn navigation(page: Signal<Page>) -> View {
    view()
        .w(px(188.0))
        .h_full()
        .p(px(14.0))
        .gap(px(8.0))
        .bg(rgb(PANEL))
        .border_r_1()
        .border_r_color(rgb(BORDER))
        .child(nav_item(
            page,
            Page::Dashboard,
            icon!("layout-dashboard"),
            "Dashboard",
        ))
        .child(nav_item(page, Page::Dialer, icon!("grid-3x3"), "Dialer"))
        .child(nav_item(
            page,
            Page::Settings,
            icon!("settings"),
            "Settings",
        ))
}

fn heading(label: &'static str) -> impl IntoElement {
    text(label)
        .text_size(px(30.0))
        .font_weight(FontWeight::BOLD)
}

fn section(label: &'static str) -> impl IntoElement {
    text(label)
        .text_size(px(16.0))
        .font_weight(FontWeight::SEMIBOLD)
}

fn card() -> View {
    view()
        .p(px(20.0))
        .gap(px(10.0))
        .rounded(px(14.0))
        .bg(rgb(PANEL))
        .border_1()
        .border_color(rgb(BORDER))
}

fn stat(icon: impl IntoElement, label: &'static str, value: impl Fn() -> String + 'static) -> View {
    card().flex_1().child(icon).child(field_label(label)).child(
        text(value)
            .text_size(px(17.0))
            .font_weight(FontWeight::SEMIBOLD)
            .number_of_lines(1),
    )
}

fn nav_item(
    page: Signal<Page>,
    target: Page,
    symbol: impl IntoElement,
    label: &'static str,
) -> View {
    button(symbol, label, PANEL)
        .bg(move || rgb(if page.get() == target { ACCENT } else { PANEL }))
        .justify_start()
        .on_press(move || page.set(target))
}

fn reactive_button(
    icon: impl IntoElement,
    label: impl Fn() -> &'static str + 'static,
    background: u32,
) -> View {
    press_target()
        .flex_row()
        .items_center()
        .justify_center()
        .gap(px(8.0))
        .child(icon)
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

fn press_target() -> View {
    view()
        .role(nsh_core::AccessibilityRole::Button)
        .focusable(true)
        .focus_visible(|style| style.border_1().border_color(rgb(0x86b7ff)))
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
    } else if state
        .first_account()
        .is_some_and(|account| account.registered)
    {
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
    } else if state
        .first_account()
        .is_some_and(|account| account.registered)
    {
        SUCCESS
    } else {
        ACCENT
    }
}

pub fn app() -> impl IntoElement {
    let client = IrisClient::start();
    let state = signal(client.state());
    let account_uri = signal("sip:alice@127.0.0.1:5060".to_owned());
    let account_uri_edited = signal(false);
    let sip_port_synced = signal(false);
    let registrar_uri = signal(String::new());
    let username = signal(String::new());
    let password = signal(String::new());
    let destination = signal(String::new());
    let page = signal(Page::Dashboard);
    let show_preview = signal(false);
    let call_id = signal(String::new());
    let microphone_muted = signal(false);
    let camera_muted = signal(false);
    let audio_menu_open = signal(false);
    let video_menu_open = signal(false);

    {
        let client = client.clone();
        set_interval(Duration::from_millis(100), move || {
            let current = client.state();
            let id = current
                .active_call()
                .map(|call| call.id.as_str())
                .unwrap_or_default();
            if call_id.get_untracked() != id {
                microphone_muted.set(false);
                camera_muted.set(false);
                call_id.set(id.to_owned());
            }
            let sip_port = current.snapshot.sip_port;
            if !sip_port_synced.get_untracked() && sip_port != 0 {
                if !account_uri_edited.get_untracked() {
                    account_uri.set(format!("sip:alice@127.0.0.1:{sip_port}"));
                }
                sip_port_synced.set(true);
            }
            state.set(current);
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
                    let target = target.trim();
                    let uri = dial_uri(target, &account.id_uri);
                    client.start_call(&account.id, &uri);
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
                    icon!("minus"),
                    "Minimize",
                    WindowControl::Minimize,
                ))
                .child(window_control_button(
                    icon!("maximize"),
                    "Maximize or restore",
                    WindowControl::Maximize,
                ))
                .child(window_control_button(
                    icon!("x"),
                    "Close",
                    WindowControl::Close,
                )),
        )
        .child(
            view()
                .flex_1()
                .flex_row()
                .child(when(
                    move || call_id.get().is_empty(),
                    move || navigation(page),
                ))
                .child(
                    view().flex_1().min_w(px(0.0)).child(
                        when(move || !call_id.get().is_empty(), {
                            let client = client.clone();
                            move || {
                                call_area(
                                    client.clone(),
                                    state,
                                    call_id,
                                    microphone_muted,
                                    camera_muted,
                                    show_preview,
                                )
                            }
                        })
                        .otherwise(move || {
                            view()
                                .flex_1()
                                .child(when(
                                    move || page.get() == Page::Dashboard,
                                    move || dashboard(state, page),
                                ))
                                .child(when(move || page.get() == Page::Dialer, {
                                    let start_call = start_call.clone();
                                    move || dialer(state, page, destination, start_call.clone())
                                }))
                                .child(when(move || page.get() == Page::Settings, {
                                    let client = client.clone();
                                    let add_account = add_account.clone();
                                    let register_account = register_account.clone();
                                    move || {
                                        settings(
                                            client.clone(),
                                            state,
                                            account_uri,
                                            account_uri_edited,
                                            registrar_uri,
                                            username,
                                            password,
                                            add_account.clone(),
                                            register_account.clone(),
                                            audio_menu_open,
                                            video_menu_open,
                                            show_preview,
                                        )
                                    }
                                }))
                        }),
                    ),
                ),
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
                                    state.snapshot.version, state.snapshot.pjproject_version
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
    glyph: impl IntoElement,
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
        .child(glyph)
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

fn page_content(title: &'static str) -> nsh_core::element::Scroll {
    scroll()
        .flex_1()
        .p(px(28.0))
        .gap(px(18.0))
        .child(heading(title))
}

fn dashboard(state: Signal<UiState>, page: Signal<Page>) -> impl IntoElement {
    page_content("Dashboard")
        .child(
            view()
                .flex_row()
                .gap(px(12.0))
                .child(stat(icon!("radio"), "SIP listener", move || {
                    match state.get().snapshot.sip_port {
                        0 => "Unavailable".to_owned(),
                        port => format!("UDP · {port}"),
                    }
                }))
                .child(stat(icon!("users"), "Registered accounts", move || {
                    let accounts = state.get().snapshot.accounts;
                    format!(
                        "{} / {}",
                        accounts.iter().filter(|account| account.registered).count(),
                        accounts.len()
                    )
                }))
                .child(stat(icon!("activity"), "Active calls", move || {
                    state
                        .get()
                        .snapshot
                        .calls
                        .iter()
                        .filter(|call| call.is_active())
                        .count()
                        .to_string()
                })),
        )
        .child(
            card()
                .child(section("SIP accounts"))
                .child(dynamic(move || {
                    let accounts = state.get().snapshot.accounts;
                    if accounts.is_empty() {
                        return vec![
                            view().child(text("No accounts configured").text_color(rgb(MUTED))),
                        ];
                    }
                    accounts
                        .into_iter()
                        .map(|account| {
                            view()
                                .flex_row()
                                .gap(px(12.0))
                                .child(text(account.id_uri).flex_1().number_of_lines(1))
                                .child(text(account.state).text_color(rgb(if account.registered {
                                    SUCCESS
                                } else {
                                    MUTED
                                })))
                        })
                        .collect()
                }))
                .child(
                    view()
                        .flex_row()
                        .gap(px(8.0))
                        .child(
                            button(icon!("settings"), "Configure", PANEL_ALT)
                                .on_press(move || page.set(Page::Settings)),
                        )
                        .child(
                            button(icon!("phone"), "Open dialer", ACCENT)
                                .on_press(move || page.set(Page::Dialer)),
                        ),
                ),
        )
        .child(
            card()
                .child(section("Recent calls"))
                .child(dynamic(move || {
                    let calls: Vec<_> = state
                        .get()
                        .events
                        .into_iter()
                        .rev()
                        .filter(|event| {
                            event.call_state == "disconnected" && !event.remote_uri.is_empty()
                        })
                        .take(5)
                        .collect();
                    if calls.is_empty() {
                        return vec![view().child(text("No calls yet").text_color(rgb(MUTED)))];
                    }
                    calls
                        .into_iter()
                        .take(5)
                        .map(|call| {
                            view()
                                .flex_row()
                                .gap(px(12.0))
                                .child(text(call.remote_uri).flex_1().number_of_lines(1))
                                .child(text(call.reason).text_color(rgb(MUTED)))
                        })
                        .collect()
                })),
        )
}

fn dial_uri(target: &str, account_uri: &str) -> String {
    if target.split_once(':').is_some_and(|(scheme, _)| {
        scheme.eq_ignore_ascii_case("sip")
            || scheme.eq_ignore_ascii_case("sips")
            || scheme.eq_ignore_ascii_case("tel")
    }) {
        return target.to_owned();
    }
    let (scheme, account) = account_uri.split_once(':').unwrap_or(("sip", account_uri));
    if target.contains('@') {
        return format!("{scheme}:{target}");
    }
    let target = target.replace('#', "%23");
    match account.rsplit_once('@') {
        Some((_, domain)) => format!("{scheme}:{target}@{domain}"),
        None => format!("{scheme}:{target}"),
    }
}

fn dialer(
    state: Signal<UiState>,
    page: Signal<Page>,
    destination: Signal<String>,
    start_call: impl Fn() + Clone + 'static,
) -> impl IntoElement {
    let digits = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"];
    page_content("Dialer").child(
        card()
            .w(px(420.0))
            .child(
                text(move || {
                    state
                        .get()
                        .first_account()
                        .map(|account| format!("From {}", account.id_uri))
                        .unwrap_or_else(|| "Add a SIP account to place calls".to_owned())
                })
                .text_color(rgb(MUTED)),
            )
            .child(
                view()
                    .flex_row()
                    .gap(px(8.0))
                    .child(view().flex_1().child(text_input_field(
                        destination,
                        "Number or SIP address",
                        false,
                        move |value| destination.set(value.to_owned()),
                    )))
                    .child(
                        button(icon!("delete"), "Delete", PANEL_ALT).on_press(move || {
                            destination.update(|value| {
                                value.pop();
                            })
                        }),
                    ),
            )
            .children(
                digits
                    .chunks_exact(3)
                    .map(|row| {
                        view().flex_row().gap(px(8.0)).children(
                            row.iter()
                                .copied()
                                .map(|digit| {
                                    press_target()
                                        .flex_1()
                                        .h(px(47.0))
                                        .items_center()
                                        .justify_center()
                                        .rounded(px(8.0))
                                        .bg(rgb(PANEL_ALT))
                                        .active(|style| style.opacity(0.7))
                                        .on_press(move || {
                                            destination.update(|value| value.push_str(digit))
                                        })
                                        .child(text(digit).text_size(px(20.0)))
                                })
                                .collect::<Vec<_>>(),
                        )
                    })
                    .collect::<Vec<_>>(),
            )
            .child(
                when(
                    move || state.get().first_account().is_none(),
                    move || {
                        button(icon!("settings"), "Configure account", PANEL_ALT)
                            .on_press(move || page.set(Page::Settings))
                    },
                )
                .otherwise(move || {
                    button(icon!("phone"), "Start call", SUCCESS)
                        .disabled(move || {
                            destination.get().trim().is_empty() || state.get().pending
                        })
                        .on_press(start_call.clone())
                }),
            ),
    )
}

fn preview_button(show_preview: Signal<bool>) -> View {
    reactive_button(
        icon!("eye"),
        move || {
            if show_preview.get() {
                "Hide self-view"
            } else {
                "Show self-view"
            }
        },
        PANEL_ALT,
    )
    .on_press(move || show_preview.update(|visible| *visible = !*visible))
}

#[allow(clippy::too_many_arguments)]
fn settings(
    client: Arc<IrisClient>,
    state: Signal<UiState>,
    account_uri: Signal<String>,
    account_uri_edited: Signal<bool>,
    registrar_uri: Signal<String>,
    username: Signal<String>,
    password: Signal<String>,
    add_account: impl Fn() + 'static,
    register_account: impl Fn() + 'static,
    audio_menu_open: Signal<bool>,
    video_menu_open: Signal<bool>,
    show_preview: Signal<bool>,
) -> impl IntoElement {
    page_content("Settings")
        .child(
            card()
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
                        .child(button(icon!("users"), "Add account", ACCENT).on_press(add_account))
                        .child(
                            button(icon!("radio"), "Register", PANEL_ALT)
                                .on_press(register_account),
                        ),
                ),
        )
        .child(
            card()
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
            card()
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
        .child(
            card()
                .items_start()
                .child(section("Video during calls"))
                .child(preview_button(show_preview)),
        )
}
fn call_area(
    client: Arc<IrisClient>,
    state: Signal<UiState>,
    call_id: Signal<String>,
    microphone_muted: Signal<bool>,
    camera_muted: Signal<bool>,
    show_preview: Signal<bool>,
) -> impl IntoElement {
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
                    native("iris.remote-video")
                        .prop("callId", move || json!(call_id.get()))
                        .flex_1(),
                )
                .child(when(
                    {
                        let call_id = call_id.clone();
                        let camera_muted = camera_muted.clone();
                        move || {
                            show_preview.get() && !call_id.get().is_empty() && !camera_muted.get()
                        }
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
                        .child(button(icon!("phone-off"), "Reject", DANGER).on_press({
                            let reject = reject.clone();
                            move || reject()
                        }))
                        .child(
                            button(icon!("phone-incoming"), "Answer", SUCCESS).on_press({
                                let answer = answer.clone();
                                move || answer()
                            }),
                        )
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
                        icon!("mic"),
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
                        icon!("video"),
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
                        icon!("pause"),
                        {
                            let state = state.clone();
                            move || {
                                if state.get().active_call().is_some_and(Call::is_held) {
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
                .child(preview_button(show_preview))
                .child(button(icon!("phone-off"), "Hang up", DANGER).on_press(hangup)),
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
                                None => "RTP statistics will appear after media starts".to_owned(),
                            }
                        }
                    })
                    .text_size(px(12.0))
                    .text_color(rgb(MUTED)),
                )
                .child(view().flex_1()),
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
            press_target()
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
                    when(move || open.get(), || icon!("chevron-up"))
                        .otherwise(|| icon!("chevron-down")),
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
    press_target()
        .min_h(px(34.0))
        .px(px(9.0))
        .rounded(px(7.0))
        .bg(if selected {
            rgb(0x1d3c61)
        } else {
            rgb(0x0d1725)
        })
        .flex_row()
        .items_center()
        .gap(px(7.0))
        .on_press(move || {
            client.set_media_input(kind, &source_value);
            open.set(false);
        })
        .child(when(move || selected, || icon!("check")))
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

#[cfg(test)]
mod tests {
    use super::dial_uri;

    #[test]
    fn resolves_dialed_extensions_and_preserves_sip_addresses() {
        let account = "sip:alice@127.0.0.1:5060";
        assert_eq!(dial_uri("123#", account), "sip:123%23@127.0.0.1:5060");
        assert_eq!(dial_uri("bob@example.com", account), "sip:bob@example.com");
        assert_eq!(
            dial_uri("sips:bob@example.com", account),
            "sips:bob@example.com"
        );
    }
}
