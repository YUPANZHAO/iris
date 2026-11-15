#include "iris/c_api.h"

#include "iris/iris.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct FrameCallbackState {
    std::mutex mutex;
    std::condition_variable condition;
    iris_bridge_video_frame_callback callback = nullptr;
    void* user_data = nullptr;
    std::size_t active = 0;
    bool accepting = true;

    void set(
        iris_bridge_video_frame_callback next_callback,
        void* next_user_data) {
        std::lock_guard<std::mutex> lock(mutex);
        callback = next_callback;
        user_data = next_user_data;
        accepting = next_callback != nullptr;
    }

    void emit(const iris::VideoFrameView& frame) {
        iris_bridge_video_frame_callback current = nullptr;
        void* current_user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!accepting || !callback) {
                return;
            }
            current = callback;
            current_user = user_data;
            ++active;
        }

        iris_bridge_video_frame native{};
        native.struct_size = sizeof(native);
        native.kind = frame.kind == iris::VideoFrameKind::LocalPreview
            ? IRIS_BRIDGE_VIDEO_LOCAL_PREVIEW
            : IRIS_BRIDGE_VIDEO_REMOTE;
        native.call_id = frame.callId.value.c_str();
        native.data = frame.data;
        native.size = frame.size;
        native.width = frame.width;
        native.height = frame.height;
        native.stride = frame.stride;
        native.timestamp_ms = frame.timestampMs;

        try {
            current(&native, current_user);
        } catch (...) {
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            --active;
        }
        condition.notify_all();
    }

    void close() {
        std::unique_lock<std::mutex> lock(mutex);
        accepting = false;
        callback = nullptr;
        user_data = nullptr;
        condition.wait(lock, [this] { return active == 0; });
    }
};

struct iris_bridge_runtime {
    std::mutex mutex;
    std::unique_ptr<iris::Runtime> runtime;
    std::shared_ptr<FrameCallbackState> frame_callback;
};

