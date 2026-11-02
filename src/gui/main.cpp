#include "window.hpp"
#include "style.hpp"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QStyleFactory>
#include <iostream>

int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--version") {
        std::cout << "JXRConverter " << jxrc::version() << '\n';
        return 0;
    }
    QApplication app(argc,argv);app.setOrganizationName("JXRConverter");app.setApplicationName("JXRConverter");app.setApplicationVersion(QString::fromStdString(jxrc::version()));
    app.setApplicationDisplayName("JXRConverter v"+QString::fromStdString(jxrc::version()));
    app.setWindowIcon(QIcon(":/icons/jxrconverter.png"));
    jxrc::gui::applyDesktopStyle();
    jxrc::gui::MainWindow window;window.show();
    QStringList paths;for(const auto& arg:app.arguments().mid(1))paths<<QFileInfo(arg).absoluteFilePath();window.addPaths(paths);
    return app.exec();
}
