#include "updater.hpp"
#include "jxrc/converter.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QVersionNumber>

namespace jxrc::gui {
namespace {
QString helperName() {
#ifdef Q_OS_WIN
    return "jxrc-update.exe";
#else
    return "jxrc-update";
#endif
}
QString architecture(QString value) {
    value=value.toLower();
    if(value=="amd64"||value=="x86_64")return "x86_64";
    if(value=="aarch64"||value=="arm64")return "arm64";
    return value;
}
}
Updater::Updater(QWidget* parent,QNetworkAccessManager* network):QObject(parent),window_(parent),network_(network?network:new QNetworkAccessManager(this)) {
    if(network)network->setParent(this);
    root_=QCoreApplication::applicationDirPath();relativeGui_=QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    helperRelative_=helperName();manifestRelative_="share/JXRConverter/package-files.txt";
#ifdef Q_OS_MACOS
    root_=QDir(root_).absoluteFilePath("../../..");root_=QDir::cleanPath(root_);
    relativeGui_="JXRConverter.app/Contents/MacOS/JXRConverter";
    helperRelative_="JXRConverter.app/Contents/Helpers/"+helperName();
    if(!QFileInfo(root_+"/jxrconvert").isFile()) {
        appOnly_=true;root_=QDir::cleanPath(QCoreApplication::applicationDirPath()+"/../..");
        relativeGui_="Contents/MacOS/JXRConverter";helperRelative_="Contents/Helpers/"+helperName();
        manifestRelative_="Contents/Resources/share/JXRConverter/package-files.txt";
    }
#endif
    QTimer::singleShot(0,this,[this]{
        QFile report(root_+"/.jxrc-update-error.txt");
        if(report.open(QIODevice::ReadOnly)) {
            const QString message=QString::fromUtf8(report.readAll());report.close();report.remove();
            QMessageBox::warning(window_,"Update could not be installed",message);
        }
    });
}
Updater::~Updater() {
    for(auto* reply:findChildren<QNetworkReply*>()){disconnect(reply,nullptr,this,nullptr);reply->abort();}
    for(auto* process:findChildren<QProcess*>()) {
        disconnect(process,nullptr,this,nullptr);
        if(process->state()!=QProcess::NotRunning){process->kill();process->waitForFinished(1000);}
    }
}
void Updater::fail(const QString& error) {
    busy_=false;emit busyChanged(false);emit statusChanged(available_?"Update available":"Update");
    emit message("Update: "+error);
    if(!checkingOnly_)QMessageBox::warning(window_,"Update",error);
}
void Updater::fetch(const QUrl& url,const QString& destination,qint64 limit,std::function<void()> next) {
    if(url.scheme()!="https"){fail("The release contains an invalid download URL.");return;}
    auto file=std::make_shared<QSaveFile>(destination);
    if(!file->open(QIODevice::WriteOnly)){fail(file->errorString());return;}
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent",("JXRConverter/"+jxrc::version()).c_str());
    request.setRawHeader("Accept",url.host()=="api.github.com"?"application/vnd.github+json":"application/octet-stream");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000);
    auto* reply=network_->get(request);
    auto received=std::make_shared<qint64>(0);auto writeError=std::make_shared<QString>();
    connect(reply,&QIODevice::readyRead,this,[reply,file,received,writeError,limit]{
        const auto bytes=reply->readAll();*received+=bytes.size();
        if(*received>limit){*writeError="The download exceeds the package size limit.";reply->abort();return;}
        if(file->write(bytes)!=bytes.size()){*writeError=file->errorString();reply->abort();}
    });
    connect(reply,&QNetworkReply::downloadProgress,this,[this](qint64 done,qint64 total){
        if(busy_&&!checkingOnly_&&total>0)emit statusChanged(QString("Updating %1%").arg(done*100/total));
    });
    connect(reply,&QNetworkReply::finished,this,[this,reply,file,writeError,next=std::move(next)]{
        const QString error=!writeError->isEmpty()?*writeError:reply->error()!=QNetworkReply::NoError?reply->errorString():QString{};
        const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();reply->deleteLater();
        if(!error.isEmpty()||status!=200){fail(error.isEmpty()?QString("Download failed (HTTP %1).").arg(status):error);return;}
        if(!file->commit()){fail(file->errorString());return;}next();
    });
}
void Updater::start() { begin(true); }
void Updater::check() { begin(false); }
void Updater::begin(bool install) {
    if(busy_)return;
    checkingOnly_=!install;
    if(install&&QDir(root_).isRoot()){fail("Updating from a drive root is not allowed. Move the app to a dedicated folder first.");return;}
    if(install&&!QFileInfo(root_+"/"+helperRelative_).isFile()) {
        fail("The update helper is missing. Extract the complete release package before updating.");return;
    }
    if(install) {
        QTemporaryDir writable(root_+"/.jxrc-write-check-XXXXXX");
        if(!writable.isValid()){fail("The application folder is not writable. Move the extracted package to a folder you own before updating.");return;}
    }
    temp_=std::make_unique<QTemporaryDir>(QDir::tempPath()+"/jxrc-update-XXXXXX");
    if(!temp_->isValid()){fail("Cannot create the update workspace.");return;}
    busy_=true;emit busyChanged(true);emit statusChanged("Checking update…");emit message("Checking for updates…");
    const QString release=temp_->path()+"/release.json";
    fetch(QUrl("https://api.github.com/repos/" JXRC_UPDATE_REPOSITORY "/releases/latest"),release,4*1024*1024,[this,release]{
        QFile file(release);if(!file.open(QIODevice::ReadOnly)){fail(file.errorString());return;}downloadRelease(file.readAll());
    });
}
void Updater::downloadRelease(const QByteArray& data) {
    const auto document=QJsonDocument::fromJson(data);
    if(!document.isObject()){fail("GitHub returned invalid release information.");return;}
    const auto release=document.object();QString version=release.value("tag_name").toString();
    if(version.startsWith('v'))version.remove(0,1);
    if(!QRegularExpression("^[0-9]+\\.[0-9]+\\.[0-9]+$").match(version).hasMatch()||release.value("draft").toBool()||release.value("prerelease").toBool()) {
        fail("No stable release is available.");return;
    }
    if(QVersionNumber::fromString(version)<=QVersionNumber::fromString(QString::fromStdString(jxrc::version()))) {
        available_=false;emit updateAvailableChanged(false);busy_=false;emit busyChanged(false);emit statusChanged("Latest");emit message("The installed version is the latest release.");return;
    }
    QString system=QStringLiteral(JXRC_PACKAGE_SYSTEM);
#ifdef Q_OS_WIN
    const QString extension=".zip";
#else
    const QString extension=".tar.gz";
#endif
    assetName_.clear();checksumUrl_=QUrl();
    const QString prefix="JXRConverter-"+version+"-"+system+"-";
    for(const auto& value:release.value("assets").toArray()) {
        const auto asset=value.toObject();const QString name=asset.value("name").toString();
        if(name=="SHA256SUMS.txt")checksumUrl_=QUrl(asset.value("browser_download_url").toString());
        if(!name.startsWith(prefix)||!name.endsWith("-desktop"+extension))continue;
        const QString cpu=name.mid(prefix.size(),name.size()-prefix.size()-QString("-desktop"+extension).size());
        if(architecture(cpu)!=architecture(QSysInfo::buildCpuArchitecture()))continue;
        if(!assetName_.isEmpty()){fail("The release has multiple packages for this platform.");return;}
        assetName_=name;assetUrl_=QUrl(asset.value("browser_download_url").toString());
    }
    if(assetName_.isEmpty()||!checksumUrl_.isValid()){fail("This release has no matching desktop package and checksum file.");return;}
    available_=true;emit updateAvailableChanged(true);
    emit message("Update available: "+version);
    if(checkingOnly_){busy_=false;emit busyChanged(false);emit statusChanged("Update available");return;}
    archive_=temp_->path()+"/"+assetName_;checksum_=temp_->path()+"/SHA256SUMS.txt";
    emit statusChanged("Downloading update…");
    fetch(checksumUrl_,checksum_,1024*1024,[this]{fetch(assetUrl_,archive_,512LL*1024*1024,[this]{verifyAndExtract();});});
}
void Updater::runTar(const QStringList& arguments,std::function<void(const QByteArray&)> next) {
#ifdef Q_OS_WIN
    const QString tar=QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/tar.exe");
#else
    const QString tar=QStandardPaths::findExecutable("tar");
#endif
    if(tar.isEmpty()||!QFileInfo::exists(tar)){fail("The system archive tool (tar) is unavailable.");return;}
    auto* process=new QProcess(this);process->setProgram(tar);process->setArguments(arguments);
    auto* timer=new QTimer(process);timer->setSingleShot(true);
    connect(timer,&QTimer::timeout,process,[process]{process->kill();});timer->start(120000);
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart){process->deleteLater();fail(process->errorString());}
    });
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,next=std::move(next)](int code,QProcess::ExitStatus state){
        const auto output=process->readAllStandardOutput(),errors=process->readAllStandardError();process->deleteLater();
        if(code!=0||state!=QProcess::NormalExit){fail("Cannot extract the update: "+QString::fromLocal8Bit(errors));return;}next(output);
    });
    process->start();
}
void Updater::verifyAndExtract() {
    QFile sums(checksum_),archive(archive_);
    if(!sums.open(QIODevice::ReadOnly)||!archive.open(QIODevice::ReadOnly)){fail("Cannot read the downloaded package.");return;}
    QByteArray expected;int matches=0;
    for(const auto& line:sums.readAll().split('\n')) {
        const auto match=QRegularExpression("^([a-fA-F0-9]{64})[ \\t]+\\*?(.+)$").match(QString::fromUtf8(line).trimmed());
        if(match.hasMatch()&&match.captured(2)==assetName_){expected=match.captured(1).toLatin1().toLower();++matches;}
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if(matches!=1||!hash.addData(&archive)||hash.result().toHex()!=expected){fail("The downloaded package did not match its release checksum.");return;}
    archive.close();extract_=temp_->path()+"/extracted";
    if(!QDir().mkpath(extract_)){fail("Cannot create the extraction folder.");return;}
    emit statusChanged("Extracting update…");
    runTar({"-tf",archive_},[this](const QByteArray& listing){
        for(auto line:listing.split('\n')) {
            if(line.endsWith('\r'))line.chop(1);
            const QString name=QString::fromUtf8(line);if(name.isEmpty())continue;
            if(QDir::isAbsolutePath(name)||name.contains('\\')||name.contains(':')||name.split('/').contains("..")) {
                fail("The package contains an unsafe archive path.");return;
            }
        }
        runTar({"-xf",archive_,"-C",extract_},[this](const QByteArray&){install();});
    });
}
void Updater::install() {
    const auto directories=QDir(extract_).entryList(QDir::Dirs|QDir::NoDotAndDotDot);
    if(directories.size()!=1){fail("The update package has an unexpected layout.");return;}
    QString source=extract_+"/"+directories.first();
    if(appOnly_)source+="/JXRConverter.app";
    if(!QFileInfo::exists(source+"/"+relativeGui_)||!QFileInfo::exists(source+"/"+helperRelative_)) {
        fail("The update package is missing application files.");return;
    }
    const QString helper=temp_->path()+"/"+helperName();
    if(!QFile::copy(source+"/"+helperRelative_,helper)){fail("Cannot prepare the update helper.");return;}
    QFile::setPermissions(helper,QFileInfo(source+"/"+helperRelative_).permissions());
    temp_->setAutoRemove(false);
    if(!QProcess::startDetached(helper,{QString::number(QCoreApplication::applicationPid()),root_,source,relativeGui_,temp_->path(),manifestRelative_},root_)) {
        temp_->setAutoRemove(true);fail("Cannot start the update helper.");return;
    }
    emit statusChanged("Restarting…");QCoreApplication::quit();
}
}
