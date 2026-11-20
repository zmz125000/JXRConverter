#include "video_frame.hpp"
#include "jxrc/converter.hpp"
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
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

struct VideoStream {
    int index=-1;
    QSize size;
    double duration=0;
    double fps=0;
    int primaries=0;
    int transfer=0;
    bool dolbyVision=false;
    QString inputMatrix;

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
    if(!std::isfinite(stream.duration)||stream.duration<=0)throw std::runtime_error("The video has no seekable duration.");

    const auto sideData=video.value("side_data_list").toArray();
    for(const auto& item:sideData) {
        const auto data=item.toObject();
        if(data.value("dv_profile").toInt()!=5)continue;
        if(data.value("rpu_present_flag").toInt()!=1)
            throw std::runtime_error("This Dolby Vision Profile 5 stream has no RPU metadata.");
        stream.dolbyVision=true;
        stream.primaries=9;
        stream.transfer=16;
        return stream;
    }
    const auto transfer=video.value("color_transfer").toString();
    const auto primaries=video.value("color_primaries").toString();
    stream.transfer=transfer=="smpte2084"?16:transfer=="arib-std-b67"?18:0;
    stream.primaries=primaries=="bt2020"?9:primaries=="smpte432"?12:primaries=="bt709"?1:0;
    if(!stream.transfer||!stream.primaries)
        throw std::runtime_error("No supported HDR signal found. Use a tagged PQ or HLG video, or Dolby Vision Profile 5.");
    const auto matrix=video.value("color_space").toString();
    if(matrix=="bt2020c")
        throw std::runtime_error("BT.2020 constant-luminance video is unsupported for frame grabs.");
    if(matrix=="bt2020nc"||(matrix.isEmpty()&&stream.primaries==9))stream.inputMatrix="bt2020";
    return stream;
}

QString frameFilter(const VideoStream& stream,bool preview) {
    const QString fit=QString("w=%1:h=%2:force_original_aspect_ratio=decrease")
        .arg(previewSize.width()).arg(previewSize.height());
    if(stream.dolbyVision) {
        // Profile 5 needs the per-frame RPU reshaping before RGB samples can be tagged as PQ.
        return "showinfo=checksum=0,libplacebo="+(preview?fit+":":QString{})+
            "format=gbrp16le:colorspace=gbr:color_primaries=bt2020:color_trc=smpte2084:"
            "range=pc:apply_dolbyvision=true:apply_filmgrain=false,format=rgb48be";
    }
    QString scale=preview?fit:"w=iw:h=ih";
    if(!stream.inputMatrix.isEmpty())scale+=":in_color_matrix="+stream.inputMatrix;
    return "showinfo=checksum=0,scale="+scale+":out_range=pc:flags=accurate_rnd+full_chroma_int,format=rgb48be";
}

void tagFramePng(QByteArray& png,const VideoStream& stream) {
    static const QByteArray signature=QByteArray::fromHex("89504e470d0a1a0a");
    if(png.size()<33||!png.startsWith(signature)||png.mid(12,4)!="IHDR"||png[24]!=16||png[25]!=2)
        throw std::runtime_error("FFmpeg did not return a 16-bit RGB PNG frame.");
    // Some FFmpeg versions omit cICP. Replace it with the RGB metadata of the
    // filter output, and discard whole-video light levels from this still.
    for(qsizetype at=33;at+12<=png.size();) {
        const auto length=qFromBigEndian<quint32>(png.constData()+at);
        if(length>png.size()-at-12)throw std::runtime_error("FFmpeg returned a truncated PNG.");
        const auto type=png.mid(at+4,4);
        if(type=="cICP"||type=="cLLI")png.remove(at,qsizetype(length)+12);
        else at+=qsizetype(length)+12;
        if(type=="IEND") {
            QByteArray cicp(16,'\0');
            qToBigEndian<quint32>(4,cicp.data());
            cicp.replace(4,4,"cICP");
            cicp[8]=char(stream.primaries);
            cicp[9]=char(stream.transfer);
            cicp[11]=1; // Full-range RGB, with matrix coefficients 0 (identity).
            const auto crc=crc32(0,reinterpret_cast<const Bytef*>(cicp.constData()+4),8);
            qToBigEndian<quint32>(quint32(crc),cicp.data()+12);
            png.insert(33,cicp);
            return;
        }
    }
    throw std::runtime_error("FFmpeg returned an incomplete PNG frame.");
}

