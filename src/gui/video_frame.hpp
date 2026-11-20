#pragma once
#include <QImage>
#include <QString>
#include <functional>

class QWidget;

namespace jxrc::gui {
struct Record;
bool supportedVideoFile(const QString& path);
QString videoFilePatterns();
QImage readVideoThumbnail(const QString& path, int edge, const std::function<bool()>& cancelled, Record* metadata);
void grabVideoFrame(QWidget* parent, const QString& source, std::function<QString()> outputDirectory,
                    std::function<void(const QString&)> frameSaved);
}
