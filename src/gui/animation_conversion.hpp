#pragma once

#include "../native/animation.h"
#include <QStringList>

namespace jxrc::gui {

struct AnimationOutput {
    QStringList inputs;
    AnimationFormat format = AnimationFormat::gif;
    AnimationTransfer transfer = AnimationTransfer::sdr;
    unsigned delayUs = 83333;
    bool slowMotion = false;
    unsigned horizontal = 1, vertical = 1; // Start, center, end on each axis.
};

QString animationExtension(AnimationFormat);
QString animationSuffix(const AnimationOutput&);
bool convertAnimation(const std::filesystem::path&, const Options&, const AnimationOutput&,
                      const std::function<bool()>& cancelled, const std::function<void(int)>& progress);

}