class VideoFrameDialog final : public QDialog {
public:
    VideoFrameDialog(QWidget* parent,QString source,QString ffmpeg,QString ffprobe)
        : QDialog(parent), source_(std::move(source)), ffmpeg_(std::move(ffmpeg)) {
        setWindowTitle("Grab video frame");
        setObjectName("videoFrameDialog");
        auto* layout=new QVBoxLayout(this);
        auto* name=new QLabel(QFileInfo(source_).fileName());
        name->setTextFormat(Qt::PlainText);
        name->setWordWrap(true);
        layout->addWidget(name);

        preview_=new QLabel;
        preview_->setObjectName("videoFramePreview");
        preview_->setAlignment(Qt::AlignCenter);
        preview_->setFixedSize(previewSize);
        layout->addWidget(preview_,0,Qt::AlignHCenter);

        seek_=new QSlider(Qt::Horizontal);
        seek_->setObjectName("videoFrameSeek");
        seek_->setAccessibleName("Video position");
        seek_->setEnabled(false);
        layout->addWidget(seek_);

        auto* timeRow=new QHBoxLayout;
        clock_=new QLabel;
        timeRow->addWidget(clock_,1);
        timeRow->addWidget(new QLabel("Seek to"));
        seconds_=new QDoubleSpinBox;
        seconds_->setObjectName("videoFrameTime");
        seconds_->setAccessibleName("Seek to seconds");
        seconds_->setDecimals(3);
        seconds_->setSuffix(" s");
        seconds_->setKeyboardTracking(false);
        seconds_->setEnabled(false);
        timeRow->addWidget(seconds_);
        layout->addLayout(timeRow);

        status_=new QLabel("Reading video information…");
        status_->setObjectName("videoFrameStatus");
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Cancel);
        grab_=buttons->addButton("Save and add frame…",QDialogButtonBox::ActionRole);
        grab_->setObjectName("saveVideoFrame");
        grab_->setAutoDefault(false);
        buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
        grab_->setEnabled(false);
        layout->addWidget(buttons);
        connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
        connect(grab_,&QPushButton::clicked,this,&VideoFrameDialog::saveFrame);

        timer_.setSingleShot(true);
        timer_.setInterval(130);
        connect(&timer_,&QTimer::timeout,this,[this] {
            if(operation_==Operation::idle)decodeFrame(Operation::preview);
            else timer_.start();
        });
        connect(seek_,&QSlider::valueChanged,this,[this](int value) {
            const QSignalBlocker blocker(seconds_);
            seconds_->setValue(value/1000.0);
            updateClock();
            invalidatePreview();
            if(!seek_->isSliderDown())timer_.start();
        });
        connect(seek_,&QSlider::sliderReleased,this,[this] {
            if(!previewReady_)timer_.start();
        });
        connect(seconds_,&QDoubleSpinBox::valueChanged,this,[this](double value) {
            seek_->setValue(qRound(value*1000));
        });
        connect(&process_,&QProcess::finished,this,&VideoFrameDialog::processFinished);
        connect(&process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
            if(error!=QProcess::FailedToStart)return;
            const auto operation=operation_;
            operation_=Operation::idle;
            errorMessage(operation,"Cannot start "+QFileInfo(process_.program()).fileName()+": "+process_.errorString());
        });
        operation_=Operation::probe;
        process_.start(ffprobe,{"-v","error","-select_streams","V:0","-show_streams","-show_format","-of","json","-i",source_});
    }

    ~VideoFrameDialog() override {
        process_.disconnect(this);
        if(process_.state()!=QProcess::NotRunning) {
            process_.kill();
            process_.waitForFinished(1000);
        }
    }

    QString savedPath() const { return saved_; }

