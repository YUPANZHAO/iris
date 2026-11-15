use std::collections::HashMap;

use serde::Deserialize;

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct Account {
    pub id: String,
    pub name: String,
    pub id_uri: String,
    pub registrar_uri: String,
    pub state: String,
    pub status_code: i32,
    pub status_text: String,
    pub registered: bool,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct Call {
    pub id: String,
    pub account_id: String,
    pub remote_uri: String,
    pub state_text: String,
    pub state: String,
    pub status_code: i32,
    pub created_at_ms: u64,
    pub connected_at_ms: u64,
}

impl Call {
    pub fn is_active(&self) -> bool {
        self.state != "disconnected"
    }

    pub fn is_incoming(&self) -> bool {
        self.state == "incoming"
    }

    pub fn is_confirmed(&self) -> bool {
        matches!(self.state.as_str(), "confirmed" | "locally-held" | "remotely-held")
    }

    pub fn is_held(&self) -> bool {
        matches!(self.state.as_str(), "locally-held" | "remotely-held")
    }
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct MediaStats {
    pub packets_sent: u64,
    pub send_errors: u64,
    pub packets_received: u64,
    pub packets_lost: u64,
    pub bytes_sent: u64,
    pub bytes_received: u64,
    pub audio_frames_decoded: u64,
    pub video_frames_decoded: u64,
    pub round_trip_time_ms: u32,
    pub jitter_ms: u32,
    pub estimated_send_bitrate_kbps: u32,
    pub estimated_receive_bitrate_kbps: u32,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct Snapshot {
    #[serde(default)]
    pub version: String,
    #[serde(default, rename = "pjprojectVersion")]
    pub pjproject_version: String,
    #[serde(default, rename = "sipPort")]
    pub sip_port: u32,
    #[serde(default)]
    pub accounts: Vec<Account>,
    #[serde(default)]
    pub calls: Vec<Call>,
    #[serde(default)]
    pub media: HashMap<String, MediaStats>,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct MediaDevice {
    pub id: String,
    pub name: String,
    #[serde(default, rename = "default")]
    pub is_default: bool,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct IrisEvent {
    pub sequence: u64,
    pub timestamp_ms: u64,
    #[serde(rename = "type")]
    pub kind: String,
    pub account_id: String,
    pub call_id: String,
    pub account_state: String,
    pub call_state: String,
    pub status_code: i32,
    pub reason: String,
    pub remote_uri: String,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct UiState {
    pub ready: bool,
    pub snapshot: Snapshot,
    pub events: Vec<IrisEvent>,
    pub error: String,
    pub pending: bool,
    pub audio_devices: Vec<MediaDevice>,
    pub video_devices: Vec<MediaDevice>,
    pub audio_input: String,
    pub video_input: String,
}

impl UiState {
    pub fn active_call(&self) -> Option<&Call> {
        self.snapshot.calls.iter().find(|call| call.is_active())
    }

    pub fn incoming_call(&self) -> Option<&Call> {
        self.snapshot.calls.iter().find(|call| call.is_incoming())
    }

    pub fn first_account(&self) -> Option<&Account> {
        self.snapshot.accounts.first()
    }

    pub fn stats_for(&self, call_id: &str) -> Option<&MediaStats> {
        self.snapshot.media.get(call_id)
    }
}
