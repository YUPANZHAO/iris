#pragma once

#include "media/capture/capture.h"

#include <memory>

namespace iris::media::v2 {

class FFmpegCaptureProvider final : public ICaptureProvider {
public:
    FFmpegCaptureProvider();
    ~FFmpegCaptureProvider() override;

    std::vector<CaptureSourceInfo> list(MediaKind kind) const override;
    std::unique_ptr<ICaptureSource> create(
        const CaptureConfig& config,
        Status* status) override;
};

} // namespace iris::media::v2
