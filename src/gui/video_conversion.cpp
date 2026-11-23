#include "video_conversion.hpp"
#include "video_source.hpp"
#include "../native/staged_output.h"
#include "../native/video_color.h"
#include <QByteArray>
#include <QProcess>
#include <QSysInfo>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace jxrc::gui {
namespace {
using video::VideoStream;

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

QString encoderPixelFormat(const VideoStream& stream,bool hdr,VideoCodec codec) {
    const auto format=stream.pixelFormat.chopped(2);
    const bool deep=stream.bitDepth>8||format.endsWith("10")||format.endsWith("12")||
        format.endsWith("14")||format.endsWith("16")||format.endsWith("48")||format.endsWith("64");
    return codec!=VideoCodec::h264&&(hdr||deep)?"yuv420p10le":"yuv420p";
}

QStringList qualityArguments(VideoCodec codec,Preset preset,int custom,int threads,const QString& contentLight) {
    QStringList args;
    if(codec==VideoCodec::h264)args << "-c:v" << "libx264" << "-preset" << "medium";
    else if(codec==VideoCodec::hevc)args << "-c:v" << "libx265" << "-preset" << "medium" << "-tag:v" << "hvc1";
    else args << "-c:v" << "libaom-av1" << "-cpu-used" << "6" << "-row-mt" << "1" << "-b:v" << "0";
    if(preset==Preset::lossless) {
        if(codec==VideoCodec::h264)args << "-qp" << "0";
        if(codec==VideoCodec::av1)args << "-aom-params" << "lossless=1";
    } else {
        const int crf=preset==Preset::custom?custom:preset==Preset::high?18:preset==Preset::medium?23:29;
        args << "-crf" << QString::number(codec==VideoCodec::av1&&preset!=Preset::custom?crf+6:crf);
    }
    if(codec==VideoCodec::hevc) {
        QStringList params={"pools="+QString::number(threads),"log-level=error"};
        if(preset==Preset::lossless)params << "lossless=1";
        if(!contentLight.isEmpty())params << "max-cll="+contentLight;
        args << "-x265-params" << params.join(':');
    }
    return args;
}

void checkCancelled(const std::function<bool()>& cancelled) {
    if(cancelled())throw std::runtime_error("Video conversion cancelled");
}

class VideoProcess {
public:
    VideoProcess(const QString& program,const QStringList& arguments) {
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

void streamFrames(const QString& ffmpeg,const QStringList& args,VideoProcess& encoder,EncodeProgress& progress,
                  const QSize& size,VideoColor source,VideoColor output,const Options& options,
                  const std::function<bool()>& cancelled) {
    const auto pixels=quint64(size.width())*quint64(size.height());
    if(pixels>quint64(std::numeric_limits<qsizetype>::max())/8)
        throw std::runtime_error("Video frame dimensions exceed addressable memory.");
    QByteArray frame(qsizetype(pixels*8),Qt::Uninitialized);
    std::vector<float> working;
    VideoProcess decoder(ffmpeg,args);
    qsizetype available=0;
    bool wroteFrame=false;
    while(true) {
        checkCancelled(cancelled);
        progress.poll();
        if(encoder.process.state()==QProcess::NotRunning)encoder.fail("Video encode failed");
        decoder.readErrors();
        const auto read=decoder.process.read(frame.data()+available,frame.size()-available);
        if(read>0)available+=read;
        if(available==frame.size()) {
            process_video_pixels(reinterpret_cast<uint8_t*>(frame.data()),size_t(pixels),source,output,options,working);
            checkCancelled(cancelled);
            if(encoder.process.write(frame)!=frame.size())encoder.fail("Cannot send frame to FFmpeg");
            while(encoder.process.bytesToWrite()>0) {
                checkCancelled(cancelled);
                encoder.process.waitForBytesWritten(50);
                progress.poll();
                if(encoder.process.state()==QProcess::NotRunning)encoder.fail("Video encode failed");
            }
            wroteFrame=true;
            available=0;
        } else if(decoder.process.state()==QProcess::NotRunning) {
            if(decoder.process.bytesAvailable()>0)continue;
            decoder.checkExit("Video decode failed");
            if(available||!wroteFrame)throw std::runtime_error("FFmpeg did not return complete video frames.");
            break;
        } else {
            decoder.process.waitForReadyRead(50);
        }
    }
    encoder.process.closeWriteChannel();
}
}

QString videoExtension(VideoCodec codec) {return codec==VideoCodec::av1?"mkv":"mp4";}
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
    auto stream=video::probeVideo(input,cancelled);
    if(!stream.hdrError.isEmpty())throw std::runtime_error(stream.hdrError.toStdString());
    const bool av1=target.codec==VideoCodec::av1,h264=target.codec==VideoCodec::h264;
    const auto transfer=target.transfer;
    const bool hdrOutput=transfer==VideoTransfer::automatic?stream.hdr()&&!h264:transfer!=VideoTransfer::sdr;
    if(hdrOutput&&!stream.hdr())throw std::runtime_error("PQ and HLG output require an HDR source. Use Auto or SDR for SDR videos.");
    if(hdrOutput&&h264)throw std::runtime_error("H.264 output uses SDR. Choose HEVC or AV1 for HDR.");
    const int outputTransfer=!hdrOutput?1:transfer==VideoTransfer::pq?16:transfer==VideoTransfer::hlg?18:stream.transfer;
    const bool processed=stream.dolbyVision||target.process_pixels||
        (stream.hdr()&&stream.transfer!=outputTransfer);
    const QString rgbFormat=QSysInfo::ByteOrder==QSysInfo::LittleEndian?"rgba64le":"rgba64be";
    const auto size=outputSize(stream.size,options.max_edge);
    const auto pixFmt=encoderPixelFormat(stream,hdrOutput,target.codec);
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
    const QString matrix=processed?(hdrOutput?"bt2020nc":"bt709"):
        tagged(stream.matrixName)&&!rgbSource?stream.matrixName:defaultMatrix;
    const QString primaries=processed?(hdrOutput?"bt2020":"bt709"):
        tagged(stream.primariesName)?stream.primariesName:defaultPrims;
    const QString trc=processed?(outputTransfer==16?"smpte2084":outputTransfer==18?"arib-std-b67":"bt709"):
        tagged(stream.transferName)?stream.transferName:"bt709";
    const QString range=!processed&&!rgbSource&&stream.rangeName=="pc"?"pc":"tv";
    const QString scale=QString("scale=%1:%2:out_color_matrix=%3:out_range=%4:flags=accurate_rnd+full_chroma_int")
        .arg(size.width()).arg(size.height()).arg(matrixFilter(matrix,defaultPrimaries),range);
    const QString sar=QString("setsar=sar=%1:max=10000").arg(stream.displayAspect*size.height()/size.width(),0,'f',9);
    const QString filter=scale+(processed?":in_range=pc":QString{})+",format="+pixFmt+","+sar;
    QStringList encode={"-hide_banner","-loglevel","error","-nostdin","-xerror","-filter_threads",encodeThreads,
                        "-progress","pipe:1","-y"};
    if(processed)encode << "-f" << "rawvideo" << "-pixel_format" << rgbFormat << "-video_size" << raster
                        << "-framerate" << stream.frameRate << "-i" << "pipe:0";
    const QString sourceIndex=processed?"1":"0";
    encode << "-threads" << decodeThreads << "-i" << input << "-map"
           << (processed?QString("0:v:0"):QString("0:%1").arg(stream.index))
           << "-map" << sourceIndex+":a?" << "-map_metadata" << sourceIndex << "-map_chapters" << sourceIndex;
    if(av1)encode << "-map" << sourceIndex+":s?" << "-c:s" << "copy" << "-c:a" << "copy";
    else encode << "-c:a" << "aac" << "-b:a" << "192k" << "-movflags" << "+faststart";
    const QString light=!processed&&outputTransfer==16&&stream.maxContentLight>0?
        QString("%1,%2").arg(stream.maxContentLight).arg(stream.maxFrameAverageLight):QString{};
    encode << qualityArguments(target.codec,options.quality.preset,target.crf,encodingThreads,light)
           << "-vf" << filter << "-pix_fmt" << pixFmt << "-threads:v" << encodeThreads
           << "-color_primaries" << primaries << "-color_trc" << trc << "-colorspace" << matrix << "-color_range" << range
           << "-fps_mode" << "passthrough";
    if(!processed)encode << "-enc_time_base:v" << "-1";

    Staging stage(output);
    encode << "-f" << (av1?"matroska":"mp4") << QString::fromStdString(stage.file.u8string());
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
                                  "-f","rawvideo","pipe:1"};
        streamFrames(ffmpeg,decode,encoder,progress,size,srcColor,destColor,options,cancelled);
    }
    while(encoder.process.state()!=QProcess::NotRunning) {
        checkCancelled(cancelled);
        encoder.process.waitForFinished(50);
        progress.poll();
    }
    checkCancelled(cancelled);
    encoder.checkExit("Video encode failed");
    if(std::filesystem::file_size(stage.file)==0)throw std::runtime_error("FFmpeg produced an empty output.");
    if(!options.overwrite&&std::filesystem::exists(output))return true;
    stage.publish(output,options.overwrite);
    return false;
}
}
