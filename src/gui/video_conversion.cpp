#include "video_conversion.hpp"
#include "animation_conversion.hpp"
#include "video_frame.hpp"
#include "video_source.hpp"
#include "../native/staged_output.h"
#include "../native/video_color.h"
#include <QByteArray>
#include <QFileInfo>
#include <QProcess>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <algorithm>
#include <array>
#include <fstream>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace jxrc::gui {
namespace {
using video::VideoStream;
constexpr qsizetype framePipeBuffer = 1024 * 1024;

bool tagged(const QString& name) {
    return !name.isEmpty()&&name!="unknown"&&name!="unspecified"&&name!="reserved";
}

unsigned sourcePrimaries(const VideoStream& stream) {
    if(stream.primaries)return unsigned(stream.primaries);
    if(stream.primariesName=="bt470bg")return 5;
    if(stream.primariesName=="smpte170m"||stream.primariesName=="smpte240m")return 6;
    if(tagged(stream.primariesName))throw std::runtime_error("Unsupported source video primaries for pixel processing.");
    return stream.size.height()<=480?6:stream.size.height()<=576?5:1;
}

unsigned sourceTransfer(const VideoStream& stream) {
    if(stream.transfer)return unsigned(stream.transfer);
    const auto& name=stream.transferName;
    if(name=="iec61966-2-1")return 13;
    if(name=="linear")return 8;
    if(name=="gamma22")return 4;
    if(name=="gamma28")return 5;
    if(name=="bt709"||name=="smpte170m"||name=="bt2020-10"||name=="bt2020-12"||!tagged(name))return 1;
    throw std::runtime_error("Unsupported source video transfer for pixel processing.");
}

QString matrixFilter(const QString& matrix,unsigned fallback) {
    if(matrix=="bt2020nc")return "bt2020";
    if(matrix=="smpte170m"||matrix=="bt470bg")return "bt601";
    if(matrix=="bt709"||matrix=="smpte240m"||matrix=="fcc")return matrix;
    return fallback==9?"bt2020":fallback==5||fallback==6?"bt601":"bt709";
}

QSize outputSize(const QSize& source,unsigned maxEdge) {
    QSize size=source;
    const int edge=std::max(size.width(),size.height());
    if(maxEdge&&unsigned(edge)>maxEdge)size.scale(int(maxEdge),int(maxEdge),Qt::KeepAspectRatio);
    return {std::max(2,size.width()&~1),std::max(2,size.height()&~1)};
}

QString encoderPixelFormat(bool deep, bool hdr, const VideoOutput& target) {
    const int depth=target.depth?target.depth:(hdr||(deep&&target.codec!=VideoCodec::h264)?10:8);
    if(target.encoder==VideoEncoder::nvenc)return depth==10?"p010le":"nv12";
    const QString chroma=target.chroma==VideoChroma::yuv444?"444":target.chroma==VideoChroma::yuv422?"422":"420";
    return "yuv"+chroma+"p"+(depth==8?QString{}:QString::number(depth)+"le");
}

QStringList encoderArguments(const VideoOutput& target, int threads, const QString& contentLight) {
    const auto& settings=videoEncoderSettings(target.codec,target.encoder);
    const auto preset=target.preset.isEmpty()?settings.defaultPreset:target.preset;
    const bool aom=settings.name=="libaom-av1";
    const bool x265=settings.name=="libx265";
    const bool nvenc=target.encoder==VideoEncoder::nvenc;
    QStringList args={"-c:v",settings.name,aom?"-cpu-used":"-preset",preset};
    if(aom)args << "-row-mt" << "1";
    if(!target.tune.isEmpty())args << "-tune" << target.tune;
    QStringList params;
    if(!target.parameters.trimmed().isEmpty())params << target.parameters.trimmed();
    switch(target.rate) {
    case VideoRate::crf:
        args << "-crf" << QString::number(target.factor);
        if(aom)args << "-b:v" << "0";
        break;
    case VideoRate::cqp:
        if(aom) {
            const auto qp=QString::number(target.factor);
            args << "-crf" << qp << "-b:v" << "0" << "-qmin" << qp << "-qmax" << qp << "-aq-mode" << "0";
        } else {
            if(nvenc)args << "-rc" << "constqp";
            if(target.encoder==VideoEncoder::svt)params << "rc=0" << "aq-mode=0" << "qp="+QString::number(target.factor);
            args << "-qp" << QString::number(target.factor);
        }
        break;
    case VideoRate::bitrate:
        if(nvenc)args << "-rc" << "vbr";
        args << "-b:v" << QString::number(target.bitrate)+"k";
        break;
    case VideoRate::lossless:
        if(x265||aom)params << "lossless=1";
        if(aom)args << "-b:v" << "0";
        if(!x265&&!aom)args << "-qp" << "0";
        break;
    }
    if(x265) {
        params << "pools="+QString::number(threads) << "log-level=error";
        if(!contentLight.isEmpty())params << "max-cll="+contentLight;
    }
    if(!params.isEmpty()&&!settings.parametersFlag.isEmpty())args << settings.parametersFlag << params.join(':');
    return args;
}

QStringList audioArguments(const VideoOutput& target, const QString& sourceIndex) {
    auto audio=target.audio;
    if(audio==VideoAudio::automatic) {
        if(target.container==VideoContainer::mkv)audio=VideoAudio::copy;
        else if(target.container==VideoContainer::webm)audio=VideoAudio::opus;
        else audio=VideoAudio::aac;
    }
    if(audio==VideoAudio::none)return {};
    QStringList args={"-map",sourceIndex+":a?","-c:a"};
    if(audio==VideoAudio::copy)return args << "copy";
    return args << (audio==VideoAudio::aac?"aac":"libopus") << "-b:a" << QString::number(target.audioBitrate)+"k";
}

void checkCancelled(const std::function<bool()>& cancelled) {
    if(cancelled())throw std::runtime_error("Video conversion cancelled");
}

class VideoProcess {
public:
    VideoProcess(const QString& program,const QStringList& arguments,bool discardOutput=false) {
        if(discardOutput)process.setStandardOutputFile(QProcess::nullDevice());
        process.start(program,arguments);
        if(!process.waitForStarted(3000))
            throw std::runtime_error(("Cannot start FFmpeg: "+process.errorString()).toStdString());
    }
    ~VideoProcess() {
        if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(1000);}
    }
    void readErrors() {
        errors+=process.readAllStandardError();
        if(errors.size()>8192)errors=errors.right(8192);
    }
    [[noreturn]] void fail(const QString& action) {
        readErrors();
        auto detail=QString::fromLocal8Bit(errors).trimmed().section('\n',-5);
        if(detail.isEmpty())detail=process.errorString();
        throw std::runtime_error((action+": "+detail).toStdString());
    }
    void checkExit(const QString& action) {
        if(process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0)fail(action);
    }
    QProcess process;
private:
    QByteArray errors;
};

