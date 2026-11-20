#pragma once
#include <QString>

class QWidget;

namespace jxrc::gui {
// Returns the canonical path of the saved HDR frame, or an empty path on cancel.
QString grabVideoFrame(QWidget* parent);
}
