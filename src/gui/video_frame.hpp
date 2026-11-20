#pragma once
#include <QImage>
#include <QString>
#include <functional>

class QWidget;

namespace jxrc::gui {
struct Record;
bool supportedVideoFile(const QString& path);
QString videoFilePatterns();
Record readVideoMetadata(const QString& path, const std::function<bool()>& cancelled);
QImage readVideoThumbnail(const QString& path, int edge, const std::function<bool()>& cancelled, Record* metadata);
void grabVideoFrame(QWidget* parent, const QString& source, std::function<QString()> outputDirectory,
                    std::function<QString(const QString&, bool convert, bool hdr)> frameSaved);
}
