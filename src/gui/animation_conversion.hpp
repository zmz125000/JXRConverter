#pragma once

#include "../native/animation.h"
#include "video_conversion.hpp"
#include <optional>

namespace jxrc::gui {

struct AnimationOutput {
    QStringList inputs;
    std::optional<VideoOutput> video;
    AnimationFormat format = AnimationFormat::gif;
    AnimationTransfer transfer = AnimationTransfer::sdr;
    unsigned delayUs = 83333;
    bool slowMotion = false;
    unsigned horizontal = 1, vertical = 1; // Start, center, end on each axis.
};

QString animationExtension(const AnimationOutput&);
QString animationSuffix(const AnimationOutput&);
bool convertAnimation(const std::filesystem::path&, const Options&, const AnimationOutput&,
                      const std::function<bool()>& cancelled, const std::function<void(int)>& progress);

}
