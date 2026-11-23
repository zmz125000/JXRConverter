#pragma once
#include <QSize>
#include <QStringList>
#include <functional>

namespace jxrc::gui::video {
struct VideoStream {
    int index=-1;
    QSize size;
    double displayAspect=0;
    double duration=0;
    double fps=0;
    QString frameRate;
    int primaries=0;
    int transfer=0;
    bool dolbyVision=false;
    QString hdrError, primariesName, transferName;
    QString inputMatrix, matrixName, rangeName, pixelFormat;
    int bitDepth=0;
    int maxContentLight=0, maxFrameAverageLight=0;

    bool hdr() const { return transfer!=0; }
    QString ffmpegPrimaries() const {
        return primaries==9?"bt2020":primaries==12?"smpte432":"bt709";
    }
    QString ffmpegTransfer() const { return transfer==16?"smpte2084":"arib-std-b67"; }
};

QString executable(const QString& name);
QStringList probeArguments(const QString& source);
VideoStream readStream(const QByteArray& json);
QString frameFilter(const VideoStream&,const QSize&,const QString& rgbFormat="rgb48be",bool fit=true);
QByteArray runVideoProcess(const QString&,const QStringList&,const std::function<bool()>& cancelled);
VideoStream probeVideo(const QString&,const std::function<bool()>& cancelled);
}
