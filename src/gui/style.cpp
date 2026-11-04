#include "style.hpp"
#include <QApplication>
#include <QStyleFactory>
#include <QStyle>
#include <QStyleHints>
#include <QPalette>
#include <QScopedValueRollback>
namespace jxrc::gui {
void applyDesktopStyle(int theme) {
    static bool applying=false;
    if(applying)return;
    QScopedValueRollback<bool> guard(applying,true);
    auto& app=*qApp;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Ask the native platform to style window decorations as well as widgets.
    app.styleHints()->setColorScheme(theme==0?Qt::ColorScheme::Unknown:theme==2?Qt::ColorScheme::Dark:Qt::ColorScheme::Light);
#endif
    static bool initialized=false;
    if(!initialized){app.setStyle(QStyleFactory::create("Fusion"));initialized=true;}
    bool dark=theme==2;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if(theme==0)dark=app.styleHints()->colorScheme()==Qt::ColorScheme::Dark;
#else
    if(theme==0)dark=app.style()->standardPalette().color(QPalette::Window).lightness()<128;
#endif
    QPalette p=app.style()->standardPalette();
    p.setColor(QPalette::Window,dark?QColor("#484848"):QColor("#f5f7f6"));
    p.setColor(QPalette::Base,dark?QColor("#3e3e3e"):QColor("#fafcfb"));
    p.setColor(QPalette::AlternateBase,dark?QColor("#505050"):QColor("#edf0ef"));
    for(auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText})p.setColor(role,dark?QColor("#eeeeee"):QColor("#22372c"));
    p.setColor(QPalette::PlaceholderText,dark?QColor("#cccccc"):QColor("#65726c"));
    p.setColor(QPalette::Button,dark?QColor("#555555"):QColor("#ffffff"));
    p.setColor(QPalette::Highlight,dark?QColor("#477a68"):QColor("#b4d8c6"));
    p.setColor(QPalette::HighlightedText,dark?Qt::white:QColor("#153c2d"));
    p.setColor(QPalette::Mid,dark?QColor("#707070"):QColor("#cad6ce"));
    for(auto role:{QPalette::Text,QPalette::WindowText,QPalette::ButtonText})p.setColor(QPalette::Disabled,role,dark?QColor("#aaaaaa"):QColor("#8b9690"));
    app.setPalette(p);
    app.setStyleSheet(R"(
        QToolBar { spacing: 8px; padding: 8px; }
        QToolButton#recentFolders::menu-indicator { image: none; width: 0px; }
        QGroupBox { font-weight: 600; border: 1px solid palette(mid); border-radius: 6px; margin-top: 10px; padding: 12px 8px 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; }
        QPushButton { padding: 7px; }
        QPushButton#convertVisible, QPushButton#convertSelected { padding: 7px 3px; border: 1px solid palette(mid); border-radius: 2px; }
        QToolButton[pushAppearance="true"] { padding: 7px; }
        QToolButton[pushAppearance="true"]::menu-indicator { image: none; width: 0px; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateEdit { padding: 5px; }
        QPushButton[primary="true"] { background: #2e8069; color: white; }
        QPushButton[primary="true"][noSelection="true"] { background: #c4e3d3; color: #567565; }
        QPushButton#cancelQueue[running="true"], QPushButton#stopProgress { background: #b83e3e; color: white; }
        QPushButton#toggleOutput { padding: 2px 7px; }
        QPushButton#stopProgress { padding: 1px; }
        QProgressBar { border: 1px solid palette(mid); border-radius: 4px; text-align: center; min-height: 17px; }
        QProgressBar::chunk { background: #72ad93; }
    )");
}
}
