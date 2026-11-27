#pragma once
#include "jxrc/converter.hpp"
#include "../native/video_gamma.h"
#include <QStringList>

namespace jxrc::gui {
enum class VideoCodec { h264, hevc, av1 };
enum class VideoContainer { mp4, mkv, mov, webm };
enum class VideoTransfer { automatic, sdr, pq, hlg };
enum class VideoEncoder { software, svt, nvenc };
enum class VideoRate { crf, cqp, bitrate, lossless };
enum class VideoChroma { yuv420, yuv422, yuv444 };
enum class VideoAudio { automatic, copy, aac, opus, none };

struct VideoEncoderSettings {
    QString name;
    QStringList presets, tunes;
    QString defaultPreset, parametersFlag;
    int defaultFactor, maximumFactor;
    QList<VideoRate> rates;
};

struct VideoOutput {
    VideoCodec codec = VideoCodec::hevc;
    VideoContainer container = VideoContainer::mp4;
    VideoTransfer transfer = VideoTransfer::automatic;
    SdrVideoGamma sdrGamma = SdrVideoGamma::automatic;
    VideoEncoder encoder = VideoEncoder::software;
    VideoRate rate = VideoRate::crf;
    VideoChroma chroma = VideoChroma::yuv420;
    VideoAudio audio = VideoAudio::automatic;
    int depth = 0;
    int factor = 23;
    int bitrate = 6000;
    int audioBitrate = 192;
    QString preset, tune, parameters;
    bool processPixels = false;
    bool gpuToneMapping = false;
};

const VideoEncoderSettings& videoEncoderSettings(VideoCodec, VideoEncoder);
bool supportsVideoContainer(VideoCodec, VideoContainer);
bool supportsVideoAudio(VideoContainer, VideoAudio);
bool supportsVideoDepth(VideoCodec, VideoEncoder, int depth);
QString videoExtension(VideoContainer);
QString videoCodecName(VideoCodec);
QString videoTransferName(VideoTransfer);
QString videoQualityName(const VideoOutput&);
bool convertVideo(const QString& input, const std::filesystem::path& output, const Options&,
                  const VideoOutput&, const std::function<bool()>& cancelled,
                  const std::function<void(int)>& progress);
}