namespace {

using Json = nlohmann::json;

char* copy_string(const std::string& value) {
    auto* result = new (std::nothrow) char[value.size() + 1];
    if (!result) {
        return nullptr;
    }
    std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}

void set_error(char** output, const std::string& message) {
    if (output) {
        *output = copy_string(message);
    }
}

void clear_output(char** output) {
    if (output) {
        *output = nullptr;
    }
}

std::string required_string(
    const Json& value,
    const char* name) {
    const auto iterator = value.find(name);
    if (iterator == value.end() ||
        !iterator->is_string() ||
        iterator->get_ref<const std::string&>().empty()) {
        throw std::invalid_argument(
            std::string("missing string field: ") + name);
    }
    return iterator->get<std::string>();
}

std::string optional_string(
    const Json& value,
    const char* name) {
    const auto iterator = value.find(name);
    return iterator != value.end() && iterator->is_string()
        ? iterator->get<std::string>()
        : std::string();
}

iris::RuntimeConfig runtime_config(const Json& value) {
    iris::RuntimeConfig config;
    config.sipPort = value.value("sipPort", config.sipPort);
    config.logLevel = value.value("logLevel", config.logLevel);
    config.nullAudioDevice =
        value.value("nullAudioDevice", config.nullAudioDevice);
    config.autoAnswerCalls =
        value.value("autoAnswerCalls", config.autoAnswerCalls);

    const auto media = value.find("media");
    if (media != value.end() && media->is_object()) {
        config.media.audioEnabled =
            media->value("audioEnabled", config.media.audioEnabled);
        config.media.videoEnabled =
            media->value("videoEnabled", config.media.videoEnabled);
        config.media.renderAudio =
            media->value("renderAudio", config.media.renderAudio);
        config.media.renderVideo =
            media->value("renderVideo", false);
        config.media.mediaAddress =
            media->value("mediaAddress", config.media.mediaAddress);
        config.media.audioInput =
            media->value("audioInput", config.media.audioInput);
        config.media.videoInput =
            media->value("videoInput", config.media.videoInput);
        config.media.audioBitrateKbps =
            media->value(
                "audioBitrateKbps",
                config.media.audioBitrateKbps);
        config.media.videoBitrateKbps =
            media->value(
                "videoBitrateKbps",
                config.media.videoBitrateKbps);
        config.media.videoWidth =
            media->value("videoWidth", config.media.videoWidth);
        config.media.videoHeight =
            media->value("videoHeight", config.media.videoHeight);
        config.media.videoFps =
            media->value("videoFps", config.media.videoFps);
    }
    return config;
}

Json account_json(const iris::AccountSnapshot& account) {
    return {
        {"id", account.id.value},
        {"name", account.name},
        {"idUri", account.idUri},
        {"registrarUri", account.registrarUri},
        {"state", iris::to_string(account.state)},
        {"statusCode", account.statusCode},
        {"statusText", account.statusText},
        {"registered", account.registered},
    };
}

Json call_json(const iris::CallSnapshot& call) {
    return {
        {"id", call.id.value},
        {"accountId", call.accountId.value},
        {"remoteUri", call.remoteUri},
        {"stateText", call.stateText},
        {"state", iris::to_string(call.state)},
        {"statusCode", call.statusCode},
        {"createdAtMs", call.createdAtMs},
        {"connectedAtMs", call.connectedAtMs},
    };
}

Json stats_json(const iris::MediaStats& stats) {
    return {
        {"packetsSent", stats.packetsSent},
        {"sendErrors", stats.sendErrors},
        {"packetsReceived", stats.packetsReceived},
        {"packetsLost", stats.packetsLost},
        {"bytesSent", stats.bytesSent},
        {"bytesReceived", stats.bytesReceived},
        {"audioFramesDecoded", stats.audioFramesDecoded},
        {"videoFramesDecoded", stats.videoFramesDecoded},
        {"roundTripTimeMs", stats.roundTripTimeMs},
        {"jitterMs", stats.jitterMs},
        {"estimatedSendBitrateKbps",
         stats.estimatedSendBitrateKbps},
        {"estimatedReceiveBitrateKbps",
         stats.estimatedReceiveBitrateKbps},
    };
}

Json event_json(const iris::Event& event) {
    return {
        {"sequence", event.sequence},
        {"timestampMs", event.timestampMs},
        {"type", iris::to_string(event.type)},
        {"accountId", event.accountId.value},
        {"callId", event.callId.value},
        {"accountState", iris::to_string(event.accountState)},
        {"callState", iris::to_string(event.callState)},
        {"statusCode", event.statusCode},
        {"reason", event.reason},
        {"remoteUri", event.remoteUri},
    };
}

Json snapshot_json(iris::Runtime& runtime) {
    Json accounts = Json::array();
    for (const auto& account : runtime.accounts()) {
        accounts.push_back(account_json(account));
    }

    Json calls = Json::array();
    Json media = Json::object();
    for (const auto& call : runtime.calls()) {
        calls.push_back(call_json(call));
        media[call.id.value] = stats_json(
            runtime.mediaStats(call.id));
    }

    return {
        {"version", runtime.version()},
        {"pjprojectVersion", runtime.pjprojectVersion()},
        {"sipPort", runtime.sipPort()},
        {"accounts", std::move(accounts)},
        {"calls", std::move(calls)},
        {"media", std::move(media)},
    };
}

Json execute_command(
    iris::Runtime& runtime,
    const Json& command) {
    const auto name = required_string(command, "command");

    if (name == "account.add") {
        iris::AccountConfig config;
        config.name = optional_string(command, "name");
        config.idUri = required_string(command, "idUri");
        config.registrarUri =
            optional_string(command, "registrarUri");
        config.username = optional_string(command, "username");
        config.password = optional_string(command, "password");
        config.authRealm = command.value("authRealm", "*");
        return {{"accountId", runtime.addAccount(config).value}};
    }
    if (name == "account.register") {
        runtime.registerAccount(
            {required_string(command, "accountId")});
        return {{"ok", true}};
    }
    if (name == "account.unregister") {
        runtime.unregisterAccount(
            {required_string(command, "accountId")});
        return {{"ok", true}};
    }
    if (name == "account.remove") {
        runtime.removeAccount(
            {required_string(command, "accountId")});
        return {{"ok", true}};
    }
    if (name == "call.start") {
        const auto id = runtime.startCall(
            {required_string(command, "accountId")},
            required_string(command, "destinationUri"));
        return {{"callId", id.value}};
    }
    if (name == "call.answer") {
        runtime.answerCall({required_string(command, "callId")});
        return {{"ok", true}};
    }
    if (name == "call.reject") {
        runtime.rejectCall({required_string(command, "callId")});
        return {{"ok", true}};
    }
    if (name == "call.hangup") {
        runtime.hangupCall({required_string(command, "callId")});
        return {{"ok", true}};
    }
    if (name == "call.hold") {
        runtime.holdCall({required_string(command, "callId")});
        return {{"ok", true}};
    }
    if (name == "call.resume") {
        runtime.resumeCall({required_string(command, "callId")});
        return {{"ok", true}};
    }
    if (name == "media.audioMuted") {
        runtime.setAudioMuted(
            {required_string(command, "callId")},
            command.value("muted", false));
        return {{"ok", true}};
    }
    if (name == "media.videoMuted") {
        runtime.setVideoMuted(
            {required_string(command, "callId")},
            command.value("muted", false));
        return {{"ok", true}};
    }
    if (name == "media.input") {
        const auto kind = required_string(command, "kind");
        const auto source = required_string(command, "source");
        runtime.setMediaInput(
            kind == "audio"
                ? iris::MediaKind::Audio
                : iris::MediaKind::Video,
            source);
        return {{"ok", true}};
    }
    if (name == "media.input.get") {
        const auto kind = required_string(command, "kind");
        return {{
            "source",
            runtime.mediaInput(
                kind == "audio"
                    ? iris::MediaKind::Audio
                    : iris::MediaKind::Video),
        }};
    }
    if (name == "media.stats") {
        return stats_json(runtime.mediaStats(
            {required_string(command, "callId")}));
    }
    if (name == "media.devices") {
        const auto kind_name = required_string(command, "kind");
        const auto catalog = iris::queryMediaDevices(
            kind_name == "audio"
                ? iris::MediaKind::Audio
                : iris::MediaKind::Video);
        Json devices = Json::array();
        for (const auto& device : catalog.devices) {
            devices.push_back({
                {"id", device.id},
                {"name", device.name},
                {"default", device.isDefault},
            });
        }
        return {
            {"ok", catalog.ok()},
            {"error", catalog.error},
            {"devices", std::move(devices)},
        };
    }

    throw std::invalid_argument(
        "unknown command: " + name);
}

bool valid_runtime(iris_bridge_runtime* runtime) {
    return runtime && runtime->runtime;
}

} // namespace

