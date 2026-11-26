#pragma once

#include "../native/animation.h"
#include "video_conversion.hpp"
#include <optional>

namespace jxrc::gui {

struct AnimationOutput {
    QStringList inputs;
    std::optional<VideoOutput> video;
    AnimationFormat format = AnimationFormat::webp;
    AnimationTransfer transfer = AnimationTransfer::sdr;
    unsigned delayUs = 1000000;
    bool gifIndexedColor = true;
    bool slowMotion = false;
    AnimationPlacement placement;
};

QString animationExtension(const AnimationOutput&);
QString animationSuffix(const AnimationOutput&);
bool convertAnimation(const std::filesystem::path&, const Options&, const AnimationOutput&,
                      const std::function<bool()>& cancelled, const std::function<void(int)>& progress);

}
