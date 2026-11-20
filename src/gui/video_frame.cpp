#include "video_frame.hpp"
#include "catalog.hpp"
#include "jxrc/converter.hpp"
#include <QColorSpace>
#include <QColorTransform>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QQueue>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QUrl>
#include <QtEndian>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace jxrc::gui {
namespace {
const QSize previewSize(640, 368);
const QSize playbackPreviewSize(480, 276);
const QString timestampFilter="settb=AVTB,showinfo=checksum=0";
const QStringList videoExtensions={"mp4","mkv","mov","m4v","webm","ts","m2ts","mts","avi","hevc","h265",
    "h264","av1","mpg","mpeg","vob","wmv","asf","flv","3gp","3g2","ogv","mxf","m2v","mpv","dv"};

QString executable(const QString& name) {
    auto paths=qEnvironmentVariable("PATH").split(QDir::listSeparator(),Qt::SkipEmptyParts);
    paths.prepend(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    paths << "/opt/homebrew/bin" << "/usr/local/bin";
#endif
    return QStandardPaths::findExecutable(name,paths);
}

QString timecode(qint64 milliseconds) {
    return QString("%1:%2:%3.%4")
        .arg(milliseconds/3600000,2,10,QChar('0'))
        .arg(milliseconds/60000%60,2,10,QChar('0'))
        .arg(milliseconds/1000%60,2,10,QChar('0'))
        .arg(milliseconds%1000,3,10,QChar('0'));
}

const QRegularExpression& timecodePattern() {
    static const QRegularExpression pattern(R"(^(\d{1,3}):([0-5]\d):([0-5]\d)(?:\.(\d{1,3}))?$)");
    return pattern;
}

QStringList probeArguments(const QString& source) {
    return {"-v","error","-select_streams","V:0","-show_streams","-show_format","-of","json","-i",source};
}

struct VideoStream {
    int index=-1;
    QSize size;
    double displayAspect=0;
    double duration=0;
    double fps=0;
    int primaries=0;
    int transfer=0;
    bool dolbyVision=false;
    QString hdrError, primariesName, transferName;
    QString inputMatrix;

    bool hdr() const { return transfer!=0; }
    QString ffmpegPrimaries() const {
        return primaries==9?"bt2020":primaries==12?"smpte432":"bt709";
    }
    QString ffmpegTransfer() const { return transfer==16?"smpte2084":"arib-std-b67"; }
};

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
    if(stream.index<0||stream.size.isEmpty())throw std::runtime_error("Video dimensions are unavailable.");
    stream.transferName=video.value("color_transfer").toString();
    stream.primariesName=video.value("color_primaries").toString();
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
    const auto matrix=video.value("color_space").toString();
    if(stream.hdr()&&!stream.primaries)
        stream.hdrError="This HDR signal has unsupported or missing color primaries.";
    else if(matrix=="bt2020c")
        stream.hdrError="BT.2020 constant-luminance video is unsupported for frame grabs.";
    if(matrix=="bt2020nc"||(matrix.isEmpty()&&stream.primaries==9))stream.inputMatrix="bt2020";
    else if(matrix=="smpte170m"||matrix=="bt470bg")stream.inputMatrix="bt601";
    else if(matrix=="bt709"||matrix=="smpte240m"||matrix=="fcc")stream.inputMatrix=matrix;
    return stream;
}

QColorSpace sdrColorSpace(const VideoStream& stream) {
    using Transfer=QColorSpace::TransferFunction;
    QColorSpace color(QColorSpace::SRgb);
    const auto& primaries=stream.primariesName;
    const QPointF d65(.3127,.3290);
    if(primaries=="smpte432")color.setPrimaries(QColorSpace::Primaries::DciP3D65);
    else if(primaries=="bt2020")color.setPrimaries(d65,{.708,.292},{.170,.797},{.131,.046});
    else if(primaries=="smpte170m"||primaries=="smpte240m")color.setPrimaries(d65,{.630,.340},{.310,.595},{.155,.070});
    else if(primaries=="bt470bg")color.setPrimaries(d65,{.640,.330},{.290,.600},{.150,.060});

    const auto& transfer=stream.transferName;
    if(transfer=="linear")color.setTransferFunction(Transfer::Linear);
    else if(transfer=="gamma22"||transfer=="gamma28")
        color.setTransferFunction(Transfer::Gamma,transfer=="gamma22"?2.2f:2.8f);
    else if(transfer=="bt709"||transfer=="smpte170m"||transfer=="bt2020-10"||transfer=="bt2020-12") {
        // ICC curves describe encoded values going to linear light. Tabulate
        // the inverse video transfer using the continuous BT.709 coefficients.
        static const auto curve=[] {
            QList<uint16_t> table;
            constexpr int steps=4096;
            constexpr double alpha=1.09929682680944, beta=0.018053968510807;
            table.reserve(steps+1);
            for(int i=0;i<=steps;++i) {
                const double code=double(i)/steps;
                const double linear=code<4.5*beta?code/4.5:std::pow((code+alpha-1.)/alpha,1./.45);
                table.append(uint16_t(std::lround(linear*65535)));
            }
            return table;
        }();
        color.setTransferFunction(curve);
    }
    return color;
}

QString frameFilter(const VideoStream& stream,const QSize& preview,const QString& rgbFormat="rgb48be",bool fit=true) {
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

QStringList frameArguments(const QString& source,const VideoStream& stream,int position,const QSize& preview,
                           bool thumbnail=false,bool keyframe=false) {
    QStringList args={"-hide_banner","-nostdin","-loglevel",thumbnail?"error":"info"};
    if(thumbnail)args << "-threads" << "1" << "-filter_threads" << "1";
    if(position||!thumbnail)args << "-ss" << QString::number(position/1000.0,'f',3);
    if(keyframe)args << "-noaccurate_seek" << "-skip_frame" << "nokey";
    if(!thumbnail)args << "-copyts" << "-start_at_zero";
    args << "-i" << source << "-map" << QString("0:%1").arg(stream.index);
    if(stream.hdrError.isEmpty()) {
        args << "-vf" << (thumbnail?frameFilter(stream,preview):timestampFilter+","+frameFilter(stream,preview)) << "-pix_fmt" << "rgb48be"
             << "-colorspace" << "rgb" << "-color_range" << "pc";
        if(stream.hdr())args << "-color_primaries" << stream.ffmpegPrimaries() << "-color_trc" << stream.ffmpegTransfer();
    } else {
        args << "-vf" << QString("scale=w=%1:h=%2:force_original_aspect_ratio=decrease").arg(preview.width()).arg(preview.height())
             << "-pix_fmt" << "rgb24";
    }
    args << "-frames:v" << "1" << "-c:v" << "png" << "-threads:v" << "1" << "-f" << "image2pipe" << "pipe:1";
    return args;
}

QStringList playbackArguments(const QString& source,const VideoStream& stream,int position,const QSize& size) {
    // Preserve the selected frames' timestamps so Pause can seek to the
    // actual last displayed frame, including in variable-rate clips.
    const auto filter="select='isnan(prev_selected_t)+gte(t-prev_selected_t,1/30-0.000001)',"+
        frameFilter(stream,size,"rgb24",false)+",realtime,"+timestampFilter;
    return {"-hide_banner","-nostdin","-nostats","-loglevel","info","-filter_threads","1",
        "-ss",QString::number(position/1000.0,'f',3),"-copyts","-start_at_zero","-i",source,
        "-map",QString("0:%1").arg(stream.index),"-vf",filter,"-c:v","rawvideo","-threads:v","1",
        "-pix_fmt","rgb24","-fps_mode","passthrough","-flush_packets","1","-f","rawvideo","pipe:1"};
}

QColorTransform playbackTransform(const VideoStream& stream) {
    auto space=sdrColorSpace(stream);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if(stream.hdr()) {
        using Transfer=QColorSpace::TransferFunction;
        space.setTransferFunction(stream.transfer==16?Transfer::St2084:Transfer::Hlg);
        if(stream.dolbyVision)space.setPrimaries(QColorSpace::Primaries::Bt2020);
    }
#endif
    return space.transformationToColorSpace(QColorSpace::SRgb);
}

qint64 frameTimestampUs(const QString& log) {
    static const QRegularExpression pts(R"(\bn:\s*\d+\s+pts:\s*(-?\d+)\s+pts_time:)");
    bool valid=false;
    const auto microseconds=pts.match(log).captured(1).toLongLong(&valid);
    return valid&&microseconds>=0&&microseconds<=qint64(std::numeric_limits<int>::max())*1000?microseconds:-1;
}

void tagFramePng(QByteArray& png,const VideoStream& stream) {
    static const QByteArray signature=QByteArray::fromHex("89504e470d0a1a0a");
    if(png.size()<33||!png.startsWith(signature)||png.mid(12,4)!="IHDR"||png[24]!=16||png[25]!=2)
        throw std::runtime_error("FFmpeg did not return a 16-bit RGB PNG frame.");
    // Some FFmpeg versions omit cICP. Use the RGB encoding of the filter
    // output. SDR uses an ICC curve when the video transfer differs from sRGB.
    for(qsizetype at=33;at+12<=png.size();) {
        const auto length=qFromBigEndian<quint32>(png.constData()+at);
        if(length>png.size()-at-12)throw std::runtime_error("FFmpeg returned a truncated PNG.");
        const auto type=png.mid(at+4,4);
        if(type=="cICP"||type=="cLLI"||(!stream.hdr()&&(type=="iCCP"||type=="sRGB"||type=="gAMA"||type=="cHRM"||type=="mDCV")))
            png.remove(at,qsizetype(length)+12);
        else at+=qsizetype(length)+12;
        if(type=="IEND") {
            QByteArray colorChunk(4,'\0');
            if(stream.hdr()) {
                colorChunk+="cICP";
                colorChunk+=char(stream.primaries);
                colorChunk+=char(stream.transfer);
                colorChunk+='\0'; // RGB identity matrix, full range.
                colorChunk+='\1';
            } else {
                const auto color=sdrColorSpace(stream);
                if(color==QColorSpace(QColorSpace::SRgb)) {
                    colorChunk+="sRGB";
                    colorChunk+='\1'; // Relative colorimetric intent.
                } else {
                    const auto profile=color.iccProfile();
                    if(profile.isEmpty())throw std::runtime_error("Cannot write the SDR frame color profile.");
                    colorChunk+="iCCP";
                    colorChunk+=QByteArray("Video SDR\0\0",11);
                    colorChunk+=qCompress(profile).mid(4); // Drop Qt's uncompressed-length prefix.
                }
            }
            qToBigEndian<quint32>(quint32(colorChunk.size()-8),colorChunk.data());
            const auto crc=crc32(0,reinterpret_cast<const Bytef*>(colorChunk.constData()+4),uInt(colorChunk.size()-4));
            colorChunk.resize(colorChunk.size()+4);
            qToBigEndian<quint32>(quint32(crc),colorChunk.data()+colorChunk.size()-4);
            png.insert(33,colorChunk);
            return;
        }
    }
    throw std::runtime_error("FFmpeg returned an incomplete PNG frame.");
}

QImage framePreview(const QByteArray& png,const VideoStream& stream,int edge) {
    if(!stream.hdr()) {
        auto image=QImage::fromData(png,"PNG");
        if(image.isNull())throw std::runtime_error("Cannot display this frame.");
        image.setColorSpace(sdrColorSpace(stream));
        image.convertToColorSpace(QColorSpace::SRgb);
        return image;
    }
    const auto* first=reinterpret_cast<const uint8_t*>(png.constData());
    const auto image=jxrc::preview_bytes(std::vector<uint8_t>(first,first+png.size()),edge);
    if(image.rgba.empty())throw std::runtime_error("Cannot display this frame.");
    return QImage(image.rgba.data(),int(image.width),int(image.height),int(image.width)*4,QImage::Format_RGBA8888).copy();
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

Record videoMetadata(const VideoStream& stream) {
    Record metadata;
    metadata.video=true;
    metadata.width=stream.size.width();
    metadata.height=stream.size.height();
    metadata.displayAspect=stream.displayAspect;
    metadata.hdr=stream.hdr();
    metadata.transfer=stream.transfer==16?"PQ":stream.transfer==18?"HLG":stream.transferName;
    metadata.primaries=stream.dolbyVision?"BT.2020":stream.primariesName;
    metadata.inspected=true;
    return metadata;
}

class FramePreview final : public QWidget {
public:
    FramePreview() {
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        setMinimumSize(160,90);
    }
    QSize sizeHint() const override { return previewSize; }
    void setImage(QImage image,double aspect) {
        image_=std::move(image);
        aspect_=aspect;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(),palette().brush(QPalette::Base));
        if(image_.isNull())return;
        const auto fit=QSizeF(aspect_,1).scaled(QSizeF(size()),Qt::KeepAspectRatio);
        const QRectF target((width()-fit.width())/2,(height()-fit.height())/2,fit.width(),fit.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target,image_);
    }
private:
    QImage image_;
    double aspect_=1;
};

class VideoFrameDialog final : public QDialog {
public:
    VideoFrameDialog(QWidget* parent,QString source,QString ffmpeg,QString ffprobe,
                     std::function<QString()> outputDirectory,std::function<QString(const QString&,bool,bool)> frameSaved)
        : QDialog(parent), source_(std::move(source)), ffmpeg_(std::move(ffmpeg)),
          outputDirectory_(std::move(outputDirectory)), frameSaved_(std::move(frameSaved)) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle("Grab video frame - "+QFileInfo(source_).fileName());
        setObjectName("videoFrameDialog");
        auto* layout=new QVBoxLayout(this);

        preview_=new FramePreview;
        preview_->setObjectName("videoFramePreview");
        layout->addWidget(preview_,1);

        seek_=new QSlider(Qt::Horizontal);
        seek_->setObjectName("videoFrameSeek");
        seek_->setAccessibleName("Video position");
        seek_->setEnabled(false);
        layout->addWidget(seek_);

        auto* playbackRow=new QHBoxLayout;
        playbackRow->addStretch();
        for(const int delta:{-60,-5,-1,0,1,5,60}) {
            auto* button=new QPushButton(delta?QString("%1%2s").arg(delta>0?"+":"").arg(delta):"Play");
            button->setAutoDefault(false);
            button->setEnabled(false);
            playbackRow->addWidget(button);
            playbackButtons_.append(button);
            if(delta) {
                button->setObjectName("videoFrameJump"+QString::number(delta));
                connect(button,&QPushButton::clicked,this,[this,delta] {
                    const qint64 position=qint64(seek_->value())+delta*1000;
                    seek_->setValue(int(std::clamp(position,qint64(0),qint64(seek_->maximum()))));
                });
            } else {
                play_=button;
                play_->setObjectName("videoFramePlay");
                connect(play_,&QPushButton::clicked,this,&VideoFrameDialog::togglePlayback);
            }
        }
        playbackRow->addStretch();
        layout->addLayout(playbackRow);

        auto* timeRow=new QHBoxLayout;
        clock_=new QLabel;
        timeRow->addWidget(clock_,1);
        timeRow->addWidget(new QLabel("Seek to"));
        time_=new QLineEdit;
        time_->setObjectName("videoFrameTime");
        time_->setAccessibleName("Seek to HH:MM:SS.ms");
        time_->setPlaceholderText("HH:MM:SS.ms");
        time_->setText(timecode(0));
        time_->setValidator(new QRegularExpressionValidator(timecodePattern(),time_));
        time_->setEnabled(false);
        timeRow->addWidget(time_);
        layout->addLayout(timeRow);

        status_=new QLabel("Reading video information…");
        status_->setObjectName("videoFrameStatus");
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close);
        auto* folder=buttons->addButton(QString{},QDialogButtonBox::ActionRole);
        folder->setObjectName("openVideoFramesFolder");
        folder->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
        folder->setToolTip("Open video_frames folder");
        folder->setAccessibleName(folder->toolTip());
        folder->ensurePolished();
        folder->setFixedWidth(folder->sizeHint().height());
        grab_=buttons->addButton("Save and add frame",QDialogButtonBox::ActionRole);
        grab_->setObjectName("saveVideoFrame");
        saveAs_=buttons->addButton("Save frame as…",QDialogButtonBox::ActionRole);
        saveAs_->setObjectName("saveVideoFrameAs");
        convert_=buttons->addButton("Convert frame",QDialogButtonBox::ActionRole);
        convert_->setObjectName("convertVideoFrame");
        for(auto* button:buttons->buttons())static_cast<QPushButton*>(button)->setAutoDefault(false);
        setSaveEnabled(false);
        layout->addWidget(buttons);
        connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
        connect(folder,&QPushButton::clicked,this,&VideoFrameDialog::openFolder);
        connect(grab_,&QPushButton::clicked,this,[this]{
            const auto directory=framesDirectory();
            if(!directory.isEmpty())startGrab(directory,FrameAction::save);
        });
        connect(saveAs_,&QPushButton::clicked,this,&VideoFrameDialog::saveFrameAs);
        connect(convert_,&QPushButton::clicked,this,&VideoFrameDialog::convertCurrentFrame);

        timer_.setSingleShot(true);
        timer_.setInterval(130);
        connect(&timer_,&QTimer::timeout,this,[this] {
            if(operation_==Operation::idle)decodeFrame(Operation::preview);
            else timer_.start();
        });
        connect(seek_,&QSlider::valueChanged,this,[this](int value) {
            time_->setText(timecode(value));
            updateClock();
            lastSavedPath_.clear();
            invalidatePreview();
            if(playing_)restartPlayback();
            else if(!seek_->isSliderDown())timer_.start();
        });
        connect(seek_,&QSlider::sliderPressed,this,[this]{if(playing_)stopPlaybackProcess();});
        connect(seek_,&QSlider::sliderReleased,this,[this] {
            if(playing_)restartPlayback();
            else if(!previewReady_)timer_.start();
        });
        connect(time_,&QLineEdit::editingFinished,this,[this] {
            const auto match=timecodePattern().match(time_->text());
            if(!match.hasMatch())return;
            const qint64 requested=match.captured(1).toLongLong()*3600000+match.captured(2).toInt()*60000
                +match.captured(3).toInt()*1000+match.captured(4).leftJustified(3,'0').toInt();
            seek_->setValue(int(std::clamp(requested,qint64(0),qint64(seek_->maximum()))));
            time_->setText(timecode(seek_->value()));
        });
        connect(&process_,&QProcess::finished,this,&VideoFrameDialog::processFinished);
        connect(&process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
            if(error!=QProcess::FailedToStart)return;
            const auto operation=operation_;
            operation_=Operation::idle;
            errorMessage(operation,"Cannot start "+QFileInfo(process_.program()).fileName()+": "+process_.errorString());
        });
        connect(&playback_,&QProcess::readyReadStandardOutput,this,&VideoFrameDialog::readPlayback);
        connect(&playback_,&QProcess::readyReadStandardError,this,&VideoFrameDialog::readPlayback);
        connect(&playback_,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status) {
            finishPlaybackProcess(code==0&&status==QProcess::NormalExit);
        });
        connect(&playback_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
            if(error==QProcess::FailedToStart)finishPlaybackProcess(false);
        });
        operation_=Operation::probe;
        process_.start(ffprobe,probeArguments(source_));
    }

    ~VideoFrameDialog() override {
        for(auto* process:{&playback_,&process_}) {
            process->disconnect(this);
            if(process->state()!=QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(1000);
            }
        }
    }

