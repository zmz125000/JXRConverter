#include "window.hpp"
#include "style.hpp"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QStyleFactory>

int main(int argc,char** argv) {
    QApplication app(argc,argv);app.setOrganizationName("JXRConverter");app.setApplicationName("JXRConverter");app.setApplicationVersion(QString::fromStdString(jxrc::version()));
    jxrc::gui::applyDesktopStyle();
    jxrc::gui::MainWindow window;window.show();
    QStringList paths;for(const auto& arg:app.arguments().mid(1))paths<<QFileInfo(arg).absoluteFilePath();window.addPaths(paths);
    return app.exec();
}
