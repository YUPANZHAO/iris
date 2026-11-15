use std::{
    ffi::c_void,
    sync::{
        Arc, Mutex, RwLock,
        mpsc::{self, RecvTimeoutError, Sender},
    },
    thread::{self, JoinHandle},
    time::Duration,
};

use serde_json::{Value, json};

use super::{
    ffi::{NativeApi, RuntimeHandle},
    model::{IrisEvent, MediaDevice, Snapshot, UiState},
    video::{frame_callback, frame_store},
};

enum ServiceMessage {
    Command(Value),
    Stop,
}

pub struct IrisClient {
    sender: Sender<ServiceMessage>,
    state: Arc<RwLock<UiState>>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl IrisClient {
    pub fn start() -> Arc<Self> {
        let (sender, receiver) = mpsc::channel();
        let state = Arc::new(RwLock::new(UiState {
            pending: true,
            audio_input: "auto".to_owned(),
            video_input: "auto".to_owned(),
            ..Default::default()
        }));
        let worker_state = state.clone();
        let worker = thread::spawn(move || {
            run_service(receiver, worker_state);
        });
        Arc::new(Self {
            sender,
            state,
            worker: Mutex::new(Some(worker)),
        })
    }

    pub fn send(&self, command: Value) {
        let _ = self.sender.send(ServiceMessage::Command(command));
    }

    pub fn state(&self) -> UiState {
        self.state
            .read()
            .unwrap_or_else(|error| error.into_inner())
            .clone()
    }

    pub fn add_account(
        &self,
        name: &str,
        id_uri: &str,
        registrar_uri: &str,
        username: &str,
        password: &str,
    ) {
        self.send(json!({
            "command": "account.add",
            "name": name,
            "idUri": id_uri,
            "registrarUri": registrar_uri,
            "username": username,
            "password": password,
        }));
    }

    pub fn register_account(&self, account_id: &str) {
        self.send(json!({
            "command": "account.register",
            "accountId": account_id,
        }));
    }

    pub fn start_call(&self, account_id: &str, destination_uri: &str) {
        self.send(json!({
            "command": "call.start",
            "accountId": account_id,
            "destinationUri": destination_uri,
        }));
    }

    pub fn answer_call(&self, call_id: &str) {
        self.call_command("call.answer", call_id);
    }

    pub fn reject_call(&self, call_id: &str) {
        self.call_command("call.reject", call_id);
    }

    pub fn hangup_call(&self, call_id: &str) {
        self.call_command("call.hangup", call_id);
    }

    pub fn hold_call(&self, call_id: &str) {
        self.call_command("call.hold", call_id);
    }

    pub fn resume_call(&self, call_id: &str) {
        self.call_command("call.resume", call_id);
    }

    pub fn set_audio_muted(&self, call_id: &str, muted: bool) {
        self.send(json!({
            "command": "media.audioMuted",
            "callId": call_id,
            "muted": muted,
        }));
    }

    pub fn set_video_muted(&self, call_id: &str, muted: bool) {
        self.send(json!({
            "command": "media.videoMuted",
            "callId": call_id,
            "muted": muted,
        }));
    }

    pub fn query_devices(&self, kind: &str) {
        self.send(json!({
            "command": "media.devices",
            "kind": kind,
        }));
    }

    pub fn set_media_input(&self, kind: &str, source: &str) {
        self.send(json!({
            "command": "media.input",
            "kind": kind,
            "source": source,
        }));
    }

    fn call_command(&self, command: &str, call_id: &str) {
        self.send(json!({
            "command": command,
            "callId": call_id,
        }));
    }
}

impl Drop for IrisClient {
    fn drop(&mut self) {
        let _ = self.sender.send(ServiceMessage::Stop);
        if let Some(worker) = self
            .worker
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .take()
        {
            let _ = worker.join();
        }
        let _ = self.sender.send(ServiceMessage::Stop);
    }
}

fn run_service(
    receiver: mpsc::Receiver<ServiceMessage>,
    state: Arc<RwLock<UiState>>,
) {
    let frame_store = frame_store();
    let runtime = NativeApi::load().and_then(|api| {
        let media_address =
            std::env::var("IRIS_MEDIA_ADDRESS").unwrap_or_else(|_| "auto".to_owned());
        let config = json!({
            "sipPort": 5060,
            "logLevel": 3,
            "autoAnswerCalls": false,
            "media": {
                "audioEnabled": true,
                "videoEnabled": true,
                "renderAudio": true,
                "renderVideo": false,
                "mediaAddress": media_address,
                "audioInput": "auto",
                "videoInput": "auto",
                "videoWidth": 1280,
                "videoHeight": 720,
                "videoFps": 30,
                "audioBitrateKbps": 64,
                "videoBitrateKbps": 1500
            }
        });
        let runtime = RuntimeHandle::create(api, &config.to_string())?;
        runtime.api().set_video_frame_sink(
            runtime.raw(),
            Some(frame_callback),
            Arc::as_ptr(&frame_store) as *mut c_void,
        )?;
        Ok(runtime)
    });

    let runtime = match runtime {
        Ok(runtime) => {
            let mut state_guard = state
                .write()
                .unwrap_or_else(|error| error.into_inner());
            state_guard.ready = true;
            state_guard.pending = false;
            drop(state_guard);
            for kind in ["audio", "video"] {
                if let Ok(result) = runtime.api().command(
                    runtime.raw(),
                    &json!({
                        "command": "media.devices",
                        "kind": kind,
                    })
                    .to_string(),
                ) {
                    let mut state = state
                        .write()
                        .unwrap_or_else(|error| error.into_inner());
                    apply_device_result(&mut state, kind, &result);
                }
            }
            runtime
        }
        Err(error) => {
            let mut state = state
                .write()
                .unwrap_or_else(|error| error.into_inner());
            state.error = error;
            state.pending = false;
            return;
        }
    };

    let mut event_log = Vec::<IrisEvent>::new();
    loop {
        match receiver.recv_timeout(Duration::from_millis(80)) {
            Ok(ServiceMessage::Stop) | Err(RecvTimeoutError::Disconnected) => break,
            Ok(ServiceMessage::Command(command)) => {
                {
                    let mut state = state
                        .write()
                        .unwrap_or_else(|error| error.into_inner());
                    state.pending = true;
                    state.error.clear();
                }
                let result = runtime
                    .api()
                    .command(runtime.raw(), &command.to_string());
                let mut state = state
                    .write()
                    .unwrap_or_else(|error| error.into_inner());
                state.pending = false;
                match result {
                    Ok(result) => {
                        apply_command_result(&mut state, &command, &result);
                    }
                    Err(error) => state.error = error,
                }
            }
            Err(RecvTimeoutError::Timeout) => {}
        }

        if let Ok(events_json) = runtime.api().poll_events(runtime.raw())
            && let Ok(events) = serde_json::from_str::<Vec<IrisEvent>>(&events_json)
        {
            event_log.extend(events);
            if event_log.len() > 200 {
                event_log.drain(0..event_log.len() - 200);
            }
        }

        if let Ok(snapshot_json) = runtime.api().snapshot(runtime.raw())
            && let Ok(snapshot) = serde_json::from_str::<Snapshot>(&snapshot_json)
        {
            let mut state = state
                .write()
                .unwrap_or_else(|error| error.into_inner());
            state.snapshot = snapshot;
            state.events = event_log.clone();
        }
    }

    let _ = runtime.api().set_video_frame_sink(
        runtime.raw(),
        None,
        std::ptr::null_mut(),
    );
    drop(runtime);
    let mut state = state
        .write()
        .unwrap_or_else(|error| error.into_inner());
    state.ready = false;
}

fn apply_command_result(state: &mut UiState, command: &Value, result: &str) {
    match command.get("command").and_then(Value::as_str) {
        Some("media.devices") => {
            if let Some(kind) = command.get("kind").and_then(Value::as_str) {
                apply_device_result(state, kind, result);
            }
        }
        Some("media.input") => {
            let kind = command.get("kind").and_then(Value::as_str);
            let source = command.get("source").and_then(Value::as_str);
            match (kind, source) {
                (Some("audio"), Some(source)) => {
                    state.audio_input = source.to_owned();
                }
                (Some("video"), Some(source)) => {
                    state.video_input = source.to_owned();
                }
                _ => {}
            }
        }
        _ => {}
    }
}

fn apply_device_result(state: &mut UiState, kind: &str, result: &str) {
    let Ok(value) = serde_json::from_str::<Value>(result) else {
        return;
    };
    if !value.get("ok").and_then(Value::as_bool).unwrap_or(false) {
        state.error = value
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or("cannot enumerate media devices")
            .to_owned();
        return;
    }
    let devices = value
        .get("devices")
        .cloned()
        .and_then(|value| serde_json::from_value::<Vec<MediaDevice>>(value).ok())
        .unwrap_or_default();
    if kind == "audio" {
        state.audio_devices = devices;
    } else {
        state.video_devices = devices;
    }
}
