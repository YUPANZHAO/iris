#include "iris/c_api.h"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "C API test failed: " << message << '\n';
    }
    return condition;
}

std::string take_string(char* value) {
    std::string result = value ? value : "";
    iris_bridge_string_free(value);
    return result;
}

} // namespace

int main() {
    iris_bridge_runtime* runtime = nullptr;
    char* error = nullptr;
    const auto create_status = iris_bridge_runtime_create(
        R"({"sipPort":0,"logLevel":0,"media":{"audioEnabled":false,"videoEnabled":false}})",
        &runtime,
        &error);
    if (!expect(create_status == 0, take_string(error).c_str()) ||
        !expect(runtime != nullptr, "runtime handle is null")) {
        return 1;
    }

    char* snapshot = nullptr;
    if (!expect(
            iris_bridge_runtime_snapshot(runtime, &snapshot, &error) == 0,
            take_string(error).c_str())) {
        iris_bridge_runtime_destroy(runtime);
        return 1;
    }
    const auto first_snapshot = take_string(snapshot);
    if (!expect(
            first_snapshot.find("\"accounts\":[]") != std::string::npos,
            "initial snapshot did not contain an empty account list")) {
        iris_bridge_runtime_destroy(runtime);
        return 1;
    }

    char* result = nullptr;
    if (!expect(
            iris_bridge_runtime_command(
                runtime,
                R"({"command":"account.add","idUri":"sip:test@127.0.0.1"})",
                &result,
                &error) == 0,
            take_string(error).c_str())) {
        iris_bridge_runtime_destroy(runtime);
        return 1;
    }
    const auto add_result = take_string(result);
    if (!expect(
            add_result.find("\"accountId\"") != std::string::npos,
            "account.add did not return an account id")) {
        iris_bridge_runtime_destroy(runtime);
        return 1;
    }

    char* events = nullptr;
    if (!expect(
            iris_bridge_runtime_poll_events(runtime, &events, &error) == 0,
            take_string(error).c_str())) {
        iris_bridge_runtime_destroy(runtime);
        return 1;
    }
    const auto event_json = take_string(events);
    if (!expect(
            event_json.find("account-state") != std::string::npos,
            "account event was not delivered to C API")) {
        iris_bridge_runtime_destroy(runtime);
        return 1;
    }

    if (!expect(
            iris_bridge_runtime_set_video_frame_sink(
                runtime, nullptr, nullptr, &error) == 0,
            take_string(error).c_str())) {
        iris_bridge_runtime_destroy(runtime);
        return 1;
    }

    iris_bridge_runtime_destroy(runtime);
    std::cout << "Iris C ABI bridge passed\n";
    return 0;
}
