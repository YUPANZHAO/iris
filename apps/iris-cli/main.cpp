#include "iris/iris.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex console_mutex;

std::string trim(const std::string& value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::vector<std::string> split(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

void print_help() {
    std::cout
        << "Commands:\n"
        << "  version\n"
        << "  account add <sip-uri> [registrar-uri] [username] [password]\n"
        << "  account list\n"
        << "  account register <account-id>\n"
        << "  account unregister <account-id>\n"
        << "  account remove <account-id>\n"
        << "  call start <account-id> <sip-uri>\n"
        << "  call list\n"
        << "  call answer <call-id>\n"
        << "  call reject <call-id>\n"
        << "  call hangup <call-id>\n"
        << "  call hold <call-id>\n"
        << "  call resume <call-id>\n"
        << "  media mute-audio <call-id>\n"
        << "  media unmute-audio <call-id>\n"
        << "  media mute-video <call-id>\n"
        << "  media unmute-video <call-id>\n"
        << "  media stats <call-id>\n"
        << "  media devices\n"
        << "  media source\n"
        << "  media source audio <index|id|auto|null>\n"
        << "  media source video <index|id|auto|null>\n"
        << "  events\n"
        << "  help\n"
        << "  quit\n";
}

void print_events(iris::Runtime& runtime) {
    auto events = runtime.pollEvents();
    std::lock_guard<std::mutex> lock(console_mutex);
    for (const auto& event : events) {
        std::cout << "[event " << event.sequence << "] "
                  << iris::to_string(event.type);
        if (!event.accountId.empty()) {
            std::cout << " account=" << event.accountId;
        }
        if (!event.callId.empty()) {
            std::cout << " call=" << event.callId;
        }
        if (event.type == iris::EventType::AccountStateChanged) {
            std::cout << " state=" << iris::to_string(event.accountState);
        } else {
            std::cout << " state=" << iris::to_string(event.callState);
        }
        if (!event.remoteUri.empty()) {
            std::cout << " remote=" << event.remoteUri;
        }
        if (event.statusCode != 0) {
            std::cout << " status=" << event.statusCode;
        }
        if (!event.reason.empty()) {
            std::cout << " reason=\"" << event.reason << "\"";
        }
        std::cout << '\n';
    }
}

void print_media_stats(iris::Runtime& runtime, const iris::CallId& id) {
    const auto stats = runtime.mediaStats(id);
    std::lock_guard<std::mutex> lock(console_mutex);
    std::cout << "media " << id
              << " packets-sent=" << stats.packetsSent
              << " send-errors=" << stats.sendErrors
              << " packets-received=" << stats.packetsReceived
              << " packets-lost=" << stats.packetsLost
              << " bytes-sent=" << stats.bytesSent
              << " bytes-received=" << stats.bytesReceived
              << " audio-frames=" << stats.audioFramesDecoded
              << " video-frames=" << stats.videoFramesDecoded << '\n';
}

void print_device_catalog(iris::MediaKind kind,
                          const iris::MediaDeviceCatalog& catalog) {
    std::cout << iris::to_string(kind) << " devices:\n";
    if (!catalog.ok()) {
        std::cout << "  error: " << catalog.error << '\n';
        return;
    }
    if (catalog.devices.empty()) {
        std::cout << "  none\n";
        return;
    }
    for (std::size_t index = 0;
         index < catalog.devices.size();
         ++index) {
        const auto& device = catalog.devices[index];
        std::cout << "  [" << index << "]"
                  << (device.isDefault ? " (default)" : "")
                  << " " << device.name << '\n'
                  << "      id: " << device.id << '\n';
    }
}

void print_media_sources(iris::Runtime& runtime) {
    print_device_catalog(
        iris::MediaKind::Audio,
        iris::queryMediaDevices(iris::MediaKind::Audio));
    print_device_catalog(
        iris::MediaKind::Video,
        iris::queryMediaDevices(iris::MediaKind::Video));
    std::cout << "current audio source: "
              << runtime.mediaInput(iris::MediaKind::Audio) << '\n'
              << "current video source: "
              << runtime.mediaInput(iris::MediaKind::Video) << '\n';
}

void print_accounts(iris::Runtime& runtime) {
    const auto accounts = runtime.accounts();
    if (accounts.empty()) {
        std::cout << "No accounts.\n";
        return;
    }

    for (const auto& account : accounts) {
        std::cout << account.id << " " << iris::to_string(account.state)
                  << " uri=" << account.idUri;
        if (!account.registrarUri.empty()) {
            std::cout << " registrar=" << account.registrarUri;
        }
        if (account.statusCode != 0) {
            std::cout << " status=" << account.statusCode;
        }
        if (!account.statusText.empty()) {
            std::cout << " reason=\"" << account.statusText << "\"";
        }
        std::cout << '\n';
    }
}

void print_calls(iris::Runtime& runtime) {
    const auto calls = runtime.calls();
    if (calls.empty()) {
        std::cout << "No calls.\n";
        return;
    }

    for (const auto& call : calls) {
        std::cout << call.id << " " << iris::to_string(call.state)
                  << " account=" << call.accountId
                  << " remote=" << call.remoteUri;
        if (call.statusCode != 0) {
            std::cout << " status=" << call.statusCode;
        }
        if (!call.stateText.empty()) {
            std::cout << " reason=\"" << call.stateText << "\"";
        }
        std::cout << '\n';
    }
}

int self_test(const iris::RuntimeConfig& config) {
    iris::Runtime runtime(config);
    std::cout << "libiris " << runtime.version() << '\n'
              << "pjproject " << runtime.pjprojectVersion() << '\n'
              << "endpoint initialized and destroyed successfully\n";
    return 0;
}

int repl(const iris::RuntimeConfig& config) {
    iris::Runtime runtime(config);
    std::cout << "Iris CLI " << runtime.version()
              << " (pjproject " << runtime.pjprojectVersion() << ")\n"
              << "SIP UDP port: " << runtime.sipPort() << "\n"
              << "media audio=" << (config.media.audioEnabled ? "on" : "off")
              << " video=" << (config.media.videoEnabled ? "on" : "off")
              << " source=" << config.media.audioInput << " / "
              << config.media.videoInput << "\n"
              << "Type 'help' for commands.\n";

    std::atomic<bool> auto_answer_stop{false};
    std::thread auto_answer_thread;
    if (config.autoAnswerCalls) {
        auto_answer_thread = std::thread([&] {
            while (!auto_answer_stop.load()) {
                auto events = runtime.pollEvents();
                for (const auto& event : events) {
                    std::lock_guard<std::mutex> lock(console_mutex);
                    std::cout << "[event " << event.sequence << "] "
                              << iris::to_string(event.type)
                              << " call=" << event.callId
                              << " state=" << iris::to_string(event.callState);
                    if (!event.remoteUri.empty()) {
                        std::cout << " remote=" << event.remoteUri;
                    }
                    std::cout << '\n';
                    if (event.type == iris::EventType::IncomingCall &&
                        !event.callId.empty()) {
                        try {
                            runtime.answerCall(event.callId);
                            std::cout << "Auto-answered " << event.callId << '\n';
                        } catch (const std::exception& error) {
                            std::cerr << "auto-answer error: "
                                      << error.what() << '\n';
                        }
                    }
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        });
    }

    std::string line;
    while (true) {
        std::cout << "iris> " << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            break;
        }

        auto tokens = split(trim(line));
        if (tokens.empty()) {
            continue;
        }

        try {
            const auto& command = tokens[0];
            if (command == "quit" || command == "exit") {
                break;
            }
            if (command == "help") {
                print_help();
            } else if (command == "version") {
                std::cout << "libiris " << runtime.version() << '\n'
                          << "pjproject " << runtime.pjprojectVersion() << '\n';
            } else if (command == "events") {
                print_events(runtime);
            } else if (command == "account" && tokens.size() >= 2 &&
                       tokens[1] == "list") {
                print_accounts(runtime);
            } else if (command == "account" && tokens.size() >= 3 &&
                       tokens[1] == "add") {
                iris::AccountConfig account;
                account.idUri = tokens[2];
                if (tokens.size() > 3) {
                    account.registrarUri = tokens[3];
                }
                if (tokens.size() > 4) {
                    account.username = tokens[4];
                }
                if (tokens.size() > 5) {
                    account.password = tokens[5];
                }
                const auto id = runtime.addAccount(account);
                std::cout << "Added account " << id << '\n';
            } else if (command == "account" && tokens.size() == 3 &&
                       tokens[1] == "register") {
                runtime.registerAccount({tokens[2]});
                std::cout << "Registration started for " << tokens[2] << '\n';
            } else if (command == "account" && tokens.size() == 3 &&
                       tokens[1] == "unregister") {
                runtime.unregisterAccount({tokens[2]});
                std::cout << "Unregistration started for " << tokens[2] << '\n';
            } else if (command == "account" && tokens.size() == 3 &&
                       tokens[1] == "remove") {
                runtime.removeAccount({tokens[2]});
                std::cout << "Removed account " << tokens[2] << '\n';
            } else if (command == "call" && tokens.size() == 2 &&
                       tokens[1] == "list") {
                print_calls(runtime);
            } else if (command == "call" && tokens.size() == 4 &&
                       tokens[1] == "start") {
                const auto id = runtime.startCall({tokens[2]}, tokens[3]);
                std::cout << "Started call " << id << '\n';
            } else if (command == "call" && tokens.size() == 3 &&
                       tokens[1] == "answer") {
                runtime.answerCall({tokens[2]});
            } else if (command == "call" && tokens.size() == 3 &&
                       tokens[1] == "reject") {
                runtime.rejectCall({tokens[2]});
            } else if (command == "call" && tokens.size() == 3 &&
                       tokens[1] == "hangup") {
                runtime.hangupCall({tokens[2]});
            } else if (command == "call" && tokens.size() == 3 &&
                       tokens[1] == "hold") {
                runtime.holdCall({tokens[2]});
            } else if (command == "call" && tokens.size() == 3 &&
                       tokens[1] == "resume") {
                runtime.resumeCall({tokens[2]});
            } else if (command == "media" && tokens.size() == 3 &&
                       tokens[1] == "mute-audio") {
                runtime.setAudioMuted({tokens[2]}, true);
            } else if (command == "media" && tokens.size() == 3 &&
                       tokens[1] == "unmute-audio") {
                runtime.setAudioMuted({tokens[2]}, false);
            } else if (command == "media" && tokens.size() == 3 &&
                       tokens[1] == "mute-video") {
                runtime.setVideoMuted({tokens[2]}, true);
            } else if (command == "media" && tokens.size() == 3 &&
                       tokens[1] == "unmute-video") {
                runtime.setVideoMuted({tokens[2]}, false);
            } else if (command == "media" && tokens.size() == 3 &&
                       tokens[1] == "stats") {
                print_media_stats(runtime, {tokens[2]});
            } else if (command == "media" && tokens.size() == 2 &&
                       tokens[1] == "devices") {
                print_media_sources(runtime);
            } else if (command == "media" && tokens.size() == 2 &&
                       tokens[1] == "source") {
                std::cout << "current audio source: "
                          << runtime.mediaInput(iris::MediaKind::Audio)
                          << '\n'
                          << "current video source: "
                          << runtime.mediaInput(iris::MediaKind::Video)
                          << '\n';
            } else if (command == "media" && tokens.size() == 4 &&
                       tokens[1] == "source" &&
                       tokens[2] == "audio") {
                runtime.setMediaInput(iris::MediaKind::Audio, tokens[3]);
                std::cout << "audio source set to " << tokens[3]
                          << " for subsequent calls\n";
            } else if (command == "media" && tokens.size() == 4 &&
                       tokens[1] == "source" &&
                       tokens[2] == "video") {
                runtime.setMediaInput(iris::MediaKind::Video, tokens[3]);
                std::cout << "video source set to " << tokens[3]
                          << " for subsequent calls\n";
            } else {
                std::cout << "Unknown or incomplete command. Type 'help'.\n";
            }

            print_events(runtime);
        } catch (const std::exception& error) {
            std::cerr << "error: " << error.what() << '\n';
        }
    }

    auto_answer_stop = true;
    if (auto_answer_thread.joinable()) {
        auto_answer_thread.join();
    }
    print_events(runtime);
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    iris::RuntimeConfig config;
    bool run_self_test = false;
    bool list_devices = false;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--self-test") {
                run_self_test = true;
            } else if (argument == "--list-devices") {
                list_devices = true;
            } else if (argument == "--help" || argument == "-h") {
                std::cout
                    << "Usage: iris-cli [--self-test] [--sip-port PORT] "
                       "[--log-level LEVEL] [--audio-input SOURCE] "
                       "[--video-input SOURCE] [--audio-device SELECTOR] "
                       "[--video-device SELECTOR] [--list-devices] "
                       "[--media-address ADDRESS] "
                       "[--no-audio] [--no-video] [--auto-answer]\n";
                return 0;
            } else if (argument == "--sip-port" && index + 1 < argc) {
                config.sipPort = static_cast<unsigned>(
                    std::stoul(argv[++index]));
            } else if (argument == "--log-level" && index + 1 < argc) {
                config.logLevel = std::stoi(argv[++index]);
            } else if (argument == "--audio-input" && index + 1 < argc) {
                config.media.audioInput = argv[++index];
            } else if (argument == "--audio-device" &&
                       index + 1 < argc) {
                config.media.audioInput = argv[++index];
            } else if (argument == "--video-input" && index + 1 < argc) {
                config.media.videoInput = argv[++index];
            } else if (argument == "--video-device" &&
                       index + 1 < argc) {
                config.media.videoInput = argv[++index];
            } else if (argument == "--media-address" && index + 1 < argc) {
                config.media.mediaAddress = argv[++index];
            } else if (argument == "--video-width" && index + 1 < argc) {
                config.media.videoWidth = std::stoi(argv[++index]);
            } else if (argument == "--video-height" && index + 1 < argc) {
                config.media.videoHeight = std::stoi(argv[++index]);
            } else if (argument == "--video-fps" && index + 1 < argc) {
                config.media.videoFps = std::stoi(argv[++index]);
            } else if (argument == "--audio-bitrate" && index + 1 < argc) {
                config.media.audioBitrateKbps = std::stoi(argv[++index]);
            } else if (argument == "--video-bitrate" && index + 1 < argc) {
                config.media.videoBitrateKbps = std::stoi(argv[++index]);
            } else if (argument == "--no-audio") {
                config.media.audioEnabled = false;
            } else if (argument == "--no-video") {
                config.media.videoEnabled = false;
            } else if (argument == "--no-audio-render") {
                config.media.renderAudio = false;
            } else if (argument == "--no-video-render") {
                config.media.renderVideo = false;
            } else if (argument == "--auto-answer") {
                config.autoAnswerCalls = true;
            } else {
                std::cerr << "Unknown argument: " << argument << '\n';
                return 2;
            }
        }

        if (list_devices) {
            print_device_catalog(
                iris::MediaKind::Audio,
                iris::queryMediaDevices(iris::MediaKind::Audio));
            print_device_catalog(
                iris::MediaKind::Video,
                iris::queryMediaDevices(iris::MediaKind::Video));
            std::cout << "Use --audio-device <index|id|auto|null> and "
                         "--video-device <index|id|auto|null>.\n";
            return 0;
        }
        if (run_self_test) {
            return self_test(config);
        }
        return repl(config);
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}