private:
    enum class Operation { idle, probe, preview, grab };
    enum class FrameAction { save, saveAs, convert };

    void updateClock() {
        clock_->setText(timecode(seek_->value())+" / "+timecode(durationMs_));
    }

    void invalidatePreview() {
        previewReady_=false;
        setSaveEnabled(false);
        if(!playing_)status_->setText("Seeking…");
        if(operation_==Operation::preview) {
            obsolete_=true;
            process_.kill();
        }
    }

    void togglePlayback() {
        if(playing_) {
            pausePlayback();
            return;
        }
        playing_=true;
        playbackFailed_=false;
        play_->setText("Pause");
        timer_.stop();
        invalidatePreview();
        restartPlayback();
    }

    void stopPlaybackProcess() {
        playbackObsolete_=true;
        if(playback_.state()!=QProcess::NotRunning)playback_.kill();
    }

    void restartPlayback() {
        if(playback_.state()!=QProcess::NotRunning) {
            stopPlaybackProcess();
            return;
        }
        if(seek_->isSliderDown())return;
        playbackObsolete_=false;
        playbackBytes_.clear();
        playbackLog_.clear();
        playbackTimes_.clear();
        status_->setText("Playing…");
        playback_.start(ffmpeg_,playbackArguments(source_,stream_,seek_->value(),playbackSize_));
    }

    void pausePlayback() {
        playing_=false;
        play_->setText("Play");
        stopPlaybackProcess();
        if(operation_==Operation::idle)decodeFrame(Operation::preview);
        else timer_.start();
    }

    void finishPlaybackProcess(bool succeeded) {
        readPlayback();
        if(!playing_)return;
        if(playbackObsolete_) {
            restartPlayback();
            return;
        }
        playbackFailed_=!succeeded;
        pausePlayback();
    }

    void readPlayback() {
        const auto bytes=playback_.readAllStandardOutput();
        const auto log=playback_.readAllStandardError();
        if(!playing_||playbackObsolete_)return;
        playbackBytes_+=bytes;
        playbackLog_+=log;
        qsizetype start=0;
        for(qsizetype end;(end=playbackLog_.indexOf('\n',start))>=0;start=end+1) {
            const auto line=QString::fromLatin1(playbackLog_.constData()+start,end-start);
            const auto microseconds=frameTimestampUs(line);
            if(microseconds>=0) {
                // Seek at or just before the frame PTS. Rounding up can miss
                // the displayed frame when its timestamp falls between milliseconds.
                playbackTimes_.enqueue(int(microseconds/1000));
            }
        }
        if(start)playbackLog_.remove(0,start);

        const qsizetype stride=3*playbackSize_.width();
        const qsizetype frameBytes=stride*playbackSize_.height();
        const auto frames=std::min(playbackTimes_.size(),playbackBytes_.size()/frameBytes);
        if(frames) {
            // If more than one frame arrived in this event, display the newest.
            for(qsizetype i=1;i<frames;++i)playbackTimes_.dequeue();
            const int position=std::clamp(playbackTimes_.dequeue(),0,seek_->maximum());
            const auto* pixels=reinterpret_cast<const uchar*>(playbackBytes_.constData()+(frames-1)*frameBytes);
            auto image=QImage(pixels,playbackSize_.width(),playbackSize_.height(),stride,QImage::Format_RGB888).copy();
            playbackBytes_.remove(0,frames*frameBytes);
            if(!playbackColor_.isIdentity())image.applyColorTransform(playbackColor_);
            preview_->setImage(std::move(image),stream_.displayAspect);
            if(position!=seek_->value()) {
                const QSignalBlocker blocker(seek_);
                seek_->setValue(position);
                if(!time_->isModified())time_->setText(timecode(position));
                updateClock();
                lastSavedPath_.clear();
            }
        }
        // The timestamp pipe can lead the pixel pipe because each message
        // is much smaller than a raw frame. Bound both buffers independently.
        if(playbackTimes_.size()>60||playbackBytes_.size()>12*frameBytes) {
            playbackFailed_=true;
            pausePlayback();
        }
    }

    void decodeFrame(Operation operation) {
        operation_=operation;
        obsolete_=false;
        decodePosition_=seek_->value();
        const bool preview=operation==Operation::preview;
        status_->setText(preview?"Decoding preview…":"Decoding full-resolution frame…");
        process_.start(ffmpeg_,frameArguments(source_,stream_,decodePosition_,preview?previewSize:QSize{}));
    }

    void processFinished(int code,QProcess::ExitStatus exit) {
        const auto operation=operation_;
        operation_=Operation::idle;
        auto output=process_.readAllStandardOutput();
        const auto log=QString::fromLocal8Bit(process_.readAllStandardError());
        if(obsolete_)return;
        if(code!=0||exit!=QProcess::NormalExit) {
            auto message=log.trimmed().section('\n',-5);
            if(message.isEmpty())message="The decoder exited without returning a frame.";
            if(operation!=Operation::probe&&stream_.dolbyVision)
                message.prepend("Profile 5 frame grabs need an FFmpeg build with libplacebo and a working Vulkan device.\n");
            errorMessage(operation,message);
            return;
        }
        try {
            if(operation==Operation::probe) {
                stream_=readStream(output);
                if(!stream_.hdrError.isEmpty())throw std::runtime_error(stream_.hdrError.toStdString());
                if(!std::isfinite(stream_.duration)||stream_.duration<=0)throw std::runtime_error("The video has no seekable duration.");
                const auto fit=stream_.size.scaled(playbackPreviewSize,Qt::KeepAspectRatio);
                playbackSize_={std::max(1,fit.width()),std::max(1,fit.height())};
                playbackColor_=playbackTransform(stream_);
                durationMs_=int(std::min(stream_.duration*1000.,double(std::numeric_limits<int>::max())));
                // Accurate seeking returns the first frame at or after the seek point.
                const auto frameMs=stream_.fps>0?std::clamp(1000./stream_.fps,1.,double(durationMs_)+1):1;
                const int last=std::max(0,durationMs_-int(std::ceil(frameMs)));
                seek_->setRange(0,last);
                seek_->setSingleStep(int(std::lround(frameMs)));
                seek_->setPageStep(std::max(1000,durationMs_/100));
                setSeekEnabled(true);
                updateClock();
                decodeFrame(Operation::preview);
                return;
            }
            if(output.isEmpty())throw std::runtime_error("No frame at this position. Try seeking a little earlier.");
            tagFramePng(output,stream_);
            if(operation==Operation::preview) {
                showPreview(output,log);
            } else {
                const auto path=frameAction_==FrameAction::saveAs?destination_:availableFilename(QDir(destination_));
                QSaveFile file(path);
                if(!file.open(QIODevice::WriteOnly)||file.write(output)!=output.size()||!file.commit())
                    throw std::runtime_error(("Cannot save frame: "+file.errorString()).toStdString());
                const QFileInfo saved(path);
                lastSavedPath_=saved.canonicalFilePath();
                if(lastSavedPath_.isEmpty())lastSavedPath_=saved.absoluteFilePath();
                publishFrame(lastSavedPath_,frameAction_==FrameAction::convert);
                setSeekEnabled(true);
                setSaveEnabled(true);
            }
        }catch(const std::exception& error) {
            errorMessage(operation,QString::fromUtf8(error.what()));
        }
    }

    void showPreview(const QByteArray& png,const QString& log) {
        preview_->setImage(framePreview(png,stream_,previewSize.width()),stream_.displayAspect);
        const auto frameTime=frameTimestampUs(log);
        framePosition_=frameTime>=0?int((frameTime+500)/1000):decodePosition_;
        const auto transfer=stream_.dolbyVision?"Dolby Vision → PQ":stream_.transfer==16?"PQ":stream_.transfer==18?"HLG":"SDR";
        auto text=QString("Frame %1 · %2 × %3 · %4")
            .arg(timecode(framePosition_)).arg(stream_.size.width()).arg(stream_.size.height()).arg(transfer);
        if(playbackFailed_)text+=" · Playback unavailable";
        status_->setText(text);
        previewReady_=true;
        setSaveEnabled(true);
    }

    void setSeekEnabled(bool enabled) {
        seek_->setEnabled(enabled);
        time_->setEnabled(enabled);
        for(auto* button:playbackButtons_)button->setEnabled(enabled);
    }

    void setSaveEnabled(bool enabled) {
        grab_->setEnabled(enabled);
        saveAs_->setEnabled(enabled);
        convert_->setEnabled(enabled);
    }

    void errorMessage(Operation operation,const QString& message) {
        status_->setText(message);
        if(operation==Operation::grab) {
            setSeekEnabled(true);
            setSaveEnabled(previewReady_);
        }
    }

    QString availableFilename(const QDir& directory) const {
        const QFileInfo source(source_);
        auto stamp=timecode(framePosition_);
        stamp.replace(':','-');
        const auto stem=directory.filePath(source.completeBaseName()+"_"+stamp);
        auto proposed=stem+".png";
        for(int suffix=2;QFileInfo::exists(proposed);++suffix)proposed=stem+QString("_%1.png").arg(suffix);
        return proposed;
    }

    QString framesDirectory() {
        const auto root=outputDirectory_();
        if(!root.isEmpty()) {
            const auto path=QDir(root).absoluteFilePath("video_frames");
            if(QDir().mkpath(path))return path;
        }
        status_->setText("Choose a writable output folder for video_frames.");
        return {};
    }

    void openFolder() {
        const auto path=framesDirectory();
        if(!path.isEmpty()&&!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            status_->setText("The system file manager could not open the video_frames folder.");
    }

    void saveFrameAs() {
        if(operation_!=Operation::idle||!previewReady_)return;
        auto* dialog=new QFileDialog(this,"Save video frame",availableFilename(QFileInfo(source_).absoluteDir()),"PNG image (*.png)");
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        dialog->setDefaultSuffix("png");
        connect(dialog,&QFileDialog::fileSelected,this,[this](const QString& path) {
            if(QFileInfo(path).suffix().compare("png",Qt::CaseInsensitive)==0)startGrab(path,FrameAction::saveAs);
            else status_->setText("Choose a filename ending in .png.");
        });
        connect(dialog,&QDialog::finished,this,[this] {
            if(operation_==Operation::idle) {
                setSeekEnabled(true);
                setSaveEnabled(previewReady_);
            }
        });
        setSeekEnabled(false);
        setSaveEnabled(false);
        dialog->show();
    }

    void publishFrame(const QString& path,bool convert) {
        const auto message=frameSaved_(path,convert,stream_.hdr());
        status_->setText(message.isEmpty()?"Saved "+QFileInfo(path).fileName():message);
    }

    void convertCurrentFrame() {
        if(operation_!=Operation::idle||!previewReady_)return;
        // Only reuse a file written by this dialog since the last seek.
        if(!lastSavedPath_.isEmpty()&&QFileInfo(lastSavedPath_).isFile()) {
            publishFrame(lastSavedPath_,true);
            return;
        }
        const auto directory=framesDirectory();
        if(!directory.isEmpty())startGrab(directory,FrameAction::convert);
    }

    void startGrab(const QString& destination,FrameAction action) {
        if(operation_!=Operation::idle||!previewReady_)return;
        destination_=destination;
        frameAction_=action;
        setSeekEnabled(false);
        setSaveEnabled(false);
        decodeFrame(Operation::grab);
    }

    const QString source_, ffmpeg_;
    const std::function<QString()> outputDirectory_;
    const std::function<QString(const QString&,bool,bool)> frameSaved_;
    QString destination_, lastSavedPath_;
    VideoStream stream_;
    QProcess process_, playback_;
    QTimer timer_;
    QSize playbackSize_;
    QColorTransform playbackColor_;
    QByteArray playbackBytes_, playbackLog_;
    QQueue<int> playbackTimes_;
    FramePreview* preview_;
    QLabel *clock_, *status_;
    QSlider* seek_;
    QLineEdit* time_;
    QList<QPushButton*> playbackButtons_;
    QPushButton *grab_, *saveAs_, *convert_, *play_=nullptr;
    Operation operation_=Operation::idle;
    FrameAction frameAction_=FrameAction::save;
    int durationMs_=0, decodePosition_=0, framePosition_=0;
    bool obsolete_=false, previewReady_=false;
    bool playing_=false, playbackObsolete_=false, playbackFailed_=false;
};
}

