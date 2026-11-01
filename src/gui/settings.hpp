#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace jxrc::gui {
inline QString configDirectory() {
    static const QString directory=[] {
        QString root=QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
        // Keep writable user data outside the signed application bundle.
        if(root.endsWith(".app/Contents/MacOS"))
            root=QDir::cleanPath(root+"/../../..");
#endif
        // GUI tests must never read or modify a portable installation's settings.
        if(QStandardPaths::isTestModeEnabled())
            root=QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        const QString path=root+"/config";
        QDir().mkpath(path);
        return path;
    }();
    return directory;
}
inline QSettings appSettings() {
    return QSettings(configDirectory()+"/settings.ini",QSettings::IniFormat);
}
}
