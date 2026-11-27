#pragma once

namespace jxrc {

enum class SdrVideoGamma { automatic, rec709, gamma22, gamma24, srgb };

float sdr_video_oetf(float linear,SdrVideoGamma gamma);

} // namespace jxrc