bool supportedVideoFile(const QString& path) {
    return videoExtensions.contains(QFileInfo(path).suffix().toLower());
}

QString videoFilePatterns() {
    return "*."+videoExtensions.join(" *.");
}

Record readVideoMetadata(const QString& path,const std::function<bool()>& cancelled) {
    return videoMetadata(probeVideo(path,cancelled));
}

QImage readVideoThumbnail(const QString& path,int edge,const std::function<bool()>& cancelled,Record* metadata) {
    const auto stream=probeVideo(path,cancelled);
    if(metadata)*metadata=videoMetadata(stream);
    const auto ffmpeg=executable("ffmpeg");
    if(ffmpeg.isEmpty())throw std::runtime_error("Video thumbnails require ffmpeg.");
    const QSize size(edge,edge);
    const int position=std::isfinite(stream.duration)&&stream.duration>0
        ?int(std::min(stream.duration*150.,double(std::numeric_limits<int>::max()))):0;
    QByteArray png;
    if(position>0) {
        try {png=runVideoProcess(ffmpeg,frameArguments(path,stream,position,size,true,true),cancelled);}
        catch(const std::exception&) {if(cancelled&&cancelled())throw;}
    }
    if(png.isEmpty())png=runVideoProcess(ffmpeg,frameArguments(path,stream,0,size,true),cancelled);
    if(stream.hdrError.isEmpty()) {
        tagFramePng(png,stream);
        return framePreview(png,stream,edge);
    }
    auto image=QImage::fromData(png,"PNG");
    if(image.isNull())throw std::runtime_error("FFmpeg did not return a video thumbnail.");
    return image;
}

void grabVideoFrame(QWidget* parent,const QString& source,std::function<QString()> outputDirectory,
                    std::function<QString(const QString&,bool,bool)> frameSaved) {
    const auto ffmpeg=executable("ffmpeg"),ffprobe=executable("ffprobe");
    if(ffmpeg.isEmpty()||ffprobe.isEmpty()) {
        auto* message=new QMessageBox(QMessageBox::Information,"Grab video frame",
            "Install FFmpeg and make both ffmpeg and ffprobe available on PATH.",QMessageBox::Ok,parent);
        message->setAttribute(Qt::WA_DeleteOnClose);
        message->show();
        return;
    }
    auto* dialog=new VideoFrameDialog(parent,source,ffmpeg,ffprobe,std::move(outputDirectory),std::move(frameSaved));
    dialog->show();
}
}