class EncodeProgress {
public:
    EncodeProgress(VideoProcess& encoder,double duration,const std::function<void(int)>& progress)
        : encoder_(encoder), duration_(duration), progress_(progress) {}
    void poll() {
        encoder_.readErrors();
        buffer_+=encoder_.process.readAllStandardOutput();
        const auto lastLine=buffer_.lastIndexOf('\n');
        if(lastLine<0)return;
        const auto lines=buffer_.left(lastLine).split('\n');
        buffer_.remove(0,lastLine+1);
        if(duration_<=0||!std::isfinite(duration_))return;
        for(const auto& line:lines)if(line.startsWith("out_time_us=")) {
            bool ok=false;
            const double seconds=line.mid(12).toLongLong(&ok)/1000000.;
            if(!ok)continue;
            const int percent=int(std::clamp(seconds/duration_*100.,0.,99.));
            if(percent>last_){last_=percent;progress_(percent);}
        }
    }
private:
    VideoProcess& encoder_;
    const double duration_;
    const std::function<void(int)>& progress_;
    QByteArray buffer_;
    int last_=-1;
};

void readFrames(const QString& ffmpeg,QStringList args,const QSize& size,unsigned bytesPerPixel,
                const std::function<bool()>& cancelled,const std::function<void(QByteArray&)>& frameReady,
                const std::function<void()>& poll={}) {
    const auto pixels=quint64(size.width())*quint64(size.height());
    if(!pixels||pixels>quint64(std::numeric_limits<qsizetype>::max())/bytesPerPixel)
        throw std::runtime_error("Video frame dimensions exceed addressable memory.");
    QByteArray frame(qsizetype(pixels*bytesPerPixel),Qt::Uninitialized);

    // QProcess has no stdout buffer limit. In particular the Windows pipe
    // reader keeps accumulating data while the conversion thread is busy.
    // A capped loopback socket lets the OS block FFmpeg when we stop reading.
    QTcpServer server;
    if(!server.listen(QHostAddress::LocalHost))
        throw std::runtime_error(("Cannot open video decoder stream: "+server.errorString()).toStdString());
    args << QString("tcp://127.0.0.1:%1").arg(server.serverPort());
    VideoProcess decoder(ffmpeg,args,true);
    const auto tick=[&] {
        checkCancelled(cancelled);
        if(poll)poll();
        decoder.readErrors();
    };
    while(!server.hasPendingConnections()) {
        tick();
        if(server.waitForNewConnection(50))break;
        if(decoder.process.waitForFinished(0)||decoder.process.state()==QProcess::NotRunning)
            decoder.fail("Video decoder did not open its frame stream");
    }
    std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
    socket->setReadBufferSize(framePipeBuffer);
    server.close();

    qsizetype available=0;
    bool wroteFrame=false;
    while(true) {
        tick();
        const auto read=socket->read(frame.data()+available,frame.size()-available);
        if(read>0)available+=read;
        if(available==frame.size()) {
            frameReady(frame);
            wroteFrame=true;
            available=0;
        } else if(socket->state()==QAbstractSocket::UnconnectedState&&socket->bytesAvailable()==0) {
            break;
        } else {
            if(!socket->waitForReadyRead(50)&&socket->state()!=QAbstractSocket::UnconnectedState)
                decoder.process.waitForFinished(0);
        }
    }
    while(decoder.process.state()!=QProcess::NotRunning) {
        tick();
        decoder.process.waitForFinished(50);
    }
    decoder.checkExit("Video decode failed");
    if(available||!wroteFrame)throw std::runtime_error("FFmpeg did not return complete video frames.");
}

void writeFrame(VideoProcess& encoder,const char* data,qsizetype size,const std::function<bool()>& cancelled,
                const std::function<void()>& poll) {
    while(size>0) {
        checkCancelled(cancelled);
        const auto chunk=std::min(size,framePipeBuffer);
        if(encoder.process.write(data,chunk)!=chunk)encoder.fail("Cannot send frame to FFmpeg");
        data+=chunk;
        size-=chunk;
        while(encoder.process.bytesToWrite()>0) {
            checkCancelled(cancelled);
            encoder.process.waitForBytesWritten(50);
            poll();
        }
    }
}

