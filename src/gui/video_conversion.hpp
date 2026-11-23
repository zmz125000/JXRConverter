#pragma once
#include "jxrc/converter.hpp"
#include <QString>

namespace jxrc::gui {
enum class VideoCodec { h264, hevc, av1 };
enum class VideoTransfer { automatic, sdr, pq, hlg };

struct VideoOutput {
    VideoCodec codec=VideoCodec::hevc;
    VideoTransfer transfer=VideoTransfer::automatic;
    int crf=22;
    bool process_pixels=false;
};

QString videoExtension(VideoCodec);
QString videoCodecName(VideoCodec);
QString videoTransferName(VideoTransfer);
bool convertVideo(const QString& input,const std::filesystem::path& output,const Options&,
                  const VideoOutput&,const std::function<bool()>& cancelled,
                  const std::function<void(int)>& progress);
}
