#pragma once
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>
class QWidget;
class QNetworkAccessManager;
class QTemporaryDir;
namespace jxrc::gui {
class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(QWidget* parent, QNetworkAccessManager* network = nullptr);
    ~Updater() override;
    bool busy() const { return busy_; }
    void start();
    void check();
    bool installing() const { return busy_&&!checkingOnly_; }
    bool updateAvailable() const { return available_; }
signals:
    void busyChanged(bool);
    void statusChanged(const QString&);
    void updateAvailableChanged(bool);
    void message(const QString&);
private:
    void begin(bool install);
    void fail(const QString&);
    void fetch(const QUrl&,const QString&,qint64,std::function<void()>);
    void runTar(const QStringList&,std::function<void(const QByteArray&)>);
    void downloadRelease(const QByteArray&);
    void verifyAndExtract();
    void install();
    QWidget* window_;
    QNetworkAccessManager* network_;
    std::unique_ptr<QTemporaryDir> temp_;
    QString root_,relativeGui_,helperRelative_,manifestRelative_,assetName_,archive_,checksum_,extract_;
    QUrl assetUrl_,checksumUrl_;
    bool busy_=false,appOnly_=false,checkingOnly_=false,available_=false;
};
}