extern "C" {

int iris_bridge_runtime_create(
    const char* config_json,
    iris_bridge_runtime** out_runtime,
    char** out_error) {
    if (out_runtime) {
        *out_runtime = nullptr;
    }
    clear_output(out_error);
    if (!out_runtime) {
        set_error(out_error, "out_runtime must not be null");
        return 1;
    }

    try {
        const auto value = config_json && *config_json
            ? Json::parse(config_json)
            : Json{};
        auto result = std::make_unique<iris_bridge_runtime>();
        result->frame_callback =
            std::make_shared<FrameCallbackState>();
        result->runtime = std::make_unique<iris::Runtime>(
            runtime_config(value));
        auto frame_callback = result->frame_callback;
        result->runtime->setVideoFrameCallback(
            [frame_callback](const iris::VideoFrameView& frame) {
                frame_callback->emit(frame);
            });
        *out_runtime = result.release();
        return 0;
    } catch (const std::exception& error) {
        set_error(out_error, error.what());
    } catch (...) {
        set_error(out_error, "unknown Iris bridge initialization error");
    }
    return 1;
}

void iris_bridge_runtime_destroy(iris_bridge_runtime* runtime) {
    if (!runtime) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        if (runtime->runtime) {
            runtime->runtime->setVideoFrameCallback({});
            runtime->frame_callback->close();
            runtime->runtime.reset();
        }
    } catch (...) {
        if (runtime->frame_callback) {
            runtime->frame_callback->close();
        }
    }
    delete runtime;
}

