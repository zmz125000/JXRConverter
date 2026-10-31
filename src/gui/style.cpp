#include "style.hpp"
#include <QApplication>
#include <QStyleFactory>
#include <QPalette>
namespace jxrc::gui {
void applyDesktopStyle() {
    auto& app=*qApp;
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    app.setStyleSheet(R"(
        QMainWindow, QTabWidget::pane { background: #f5f7f6; }
        QToolBar { spacing: 8px; padding: 8px; border-bottom: 1px solid #dbe2de; }
        QGroupBox { font-weight: 600; border: 1px solid #d6dfd9; border-radius: 6px; margin-top: 10px; padding: 12px 8px 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
        QPushButton { padding: 7px 10px; border: 1px solid #cad6ce; border-radius: 5px; background: #ffffff; }
        QPushButton:hover { background: #e7f0eb; }
        QPushButton:disabled { color: #9ca8a1; background: #eff2f0; }
        QPushButton[primary="true"] { background: #2e8069; color: white; font-weight: 600; }
        QPushButton[primary="true"]:disabled { background: #a3c0b5; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateEdit { padding: 5px; border: 1px solid #cbd7cf; border-radius: 4px; background: white; color: #22372c; }
        QWidget:disabled { color: #9ca8a1; }
        QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QDateEdit:disabled { background: #eff2f0; color: #9ca8a1; border-color: #dce4de; }
        QLineEdit:focus, QComboBox:focus { border: 1px solid #2e8069; }
        QListView, QTableView, QPlainTextEdit { background: #fafcfb; color: #22372c; border: 1px solid #d6dfd9; border-radius: 5px; selection-background-color: #c7e2d5; selection-color: #153c2d; }
        QProgressBar { border: 1px solid #d6dfd9; border-radius: 4px; text-align: center; min-height: 17px; }
        QProgressBar::chunk { background: #72ad93; }
    )");
    QPalette palette=app.palette();palette.setColor(QPalette::WindowText,QColor("#22372c"));palette.setColor(QPalette::Text,QColor("#22372c"));palette.setColor(QPalette::Base,QColor("#fafcfb"));palette.setColor(QPalette::Window,QColor("#f5f7f6"));palette.setColor(QPalette::Highlight,QColor("#b4d8c6"));palette.setColor(QPalette::HighlightedText,QColor("#153c2d"));app.setPalette(palette);
}
}