void streamFrames(const QString& ffmpeg,const QStringList& args,VideoProcess& encoder,EncodeProgress& progress,
                  const QSize& size,VideoColor source,VideoColor output,const Options& options,
                  const std::function<bool()>& cancelled) {
    std::vector<float> working;
    const auto poll=[&] {
        progress.poll();
        if(encoder.process.state()==QProcess::NotRunning)encoder.fail("Video encode failed");
    };
    readFrames(ffmpeg,args,size,6,cancelled,[&](QByteArray& frame) {
        process_video_pixels(reinterpret_cast<uint8_t*>(frame.data()),size_t(frame.size()/6),source,output,options,working);
        writeFrame(encoder,frame.constData(),frame.size(),cancelled,poll);
    },poll);
    encoder.process.closeWriteChannel();
}
}

const VideoEncoderSettings& videoEncoderSettings(VideoCodec codec, VideoEncoder encoder) {
    static const QStringList x26xPresets={"ultrafast","superfast","veryfast","faster","fast","medium","slow","slower","veryslow","placebo"};
    static const QList<VideoRate> x26xRates={VideoRate::crf,VideoRate::cqp,VideoRate::bitrate,VideoRate::lossless};
    static const VideoEncoderSettings x264={"libx264",x26xPresets,
        {"film","animation","grain","stillimage","psnr","ssim","fastdecode","zerolatency"},
        "medium","-x264-params",23,51,x26xRates};
    static const VideoEncoderSettings x265={"libx265",x26xPresets,
        {"psnr","ssim","grain","fastdecode","zerolatency","animation"},
        "medium","-x265-params",23,51,x26xRates};
    static const VideoEncoderSettings aom={"libaom-av1",{"0","1","2","3","4","5","6","7","8"},
        {},"6","-aom-params",30,63,{VideoRate::crf,VideoRate::cqp,VideoRate::bitrate,VideoRate::lossless}};
    static const VideoEncoderSettings svt={"libsvtav1",{"0","1","2","3","4","5","6","7","8","9","10","11","12","13"},
        {},"8","-svtav1-params",35,63,{VideoRate::crf,VideoRate::cqp,VideoRate::bitrate}};
    static const QStringList nvencPresets={"p1","p2","p3","p4","p5","p6","p7"};
    static const QStringList nvencTunes={"hq","ll","ull"};
    static const QList<VideoRate> nvencRates={VideoRate::cqp,VideoRate::bitrate};
    static const VideoEncoderSettings h264Nvenc={"h264_nvenc",nvencPresets,nvencTunes,"p4",{},23,51,nvencRates};
    static const VideoEncoderSettings hevcNvenc={"hevc_nvenc",nvencPresets,nvencTunes,"p4",{},23,51,nvencRates};
    static const VideoEncoderSettings av1Nvenc={"av1_nvenc",nvencPresets,nvencTunes,"p4",{},28,63,nvencRates};
    if(encoder==VideoEncoder::nvenc)return codec==VideoCodec::h264?h264Nvenc:codec==VideoCodec::hevc?hevcNvenc:av1Nvenc;
    if(codec==VideoCodec::h264)return x264;
    if(codec==VideoCodec::hevc)return x265;
    return encoder==VideoEncoder::svt?svt:aom;
}

bool supportsVideoContainer(VideoCodec codec, VideoContainer container) {
    return container!=VideoContainer::webm || codec==VideoCodec::av1;
}

bool supportsVideoAudio(VideoContainer container, VideoAudio audio) {
    if(audio==VideoAudio::aac)return container!=VideoContainer::webm;
    if(audio==VideoAudio::opus)return container==VideoContainer::webm || container==VideoContainer::mkv;
    return true;
}

bool supportsVideoDepth(VideoCodec codec, VideoEncoder encoder, int depth) {
    return (depth==12 && encoder==VideoEncoder::software && codec!=VideoCodec::h264) || depth==0 || depth==8 || (depth==10 && !(codec==VideoCodec::h264 && encoder==VideoEncoder::nvenc));
}

QString videoExtension(VideoContainer container) {
    switch(container) {
    case VideoContainer::mkv: return "mkv";
    case VideoContainer::mov: return "mov";
    case VideoContainer::webm: return "webm";
    default: return "mp4";
    }
}

QString videoQualityName(const VideoOutput& target) {
    switch(target.rate) {
    case VideoRate::cqp: return "qp"+QString::number(target.factor);
    case VideoRate::bitrate: return QString::number(target.bitrate)+"k";
    case VideoRate::lossless: return "lossless";
    default: return "crf"+QString::number(target.factor);
    }
}
QString videoCodecName(VideoCodec codec) {
    return codec==VideoCodec::h264?"h264":codec==VideoCodec::hevc?"hevc":"av1";
}
QString videoTransferName(VideoTransfer transfer) {
    return transfer==VideoTransfer::sdr?"sdr":transfer==VideoTransfer::pq?"pq":transfer==VideoTransfer::hlg?"hlg":"auto";
}

