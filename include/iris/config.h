#pragma once

#include "iris/media.h"

#include <string>

namespace iris {

struct RuntimeConfig {
    unsigned sipPort = 0;
    int logLevel = 3;
    bool nullAudioDevice = true;
    bool autoAnswerCalls = false;
    MediaRuntimeConfig media;
};

struct AccountConfig {
    std::string name;
    std::string idUri;
    std::string registrarUri;
    std::string username;
    std::string password;
    std::string authRealm = "*";
};

} // namespace iris