int iris_bridge_runtime_command(
    iris_bridge_runtime* runtime,
    const char* command_json,
    char** out_result_json,
    char** out_error) {
    clear_output(out_result_json);
    clear_output(out_error);
    if (!valid_runtime(runtime)) {
        set_error(out_error, "invalid Iris runtime");
        return 1;
    }
    try {
        const auto command = Json::parse(
            command_json ? command_json : "");
        std::lock_guard<std::mutex> lock(runtime->mutex);
        if (!runtime->runtime) {
            throw std::runtime_error("Iris runtime is closed");
        }
        const auto result = execute_command(
            *runtime->runtime, command).dump();
        if (out_result_json) {
            *out_result_json = copy_string(result);
        }
        return 0;
    } catch (const std::exception& error) {
        set_error(out_error, error.what());
    } catch (...) {
        set_error(out_error, "unknown Iris bridge command error");
    }
    return 1;
}

int iris_bridge_runtime_poll_events(
    iris_bridge_runtime* runtime,
    char** out_events_json,
    char** out_error) {
    clear_output(out_events_json);
    clear_output(out_error);
    if (!valid_runtime(runtime)) {
        set_error(out_error, "invalid Iris runtime");
        return 1;
    }
    try {
        Json events = Json::array();
        std::lock_guard<std::mutex> lock(runtime->mutex);
        if (!runtime->runtime) {
            throw std::runtime_error("Iris runtime is closed");
        }
        for (const auto& event : runtime->runtime->pollEvents()) {
            events.push_back(event_json(event));
        }
        if (out_events_json) {
            *out_events_json = copy_string(events.dump());
        }
        return 0;
    } catch (const std::exception& error) {
        set_error(out_error, error.what());
    } catch (...) {
        set_error(out_error, "unknown Iris bridge event error");
    }
    return 1;
}

int iris_bridge_runtime_snapshot(
    iris_bridge_runtime* runtime,
    char** out_snapshot_json,
    char** out_error) {
    clear_output(out_snapshot_json);
    clear_output(out_error);
    if (!valid_runtime(runtime)) {
        set_error(out_error, "invalid Iris runtime");
        return 1;
    }
    try {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        if (!runtime->runtime) {
            throw std::runtime_error("Iris runtime is closed");
        }
        if (out_snapshot_json) {
            *out_snapshot_json =
                copy_string(snapshot_json(*runtime->runtime).dump());
        }
        return 0;
    } catch (const std::exception& error) {
        set_error(out_error, error.what());
    } catch (...) {
        set_error(out_error, "unknown Iris bridge snapshot error");
    }
    return 1;
}

int iris_bridge_runtime_set_video_frame_sink(
    iris_bridge_runtime* runtime,
    iris_bridge_video_frame_callback callback,
    void* user_data,
    char** out_error) {
    clear_output(out_error);
    if (!valid_runtime(runtime)) {
        set_error(out_error, "invalid Iris runtime");
        return 1;
    }
    runtime->frame_callback->set(callback, user_data);
    return 0;
}

void iris_bridge_string_free(char* value) {
    delete[] value;
}

const char* iris_bridge_version(void) {
    return "0.1.0";
}

} // extern "C"