bool convertVideo(const QString& input,const std::filesystem::path& output,const Options& options,
                  const VideoOutput& target,const std::function<bool()>& cancelled,
                  const std::function<void(int)>& reportProgress) {
    validate(options);
    checkCancelled(cancelled);
    const auto ffmpeg=video::executable("ffmpeg");
    if(ffmpeg.isEmpty())throw std::runtime_error("Video conversion requires ffmpeg and ffprobe on PATH.");
    const auto& encoderSettings=videoEncoderSettings(target.codec,target.encoder);
    if(!supportsVideoContainer(target.codec,target.container)||!supportsVideoAudio(target.container,target.audio))
        throw std::invalid_argument("The selected codec or audio mode is incompatible with the container.");
    if(!encoderSettings.rates.contains(target.rate)||!supportsVideoDepth(target.codec,target.encoder,target.depth)||
       (target.encoder!=VideoEncoder::software&&target.chroma!=VideoChroma::yuv420))
        throw std::invalid_argument("The selected rate control or pixel format is incompatible with the encoder.");
    auto stream=video::probeVideo(input,cancelled);
    if(!stream.hdrError.isEmpty())throw std::runtime_error(stream.hdrError.toStdString());
    const bool h264=target.codec==VideoCodec::h264;
    const auto transfer=target.transfer;
    const bool hdrOutput=transfer==VideoTransfer::automatic?stream.hdr()&&!h264&&target.depth!=8:transfer!=VideoTransfer::sdr;
    if(hdrOutput&&!stream.hdr())throw std::runtime_error("PQ and HLG output require an HDR source. Use Auto or SDR for SDR videos.");
    if(hdrOutput&&h264)throw std::runtime_error("H.264 output uses SDR. Choose HEVC or AV1 for HDR.");
    if(hdrOutput&&target.depth==8)throw std::runtime_error("HDR output requires Auto, 10-bit, or 12-bit output depth.");
    const int outputTransfer=!hdrOutput?1:transfer==VideoTransfer::pq?16:transfer==VideoTransfer::hlg?18:stream.transfer;
    const bool gpuToneMap=target.gpuToneMapping&&!target.processPixels&&stream.hdr()&&!hdrOutput;
    const bool processed=!gpuToneMap&&(stream.dolbyVision||target.processPixels||
        (stream.hdr()&&stream.transfer!=outputTransfer));
    const bool converted=processed||gpuToneMap;
    const QString rgbFormat=QSysInfo::ByteOrder==QSysInfo::LittleEndian?"rgb48le":"rgb48be";
    const auto size=outputSize(stream.size,options.max_edge);
    const auto format=stream.pixelFormat.chopped(2);
    const bool deep=stream.bitDepth>8||format.endsWith("10")||format.endsWith("12")||
        format.endsWith("14")||format.endsWith("16")||format.endsWith("48")||format.endsWith("64");
    const auto pixFmt=encoderPixelFormat(deep,hdrOutput,target);
    const int budget=int(std::max(1u,options.threads));
    const int decodingThreads=processed?std::clamp(budget/3,1,4):budget;
    const int encodingThreads=processed?std::max(1,budget-decodingThreads):budget;
    const QString encodeThreads=QString::number(encodingThreads);
    const QString decodeThreads=QString::number(decodingThreads);
    const QString raster=QString("%1x%2").arg(size.width()).arg(size.height());
    const bool rgbSource=stream.matrixName=="gbr"||stream.pixelFormat.startsWith("rgb")||
        stream.pixelFormat.startsWith("bgr")||stream.pixelFormat.startsWith("gbr");
    const unsigned defaultPrimaries=stream.primaries?unsigned(stream.primaries):
        stream.size.height()<=480?6:stream.size.height()<=576?5:1;
    const QString defaultMatrix=defaultPrimaries==9?"bt2020nc":defaultPrimaries==5?"bt470bg":defaultPrimaries==6?"smpte170m":"bt709";
    const QString defaultPrims=defaultPrimaries==12?"smpte432":defaultMatrix=="bt2020nc"?"bt2020":defaultMatrix;
    const QString matrix=converted?(hdrOutput?"bt2020nc":"bt709"):
        tagged(stream.matrixName)&&!rgbSource?stream.matrixName:defaultMatrix;
    const QString primaries=converted?(hdrOutput?"bt2020":"bt709"):
        tagged(stream.primariesName)?stream.primariesName:defaultPrims;
    const QString trc=converted?(outputTransfer==16?"smpte2084":outputTransfer==18?"arib-std-b67":"bt709"):
        tagged(stream.transferName)?stream.transferName:"bt709";
    const QString range=!converted&&!rgbSource&&stream.rangeName=="pc"?"pc":"tv";
    const QString scale=QString("scale=%1:%2:out_color_matrix=%3:out_range=%4:flags=accurate_rnd+full_chroma_int")
        .arg(size.width()).arg(size.height()).arg(matrixFilter(matrix,defaultPrimaries),range);
    const QString sar=QString("setsar=sar=%1:max=10000").arg(stream.displayAspect*size.height()/size.width(),0,'f',9);
    QString filter=scale+(processed?":in_range=pc":QString{})+",format="+pixFmt+","+sar;
    if(gpuToneMap) {
        // Let libplacebo convert directly from the source YUV and preserve VFR timestamps.
        filter=QString("libplacebo=w=%1:h=%2:colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv:"
                       "tonemapping=bt.2390:peak_detect=1:apply_dolbyvision=1:apply_filmgrain=0,format=%3,%4")
            .arg(size.width()).arg(size.height()).arg(pixFmt,sar);
        if(!stream.dolbyVision&&!rgbSource&&!tagged(stream.matrixName))
            filter="setparams=colorspace="+defaultMatrix+","+filter;
    }
    QStringList encode={"-hide_banner","-loglevel","error","-nostdin","-xerror","-filter_threads",encodeThreads,
                        "-progress","pipe:1","-y"};
    if(processed)encode << "-thread_queue_size" << "2" << "-f" << "rawvideo" << "-pixel_format" << rgbFormat << "-video_size" << raster
                        << "-framerate" << stream.frameRate << "-i" << "pipe:0";
    const QString sourceIndex=processed?"1":"0";
    encode << "-threads" << decodeThreads << "-i" << input << "-map"
           << (processed?QString("0:v:0"):QString("0:%1").arg(stream.index))
           << "-map_metadata" << sourceIndex << "-map_chapters" << sourceIndex;
    encode << audioArguments(target,sourceIndex);
    const bool quickTime=target.container==VideoContainer::mp4||target.container==VideoContainer::mov;
    if(target.container==VideoContainer::mkv)encode << "-map" << sourceIndex+":s?" << "-c:s" << "copy";
    if(quickTime) {
        encode << "-movflags" << "+faststart";
        if(target.codec==VideoCodec::hevc)encode << "-tag:v" << "hvc1";
    }
    const QString light=!processed&&outputTransfer==16&&stream.maxContentLight>0?
        QString("%1,%2").arg(stream.maxContentLight).arg(stream.maxFrameAverageLight):QString{};
    encode << encoderArguments(target,encodingThreads,light)
           << "-vf" << filter << "-pix_fmt" << (target.depth==12?"+"+pixFmt:pixFmt) << "-threads:v" << encodeThreads
           << "-color_primaries" << primaries << "-color_trc" << trc << "-colorspace" << matrix << "-color_range" << range
           << "-fps_mode" << "passthrough";
    if(!processed&&!stream.timeBase.isEmpty())encode << "-enc_time_base:v" << stream.timeBase;

    Staging stage(output);
    encode << "-f" << (target.container==VideoContainer::mkv?QString("matroska"):videoExtension(target.container)) << QString::fromStdString(stage.file.u8string());
    const VideoColor destColor{hdrOutput?9u:1u,unsigned(outputTransfer)};
    VideoColor srcColor{};
    if(processed)srcColor={sourcePrimaries(stream),sourceTransfer(stream)};
    VideoProcess encoder(ffmpeg,encode);
    EncodeProgress progress(encoder,stream.duration,reportProgress);
    if(processed) {
        if(stream.inputMatrix.isEmpty()&&!rgbSource)stream.inputMatrix=matrixFilter(stream.matrixName,srcColor.primaries);
        const QString decodeFilter="fps=fps="+stream.frameRate+":start_time=0,"+video::frameFilter(stream,size,rgbFormat,false)
            .replace("flags=fast_bilinear","flags=accurate_rnd+full_chroma_int");
        const QStringList decode={"-hide_banner","-loglevel","error","-nostdin","-xerror","-filter_threads",decodeThreads,
                                  "-threads",decodeThreads,"-i",input,"-map",QString("0:%1").arg(stream.index),
                                  "-an","-vf",decodeFilter,"-c:v","rawvideo","-threads:v","1","-pix_fmt",rgbFormat,
                                  "-f","rawvideo"};
        streamFrames(ffmpeg,decode,encoder,progress,size,srcColor,destColor,options,cancelled);
    }
    while(encoder.process.state()!=QProcess::NotRunning) {
        checkCancelled(cancelled);
        encoder.process.waitForFinished(50);
        progress.poll();
    }
    checkCancelled(cancelled);
    encoder.checkExit(gpuToneMap?"GPU video conversion failed (requires FFmpeg with libplacebo and Vulkan)":"Video encode failed");
    if(std::filesystem::file_size(stage.file)==0)throw std::runtime_error("FFmpeg produced an empty output.");
    if(!options.overwrite&&std::filesystem::exists(output))return true;
    stage.publish(output,options.overwrite);
    return false;
}
QString animationExtension(const AnimationOutput& target) {
    if(target.video)return videoExtension(target.video->container);
    switch(target.format) {
    case AnimationFormat::gif:return "gif";
    case AnimationFormat::webp:return "webp";
    case AnimationFormat::png:return "png";
    case AnimationFormat::jxl:return "jxl";
    case AnimationFormat::avif:return "avif";
    }
    throw std::invalid_argument("Unknown animation format.");
}

