#include "video_source.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <cmath>
#include <stdexcept>

namespace jxrc::gui::video {
QString executable(const QString& name) {
    auto paths=qEnvironmentVariable("PATH").split(QDir::listSeparator(),Qt::SkipEmptyParts);
    paths.prepend(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    paths << "/opt/homebrew/bin" << "/usr/local/bin";
#endif
    return QStandardPaths::findExecutable(name,paths);
}

QStringList probeArguments(const QString& source) {
    return {"-v","error","-select_streams","V:0","-show_streams","-show_format","-of","json","-i",source};
}

VideoStream readStream(const QByteArray& json) {
    const auto root=QJsonDocument::fromJson(json).object();
    const auto streams=root.value("streams").toArray();
    if(streams.isEmpty())throw std::runtime_error("No video stream found.");
    const auto video=streams.first().toObject();
    VideoStream stream;
    stream.index=video.value("index").toInt(-1);
    stream.size={video.value("width").toInt(),video.value("height").toInt()};
    stream.duration=video.value("duration").toString().toDouble();
    if(!(stream.duration>0))stream.duration=root.value("format").toObject().value("duration").toString().toDouble();
    const auto rate=video.value("avg_frame_rate").toString().split('/');
    if(rate.size()==2&&rate[1].toDouble()>0)stream.fps=rate[0].toDouble()/rate[1].toDouble();
    stream.frameRate=video.value("r_frame_rate").toString();
    const auto nominalRate=stream.frameRate.split('/');
    if(nominalRate.size()!=2||nominalRate[1].toInt()<=0||nominalRate[0].toInt()<=0||
       nominalRate[0].toDouble()/nominalRate[1].toDouble()>240)
        stream.frameRate=stream.fps>0?rate.join('/'):QString("30");
    if(stream.index<0||stream.size.isEmpty())throw std::runtime_error("Video dimensions are unavailable.");
    stream.transferName=video.value("color_transfer").toString();
    stream.primariesName=video.value("color_primaries").toString();
    stream.matrixName=video.value("color_space").toString();
    stream.rangeName=video.value("color_range").toString();
    stream.pixelFormat=video.value("pix_fmt").toString();
    stream.bitDepth=video.value("bits_per_raw_sample").toString().toInt();
    const auto sideData=video.value("side_data_list").toArray();
    const auto sampleAspect=video.value("sample_aspect_ratio").toString().split(':');
    stream.displayAspect=double(stream.size.width())/stream.size.height();
    if(sampleAspect.size()==2&&sampleAspect[0].toInt()>0&&sampleAspect[1].toInt()>0)
        stream.displayAspect*=double(sampleAspect[0].toInt())/sampleAspect[1].toInt();
    for(const auto& item:sideData) {
        if(std::abs(item.toObject().value("rotation").toInt()%180)==90) {
            stream.size.transpose();
            stream.displayAspect=1./stream.displayAspect;
            break;
        }
    }
    for(const auto& item:sideData) {
        const auto data=item.toObject();
        if(data.value("side_data_type").toString()=="Content light level metadata") {
            stream.maxContentLight=data.value("max_content").toInt();
            stream.maxFrameAverageLight=data.value("max_average").toInt();
        }
        if(data.value("dv_profile").toInt()!=5)continue;
        if(data.value("rpu_present_flag").toInt()!=1)
            stream.hdrError="This Dolby Vision Profile 5 stream has no RPU metadata.";
        stream.dolbyVision=true;
        stream.primaries=9;
        stream.transfer=16;
        return stream;
    }
    const auto& transfer=stream.transferName;
    const auto& primaries=stream.primariesName;
    stream.transfer=transfer=="smpte2084"?16:transfer=="arib-std-b67"?18:0;
    stream.primaries=primaries=="bt2020"?9:primaries=="smpte432"?12:primaries=="bt709"?1:0;
    const auto& matrix=stream.matrixName;
    if(stream.hdr()&&!stream.primaries)
        stream.hdrError="This HDR signal has unsupported or missing color primaries.";
    else if(matrix=="bt2020c")
        stream.hdrError="BT.2020 constant-luminance video is unsupported.";
    if(matrix=="bt2020nc"||(matrix.isEmpty()&&stream.primaries==9))stream.inputMatrix="bt2020";
    else if(matrix=="smpte170m"||matrix=="bt470bg")stream.inputMatrix="bt601";
    else if(matrix=="bt709"||matrix=="smpte240m"||matrix=="fcc")stream.inputMatrix=matrix;
    return stream;
}

QString frameFilter(const VideoStream& stream,const QSize& preview,const QString& rgbFormat,bool fit) {
    QString scale=preview.isEmpty()?"w=iw:h=ih":QString("w=%1:h=%2").arg(preview.width()).arg(preview.height());
    if(!preview.isEmpty()&&fit)scale+=":force_original_aspect_ratio=decrease";
    if(stream.dolbyVision) {
        // Profile 5 needs the per-frame RPU reshaping before RGB samples can be tagged as PQ.
        return "libplacebo="+(!preview.isEmpty()?scale+":":QString{})+
            "format=gbrp16le:colorspace=gbr:color_primaries=bt2020:color_trc=smpte2084:"
            "range=pc:apply_dolbyvision=true:apply_filmgrain=false,format="+rgbFormat;
    }
    if(!stream.inputMatrix.isEmpty())scale+=":in_color_matrix="+stream.inputMatrix;
    const QString flags=fit?"accurate_rnd+full_chroma_int":"fast_bilinear";
    return "scale="+scale+":out_range=pc:flags="+flags+",format="+rgbFormat;
}

QByteArray runVideoProcess(const QString& program,const QStringList& args,const std::function<bool()>& cancelled) {
    if(cancelled&&cancelled())throw std::runtime_error("Video read cancelled.");
    QProcess process;
    process.start(program,args);
    QElapsedTimer timer;
    timer.start();
    while(process.state()!=QProcess::NotRunning) {
        process.waitForFinished(50);
        if(process.state()==QProcess::NotRunning)break;
        if((cancelled&&cancelled())||timer.elapsed()>=15000) {
            process.kill();
            process.waitForFinished(1000);
            throw std::runtime_error("Video read timed out or was cancelled.");
        }
    }
    if(process.error()==QProcess::FailedToStart||process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0) {
        auto error=QString::fromLocal8Bit(process.readAllStandardError()).trimmed().section('\n',-3);
        if(error.isEmpty())error=process.errorString();
        throw std::runtime_error(error.toStdString());
    }
    return process.readAllStandardOutput();
}

VideoStream probeVideo(const QString& path,const std::function<bool()>& cancelled) {
    const auto ffprobe=executable("ffprobe");
    if(ffprobe.isEmpty())throw std::runtime_error("Video metadata requires ffprobe.");
    return readStream(runVideoProcess(ffprobe,probeArguments(path),cancelled));
}
}
