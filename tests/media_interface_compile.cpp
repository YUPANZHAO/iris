#include "iris/media.h"
#include "media/engine/media_interfaces.h"

#include <type_traits>

static_assert(std::is_abstract_v<iris::media::IMediaTransport>);
static_assert(std::is_abstract_v<iris::media::IMediaBackend>);
static_assert(std::is_abstract_v<iris::media::ISdpController>);
static_assert(std::is_abstract_v<iris::media::IMediaEngine>);

int main() {
    iris::MediaSessionConfig config;
    config.audioEnabled = true;
    config.videoEnabled = true;
    config.audioDirection = iris::MediaDirection::SendRecv;
    config.videoDirection = iris::MediaDirection::SendRecv;
    return 0;
}