QString animationSuffix(const AnimationOutput& target) {
    if(target.video) {
        const auto& video=*target.video;
        return "_animation_"+videoCodecName(video.codec)+"_"+videoQualityName(video)+"_"+
            videoTransferName(video.transfer)+"."+animationExtension(target);
    }
    QString range;
    switch(target.transfer) {
    case AnimationTransfer::sdr:break;
    case AnimationTransfer::pq:range="_pq";break;
    case AnimationTransfer::hlg:range="_hlg";break;
    case AnimationTransfer::linear:range="_linear";break;
    case AnimationTransfer::scrgb:range="_scrgb";break;
    }
    return "_animation"+range+"."+animationExtension(target);
}

namespace {
std::filesystem::path pathFromQt(const QString& value) {
    return std::filesystem::u8path(value.toUtf8().constData());
}
QSize animationSize(QSize size,unsigned edge) {
    if(edge&&unsigned(std::max(size.width(),size.height()))>edge) {
        size.scale(int(edge),int(edge),Qt::KeepAspectRatio);
        size=size.expandedTo({1,1});
    }
    return size;
}
void writeFixedGifPalette(const std::filesystem::path& path) {
    std::array<uint8_t,256*4> rgba{};
    size_t offset=0;
    const auto color=[&](unsigned red,unsigned green,unsigned blue) {
        rgba[offset++]=uint8_t(red);
        rgba[offset++]=uint8_t(green);
        rgba[offset++]=uint8_t(blue);
        rgba[offset++]=255;
    };
    for(unsigned red=0;red<=255;red+=51)
        for(unsigned green=0;green<=255;green+=51)
            for(unsigned blue=0;blue<=255;blue+=51)
                color(red,green,blue);
    for(unsigned i=1;i<40;++i) {
        const unsigned gray=i*255/40;
        color(gray,gray,gray);
    }
    // The final entry is transparent in both palette modes.
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(rgba.data()),rgba.size());
    file.close();
    if(!file)throw std::runtime_error("Cannot write the GIF color palette.");
}

struct AnimationSource {
    QString path;
    QSize size;
    VideoStream video;
    bool isVideo() const {return video.index>=0;}
};
}