private:
    enum class Operation { idle, probe, preview, grab };

    void updateClock() {
        clock_->setText(timecode(seek_->value())+" / "+timecode(durationMs_));
    }

    void invalidatePreview() {
        previewReady_=false;
        grab_->setEnabled(false);
        status_->setText("Seeking…");
        if(operation_==Operation::preview) {
            obsolete_=true;
            process_.kill();
        }
    }

    void decodeFrame(Operation operation) {
        operation_=operation;
        obsolete_=false;
        decodePosition_=seek_->value();
        const bool preview=operation==Operation::preview;
        status_->setText(preview?"Decoding preview…":"Decoding full-resolution frame…");
        process_.start(ffmpeg_,{
            "-hide_banner","-nostdin","-loglevel","info","-ss",QString::number(decodePosition_/1000.0,'f',3),
            "-copyts","-start_at_zero","-i",source_,"-map",QString("0:%1").arg(stream_.index),
            "-vf",frameFilter(stream_,preview),"-frames:v","1","-c:v","png","-threads:v","1",
            "-pix_fmt","rgb48be","-colorspace","rgb","-color_primaries",stream_.ffmpegPrimaries(),
            "-color_trc",stream_.ffmpegTransfer(),"-color_range","pc","-f","image2pipe","pipe:1"
        });
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
                durationMs_=int(std::min(stream_.duration*1000.,double(std::numeric_limits<int>::max())));
                // Accurate seeking returns the first frame at or after the seek point.
                const auto frameMs=stream_.fps>0?std::clamp(1000./stream_.fps,1.,double(durationMs_)+1):1;
                const int last=std::max(0,durationMs_-int(std::ceil(frameMs)));
                seek_->setRange(0,last);
                seek_->setSingleStep(int(std::lround(frameMs)));
                seek_->setPageStep(std::max(1000,durationMs_/100));
                seconds_->setRange(0,last/1000.0);
                seconds_->setSingleStep(seek_->singleStep()/1000.0);
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
                QSaveFile file(destination_);
                if(!file.open(QIODevice::WriteOnly)||file.write(output)!=output.size()||!file.commit())
                    throw std::runtime_error(("Cannot save frame: "+file.errorString()).toStdString());
                saved_=QFileInfo(destination_).canonicalFilePath();
                accept();
            }
        }catch(const std::exception& error) {
            errorMessage(operation,QString::fromUtf8(error.what()));
        }
    }

    void showPreview(const QByteArray& png,const QString& log) {
        const auto* first=reinterpret_cast<const uint8_t*>(png.constData());
        const auto image=jxrc::preview_bytes(std::vector<uint8_t>(first,first+png.size()),previewSize.width());
        if(image.rgba.empty())throw std::runtime_error("Cannot display this frame.");
        const QImage view(image.rgba.data(),int(image.width),int(image.height),int(image.width)*4,QImage::Format_RGBA8888);
        preview_->setPixmap(QPixmap::fromImage(view));
        const static QRegularExpression pts(R"(\bn:\s*0\s+pts:\s*\S+\s+pts_time:(\S+))");
        bool valid=false;
        const auto frameTime=pts.match(log).captured(1).toDouble(&valid);
        framePosition_=valid&&std::isfinite(frameTime)&&frameTime>=0&&frameTime<=std::numeric_limits<int>::max()/1000.0
            ?qRound(frameTime*1000):decodePosition_;
        const auto transfer=stream_.dolbyVision?"Dolby Vision → PQ":stream_.transfer==16?"PQ":"HLG";
        status_->setText(QString("Frame %1 · %2 × %3 · %4")
            .arg(timecode(framePosition_)).arg(stream_.size.width()).arg(stream_.size.height()).arg(transfer));
        previewReady_=true;
        grab_->setEnabled(true);
    }

    void setSeekEnabled(bool enabled) {
        seek_->setEnabled(enabled);
        seconds_->setEnabled(enabled);
    }

    void errorMessage(Operation operation,const QString& message) {
        status_->setText(message);
        if(operation==Operation::grab) {
            setSeekEnabled(true);
            grab_->setEnabled(previewReady_);
        }
    }

    void saveFrame() {
        if(operation_!=Operation::idle||!previewReady_)return;
        const QFileInfo source(source_);
        auto stamp=timecode(framePosition_);
        stamp.replace(':','-');
        const auto stem=source.absoluteDir().filePath(source.completeBaseName()+"_"+stamp);
        auto proposed=stem+".png";
        for(int suffix=2;QFileInfo::exists(proposed);++suffix)proposed=stem+QString("_%1.png").arg(suffix);
        QFileDialog dialog(this,"Save HDR video frame",proposed,"PNG image (*.png)");
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix("png");
        if(dialog.exec()!=QDialog::Accepted)return;
        const auto path=dialog.selectedFiles().value(0);
        if(QFileInfo(path).suffix().compare("png",Qt::CaseInsensitive)!=0) {
            status_->setText("Choose a filename ending in .png.");
            return;
        }
        destination_=path;
        setSeekEnabled(false);
        grab_->setEnabled(false);
        decodeFrame(Operation::grab);
    }

    const QString source_, ffmpeg_;
    QString destination_, saved_;
    VideoStream stream_;
    QProcess process_;
    QTimer timer_;
    QLabel *preview_, *clock_, *status_;
    QSlider* seek_;
    QDoubleSpinBox* seconds_;
    QPushButton* grab_;
    Operation operation_=Operation::idle;
    int durationMs_=0, decodePosition_=0, framePosition_=0;
    bool obsolete_=false, previewReady_=false;
};
}

QString grabVideoFrame(QWidget* parent) {
    const auto ffmpeg=executable("ffmpeg"),ffprobe=executable("ffprobe");
    if(ffmpeg.isEmpty()||ffprobe.isEmpty()) {
        QMessageBox::information(parent,"Grab video frame","Install FFmpeg and make both ffmpeg and ffprobe available on PATH.");
        return {};
    }
    const auto source=QFileDialog::getOpenFileName(parent,"Open HDR video",{},
        "Video files (*.mp4 *.mkv *.mov *.m4v *.webm *.ts *.m2ts *.mts *.avi *.hevc *.h265 *.av1);;All files (*)");
    if(source.isEmpty())return {};
    VideoFrameDialog dialog(parent,source,ffmpeg,ffprobe);
    return dialog.exec()==QDialog::Accepted?dialog.savedPath():QString{};
}
}