bool convertAnimation(const std::filesystem::path& output,const Options& options,const AnimationOutput& target,
                      const std::function<bool()>& cancelled,const std::function<void(int)>& progress) {
    validate(options);
    checkCancelled(cancelled);
    const auto& placement=target.placement;
    if(target.inputs.isEmpty()||target.delayUs<1000||target.delayUs>600000000||
       placement.horizontal>2||placement.vertical>2)
        throw std::invalid_argument("Invalid animation frame timing or alignment.");
    const bool asVideo=target.video.has_value();
    const bool gif=!asVideo&&target.format==AnimationFormat::gif, webp=!asVideo&&target.format==AnimationFormat::webp;
    auto transfer=asVideo?AnimationTransfer::sdr:target.transfer;
    bool sdr=transfer==AnimationTransfer::sdr;
    if(asVideo) {
        const auto& video=*target.video;
        if(!supportsVideoContainer(video.codec,video.container)||
           !supportsVideoDepth(video.codec,video.encoder,video.depth)||
           !videoEncoderSettings(video.codec,video.encoder).rates.contains(video.rate)||
           (video.encoder!=VideoEncoder::software&&video.chroma!=VideoChroma::yuv420))
            throw std::invalid_argument("Incompatible video encoder, container or pixel format.");
        if((video.transfer==VideoTransfer::pq||video.transfer==VideoTransfer::hlg)&&
           (video.codec==VideoCodec::h264||video.depth==8))
            throw std::invalid_argument("HDR video requires 10- or 12-bit HEVC or AV1.");
    }
    if((gif||webp)&&!sdr)throw std::invalid_argument("GIF and WebP animation require SDR output.");
    if(gif&&target.delayUs<10000)throw std::invalid_argument("GIF frame delays must be at least 10 ms.");
    if((transfer==AnimationTransfer::linear||transfer==AnimationTransfer::scrgb)&&target.format!=AnimationFormat::jxl)
        throw std::invalid_argument("Linear animation output requires JPEG XL.");
    const auto exists=[&] {return std::filesystem::exists(std::filesystem::symlink_status(output));};
    if(!options.overwrite&&exists())return true;
    QString ffmpeg;
    QList<AnimationSource> sources;
    QSize canvas(0,0);
    unsigned sourceBits=8;
    bool wide=false;
    std::optional<AnimationTransfer> firstHdr;
    for(const auto& input:target.inputs) {
        checkCancelled(cancelled);
        const auto path=pathFromQt(input);
        std::error_code ec;
        if(std::filesystem::equivalent(output,path,ec))throw std::invalid_argument("Output would replace an input file.");
        AnimationSource source;
        source.path=input;
        if(supportedVideoFile(input)) {
            source.video=video::probeVideo(input,cancelled);
            if(!source.video.hdrError.isEmpty())throw std::runtime_error(source.video.hdrError.toStdString());
            source.size=source.video.size;
            const auto aspect=source.video.displayAspect;
            if(aspect>0&&std::isfinite(aspect)) {
                const double width=source.size.height()*aspect;
                if(width>std::numeric_limits<int>::max())throw std::runtime_error("Video display dimensions are too large.");
                source.size.setWidth(std::max(1,qRound(width)));
            }
            const auto& fmt=source.video.pixelFormat;
            const unsigned bits=source.video.bitDepth>0?unsigned(source.video.bitDepth):
                fmt.contains("12")?12:fmt.contains("10")||fmt.startsWith("p010")?10:fmt.contains("16")?16:8;
            sourceBits=std::max(sourceBits,bits);
            if(!firstHdr&&source.video.hdr())
                firstHdr=source.video.transfer==18?AnimationTransfer::hlg:AnimationTransfer::pq;
            wide|=source.video.primaries==9||source.video.primaries==12;
        } else {
            const auto info=inspect(path,cancelled);
            if(info.width>unsigned(std::numeric_limits<int>::max())||info.height>unsigned(std::numeric_limits<int>::max()))
                throw std::runtime_error("Image dimensions are too large for animation.");
            source.size={int(info.width),int(info.height)};
            if(!firstHdr&&(info.hdr||info.raw||info.transfer=="Linear")&&
               (!info.gainmap||options.gain_map_reconstruction==GainMapReconstruction::full_image))
                firstHdr=info.transfer=="HLG"?AnimationTransfer::hlg:AnimationTransfer::pq;
            sourceBits=std::max(sourceBits,info.gainmap&&options.gain_map_reconstruction==GainMapReconstruction::full_image?32u:info.bits);
            // JXR and linear sources may have extended sRGB colors even when
            // the source container names its primaries BT.709.
            wide|=info.hdr||info.raw||info.wide_gamut||info.transfer=="Linear";
        }
        source.size=animationSize(source.size,options.max_edge);
        if(source.size.isEmpty())throw std::runtime_error("Animation source dimensions are unavailable.");
        if(sources.isEmpty())canvas=source.size;
        else if(placement.resize==AnimationResize::fit_down||placement.resize==AnimationResize::fill_down)
            canvas=canvas.boundedTo(source.size);
        else canvas=canvas.expandedTo(source.size);
        sources.append(std::move(source));
    }
    if(gif||asVideo||std::any_of(sources.cbegin(),sources.cend(),[](const auto& source){return source.isVideo();})) {
        ffmpeg=video::executable("ffmpeg");
        if(ffmpeg.isEmpty())throw std::runtime_error("GIF and video conversion require FFmpeg on PATH.");
    }

    if(asVideo) {
        const auto& video=*target.video;
        switch(video.transfer) {
        case VideoTransfer::pq:transfer=AnimationTransfer::pq;break;
        case VideoTransfer::hlg:transfer=AnimationTransfer::hlg;break;
        case VideoTransfer::automatic:
            if(firstHdr&&video.codec!=VideoCodec::h264&&video.depth!=8)transfer=*firstHdr;
            break;
        case VideoTransfer::sdr:break;
        }
        sdr=transfer==AnimationTransfer::sdr;
        // Chroma-subsampled video uses even dimensions. Round the shared
        // canvas down where possible and fit any oversized frame within it.
        canvas={std::max(2,canvas.width()&~1),std::max(2,canvas.height()&~1)};
    }
    unsigned bits=!(gif||webp)&&(sourceBits>8||!sdr)?16:8;
    if(!asVideo&&target.format==AnimationFormat::png&&sdr&&!options.quality.auto_bit_depth)bits=unsigned(options.quality.png_depth);
    if(!asVideo&&target.format==AnimationFormat::avif) {
        const auto& q=options.quality;
        const unsigned cap=q.preset==Preset::custom?unsigned(q.avif_depth):
            q.preset==Preset::lossless||q.preset==Preset::high?12u:10u;
        bits=std::min(cap,!sdr?std::max(10u,sourceBits>10?12u:10u):sourceBits>10?12u:sourceBits>8?10u:8u);
        if(!sdr&&bits<10)throw std::invalid_argument("HDR AVIF animation requires a 10- or 12-bit AVIF maximum depth.");
    }
    const bool srgb=gif||options.srgb_clamp||transfer==AnimationTransfer::scrgb;
    const unsigned primaries=asVideo?(sdr?1u:9u):srgb?1u:!sdr||wide?9u:1u;
    const AnimationEncoding encoding{target.format,transfer,unsigned(canvas.width()),unsigned(canvas.height()),bits,primaries,target.delayUs,asVideo};
    Staging stage(output);
    std::unique_ptr<AnimationEncoder> native;
    std::unique_ptr<VideoProcess> ffmpegEncoder;
    const auto rate=QString("1000000/%1").arg(target.delayUs);
    const QString threads=QString::number(std::clamp(options.threads,1u,4u));
    const QString size=QString("%1x%2").arg(canvas.width()).arg(canvas.height());
    if(asVideo) {
        const auto& video=*target.video;
        const auto rgb=QSysInfo::ByteOrder==QSysInfo::LittleEndian?"rgba64le":"rgba64be";
        const auto pixFmt=encoderPixelFormat(sourceBits>8,!sdr,video);
        const auto matrix=sdr?"bt709":"bt2020";
        const auto trc=sdr?"bt709":transfer==AnimationTransfer::pq?"smpte2084":"arib-std-b67";
        const auto colorspace=sdr?"bt709":"bt2020nc";
        const auto filter=QString("scale=in_range=pc:out_range=tv:out_color_matrix=%1:flags=accurate_rnd+full_chroma_int,format=%2,"
                                  "setparams=range=limited:color_primaries=%1:color_trc=%3:colorspace=%4")
            .arg(matrix,pixFmt,trc,colorspace);
        const auto container=video.container==VideoContainer::mkv?QString("matroska"):videoExtension(video.container);
        QStringList args={"-hide_banner","-loglevel","error","-nostdin","-xerror","-y","-filter_threads",threads,
            "-f","rawvideo","-pixel_format",rgb,"-video_size",size,"-framerate",rate,"-i","pipe:0","-an"};
        args << encoderArguments(video,threads.toInt(),{}) << "-vf" << filter
             << "-pix_fmt" << (video.depth==12?"+"+pixFmt:pixFmt) << "-threads:v" << threads
             << "-color_primaries" << matrix << "-color_trc" << trc << "-colorspace" << colorspace
             << "-color_range" << "tv" << "-fps_mode" << "passthrough";
        // Some NVENC builds omit the HDR primaries and transfer from the HEVC SPS.
        if(!sdr&&video.codec==VideoCodec::hevc&&video.encoder==VideoEncoder::nvenc)
            args << "-bsf:v" << QString("hevc_metadata=colour_primaries=9:transfer_characteristics=%1:matrix_coefficients=9:video_full_range_flag=0")
                .arg(transfer==AnimationTransfer::pq?16:18);
        if(video.container==VideoContainer::mp4||video.container==VideoContainer::mov) {
            args << "-movflags" << "+faststart";
            if(video.codec==VideoCodec::hevc)args << "-tag:v" << "hvc1";
        }
        args << "-f" << container << QString::fromStdString(stage.file.u8string());
        ffmpegEncoder=std::make_unique<VideoProcess>(ffmpeg,args);
    } else if(gif) {
        if(canvas.width()>65535||canvas.height()>65535)throw std::runtime_error("GIF animation exceeds the codec dimensions.");
        QStringList args={"-hide_banner","-loglevel","error","-nostdin","-xerror","-y",
            "-filter_complex_threads",threads,"-f","rawvideo","-pixel_format","rgba","-video_size",size,"-framerate",rate,"-i","pipe:0"};
        if(target.gifIndexedColor) {
            args << "-filter_complex"
                 << "split[frames][palette];[palette]palettegen=stats_mode=single:reserve_transparent=1[p];[frames][p]paletteuse=new=1:alpha_threshold=128";
        } else {
            const auto palette=stage.directory/"palette.rgba";
            writeFixedGifPalette(palette);
            args << "-f" << "rawvideo" << "-pixel_format" << "rgba" << "-video_size" << "16x16"
                 << "-i" << QString::fromStdString(palette.u8string())
                 << "-filter_complex" << "[0:v][1:v]paletteuse=alpha_threshold=128";
        }
        args << "-gifflags" << "0"
             << "-loop" << "0" << "-final_delay" << QString::number(qRound(target.delayUs/10000.))
             << "-f" << "gif" << QString::fromStdString(stage.file.u8string());
        ffmpegEncoder=std::make_unique<VideoProcess>(ffmpeg,args);
    } else native=std::make_unique<AnimationEncoder>(stage.file,encoding,options);

    const auto errorMessage=asVideo?"Animation video encode failed":"GIF encode failed";
    std::vector<float> stack;
    bool transparentGif=false;
    auto renderOptions=options;
    if(placement.stack!=AnimationStack::off||placement.resize!=AnimationResize::off) {
        // Preserve transparency through rendering. Flatten the completed canvas if requested.
        if(renderOptions.transparency_background==TransparencyBackground::black_remove_alpha)
            renderOptions.transparency_background=TransparencyBackground::black;
        else if(renderOptions.transparency_background==TransparencyBackground::white_remove_alpha)
            renderOptions.transparency_background=TransparencyBackground::white;
    }
    const auto send=[&](const char* data,qsizetype bytes) {
        writeFrame(*ffmpegEncoder,data,bytes,cancelled,[&] {
            ffmpegEncoder->readErrors();
            if(ffmpegEncoder->process.state()==QProcess::NotRunning)ffmpegEncoder->fail(errorMessage);
        });
    };
    const auto sendVideo=[&](const std::vector<uint16_t>& rgba) {
        send(reinterpret_cast<const char*>(rgba.data()),qsizetype(rgba.size()*sizeof(uint16_t)));
    };
    const auto sendGif=[&](const std::vector<uint8_t>& rgba) {
        if(!transparentGif)for(size_t i=3;i<rgba.size();i+=4) {
            if(rgba[i]<128){transparentGif=true;break;}
        }
        send(reinterpret_cast<const char*>(rgba.data()),qsizetype(rgba.size()));
    };
    const auto addFrame=[&](AnimationFrame&& frame) {
        checkCancelled(cancelled);
        if(!frame.rgba8.empty()) {
            const auto rgba=place_animation_rgba8(std::move(frame),encoding,placement,options);
            if(native)native->add(rgba);
            else if(asVideo)sendVideo(animation_bt709_rgba16(rgba));
            else sendGif(rgba);
        } else {
            const auto pixels=compose_animation_frame(std::move(frame),encoding,placement,options,stack);
            if(native)native->add(pixels);
            else if(asVideo)sendVideo(animation_rgba16(pixels));
            else sendGif(animation_rgba8(pixels));
        }
    };

    int lastProgress=-1;
    for(qsizetype i=0;i<sources.size();++i) {
        checkCancelled(cancelled);
        const auto& source=sources[i];
        const auto update=[&](double fraction) {
            const int percent=std::min(99,int((i+std::clamp(fraction,0.,1.))*100./sources.size()));
            if(percent>lastProgress){lastProgress=percent;progress(percent);}
        };
        try {
            if(!source.isVideo()) {
                addFrame(AnimationImageRenderer::render(pathFromQt(source.path),renderOptions,encoding,placement,cancelled));
            } else {
                auto stream=source.video;
                const VideoColor color{sourcePrimaries(stream),sourceTransfer(stream)};
                const bool rgb=stream.matrixName=="gbr"||stream.pixelFormat.startsWith("rgb")||
                    stream.pixelFormat.startsWith("bgr")||stream.pixelFormat.startsWith("gbr");
                if(stream.inputMatrix.isEmpty()&&!rgb)stream.inputMatrix=matrixFilter(stream.matrixName,color.primaries);
                const QString rgbFormat=QSysInfo::ByteOrder==QSysInfo::LittleEndian?"rgba64le":"rgba64be";
                const QString sampling=target.slowMotion?QString{}:"fps=fps="+rate+":start_time=0:round=up,";
                const auto filter=sampling+video::frameFilter(stream,source.size,rgbFormat,false)
                    .replace("flags=fast_bilinear","flags=accurate_rnd+full_chroma_int");
                const QStringList args={"-hide_banner","-loglevel","error","-nostdin","-xerror","-filter_threads",threads,
                    "-threads",threads,"-i",source.path,"-map",QString("0:%1").arg(stream.index),"-an","-vf",filter,
                    "-c:v","rawvideo","-threads:v","1","-pix_fmt",rgbFormat,"-fps_mode","passthrough","-f","rawvideo"};
                uint64_t frames=0;
                const double step=target.slowMotion?(stream.fps>0?1./stream.fps:0):target.delayUs/1000000.;
                readFrames(ffmpeg,args,source.size,8,cancelled,[&](QByteArray& frame) {
                    addFrame(render_animation_video_frame(reinterpret_cast<const uint8_t*>(frame.constData()),
                        unsigned(source.size.width()),unsigned(source.size.height()),color,sdr,options));
                    ++frames;
                    if(stream.duration>0)update(frames*step/stream.duration);
                });
            }
        } catch(const std::exception& error) {
            throw std::runtime_error((QFileInfo(source.path).fileName()+": "+QString::fromUtf8(error.what())).toStdString());
        }
        update(1);
    }
    if(native)native->finish(cancelled);
    else {
        ffmpegEncoder->process.closeWriteChannel();
        while(ffmpegEncoder->process.state()!=QProcess::NotRunning) {
            checkCancelled(cancelled);
            ffmpegEncoder->process.waitForFinished(50);
            ffmpegEncoder->readErrors();
        }
        ffmpegEncoder->checkExit(errorMessage);
        if(transparentGif)clear_gif_canvas_between_frames(stage.file,cancelled);
    }
    checkCancelled(cancelled);
    if(std::filesystem::file_size(stage.file)==0)throw std::runtime_error("The animation encoder produced no output.");
    if(!options.overwrite&&exists())return true;
    stage.publish(output,options.overwrite);
    return false;
}

}
