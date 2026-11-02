#include <QtTest>
#include <QAbstractItemModelTester>
#include <QCheckBox>
#include <QComboBox>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QHeaderView>
#include <QIcon>
#include <cmath>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QLocale>
#include <QListWidget>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QSemaphore>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QScrollBar>
#include <QScrollArea>
#include <QGroupBox>
#include <QLayout>
#include <QPushButton>
#include <QProgressBar>
#include <QDateEdit>
#include <QDesktopServices>
#include <QScopeGuard>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QToolBar>
#include <QTimer>
#include <QWheelEvent>
#include <QTemporaryDir>
#include <exiv2/exiv2.hpp>
#include <atomic>
#include <future>
#include "window.hpp"
#include "updater.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSysInfo>
#include <QTabWidget>
#include <QToolButton>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include "output_capture.hpp"
#include <cstdio>
#include <cstring>
#include "settings.hpp"
#include "style.hpp"
#include "views.hpp"
using namespace jxrc::gui;

class ReleaseReply : public QNetworkReply {
    QByteArray body_;qint64 offset_=0;
public:
    ReleaseReply(const QNetworkRequest& request,QByteArray body,bool fail,QObject* parent):QNetworkReply(parent),body_(std::move(body)) {
        setRequest(request);setUrl(request.url());setOperation(QNetworkAccessManager::GetOperation);setAttribute(QNetworkRequest::HttpStatusCodeAttribute,200);open(QIODevice::ReadOnly|QIODevice::Unbuffered);
        QTimer::singleShot(0,this,[this,fail]{if(isFinished())return;if(fail)setError(QNetworkReply::HostNotFoundError,"Test network failure");else emit readyRead();setFinished(true);emit finished();});
    }
    void abort() override {if(!isFinished()){setError(QNetworkReply::OperationCanceledError,"Cancelled");setFinished(true);emit finished();}}
    qint64 bytesAvailable() const override {return body_.size()-offset_+QNetworkReply::bytesAvailable();}
protected:
    qint64 readData(char* data,qint64 maximum) override {if(offset_==body_.size())return -1;const auto count=std::min(maximum,qint64(body_.size())-offset_);std::memcpy(data,body_.constData()+offset_,size_t(count));offset_+=count;return count;}
};
class ReleaseNetwork : public QNetworkAccessManager {
public:
    QByteArray response;bool fail=false;int requests=0;
protected:
    QNetworkReply* createRequest(Operation,const QNetworkRequest& request,QIODevice*) override {++requests;return new ReleaseReply(request,response,fail,this);}
};

class FolderReceiver : public QObject {
    Q_OBJECT
public:
    QUrl opened;
public slots:
    void open(const QUrl& url){opened=url;}
};

class GuiTests : public QObject {
    Q_OBJECT
    QTemporaryDir temp_;
    QString makeImage(const QString& directory,const QString& name,int width,int height,int iso,const QString& captured="2025:06:12 14:30:00") {
        QDir().mkpath(directory);const QString path=directory+"/"+name;
        QImage image(width,height,QImage::Format_RGB32);image.fill(QColor(180,95,45));
        if(!image.save(path,"JPEG"))return {};
        auto file=Exiv2::ImageFactory::open(path.toStdString());file->readMetadata();auto& exif=file->exifData();
        exif["Exif.Image.Make"]="Test";exif["Exif.Image.Model"]="Camera Body";exif["Exif.Photo.LensModel"]="Prime 50";
        exif["Exif.Photo.ISOSpeedRatings"]=uint16_t(iso);exif["Exif.Photo.FNumber"]=Exiv2::URational(28,10);
        exif["Exif.Photo.ExposureTime"]=Exiv2::URational(1,125);exif["Exif.Photo.FocalLength"]=Exiv2::URational(50,1);
        exif["Exif.Photo.DateTimeOriginal"]=captured.toStdString();file->writeMetadata();
        // Catalog paths are canonical; macOS temporary paths often pass through
        // the /var -> /private/var symlink.
        return QFileInfo(path).canonicalFilePath();
    }
    QString root(const char* name) {return temp_.path()+"/"+QString::fromUtf8(name);}
    bool inspected(Catalog& model) {for(int row=0;row<model.rowCount();++row)if(!model.record(row).inspected)return false;return true;}
    QAction* savedJob(MainWindow& window,qint64 number) {for(auto* action:window.findChild<QMenu*>("jobMenu")->actions())if(action->data().toLongLong()==number)return action;return nullptr;}
    bool chooseJob(MainWindow& window,qint64 number) {auto* action=savedJob(window,number);if(!action)return false;action->trigger();return true;}
private slots:
    void initTestCase() {
        applyDesktopStyle();
        QVERIFY(temp_.isValid());QStandardPaths::setTestModeEnabled(true);QCoreApplication::setOrganizationName("JxrcTests");QCoreApplication::setApplicationName("DesktopTests");appSettings().clear();
    }
    void init() {QFile::remove(configDirectory()+"/jobs.json");}
    void metadataAndThumbnails() {
        const auto file=makeImage(root("metadata"),"orange.jpg",128,64,400);QVERIFY(!file.isEmpty());
        const auto metadata=readMetadata(file);QCOMPARE(metadata.width,128);QCOMPARE(metadata.height,64);QCOMPARE(metadata.iso,400);
        QCOMPARE(metadata.format,QString("JPEG"));QCOMPARE(metadata.camera,QString("Test Camera Body"));QCOMPARE(metadata.lens,QString("Prime 50"));
        QVERIFY(std::abs(metadata.aperture-2.8)<0.01);QVERIFY(std::abs(metadata.shutter-0.008)<0.00001);QCOMPARE(metadata.captured.date(),QDate(2025,6,12));
        QVERIFY(metadata.exif.contains("Exif.Photo.ISOSpeedRatings"));
        const auto thumbnail=readThumbnail(file,48);QVERIFY(!thumbnail.isNull());QCOMPARE(thumbnail.size(),QSize(48,24));
        auto info=jxrc::inspect(std::filesystem::u8path(file.toStdString()));QCOMPARE(info.width,128u);QVERIFY(!info.hdr);
        jxrc::Options options;options.target=jxrc::Target::avif;options.quality.preset=jxrc::Preset::lossless;
        const QString avif=root("metadata")+"/orange.avif";jxrc::convert(std::filesystem::u8path(file.toStdString()),std::filesystem::u8path(avif.toStdString()),options);
        const auto avifRecord=readMetadata(avif);QCOMPARE(avifRecord.width,128);QCOMPARE(avifRecord.format,QString("AVIF"));
        const auto nativeThumbnail=readThumbnail(avif,40);QVERIFY(!nativeThumbnail.isNull());QCOMPARE(nativeThumbnail.width(),40);
    }
    void catalogImportAndFilters() {
        const QString directory=root("catalog");const auto a=makeImage(directory,"a.jpg",160,80,80),b=makeImage(directory,"b.jpg",240,120,800,"2026:01:02 12:00:00");
        const auto c=makeImage(directory+"/nested","c.JPG",100,50,200);QVERIFY(!a.isEmpty()&&!b.isEmpty()&&!c.isEmpty());
        QFile nonImage(directory+"/notes.txt");QVERIFY(nonImage.open(QIODevice::WriteOnly));nonImage.write("not an image");nonImage.close();
        Catalog model;QAbstractItemModelTester tester(&model,QAbstractItemModelTester::FailureReportingMode::QtTest);model.addPaths({directory},false);
        QTRY_COMPARE(model.rowCount(),2);QTRY_VERIFY(inspected(model));model.addPaths({directory,a},true);QTRY_COMPARE(model.rowCount(),3);QTRY_VERIFY(!model.scanning()&&inspected(model));
        CatalogFilter proxy;proxy.setSourceModel(&model);proxy.sort(Catalog::Iso,Qt::AscendingOrder);QCOMPARE(proxy.index(0,Catalog::Iso).data().toString(),QString("80"));
        proxy.sort(Catalog::Iso,Qt::DescendingOrder);QCOMPARE(proxy.index(0,Catalog::Iso).data().toString(),QString("800"));
        Filter f;f.minMaxEdge=200;proxy.setFilter(f);QCOMPARE(proxy.rowCount(),1);QCOMPARE(proxy.index(0,0).data(Catalog::PathRole).toString(),b);
        f={};f.minWidth=150;f.minHeight=70;proxy.setFilter(f);QCOMPARE(proxy.rowCount(),2);
        f={};f.from=QDate(2026,1,1);f.to=QDate(2026,12,31);proxy.setFilter(f);QCOMPARE(proxy.rowCount(),1);
        f={};f.text="Exif.Photo.LensModel Prime 50";proxy.setFilter(f);QCOMPARE(proxy.rowCount(),3);
        f={};f.format="AVIF";proxy.setFilter(f);QCOMPARE(proxy.rowCount(),0);
        f={};f.range="SDR";proxy.setFilter(f);QCOMPARE(proxy.rowCount(),3);
        f={};f.minMegapixels=0.02;proxy.setFilter(f);QCOMPARE(proxy.rowCount(),1);
        f={};f.minMaxEdge=160;f.minMegapixels=0.02;proxy.setFilter(f);QCOMPARE(proxy.rowCount(),1);
        f.minMaxEdge=250;proxy.setFilter(f);QCOMPARE(proxy.rowCount(),0);
        f={};f.literalText=true;f.text="Prime 50";proxy.setFilter(f);QCOMPARE(proxy.rowCount(),3);
        f.text="50 Prime";proxy.setFilter(f);QCOMPARE(proxy.rowCount(),0);
        f.literalText=false;proxy.setFilter(f);QCOMPARE(proxy.rowCount(),3);
        // Today must prefer capture date over the newly created file's mtime.
        const auto todayFile=makeImage(directory,"today.jpg",80,160,100,QDate::currentDate().toString("yyyy:MM:dd")+" 12:00:00");
        model.addPaths({todayFile});QTRY_COMPARE(model.rowCount(),4);QTRY_VERIFY(!model.scanning()&&inspected(model));
        f={};f.today=true;f.minMaxEdge=150;proxy.setFilter(f);QCOMPARE(proxy.rowCount(),1);
        QCOMPARE(proxy.index(0,0).data(Catalog::PathRole).toString(),todayFile);
        model.removePaths({todayFile});
        f={};proxy.setFilter(f);model.beginThumbnailFrame([](const QModelIndex& i){return i.row()==0;});auto decoration=model.data(model.index(0,0),Catalog::ThumbnailRole);Q_UNUSED(decoration);model.endThumbnailFrame();
        QTRY_VERIFY(!qvariant_cast<QIcon>(model.data(model.index(0,0),Qt::DecorationRole)).isNull());
        model.removePaths({b});QCOMPARE(model.rowCount(),2);QCOMPARE(proxy.rowCount(),2);
    }
    void thumbnailWindowAndCancellation() {
        const auto directory=root("thumbnail-window");
        for(int i=0;i<40;++i)QVERIFY(!makeImage(directory,QString("%1.jpg").arg(i,2,10,QChar('0')),32,16,100).isEmpty());
        Catalog model;model.addPaths({directory});QTRY_COMPARE(model.rowCount(),40);QTRY_VERIFY(!model.scanning()&&inspected(model));
        // Even a layout probing the entire library must not schedule work.
        for(int i=0;i<40;++i)model.index(i,0).data(Qt::DecorationRole);
        QCOMPARE(model.thumbnailJobCount(),0);
        model.beginThumbnailFrame([](const QModelIndex& i){return i.row()<20;});
        for(int i=0;i<40;++i)model.index(i,0).data(Catalog::ThumbnailRole);
        model.endThumbnailFrame();QCOMPARE(model.thumbnailJobCount(),6);
        for(int i=6;i<40;++i)QVERIFY(!model.record(i).thumbnailPending);
        // Scroll before processing completions: all old pending jobs are
        // cancelled, and the rolling window replenishes only the new viewport.
        model.beginThumbnailFrame([](const QModelIndex& i){return i.row()>=30;});
        for(int i=30;i<40;++i)model.index(i,0).data(Catalog::ThumbnailRole);
        model.endThumbnailFrame();QCOMPARE(model.thumbnailJobCount(),6);
        auto ready=[&]{
            if(model.thumbnailJobCount()>6)return false;
            for(int i=30;i<40;++i)if(qvariant_cast<QIcon>(model.index(i,0).data(Qt::DecorationRole)).isNull())return false;
            return true;
        };
        QTRY_VERIFY(ready());QTRY_COMPARE(model.thumbnailJobCount(),0);
        for(int i=0;i<30;++i){QVERIFY(!model.record(i).thumbnailPending);QVERIFY(qvariant_cast<QIcon>(model.index(i,0).data(Qt::DecorationRole)).isNull());}
    }
    void thumbnailCacheBudgetAndSize() {
        const auto directory=root("thumbnail-cache");
        for(int i=0;i<40;++i)QVERIFY(!makeImage(directory,QString("%1.jpg").arg(i),384,192,100).isEmpty());
        Catalog model(nullptr,64);model.addPaths({directory});QTRY_COMPARE(model.rowCount(),40);QTRY_VERIFY(!model.scanning()&&inspected(model));
        model.beginThumbnailFrame([](const QModelIndex&){return true;});
        for(int i=0;i<40;++i)model.index(i,0).data(Catalog::ThumbnailRole);
        model.endThumbnailFrame();
        auto ready=[&]{for(int i=0;i<40;++i)if(qvariant_cast<QIcon>(model.index(i,0).data(Qt::DecorationRole)).isNull())return false;return true;};
        // Forty full thumbnails cannot fit this cache. They must be reduced
        // so every visible cell finishes, rather than evicting in a decode loop.
        QTRY_VERIFY(ready());QTRY_COMPARE(model.thumbnailJobCount(),0);QVERIFY(model.thumbnailCacheCost()<=64);
        model.beginThumbnailFrame([](const QModelIndex&){return true;});
        for(int i=0;i<40;++i)model.index(i,0).data(Catalog::ThumbnailRole);
        model.endThumbnailFrame();QCOMPARE(model.thumbnailJobCount(),0);

        const auto jpeg=makeImage(directory,"large.jpg",1536,1024,100);
        QCOMPARE(readThumbnail(jpeg,1024).size(),QSize(768,512));
        QImage raster(1024,1536,QImage::Format_RGB32);raster.fill(QColor(60,110,180));
        for(const auto& extension:{"png","bmp","tif"}) {
            const auto path=directory+"/large."+extension;
            if(!raster.save(path))continue; // Qt plugin availability varies by platform.
            QCOMPARE(readThumbnail(path,1024).size(),QSize(512,768));
        }
    }
    void lazyMetadataAndClearCancellation() {
        const auto directory=root("lazy-io");
        QStringList files;
        for(int i=0;i<40;++i)files<<makeImage(directory,QString("%1.jpg").arg(i,2,10,QChar('0')),96,48,100);
        Catalog model;model.setBackgroundMetadata(false);model.addPaths({directory});
        QTRY_COMPARE(model.rowCount(),40);QTRY_VERIFY(!model.scanning());
        model.beginThumbnailFrame([](const QModelIndex& i){return i.row()<3;});
        for(int i=0;i<3;++i)model.index(i,0).data(Catalog::ThumbnailRole);
        model.endThumbnailFrame();
        QTRY_VERIFY(model.record(0).inspected&&model.record(1).inspected&&model.record(2).inspected);
        QTRY_COMPARE(model.metadataJobCount(),0);
        for(int i=3;i<40;++i)QVERIFY(!model.record(i).inspected);
        model.beginThumbnailFrame([](const QModelIndex& i){return i.row()>=20&&i.row()<26;});
        for(int i=20;i<26;++i)model.index(i,0).data(Catalog::ThumbnailRole);
        model.endThumbnailFrame();QVERIFY(model.thumbnailJobCount()<=6);
        auto visibleReady=[&]{for(int i=20;i<26;++i)if(!model.record(i).inspected||qvariant_cast<QIcon>(model.index(i,0).data(Qt::DecorationRole)).isNull())return false;return true;};
        QTRY_VERIFY(visibleReady());QTRY_COMPARE(model.metadataJobCount(),0);
        for(int i=3;i<20;++i)QVERIFY(!model.record(i).inspected);
        model.beginThumbnailFrame([](const QModelIndex& i){return i.row()>=30;});
        for(int i=30;i<40;++i)model.index(i,0).data(Catalog::ThumbnailRole);
        model.endThumbnailFrame();model.clear();
        QCOMPARE(model.rowCount(),0);QTRY_COMPARE(model.thumbnailJobCount(),0);QTRY_COMPARE(model.metadataJobCount(),0);
        // Stale enumeration batches must never repopulate a cleared library.
        model.addPaths({directory});model.clear();
        model.addPaths({files.first()});QTRY_COMPARE(model.rowCount(),1);
        model.beginThumbnailFrame([](const QModelIndex&){return true;});model.index(0,0).data(Catalog::ThumbnailRole);model.endThumbnailFrame();
        QTRY_VERIFY(inspected(model)&&!model.scanning());
        QTest::qWait(30);QCOMPARE(model.rowCount(),1);
    }
    void thumbnailAndMetadataShareSource() {
        const auto path=makeImage(root("shared-source"),"photo.jpg",128,64,400);
        QVERIFY(QFileInfo(path).size()<256*1024);
        int checks=0;bool removed=false;Record metadata;
        // Make the path unavailable immediately after the single source read
        // closes. EXIF and native inspection must both finish from its buffer.
        const auto image=readThumbnail(path,48,[&]{if(++checks==3)removed=QFile::remove(path);return false;},&metadata);
        QVERIFY(removed);QVERIFY(!QFileInfo::exists(path));QCOMPARE(image.size(),QSize(48,24));
        QVERIFY(metadata.inspected);QCOMPARE(metadata.width,128);QCOMPARE(metadata.height,64);QCOMPARE(metadata.bits,8);
        QCOMPARE(metadata.iso,400);QCOMPARE(metadata.format,QString("JPEG"));QCOMPARE(metadata.camera,QString("Test Camera Body"));

        QImage raster(1024,512,QImage::Format_RGBA64);raster.fill(QColor(50,100,150));
        const auto png=root("shared-source")+"/sixteen.png";QVERIFY(raster.save(png));
        const auto thumbnail=readThumbnail(png,128,{},&metadata);
        QCOMPARE(thumbnail.size(),QSize(128,64));QCOMPARE(metadata.width,1024);QCOMPARE(metadata.height,512);QCOMPARE(metadata.bits,16);
    }
    void boundedReadsAndParallelDecoding() {
        const auto path=makeImage(root("parallel-thumbs"),"photo.jpg",128,64,100);
        QSemaphore sourceReadSlots(2),readEntered,readRelease,decodeEntered,decodeRelease;
        auto run=[&] {
            int checks=0;
            return readThumbnail(path,48,[&] {
                ++checks;
                if(checks==2){readEntered.release();readRelease.acquire();}
                // preview_bytes begins after closing the file and releasing
                // its read slot. All six decoders must reach this together.
                if(checks==4){decodeEntered.release();decodeRelease.acquire();}
                return false;
            },nullptr,&sourceReadSlots);
        };
        std::vector<std::future<QImage>> jobs;
        for(int i=0;i<6;++i)jobs.push_back(std::async(std::launch::async,run));
        const bool parallelReads=readEntered.tryAcquire(2,3000);
        const bool exceededReadLimit=readEntered.tryAcquire(1,50);
        readRelease.release(6);
        const bool parallelDecoding=decodeEntered.tryAcquire(6,3000);
        decodeRelease.release(6);
        QList<QImage> images;for(auto& job:jobs)images.append(job.get());
        QVERIFY(parallelReads);QVERIFY(!exceededReadLimit);QVERIFY(parallelDecoding);
        for(const auto& image:images)QCOMPARE(image.size(),QSize(48,24));

        // Cancellation while waiting for a read slot must prevent file opens.
        sourceReadSlots.acquire(2);std::atomic_bool cancel{false};QSemaphore started;
        auto waiting=std::async(std::launch::async,[&] {
            started.release();
            try{readThumbnail(root("missing.jpg"),48,[&]{return cancel.load();},nullptr,&sourceReadSlots);}
            catch(const std::exception& e){return QString::fromUtf8(e.what());}
            return QString{};
        });
        const bool launched=started.tryAcquire(1,3000);cancel.store(true);sourceReadSlots.release(2);
        const auto error=waiting.get();QVERIFY(launched);QVERIFY(error.contains("cancelled"));
    }
    void cancellableImageReads() {
        const auto path=root("cancel-read.bin");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.resize(8*1024*1024));file.close();
        const auto input=std::filesystem::u8path(path.toStdString());
        int checks=0;bool cancelled=false;
        try{jxrc::preview(input,384,[&]{return ++checks>=4;});}catch(const std::exception& e){cancelled=QString::fromUtf8(e.what()).contains("cancelled");}
        QVERIFY(cancelled);QCOMPARE(checks,4);
        checks=0;cancelled=false;
        try{jxrc::inspect(input,[&]{return ++checks>=4;});}catch(const std::exception& e){cancelled=QString::fromUtf8(e.what()).contains("cancelled");}
        QVERIFY(cancelled);QCOMPARE(checks,4);
    }
    void scrollingAndCellShape() {
        QStandardItemModel model(200,1);FileListView list;ThumbnailDelegate delegate;
        list.setItemDelegate(&delegate);list.setModel(&model);list.resize(640,300);list.setPresentation(true,200);list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        // LocalViewer's adaptive rule includes the fixed gaps when choosing
        // columns: two 200 dp cells fit here, then stretch to fill the row.
        QTRY_COMPARE(list.visualRect(model.index(1,0)).top(),list.visualRect(model.index(0,0)).top());
        QTRY_VERIFY(list.visualRect(model.index(2,0)).top()>list.visualRect(model.index(0,0)).top());
        const int unused=list.viewport()->width()-list.visualRect(model.index(1,0)).right()-1;
        QVERIFY(unused>=1&&unused<=2);
        const int squareHeight=list.gridSize().height();list.setPresentation(true,200,16.0/9);
        QVERIFY(list.gridSize().height()<squareHeight);QCOMPARE(delegate.aspect,16.0/9);
        list.setPresentation(false,100);auto* bar=list.verticalScrollBar();QTRY_VERIFY(bar->maximum()>200);bar->setSingleStep(40);bar->setValue(0);
        auto wheel=[&]{const QPointF point(50,50);QWheelEvent event(point,list.viewport()->mapToGlobal(point.toPoint()),QPoint(),QPoint(0,-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(list.viewport(),&event);};
        wheel();QCOMPARE(bar->value(),qRound(QApplication::wheelScrollLines()*40/2.0));
        const int target=bar->value()*2;list.setSmoothScrolling(true);wheel();QVERIFY(bar->value()<target);QTRY_COMPARE(bar->value(),target);
    }
    void nativeHdrMetadata() {
        const auto file=makeImage(root("hdr"),"sdr.jpg",96,48,100);
        jxrc::Options o;o.mode=jxrc::Mode::process;o.target=jxrc::Target::jxl;o.quality.preset=jxrc::Preset::lossless;
        const QString sdr=root("hdr")+"/sdr.jxl",hdr=root("hdr")+"/hdr.jxl";
        jxrc::convert(file.toStdString(),sdr.toStdString(),o);o.target=jxrc::Target::jxl_linear;jxrc::convert(file.toStdString(),hdr.toStdString(),o);
        const auto a=readMetadata(sdr),b=readMetadata(hdr);QVERIFY(!a.hdr);QVERIFY(b.hdr);QCOMPARE(b.transfer,QString("Linear"));
        auto preview=readThumbnail(hdr,24);QVERIFY(!preview.isNull());QCOMPARE(preview.size(),QSize(24,12));
    }
    void viewsSelectionAndDrop() {
        const QString directory=root("views");makeImage(directory,"one.jpg",160,80,100);makeImage(directory,"two.jpg",160,80,200);makeImage(directory,"three.jpg",160,80,300);
        MainWindow window;window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));window.addPaths({directory});QTRY_COMPARE(window.catalog()->rowCount(),3);
        auto* list=window.findChild<FileListView*>("fileList");auto* modes=window.findChild<QButtonGroup*>("viewMode");auto* slider=window.findChild<QSlider*>("thumbnailSize");QVERIFY(list&&modes&&slider);QCOMPARE(slider->value(),200);
        QVERIFY(window.findChild<QCheckBox*>("smoothScrolling"));QVERIFY(window.findChild<QToolButton*>("thumbnailAspect"));
        QTest::qWait(120);window.grab().save(QDir::tempPath()+"/jxrc-desktop-test.png");
        modes->button(0)->click();slider->setValue(100);QTRY_VERIFY(list->visualRect(window.filterModel()->index(2,0)).isValid());
        QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,list->visualRect(window.filterModel()->index(0,0)).center());
        QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::ControlModifier,list->visualRect(window.filterModel()->index(2,0)).center());
        QCOMPARE(list->selectionModel()->selectedRows(0).size(),2);
        modes->button(2)->click();auto* table=window.findChild<FileTableView*>("fileDetails");QCOMPARE(table->selectionModel()->selectedRows(0).size(),2);
        slider->setValue(200);QCOMPARE(table->verticalHeader()->defaultSectionSize(),108);
        modes->button(1)->click();QCOMPARE(list->viewMode(),QListView::ListMode);QCOMPARE(list->selectionModel()->selectedRows(0).size(),2);
        modes->button(0)->click();slider->setValue(100);list->selectionModel()->clearSelection();QTest::qWait(30);
        const QPoint start(list->viewport()->width()-15,list->viewport()->height()-15),end(3,3);
        QTest::mousePress(list->viewport(),Qt::LeftButton,Qt::NoModifier,start);QTest::mouseMove(list->viewport(),end,50);QTest::mouseRelease(list->viewport(),Qt::LeftButton,Qt::NoModifier,end);
        QTRY_COMPARE(list->selectionModel()->selectedRows(0).size(),3);
        const QString dropped=makeImage(root("dropped"),"drop.jpg",100,60,400);QMimeData mime;mime.setUrls({QUrl::fromLocalFile(dropped)});
        QDragEnterEvent enter(QPoint(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(list->viewport(),&enter);QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(list->viewport(),&drop);QVERIFY(drop.isAccepted());QTRY_COMPARE(window.catalog()->rowCount(),4);
        // Keep a screenshot artifact for visual inspection without requiring a display server.
        window.grab().save(QDir::tempPath()+"/jxrc-desktop-test.png");
    }
    void libraryAspectAndSearchReset() {
        appSettings().clear();
        const auto directory=root("wide-library");
        QStringList files;
        for(const auto& name:{"alpha.jpg","beta.jpg","gamma.jpg"})files<<makeImage(directory,name,160,90,100);
        MainWindow window;window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* aspect=window.findChild<QToolButton*>("thumbnailAspect");QVERIFY(aspect);aspect->menu()->actions()[2]->trigger();QCOMPARE(aspect->text(),QString("1:1"));
        window.addPaths(files);QTRY_COMPARE(window.catalog()->rowCount(),3);QTRY_VERIFY(inspected(*window.catalog()));
        QTRY_COMPARE(aspect->text(),QString("16:9"));
        window.findChild<QCheckBox*>("autoThumbnailAspect")->setChecked(false);aspect->menu()->actions()[2]->trigger();window.catalog()->setStatus(files.first(),"Ready");QCOMPARE(aspect->text(),QString("1:1"));

        auto* search=window.findChild<QLineEdit*>("search");auto* instant=window.findChild<QLineEdit*>("instantFilter");QVERIFY(search&&instant);
        search->setText("alpha");QTest::keyClick(search,Qt::Key_Return);QCOMPARE(window.filterModel()->rowCount(),1);
        instant->setText("beta");QCOMPARE(window.filterModel()->rowCount(),0);
        auto* clearSearch=search->findChild<QAbstractButton*>();QVERIFY(clearSearch);QTest::mouseClick(clearSearch,Qt::LeftButton);
        QVERIFY(search->text().isEmpty());QCOMPARE(window.filterModel()->rowCount(),1);
        QVERIFY(window.filterModel()->index(0,0).data().toString().contains("beta"));
        instant->clear();QCOMPARE(window.filterModel()->rowCount(),3);

        // Auto off also prevents extension-based changes on subsequent imports.
        const auto alias=directory+"/added.JXR";QVERIFY(QFile::copy(files.first(),alias));window.addPaths({alias});QCOMPARE(aspect->text(),QString("1:1"));
        window.findChild<QCheckBox*>("metadataFilters")->setChecked(true);QTest::qWait(30);
        for(const auto& name:{"minImageEdge","minMegapixels"}) {
            auto* input=window.findChild<QAbstractSpinBox*>(name);QVERIFY(input);
            auto* editor=input->findChild<QLineEdit*>();QVERIFY(editor);
            QVERIFY2(editor->width()>=editor->fontMetrics().horizontalAdvance(editor->text()),name);
        }
        window.grab().save(QDir::tempPath()+"/jxrc-filter-inputs-test.png");
        window.findChild<QCheckBox*>("metadataFilters")->setChecked(false);
    }
    void logClearAndPanelWidths() {
        appSettings().clear();MainWindow window;window.show();
        auto* tabs=window.findChild<QTabWidget*>("settingsTabs");auto* conversion=window.findChild<QScrollArea*>("conversionScroll");auto* output=window.findChild<QScrollArea*>("outputQueueScroll");
        auto* formats=window.findChild<QGroupBox*>("outputFormats");auto* destination=window.findChild<QGroupBox*>("outputDestination");
        QTRY_COMPARE(output->width(),conversion->width());QTRY_COMPARE(destination->width(),formats->width());
        auto* panel=window.findChild<QWidget*>("outputPanel");QVERIFY(panel);QCOMPARE(panel->width(),conversion->width());auto* progress=window.findChild<QProgressBar*>("conversionProgress");QCOMPARE(progress->mapTo(panel,QPoint()).x(),destination->mapTo(panel,QPoint()).x());QCOMPARE(progress->width(),destination->width());auto* toggle=window.findChild<QPushButton*>("toggleOutput");QCOMPARE(toggle->width(),progress->width());QCOMPARE(toggle->mapTo(&window,QPoint()).x(),progress->mapTo(&window,QPoint()).x());
        window.findChild<QPushButton*>("stopProgress")->show();QCoreApplication::processEvents();QCOMPARE(progress->width(),destination->width());window.findChild<QPushButton*>("stopProgress")->hide();QCOMPARE(window.findChild<QPushButton*>("convertVisible")->parentWidget(),panel);QVERIFY(panel->isAncestorOf(window.findChild<QProgressBar*>("conversionProgress")));
        tabs->setCurrentIndex(2);QCOMPARE(tabs->tabText(2),QString("Log"));auto* log=window.findChild<QPlainTextEdit*>("outputLog");auto* clear=window.findChild<QPushButton*>("clearLog");
        QVERIFY(tabs->widget(2)->isAncestorOf(clear));QCOMPARE(clear->parentWidget(),log);QCOMPARE(tabs->widget(2)->layout()->count(),1);QVERIFY(log->viewport()->geometry().contains(clear->geometry()));log->appendPlainText("Clear this log");clear->click();QVERIFY(log->toPlainText().isEmpty());QVERIFY(clear->x()>tabs->widget(2)->width()/2);
        log->setPlainText(QString("log entry\n").repeated(200));QTest::qWait(10);const auto position=clear->pos();log->verticalScrollBar()->setValue(0);log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());QCOMPARE(clear->pos(),position);
        window.resize(1600,850);QTRY_COMPARE(output->width(),conversion->width());QTRY_COMPARE(destination->width(),formats->width());QCOMPARE(progress->width(),destination->width());
    }
    void aspectRatioMenuAndAutoSampling() {
        appSettings().clear();MainWindow window;window.show();auto* aspect=window.findChild<QToolButton*>("thumbnailAspect");auto* automatic=window.findChild<QCheckBox*>("autoThumbnailAspect");
        QVERIFY(automatic->isChecked());QVERIFY(aspect->menu());QCOMPARE(aspect->popupMode(),QToolButton::InstantPopup);QCOMPARE(aspect->height(),window.findChild<QPushButton*>("thumbnailFit")->height());QCOMPARE(aspect->menu()->actions().size(),5);
        const QStringList names={"16:9","4:3","1:1","9:16 (portrait)","3:4 (portrait)"};const double ratios[]={16.0/9,4.0/3,1,9.0/16,3.0/4};
        auto* view=window.findChild<FileListView*>();automatic->setChecked(false);
        for(int i=0;i<5;++i){auto* action=aspect->menu()->actions()[i];QCOMPARE(action->text(),names[i]);action->trigger();auto* delegate=dynamic_cast<ThumbnailDelegate*>(view->itemDelegate());QCOMPARE(delegate->aspect,ratios[i]);QCOMPARE(view->gridSize().height(),qRound(delegate->edge/ratios[i])+94);}
        QStringList files;for(int i=0;i<3;++i)files.append(makeImage(root("auto-four-three"),QString::number(i)+".jpg",i==0?120:i==1?150:200,i==0?90:100,100));
        window.addPaths(files);QTRY_COMPARE(window.catalog()->rowCount(),3);QTRY_VERIFY(inspected(*window.catalog()));QTest::qWait(70);QCOMPARE(aspect->text(),QString("3:4"));
        automatic->setChecked(true);QTRY_COMPARE(aspect->text(),QString("4:3"));
        window.catalog()->clear();QStringList mixed{makeImage(root("mixed-ar"),"a.jpg",160,90,100),makeImage(root("mixed-ar"),"b.jpg",120,90,100),makeImage(root("mixed-ar"),"c.jpg",90,120,100)};
        window.addPaths(mixed);QTRY_COMPARE(window.catalog()->rowCount(),3);QTRY_VERIFY(inspected(*window.catalog()));QTest::qWait(100);QCOMPARE(aspect->text(),QString("4:3"));
        // Auto off stops metadata work requested only for the initial sample.
        Catalog model;model.setBackgroundMetadata(false);model.setAspectSampling(false);model.addPaths(files);QTRY_COMPARE(model.rowCount(),3);model.setBackgroundMetadata(false);QTest::qWait(50);QCOMPARE(model.metadataJobCount(),0);QVERIFY(!model.record(0).inspected);
        model.setAspectSampling(true);QTRY_VERIFY(model.record(0).inspected&&model.record(1).inspected);QVERIFY(!model.record(2).inspected);
    }
    void portraitGateAndToolbarJobActions() {
        appSettings().clear();
        {
            MainWindow window;window.show();auto* automatic=window.findChild<QCheckBox*>("autoThumbnailAspect");auto* portrait=window.findChild<QCheckBox*>("portraitThumbnailAspect");auto* aspect=window.findChild<QToolButton*>("thumbnailAspect");
            QVERIFY(automatic->x()<portrait->x());QVERIFY(portrait->x()<aspect->x());QVERIFY(!portrait->isChecked());
            auto* clear=window.findChild<QAction*>("clearJobs");QVERIFY(clear);auto* toolbar=window.findChild<QToolBar*>();QVERIFY(qobject_cast<QToolButton*>(toolbar->widgetForAction(clear)));
            auto* first=savedJob(window,0);QVERIFY(first->actionGroup());QVERIFY(first->actionGroup()->isExclusive());
            const auto a=makeImage(root("portrait-gate"),"a.jpg",90,160,100),b=makeImage(root("portrait-gate"),"b.jpg",99,176,100);
            window.addPaths({a,b});QTRY_COMPARE(window.catalog()->rowCount(),2);QTRY_VERIFY(inspected(*window.catalog()));QTest::qWait(70);QCOMPARE(aspect->text(),QString("1:1"));
            portrait->setChecked(true);QTRY_COMPARE(aspect->text(),QString("9:16"));portrait->setChecked(false);QCOMPARE(aspect->text(),QString("1:1"));
            automatic->setChecked(false);aspect->menu()->actions()[4]->trigger();QCOMPARE(aspect->text(),QString("3:4"));automatic->setChecked(true);QCOMPARE(aspect->text(),QString("1:1"));
            portrait->setChecked(true);QTRY_COMPARE(aspect->text(),QString("9:16"));
            window.findChild<QLineEdit*>("outputDirectory")->setText(root("portrait-gate-output"));QSignalSpy done(window.queue(),&ConversionQueue::batchFinished);window.findChild<QPushButton*>("convertVisible")->click();QTRY_COMPARE(done.count(),1);
            QCOMPARE(savedJob(window,1)->actionGroup(),savedJob(window,0)->actionGroup());QVERIFY(savedJob(window,1)->isChecked());QVERIFY(!savedJob(window,0)->isChecked());
            clear->trigger();QVERIFY(savedJob(window,0)->actionGroup()->isExclusive());QCOMPARE(window.findChild<QMenu*>("jobMenu")->actions().size(),1);
        }
        MainWindow restored;QVERIFY(restored.findChild<QCheckBox*>("portraitThumbnailAspect")->isChecked());
    }
    void viewportAspectSamplingAndAutoPersistence() {
        appSettings().clear();appSettings().setValue("controls/portraitThumbnailAspect",true);QStringList files;const auto directory=root("viewport-ar");
        for(int i=0;i<30;++i)files.append(makeImage(directory,QString("%1.jpg").arg(i,2,10,QChar('0')),i<15?160:90,i<15?90:160,100));
        {
            MainWindow window;window.resize(1400,760);window.show();window.addPaths(files);QTRY_COMPARE(window.catalog()->rowCount(),30);
            auto* aspect=window.findChild<QToolButton*>("thumbnailAspect");auto* automatic=window.findChild<QCheckBox*>("autoThumbnailAspect");auto* view=window.findChild<FileListView*>();QTRY_COMPARE(aspect->text(),QString("16:9"));
            view->scrollTo(window.filterModel()->index(27,0),QAbstractItemView::PositionAtTop);QTRY_COMPARE(aspect->text(),QString("9:16"));
            const auto row=view->firstVisibleRow();QVERIFY(!row.isEmpty());for(const auto& index:row)QVERIFY(index.row()>=15);
            automatic->setChecked(false);aspect->menu()->actions()[4]->trigger();view->verticalScrollBar()->setValue(0);QTest::qWait(100);QCOMPARE(aspect->text(),QString("3:4"));
        }
        MainWindow restored;restored.show();QVERIFY(!restored.findChild<QCheckBox*>("autoThumbnailAspect")->isChecked());QCOMPARE(restored.findChild<QToolButton*>("thumbnailAspect")->text(),QString("3:4"));
    }
    void detachedLogCaptureShutdown() {
        QTemporaryDir directory;QVERIFY(directory.isValid());const auto marker=directory.filePath("parent-exited");
        QProcess process;process.start(QCoreApplication::applicationFilePath(),{"--capture-exit-probe",marker});QVERIFY(process.waitForStarted());
        const bool finished=process.waitForFinished(5000);
        if(!finished){process.kill();process.waitForFinished(1000);}
        QVERIFY2(finished,"Log capture waited for a detached child which was waiting for the parent to exit");
        QCOMPARE(process.exitStatus(),QProcess::NormalExit);QCOMPARE(process.exitCode(),0);
        QVERIFY(QFile::exists(marker));QVERIFY(QFile::exists(marker+".child-done"));
    }
    void outputLogRawResetAndUpdateSchedule() {
        appSettings().clear();MainWindow window;window.show();
        auto* tabs=window.findChild<QTabWidget*>("settingsTabs");QCOMPARE(tabs->tabText(2),QString("Log"));
        auto* output=window.findChild<QPlainTextEdit*>("outputLog");QVERIFY(output->isReadOnly());QCOMPARE(output->maximumBlockCount(),5000);
        std::fputs("Native stdout marker\n",stdout);std::fflush(stdout);std::fputs("Native stderr marker\n",stderr);std::fflush(stderr);qWarning("Qt warning marker");
        QTRY_VERIFY(output->toPlainText().contains("Native stdout marker"));QTRY_VERIFY(output->toPlainText().contains("Native stderr marker"));QTRY_VERIFY(output->toPlainText().contains("Qt warning marker"));
        auto* automatic=window.findChild<QCheckBox*>("checkForUpdates");QVERIFY(automatic->isChecked());
        auto* timer=window.findChild<QTimer*>("hourlyUpdateCheck");QTRY_VERIFY(timer->isActive());QVERIFY(timer->interval()>0&&timer->interval()<=3600000);QCOMPARE(timer->timerType(),Qt::PreciseTimer);
        automatic->setChecked(false);QVERIFY(!timer->isActive());automatic->setChecked(true);QVERIFY(timer->isActive());
        auto* updater=window.findChild<Updater*>();updater->updateAvailableChanged(true);QVERIFY(window.findChild<QPushButton*>("updateApplication")->property("primary").toBool());updater->updateAvailableChanged(false);QVERIFY(!window.findChild<QPushButton*>("updateApplication")->property("primary").toBool());
        window.findChild<QComboBox*>("rawMode")->setCurrentIndex(2);window.findChild<QComboBox*>("rawWhiteBalance")->setCurrentIndex(8);
        window.findChild<QSpinBox*>("rawKelvin")->setValue(7500);window.findChild<QDoubleSpinBox*>("rawExposure")->setValue(1.5);window.findChild<QDoubleSpinBox*>("rawShoulder")->setValue(.8);window.findChild<QSpinBox*>("rawTargetNits")->setValue(5000);window.findChild<QCheckBox*>("rawCameraLook")->setChecked(false);
        window.findChild<QPushButton*>("resetRaw")->click();
        QCOMPARE(window.findChild<QComboBox*>("rawMode")->currentIndex(),1);QCOMPARE(window.findChild<QComboBox*>("rawWhiteBalance")->currentIndex(),0);QCOMPARE(window.findChild<QDoubleSpinBox*>("rawExposure")->value(),0.);QCOMPARE(window.findChild<QSpinBox*>("rawKelvin")->value(),5200);QCOMPARE(window.findChild<QSlider*>("rawShoulderSlider")->value(),50);QCOMPARE(window.findChild<QSlider*>("rawTargetNitsSlider")->value(),1500);QVERIFY(window.findChild<QCheckBox*>("rawCameraLook")->isChecked());
        const auto input=makeImage(root("log"),"input.jpg",64,48,100);window.addPaths({input});QTRY_COMPARE(window.catalog()->rowCount(),1);
        QSignalSpy finished(window.queue(),&ConversionQueue::batchFinished);jxrc::Options invalid;invalid.target=jxrc::Target(999);
        window.queue()->start({{input,root("log/bad-output"),invalid}},1,1);QTRY_COMPARE(finished.count(),1);QCOMPARE(finished[0][1].toInt(),1);QVERIFY(output->toPlainText().contains("Failed: "+input));
    }
    void silentUpdateChecks() {
        auto* network=new ReleaseNetwork;Updater updater(nullptr,network);QSignalSpy status(&updater,&Updater::statusChanged),messages(&updater,&Updater::message);
        network->response=QJsonDocument(QJsonObject{{"tag_name",QString::fromStdString(jxrc::version())}}).toJson();updater.check();QVERIFY(updater.busy());QVERIFY(!updater.installing());QTRY_VERIFY(!updater.busy());QCOMPARE(status.last()[0].toString(),QString("Latest"));QVERIFY(!updater.updateAvailable());
#ifdef Q_OS_WIN
        const QString system="Windows",extension=".zip";
#elif defined(Q_OS_MACOS)
        const QString system="Darwin",extension=".tar.gz";
#else
        const QString system="Linux",extension=".tar.gz";
#endif
        const QString name="JXRConverter-999.0.0-"+system+"-"+QSysInfo::buildCpuArchitecture()+"-desktop"+extension;
        network->response=QJsonDocument(QJsonObject{{"tag_name","v999.0.0"},{"assets",QJsonArray{QJsonObject{{"name",name},{"browser_download_url","https://example.invalid/package"}},QJsonObject{{"name","SHA256SUMS.txt"},{"browser_download_url","https://example.invalid/checksums"}}}}}).toJson();
        updater.check();QTRY_VERIFY(!updater.busy());QVERIFY(updater.updateAvailable());QCOMPARE(status.last()[0].toString(),QString("Update available"));QCOMPARE(network->requests,2);
        network->fail=true;updater.check();QTRY_VERIFY(!updater.busy());QVERIFY(messages.last()[0].toString().contains("Test network failure"));QVERIFY(!QApplication::activeModalWidget());QCOMPARE(network->requests,3);
    }
    void contextImageConversion() {
        appSettings().clear();MainWindow window;window.show();
        const auto a=makeImage(root("context-convert"),"a.jpg",64,48,100),b=makeImage(root("context-convert"),"b.jpg",64,48,100);window.addPaths({a,b});QTRY_COMPARE(window.catalog()->rowCount(),2);
        window.findChild<QLineEdit*>("outputDirectory")->setText(root("context-output"));
        auto* targets=window.findChild<QListWidget*>("conversionTargets");for(int i=0;i<targets->count();++i)targets->item(i)->setCheckState(targets->item(i)->data(Qt::UserRole).toInt()==4?Qt::Checked:Qt::Unchecked);
        auto* view=window.findChild<FileListView*>();view->selectAll();QCOMPARE(view->selectionModel()->selectedRows().size(),2);
        const auto index=window.filterModel()->index(1,0);QTRY_VERIFY(!view->visualRect(index).isEmpty());QSignalSpy finished(window.queue(),&ConversionQueue::batchFinished);
        bool chosen=false;
        QTimer::singleShot(0,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;auto* action=menu->findChild<QAction*>("convertImage");chosen=action&&action->isEnabled();if(chosen){menu->setActiveAction(action);QTest::keyClick(menu,Qt::Key_Return);}else menu->close();});
        view->customContextMenuRequested(view->visualRect(index).center());QVERIFY(chosen);QTRY_COMPARE(finished.count(),1);QCOMPARE(window.queue()->total(),1);QVERIFY(QFileInfo::exists(root("context-output")+"/b_sdr.avif"));QVERIFY(!QFileInfo::exists(root("context-output")+"/a_sdr.avif"));
        for(int i=0;i<targets->count();++i)targets->item(i)->setCheckState(targets->item(i)->data(Qt::UserRole).toInt()==2?Qt::Checked:Qt::Unchecked);
        chosen=false;window.findChild<QPushButton*>("statusAll")->click();
        QTimer::singleShot(0,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;auto* submenu=menu->findChild<QMenu*>("convertTo");auto* action=submenu?submenu->findChild<QAction*>("convertTarget5"):nullptr;chosen=action&&action->isEnabled();if(chosen){menu->setActiveAction(submenu->menuAction());QTest::keyClick(menu,Qt::Key_Right);submenu->setActiveAction(action);QTest::keyClick(submenu,Qt::Key_Return);}else menu->close();});
        view->customContextMenuRequested(view->visualRect(index).center());QVERIFY(chosen);QTRY_COMPARE(finished.count(),2);QCOMPARE(window.queue()->total(),1);QVERIFY(QFileInfo::exists(root("context-output")+"/b_pq.avif"));QCOMPARE(jxrc::inspect(std::filesystem::u8path((root("context-output")+"/b_pq.avif").toStdString())).transfer,std::string("PQ"));
        QCOMPARE(window.options().target,jxrc::Target::jxl);
    }
    void imageDateFallbackAndSingleDayMenu() {
        appSettings().clear();
        const auto directory=root("file-dates");QDir().mkpath(directory);
        auto path=directory+"/no-exif.png";
        QImage image(32,24,QImage::Format_RGB32);image.fill(Qt::green);QVERIFY(image.save(path));path=QFileInfo(path).canonicalFilePath();
        const QDateTime modified(QDate(2024,3,7),QTime(12,30));
        QFile file(path);QVERIFY(file.open(QIODevice::ReadWrite));QVERIFY(file.setFileTime(modified,QFileDevice::FileModificationTime));file.close();
        const auto record=readMetadata(path);QVERIFY(!record.captured.isValid());QCOMPARE(record.modified,modified);QCOMPARE(record.imageDate(),modified);QCOMPARE(record.created,QFileInfo(path).birthTime());
        Record creationOnly;creationOnly.created=modified;QCOMPARE(creationOnly.imageDate(),modified);
        const auto exifPath=makeImage(directory,"with-exif.jpg",32,24,100);
        const auto exif=readMetadata(exifPath);QCOMPARE(exif.imageDate(),exif.captured);QCOMPARE(exif.imageDate().date(),QDate(2025,6,12));
        Catalog model;CatalogFilter proxy;proxy.setSourceModel(&model);model.addPaths({path,exifPath});QTRY_COMPARE(model.rowCount(),2);QTRY_VERIFY(inspected(model));
        proxy.sort(Catalog::Captured);QCOMPARE(proxy.index(0,0).data(Catalog::PathRole).toString(),path);
        Filter filter;filter.from=modified.date();filter.to=modified.date();proxy.setFilter(filter);QCOMPARE(proxy.rowCount(),1);
        const int row=proxy.mapToSource(proxy.index(0,0)).row();
        QCOMPARE(model.index(row,Catalog::Captured).data().toString(),modified.toString("yyyy-MM-dd HH:mm:ss"));
        QCOMPARE(model.index(row,Catalog::Modified).data().toString(),modified.toString("yyyy-MM-dd HH:mm:ss"));
        QCOMPARE(model.index(row,Catalog::Created).data().toString(),record.created.toString("yyyy-MM-dd HH:mm:ss"));
        QVERIFY(model.index(row,Catalog::ExifDate).data().toString().isEmpty());
        MainWindow window;window.show();window.addPaths({path});QTRY_COMPARE(window.catalog()->rowCount(),1);QTRY_VERIFY(window.catalog()->record(0).inspected);
        auto* view=window.findChild<FileListView*>();const auto index=window.filterModel()->index(0,0);QTRY_VERIFY(!view->visualRect(index).isEmpty());
        bool enabled=false;
        QTimer::singleShot(0,&window,[&]{
            auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;
            auto* action=menu->findChild<QAction*>("setImageDate");enabled=action&&action->isEnabled();
            if(enabled){menu->setActiveAction(action);QTest::keyClick(menu,Qt::Key_Return);}else menu->close();
        });
        view->customContextMenuRequested(view->visualRect(index).center());QVERIFY(enabled);
        QVERIFY(window.findChild<QCheckBox*>("dateFilter")->isChecked());
        QCOMPARE(window.findChild<QDateEdit*>("dateFrom")->date(),modified.date());QCOMPARE(window.findChild<QDateEdit*>("dateTo")->date(),modified.date());QCOMPARE(window.filterModel()->rowCount(),1);
    }
    void contextFiltersAndLibraryLayout() {
        appSettings().clear();MainWindow window;window.show();
        QCOMPARE(window.windowTitle(),"JXRConverter v"+QString::fromStdString(jxrc::version()));
        auto* footer=window.findChild<QWidget*>("libraryActions");
        QCOMPARE(window.findChild<QLabel*>("catalogCounts")->parentWidget(),footer);
        const auto directory=root("field-filters");
        const auto a=makeImage(directory,"a.jpg",32,24,100),b=makeImage(directory,"b.jpg",64,48,400);
        window.findChild<QPushButton*>("statusFailed")->click();window.addPaths({directory});
        QVERIFY(window.findChild<QPushButton*>("statusAll")->isChecked());
        QTRY_COMPARE(window.catalog()->rowCount(),2);window.catalog()->setBackgroundMetadata(true);QTRY_VERIFY(inspected(*window.catalog()));
        auto* recent=window.findChild<QToolButton*>("recentFolders");QVERIFY(recent);QVERIFY(recent->menu());
        QCOMPARE(recent->popupMode(),QToolButton::InstantPopup);
        QMetaObject::invokeMethod(recent->menu(),"aboutToShow");
        QCOMPARE(recent->menu()->actions().first()->text(),QFileInfo(directory).canonicalFilePath());
        auto* mode=window.findChild<QButtonGroup*>("viewMode");mode->button(2)->click();
        auto* table=window.findChild<FileTableView*>();auto* search=window.findChild<QLineEdit*>("search");
        auto choose=[&](const QPoint& point,const QString& name){
            bool chosen=false;
            QTimer::singleShot(0,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;auto* action=menu->findChild<QAction*>(name);chosen=action&&action->isEnabled();if(chosen){menu->setActiveAction(action);QTest::keyClick(menu,Qt::Key_Return);}else menu->close();});
            table->customContextMenuRequested(point);return chosen;
        };
        const auto field=window.filterModel()->index(0,Catalog::Iso);table->scrollTo(field);QTest::qWait(10);
        QVERIFY(choose(table->visualRect(field).center(),"addFieldToFilters"));
        QCOMPARE(search->text(),QString("iso:\"100\""));QCOMPARE(window.filterModel()->rowCount(),1);
        QCOMPARE(window.filterModel()->index(0,0).data(Catalog::PathRole).toString(),a);
        window.findChild<QLineEdit*>("instantFilter")->setText("nothing");window.findChild<QCheckBox*>("todayFilter")->setChecked(true);
        window.findChild<QPushButton*>("statusFailed")->click();
        QVERIFY(choose(QPoint(-1,-1),"resetFilters"));QCOMPARE(window.filterModel()->rowCount(),2);
        QVERIFY(search->text().isEmpty());QVERIFY(window.findChild<QLineEdit*>("instantFilter")->text().isEmpty());
        QVERIFY(!window.findChild<QCheckBox*>("todayFilter")->isChecked());QVERIFY(window.findChild<QPushButton*>("statusAll")->isChecked());
        // Every displayed Details field supports an exact field-specific query.
        for(int column=0;column<Catalog::ColumnCount;++column){
            const auto value=window.catalog()->index(0,column).data().toString();if(value.isEmpty())continue;
            Filter f;f.search=catalogFieldKey(column)+":\""+value+"\"";window.filterModel()->setFilter(f);
            QVERIFY2(window.filterModel()->rowCount()>0,qPrintable(f.search));
        }
        window.findChild<QPushButton*>("statusFailed")->click();window.addPaths({b});
        QVERIFY(window.findChild<QPushButton*>("statusAll")->isChecked());QCOMPARE(window.filterModel()->rowCount(),2);
    }
    void libraryFooterAndStatusFilters() {
        appSettings().clear();MainWindow window;window.show();
        auto* progress=window.findChild<QProgressBar*>("conversionProgress");progress->setRange(0,100);progress->setValue(60);QCOMPARE(progress->text(),QString("60% (60/100)"));
        const auto a=makeImage(root("footer"),"a.jpg",32,24,100),b=makeImage(root("footer"),"b.jpg",32,24,100),c=makeImage(root("footer"),"c.jpg",32,24,100);
        window.addPaths({a,b,c});QTRY_COMPARE(window.catalog()->rowCount(),3);
        auto* footer=window.findChild<QWidget*>("libraryActions");auto* view=window.findChild<FileListView*>();
        footer->findChild<QPushButton*>("selectAllImages")->click();QCOMPARE(view->selectionModel()->selectedRows().size(),3);
        footer->findChild<QPushButton*>("deselectImages")->click();QVERIFY(view->selectionModel()->selectedRows().isEmpty());
        window.catalog()->setStatus(b,"Done");window.catalog()->setStatus(c,"Failed","failure");
        footer->findChild<QPushButton*>("statusFinished")->click();QCOMPARE(window.filterModel()->rowCount(),1);
        window.catalog()->setStatus(b,"Skipped");QCOMPARE(window.filterModel()->rowCount(),1);
        footer->findChild<QPushButton*>("statusFailed")->click();QCOMPARE(window.filterModel()->rowCount(),1);QCOMPARE(window.filterModel()->index(0,0).data(Catalog::PathRole).toString(),c);
        footer->findChild<QPushButton*>("statusAll")->click();QCOMPARE(window.filterModel()->rowCount(),3);
        QVERIFY(footer->findChild<QWidget*>("collectionSeparator"));QVERIFY(footer->findChild<QWidget*>("selectionSeparator"));
        footer->findChild<QPushButton*>("clearLibrary")->click();QCOMPARE(window.catalog()->rowCount(),0);
    }
    void selectionAcrossLibraryTabs() {
        appSettings().clear();MainWindow window;window.show();
        const auto a=makeImage(root("tab-selection"),"a.jpg",32,24,100),b=makeImage(root("tab-selection"),"b.jpg",32,24,100),c=makeImage(root("tab-selection"),"c.jpg",32,24,100);
        window.addPaths({a,b,c});QTRY_COMPARE(window.catalog()->rowCount(),3);
        auto* view=window.findChild<FileListView*>();auto* model=window.filterModel();auto* selection=view->selectionModel();
        selection->select(model->index(0,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
        selection->select(model->index(1,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
        window.catalog()->setStatus(b,"Done");window.catalog()->setStatus(c,"Failed");
        auto tab=[&](const char* name){if(QString::fromLatin1(name)=="statusJob")chooseJob(window,0);else window.findChild<QAbstractButton*>(name)->click();};
        tab("statusFinished");QCOMPARE(model->rowCount(),1);QCOMPARE(selection->selectedRows().size(),1);
        QVERIFY(window.findChild<QLabel*>("catalogCounts")->text().contains("2 selected"));
        tab("statusAll");QCOMPARE(selection->selectedRows().size(),2);
        tab("statusSelected");QCOMPARE(model->rowCount(),2);QCOMPARE(view->selectionMode(),QAbstractItemView::NoSelection);
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::ControlModifier,view->visualRect(model->index(0,0)).center());QCOMPARE(model->rowCount(),2);QCOMPARE(selection->selectedRows().size(),2);
        tab("statusRemember");QCOMPARE(model->rowCount(),2);QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(model->index(1,0)).center());
        tab("statusSelected");QCOMPARE(model->rowCount(),1);
        tab("statusAll");selection->select(model->index(0,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
        tab("statusJob");QCOMPARE(model->rowCount(),0);tab("statusSelected");QCOMPARE(model->rowCount(),2);
        tab("statusAll");auto* instant=window.findChild<QLineEdit*>("instantFilter");instant->setText("c.jpg");QCOMPARE(model->rowCount(),1);instant->clear();QCOMPARE(selection->selectedRows().size(),2);
        // Automatic status-driven row removal must not deselect hidden images.
        tab("statusFinished");window.catalog()->setStatus(b,"Failed");QCOMPARE(model->rowCount(),0);
        tab("statusAll");QTRY_COMPARE(selection->selectedRows().size(),2);
        window.findChild<QButtonGroup*>("viewMode")->button(2)->click();QCOMPARE(window.findChild<FileTableView*>()->selectionModel()->selectedRows().size(),2);
        tab("statusSelected");window.findChild<QPushButton*>("deselectImages")->click();QCOMPARE(model->rowCount(),0);
        tab("statusRemember");QCOMPARE(model->rowCount(),2);
    }
    void libraryTabScrollPositions() {
        appSettings().clear();MainWindow window;window.resize(1200,700);window.show();
        const auto directory=root("tab-scroll");const auto source=makeImage(directory,"00.jpg",32,24,100);QStringList files{source};
        for(int i=1;i<50;++i){const auto file=directory+QString("/%1.jpg").arg(i,2,10,QChar('0'));QVERIFY(QFile::copy(source,file));files.append(QFileInfo(file).canonicalFilePath());}
        window.addPaths(files);QTRY_COMPARE(window.catalog()->rowCount(),50);
        window.findChild<QButtonGroup*>("viewMode")->button(1)->click();window.findChild<QSlider*>("thumbnailSize")->setValue(100);
        auto* view=window.findChild<FileListView*>();auto* bar=view->verticalScrollBar();
        view->selectionModel()->select(QItemSelection(window.filterModel()->index(0,0),window.filterModel()->index(49,0)),QItemSelectionModel::Select|QItemSelectionModel::Rows);for(int row=0;row<25;++row)window.catalog()->setStatus(files[row],"Done");
        QTRY_VERIFY(bar->maximum()>0);QTest::qWait(20);const int allPosition=bar->maximum()/2;bar->setValue(allPosition);
        window.findChild<QPushButton*>("statusFinished")->click();QTest::qWait(20);QVERIFY(bar->maximum()>0);const int finishedPosition=bar->maximum();bar->setValue(finishedPosition);
        window.findChild<QPushButton*>("statusRemember")->click();QTest::qWait(20);const int rememberedPosition=bar->maximum()/3;bar->setValue(rememberedPosition);
        window.findChild<QPushButton*>("statusAll")->click();QTRY_COMPARE(bar->value(),allPosition);
        window.findChild<QPushButton*>("statusFinished")->click();QTRY_COMPARE(bar->value(),finishedPosition);
        window.findChild<QPushButton*>("statusRemember")->click();QTRY_COMPARE(bar->value(),rememberedPosition);
    }
    void jobFileListsAndRestore() {
        appSettings().clear();const auto a=makeImage(root("job-history"),"a.jpg",64,48,100),b=makeImage(root("job-history"),"b.jpg",64,48,100),c=makeImage(root("job-history"),"c.jpg",64,48,100);
        {
            MainWindow window;window.findChild<QCheckBox*>("loadPreviousJobs")->setChecked(true);window.show();window.addPaths({a,b});QTRY_COMPARE(window.catalog()->rowCount(),2);
            window.findChild<QLineEdit*>("outputDirectory")->setText(root("job-history-output"));QSignalSpy done(window.queue(),&ConversionQueue::batchFinished);
            window.findChild<QPushButton*>("convertVisible")->click();QTRY_COMPARE(done.count(),1);
            auto* menu=window.findChild<QMenu*>("jobMenu");QCOMPARE(menu->actions().size(),2);QVERIFY(savedJob(window,1)->isChecked());QCOMPARE(savedJob(window,1)->text(),QString("Job #1 (2/2/0/0)"));
            window.findChild<QPushButton*>("statusAll")->click();
            auto* view=window.findChild<FileListView*>();view->selectionModel()->select(window.filterModel()->index(0,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
            window.findChild<QPushButton*>("convertSelected")->click();QTRY_COMPARE(done.count(),2);QVERIFY(savedJob(window,2)->isChecked());QCOMPARE(savedJob(window,2)->text(),QString("Job #2 (0/1/1/0)"));
            window.addPaths({c});QTRY_COMPARE(window.catalog()->rowCount(),3);
            auto single=[&](int row){bool chosen=false;QTimer::singleShot(0,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;auto* action=menu->findChild<QAction*>("convertImage");chosen=action&&action->isEnabled();if(chosen){menu->setActiveAction(action);QTest::keyClick(menu,Qt::Key_Return);}else menu->close();});view->customContextMenuRequested(view->visualRect(window.filterModel()->index(row,0)).center());return chosen;};
            QVERIFY(single(2));QTRY_COMPARE(done.count(),3);window.findChild<QPushButton*>("statusAll")->click();QVERIFY(single(1));QTRY_COMPARE(done.count(),4);
            QCOMPARE(menu->actions().size(),3);QVERIFY(savedJob(window,0)->isChecked());
            QVERIFY(chooseJob(window,0));QCOMPARE(window.filterModel()->rowCount(),2);
            QVERIFY(chooseJob(window,1));QCOMPARE(window.filterModel()->rowCount(),2);
            const auto selected=view->selectionModel()->selectedRows().size();
            QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(window.filterModel()->index(0,0)).center());QCOMPARE(view->selectionModel()->selectedRows().size(),1);QCOMPARE(selected,0);
            window.findChild<QPushButton*>("statusRemember")->click();window.findChild<QPushButton*>("statusSelected")->click();QVERIFY(savedJob(window,1)->isChecked());
            QFile file(configDirectory()+"/jobs.json");QVERIFY(file.open(QIODevice::ReadOnly));const auto json=QJsonDocument::fromJson(file.readAll()).object();QCOMPARE(json.value("jobs").toArray().size(),2);QCOMPARE(json.value("temporary").toArray().size(),2);
        }
        MainWindow restored;restored.show();QTest::qWait(60);QCOMPARE(restored.catalog()->rowCount(),0);QVERIFY(!restored.queue()->busy());
        QCOMPARE(restored.findChild<QMenu*>("jobMenu")->actions().size(),3);QVERIFY(savedJob(restored,1)->isChecked());
        QVERIFY(chooseJob(restored,1));QTRY_COMPARE(restored.filterModel()->rowCount(),2);QCOMPARE(restored.catalog()->rowCount(),2);
        QVERIFY(chooseJob(restored,2));QCOMPARE(restored.filterModel()->rowCount(),1);QCOMPARE(restored.filterModel()->index(0,0).data(Catalog::PathRole).toString(),a);
    }
    void jobStartupPreferencesAndClear() {
        appSettings().clear();const auto directory=root("job-startup");const auto a=makeImage(directory,"a.jpg",64,48,100),b=makeImage(directory,"b.jpg",64,48,100);
        QJsonObject job;job.insert("number","7");job.insert("files",QJsonArray{a,b});QJsonObject jobs;jobs.insert("jobs",QJsonArray{job});jobs.insert("temporary",QJsonArray{b});jobs.insert("viewedJob","7");
        const auto saved=QJsonDocument(jobs).toJson();QFile file(configDirectory()+"/jobs.json");QVERIFY(file.open(QIODevice::WriteOnly));file.write(saved);file.close();
        QJsonObject history;history.insert("folders",QJsonArray{directory});history.insert("files",QJsonArray{a});history.insert("lastFiles",QJsonArray{a});QFile recent(configDirectory()+"/history.json");QVERIFY(recent.open(QIODevice::WriteOnly));recent.write(QJsonDocument(history).toJson());recent.close();
        appSettings().setValue("jobs/nextNumber",12);
        {
            MainWindow window;window.show();QTest::qWait(80);QCOMPARE(window.catalog()->rowCount(),0);
            QVERIFY(!window.findChild<QCheckBox*>("loadLastFolder")->isChecked());QVERIFY(!window.findChild<QCheckBox*>("loadLastFiles")->isChecked());QVERIFY(!window.findChild<QCheckBox*>("loadPreviousJobs")->isChecked());
            QCOMPARE(window.findChild<QMenu*>("jobMenu")->actions().size(),1);QCOMPARE(window.findChild<QToolButton*>("statusJob")->text(),QString("Jobs"));
            auto* icon=window.findChild<QPushButton*>("openQueueOutputFolder");auto* all=window.findChild<QPushButton*>("convertVisible");auto* selected=window.findChild<QPushButton*>("convertSelected");QVERIFY(icon->x()<all->x());QVERIFY(all->x()<selected->x());
        }
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),saved);file.close();
        appSettings().setValue("controls/loadPreviousJobs",true);
        {
            MainWindow window;window.show();QTest::qWait(80);QCOMPARE(window.catalog()->rowCount(),0);QVERIFY(window.findChild<QCheckBox*>("loadPreviousJobs")->isChecked());
            QCOMPARE(window.findChild<QMenu*>("jobMenu")->actions().size(),2);QVERIFY(chooseJob(window,7));QTRY_COMPARE(window.catalog()->rowCount(),2);
            window.findChild<QPushButton*>("statusAll")->click();QCOMPARE(window.findChild<QToolButton*>("statusJob")->text(),QString("Jobs"));
            window.findChild<QLineEdit*>("outputDirectory")->setText(root("job-startup-output"));QSignalSpy done(window.queue(),&ConversionQueue::batchFinished);
            window.findChild<QPushButton*>("convertVisible")->click();QTRY_COMPARE(done.count(),1);QVERIFY(savedJob(window,12));QCOMPARE(appSettings().value("jobs/nextNumber").toLongLong(),13);
            window.findChild<QAction*>("clearJobs")->trigger();QCOMPARE(window.findChild<QMenu*>("jobMenu")->actions().size(),1);QCOMPARE(window.catalog()->rowCount(),2);QCOMPARE(appSettings().value("jobs/nextNumber").toLongLong(),1);
            QCOMPARE(window.findChild<QToolButton*>("statusJob")->text(),QString("Jobs"));
            window.findChild<QPushButton*>("convertVisible")->click();QTRY_COMPARE(done.count(),2);QVERIFY(savedJob(window,1));QVERIFY(!savedJob(window,12));
            window.findChild<QCheckBox*>("loadPreviousJobs")->setChecked(false);
        }
        {
            MainWindow window;window.show();QTest::qWait(80);QCOMPARE(window.catalog()->rowCount(),0);QCOMPARE(window.findChild<QMenu*>("jobMenu")->actions().size(),1);
            window.addPaths({a});QTRY_COMPARE(window.catalog()->rowCount(),1);QSignalSpy done(window.queue(),&ConversionQueue::batchFinished);window.findChild<QPushButton*>("convertVisible")->click();QTRY_COMPARE(done.count(),1);QVERIFY(savedJob(window,2));QVERIFY(!savedJob(window,1));
        }
    }
    void selectAllRememberAndTabClear() {
        appSettings().clear();MainWindow window;window.show();
        const auto a=makeImage(root("bulk-remember"),"a.jpg",32,24,100),b=makeImage(root("bulk-remember"),"b.jpg",32,24,100),c=makeImage(root("bulk-remember"),"c.jpg",32,24,100);
        window.addPaths({a,b,c});QTRY_COMPARE(window.catalog()->rowCount(),3);auto* view=window.findChild<FileListView*>();auto* model=window.filterModel();
        auto tab=[&](const char* name){if(QString::fromLatin1(name)=="statusJob")chooseJob(window,0);else window.findChild<QAbstractButton*>(name)->click();};
        window.findChild<QPushButton*>("selectAllImages")->click();tab("statusRemember");QCOMPARE(model->rowCount(),0);
        tab("statusAll");window.findChild<QPushButton*>("deselectImages")->click();view->setFocus();QTest::keyClick(view,Qt::Key_A,Qt::ControlModifier);QCOMPARE(view->selectionModel()->selectedRows().size(),3);tab("statusRemember");QCOMPARE(model->rowCount(),0);
        tab("statusAll");window.findChild<QPushButton*>("deselectImages")->click();view->selectAll();tab("statusRemember");QCOMPARE(model->rowCount(),0);
        tab("statusAll");window.findChild<QPushButton*>("deselectImages")->click();
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(model->index(0,0)).center());
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::ControlModifier,view->visualRect(model->index(1,0)).center());tab("statusRemember");QCOMPARE(model->rowCount(),2);
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::ControlModifier,view->visualRect(model->index(0,0)).center());tab("statusSelected");QCOMPARE(model->rowCount(),1);
        window.findChild<QPushButton*>("clearLibrary")->click();QCOMPARE(model->rowCount(),0);QCOMPARE(window.catalog()->rowCount(),3);
        tab("statusRemember");window.findChild<QPushButton*>("clearLibrary")->click();QCOMPARE(model->rowCount(),0);QCOMPARE(window.catalog()->rowCount(),3);
        window.catalog()->setStatus(b,"Done");window.catalog()->setStatus(c,"Failed");tab("statusFinished");window.findChild<QPushButton*>("clearLibrary")->click();QCOMPARE(window.catalog()->rowCount(),2);
        tab("statusFailed");QCOMPARE(model->rowCount(),1);QCOMPARE(model->index(0,0).data(Catalog::PathRole).toString(),c);
    }
    void jobFoldersRunningAndSnapshot() {
        appSettings().clear();MainWindow window;window.show();
        const auto a=makeImage(root("job-folders"),"a.jpg",64,48,100),b=makeImage(root("job-folders"),"b.jpg",64,48,100);
        window.addPaths({a,b});QTRY_COMPARE(window.catalog()->rowCount(),2);
        const auto output=root("job-folders-output");window.findChild<QLineEdit*>("outputDirectory")->setText(output);
        window.findChild<QSpinBox*>("parallelJobs")->setValue(1);window.findChild<QCheckBox*>("jobSubfolders")->setChecked(true);window.findChild<QCheckBox*>("dateSubfolders")->setChecked(true);
        QSignalSpy done(window.queue(),&ConversionQueue::batchFinished);window.findChild<QPushButton*>("convertVisible")->click();
        auto* jobButton=window.findChild<QToolButton*>("statusJob");QVERIFY(jobButton->isChecked());QVERIFY(jobButton->menu());QCOMPARE(jobButton->popupMode(),QToolButton::InstantPopup);QCOMPARE(jobButton->popupMode(),window.findChild<QToolButton*>("recentFolders")->popupMode());QCOMPARE(jobButton->height(),window.findChild<QPushButton*>("runningJob")->height());QCOMPARE(window.findChild<QPushButton*>("clearLibrary")->text(),QString("Clear"));QVERIFY(!window.findChild<QPushButton*>("clearLibrary")->isEnabled());QVERIFY(window.findChild<QPushButton*>("cancelJob")->isVisible());QVERIFY(jobButton->text().endsWith("*"));QVERIFY(savedJob(window,1)->text().endsWith("*"));
        window.findChild<QPushButton*>("statusAll")->click();QVERIFY(!window.findChild<QPushButton*>("cancelJob")->isVisible());QVERIFY(savedJob(window,1)->text().endsWith("*"));window.findChild<QPushButton*>("runningJob")->click();QVERIFY(jobButton->isChecked());QVERIFY(savedJob(window,1)->isChecked());
        // UI edits after submission must not move the pending output.
        window.findChild<QLineEdit*>("outputDirectory")->setText(root("different-job-output"));window.findChild<QCheckBox*>("jobSubfolders")->setChecked(false);
        QTRY_COMPARE(done.count(),1);const auto folder=output+"/"+QDate::currentDate().toString(Qt::ISODate)+"-Job-1";
        QVERIFY(QFileInfo::exists(folder+"/a_sdr.avif"));QVERIFY(QFileInfo::exists(folder+"/b_sdr.avif"));QVERIFY(!QFileInfo::exists(root("different-job-output")+"/b_sdr.avif"));
        QCOMPARE(jobButton->text(),QString("Job #1 (2/2/0/0)"));QVERIFY(!savedJob(window,1)->text().endsWith("*"));QVERIFY(window.findChild<QPushButton*>("cancelJob")->x()<jobButton->x());QCOMPARE(savedJob(window,1)->text(),jobButton->text());
        FolderReceiver receiver;QDesktopServices::setUrlHandler("file",&receiver,"open");auto restore=qScopeGuard([]{QDesktopServices::unsetUrlHandler("file");});
        auto* icon=window.findChild<QPushButton*>("openQueueOutputFolder");QVERIFY(icon);QVERIFY(!icon->icon().isNull());icon->click();QCOMPARE(receiver.opened.toLocalFile(),folder);
        QTest::mouseMove(jobButton,jobButton->rect().center());QTest::qWait(450);QVERIFY(!window.findChild<QMenu*>("jobMenu")->isVisible());
        QSignalSpy opened(jobButton->menu(),&QMenu::aboutToShow);QTimer::singleShot(100,jobButton->menu(),&QMenu::close);QTest::mouseClick(jobButton,Qt::LeftButton);QTRY_COMPARE(opened.count(),1);QTRY_VERIFY(!jobButton->menu()->isVisible());
    }
    void independentJobSelectionsAndConvertSelected() {
        appSettings().clear();MainWindow window;window.show();
        const auto directory=root("job-selections"),output=root("job-selection-output");
        const auto a=makeImage(directory,"a.jpg",64,48,100),b=makeImage(directory,"b.jpg",64,48,100),c=makeImage(directory,"c.jpg",64,48,100);
        window.addPaths({a,b,c});QTRY_COMPARE(window.catalog()->rowCount(),3);window.findChild<QLineEdit*>("outputDirectory")->setText(output);
        auto* view=window.findChild<FileListView*>();auto* model=window.filterModel();auto* selection=view->selectionModel();
        selection->select(model->index(2,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
        QSignalSpy done(window.queue(),&ConversionQueue::batchFinished);window.findChild<QPushButton*>("convertVisible")->click();
        // Selecting work in an already running job promotes its pending output
        // without creating another batch or submitting the output twice.
        selection->select(model->index(2,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
        QVERIFY(window.findChild<QPushButton*>("convertSelected")->isEnabled());window.findChild<QPushButton*>("convertSelected")->click();
        QCOMPARE(window.queue()->total(),3);QCOMPARE(window.findChild<QMenu*>("jobMenu")->actions().size(),2);
        window.findChild<QPushButton*>("deselectImages")->click();QTRY_COMPARE(done.count(),1);
        QCOMPARE(view->selectionMode(),QAbstractItemView::ExtendedSelection);QCOMPARE(selection->selectedRows().size(),0);
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(model->index(0,0)).center());
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::ControlModifier,view->visualRect(model->index(1,0)).center());QCOMPARE(selection->selectedRows().size(),2);
        window.findChild<QPushButton*>("statusSelected")->click();QCOMPARE(model->rowCount(),1);QCOMPARE(model->index(0,0).data(Catalog::PathRole).toString(),c);
        QVERIFY(chooseJob(window,1));QCOMPARE(selection->selectedRows().size(),2);
        window.findChild<QLineEdit*>("instantFilter")->setText("a.jpg");QCOMPARE(selection->selectedRows().size(),1);window.findChild<QLineEdit*>("instantFilter")->clear();QCOMPARE(selection->selectedRows().size(),2);
        window.findChild<QPushButton*>("deselectImages")->click();QCOMPARE(selection->selectedRows().size(),0);window.findChild<QPushButton*>("selectAllImages")->click();QCOMPARE(selection->selectedRows().size(),3);
        window.findChild<QPushButton*>("convertVisible")->click();QTRY_COMPARE(done.count(),2);QVERIFY(savedJob(window,2)->isChecked());QCOMPARE(selection->selectedRows().size(),0);
        QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(model->index(2,0)).center());QCOMPARE(selection->selectedRows().size(),1);
        QVERIFY(chooseJob(window,1));QCOMPARE(selection->selectedRows().size(),3);QVERIFY(chooseJob(window,2));QCOMPARE(selection->selectedRows().size(),1);
        const auto jobs=window.findChild<QMenu*>("jobMenu")->actions().size();QVERIFY(QFile::remove(output+"/c_sdr.avif"));
        window.findChild<QPushButton*>("convertSelected")->click();QTRY_COMPARE(done.count(),3);
        QCOMPARE(window.queue()->total(),1);QVERIFY(QFileInfo::exists(output+"/c_sdr.avif"));QVERIFY(savedJob(window,2)->isChecked());
        QCOMPARE(window.findChild<QMenu*>("jobMenu")->actions().size(),jobs);QCOMPARE(model->rowCount(),3);QCOMPARE(selection->selectedRows().size(),1);
        QCOMPARE(savedJob(window,2)->text(),QString("Job #2 (1/3/2/0)"));QCOMPARE(savedJob(window,1)->text(),QString("Job #1 (3/3/0/0)"));
        window.findChild<QPushButton*>("statusSelected")->click();QCOMPARE(model->rowCount(),1);QCOMPARE(model->index(0,0).data(Catalog::PathRole).toString(),c);
        QVERIFY(chooseJob(window,2));window.findChild<QPushButton*>("convertVisible")->click();QTRY_COMPARE(done.count(),4);QVERIFY(savedJob(window,3));
    }
    void jobCountsAcrossOutputsAndRestore() {
        appSettings().clear();const auto file=makeImage(root("job-counts"),"photo.jpg",64,48,100);
        QJsonObject outputs;
        for(const auto& state:{QString("Failed"),QString("Cancelled")}){QJsonObject output;output.insert("input",file);output.insert("state",state);outputs.insert(root("job-counts")+"/"+state,output);}
        QJsonObject job;job.insert("number","3");job.insert("files",QJsonArray{file});job.insert("outputs",outputs);
        QJsonObject json;json.insert("jobs",QJsonArray{job});json.insert("viewedJob","3");QFile history(configDirectory()+"/jobs.json");QVERIFY(history.open(QIODevice::WriteOnly));history.write(QJsonDocument(json).toJson());history.close();
        appSettings().setValue("controls/loadPreviousJobs",true);MainWindow window;window.show();QCOMPARE(window.catalog()->rowCount(),0);QVERIFY(chooseJob(window,3));QTRY_COMPARE(window.catalog()->rowCount(),1);
        QCOMPARE(savedJob(window,3)->text(),QString("Job #3 (0/1/0/1)"));
        QCOMPARE(window.findChild<QToolButton*>("statusJob")->text(),savedJob(window,3)->text());QVERIFY(!window.queue()->busy());
    }
    void numberedJobCancellationAndOptionsSnapshot() {
        QSemaphore gate;QVector<Job> converted;
        ConversionQueue queue(nullptr,[&](const std::filesystem::path& input,const std::filesystem::path& output,const jxrc::Options& options){if(input.filename()=="a")gate.acquire();converted.append({QString::fromStdString(input.string()),QString::fromStdString(output.string()),options});return jxrc::Result{};});
        jxrc::Options first;first.quality.codec_quality=42;first.exposure=1.5f;Job a{"a",root("snapshots/a"),first,1},b{"b",root("snapshots/b"),first,1};
        QSignalSpy done(&queue,&ConversionQueue::batchFinished),outputs(&queue,&ConversionQueue::outputFinished);
        queue.start({a,b},1,4);jxrc::Options second;second.quality.codec_quality=17;second.exposure=-2;Job c{"c",root("snapshots/c"),second,2};queue.enqueue({c},3,12);
        auto promote=b;promote.batch=0;queue.enqueueNext({promote},1,4);const bool pending=queue.hasPendingBatch(0);queue.cancelBatch(0);
        a.options.quality.codec_quality=99;c.options.exposure=8;c.output="changed";gate.release();
        QVERIFY(pending);QVERIFY(!queue.hasPendingBatch(0));QVERIFY(queue.hasPendingBatch(2));QTRY_COMPARE(done.count(),1);
        QCOMPARE(done[0][0].toInt(),2);QCOMPARE(done[0][2].toInt(),1);QCOMPARE(converted.size(),2);
        QCOMPARE(converted[0].options.quality.codec_quality,42);QCOMPARE(converted[0].options.exposure,1.5f);QCOMPARE(converted[1].options.quality.codec_quality,17);QCOMPARE(converted[1].options.exposure,-2.f);QCOMPARE(converted[1].output,root("snapshots/c"));
        bool owner=false,alias=false;for(const auto& value:outputs)if(value[3].toString()=="Cancelled"){owner|=value[0].toLongLong()==1;alias|=value[0].toLongLong()==0;}QVERIFY(owner);QVERIFY(alias);
    }
    void runningJobFlagsAndSharedPriorityOutput() {
        QSemaphore gate;ConversionQueue queue(nullptr,[&](const std::filesystem::path&,const std::filesystem::path&,const jxrc::Options&){gate.acquire();return jxrc::Result{};});
        const Job a{"a",root("running-flags/a"),{},1},b{"b",root("running-flags/b"),{},2},c{"c",root("running-flags/c"),{},3};
        QSignalSpy done(&queue,&ConversionQueue::batchFinished);queue.start({a,b,c},2,2);
        const bool first=queue.hasRunningBatch(1),second=queue.hasRunningBatch(2),pending=queue.hasRunningBatch(3);
        auto single=a;single.batch=0;queue.enqueueNext({single},2,2);const bool promoted=queue.hasRunningBatch(0);gate.release(3);
        QVERIFY(first);QVERIFY(second);QVERIFY(!pending);QVERIFY(promoted);QCOMPARE(queue.total(),3);QTRY_COMPARE(done.count(),1);
        for(qint64 number=0;number<4;++number)QVERIFY(!queue.hasRunningBatch(number));
    }
    void sendSelectedJobAndCancelJobButton() {
        appSettings().clear();MainWindow window;window.show();
        const auto directory=root("send-job");QStringList files;for(int i=0;i<3;++i)files.append(makeImage(directory,QString::number(i)+".jpg",64,48,100));
        window.addPaths(files);QTRY_COMPARE(window.catalog()->rowCount(),3);window.findChild<QLineEdit*>("outputDirectory")->setText(root("send-job-output"));window.findChild<QSpinBox*>("parallelJobs")->setValue(1);
        window.findChild<QPushButton*>("selectAllImages")->click();auto* view=window.findChild<FileListView*>();bool chosen=false;
        QTimer::singleShot(0,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;auto* action=menu->findChild<QAction*>("sendSelectedToJob");chosen=action&&action->isEnabled();if(chosen){menu->setActiveAction(action);QTest::keyClick(menu,Qt::Key_Return);}else menu->close();});
        QSignalSpy done(window.queue(),&ConversionQueue::batchFinished);view->customContextMenuRequested(view->visualRect(window.filterModel()->index(0,0)).center());QVERIFY(chosen);
        QVERIFY(savedJob(window,1)->isChecked());QVERIFY(window.findChild<QToolButton*>("statusJob")->isChecked());QCOMPARE(window.queue()->total(),0);QVERIFY(!window.queue()->busy());QCOMPARE(done.count(),0);QVERIFY(savedJob(window,1)->text().endsWith("(?)"));
        window.findChild<QPushButton*>("convertVisible")->click();QCOMPARE(window.queue()->total(),3);QVERIFY(savedJob(window,1)->isChecked());QVERIFY(!savedJob(window,1)->text().endsWith("(?)"));QVERIFY(!savedJob(window,2));
        auto* cancel=window.findChild<QPushButton*>("cancelJob");QCOMPARE(cancel->text(),QString("Cancel job"));QVERIFY(cancel->isEnabled());cancel->click();
        QTRY_COMPARE(done.count(),1);QCOMPARE(done[0][0].toInt(),1);QCOMPARE(done[0][2].toInt(),2);QCOMPARE(window.catalog()->rowCount(),3);QCOMPARE(window.filterModel()->rowCount(),3);
    }
    void collapsedQueueAndDateFolders() {
        appSettings().clear();
        MainWindow window;window.show();QTest::qWait(20);
        QVERIFY(window.options().keep_exif);
        QCOMPARE(window.options().raw.hdr_mode,jxrc::RawHdrMode::boost);
        auto* toggle=window.findChild<QPushButton*>("toggleOutput");
        auto* all=window.findChild<QPushButton*>("convertVisible");
        auto* selected=window.findChild<QPushButton*>("convertSelected");
        auto* stop=window.findChild<QPushButton*>("stopProgress");
        toggle->setChecked(false);
        QVERIFY(all->isVisible());QVERIFY(selected->isVisible());
        QVERIFY(!window.findChild<QLineEdit*>("outputDirectory")->isVisible());
        QVERIFY(!selected->isEnabled());QVERIFY(selected->property("noSelection").toBool());
        QVERIFY(!stop->isVisible());
        const QLocale previous;
        for(const auto& locale:{QLocale(QLocale::English,QLocale::UnitedStates),QLocale(QLocale::German,QLocale::Germany)}) {
            QLocale::setDefault(locale);
            auto expected=locale.toString(QDate::currentDate(),QLocale::ShortFormat);expected.replace('/','-');
            QCOMPARE(outputDateFolder(),expected);
            OutputLayout layout;layout.date_subfolders=true;layout.format_subfolders=true;
            jxrc::Options options;options.target=jxrc::Target::avif;
            const auto source=root("dated/source.jpg");
            const auto jobs=makeJobs({source},root("dated/output"),options,layout);
            QCOMPARE(QFileInfo(jobs.first().output).absolutePath(),root("dated/output")+"/"+expected+"/avif");
            layout.source_folder=true;
            QCOMPARE(QFileInfo(makeJobs({source},{},options,layout).first().output).absolutePath(),root("dated")+"/"+expected+"/avif");
        }
        QLocale::setDefault(previous);
        window.findChild<QCheckBox*>("dateSubfolders")->setChecked(true);
        const auto input=makeImage(root("queue-ui"),"source.jpg",32,24,100);
        QDir().mkpath(root("queue-ui/output"));
        jxrc::Options options;options.target=jxrc::Target::avif;
        QSignalSpy finished(window.queue(),&ConversionQueue::batchFinished);
        window.queue()->start(makeJobs({input,input},root("queue-ui/output"),options),1,1);
        QVERIFY(stop->isVisible());
        auto* cancel=window.findChild<QPushButton*>("cancelQueue");
        QVERIFY(cancel->isEnabled());QVERIFY(cancel->property("running").toBool());
        stop->click();QTRY_COMPARE(finished.count(),1);
        QCOMPARE(finished[0][2].toInt(),1);
        QVERIFY(!stop->isVisible());QVERIFY(!cancel->isEnabled());
    }
    void controlsSnapshot() {
        MainWindow window;
        auto* keep=window.findChild<QCheckBox*>("convertGamut");QVERIFY(keep);QVERIFY(keep->isChecked());
        auto* mode=window.findChild<QComboBox*>("conversionMode");mode->setCurrentIndex(1);
        auto* targets=window.findChild<QListWidget*>("conversionTargets");QVERIFY(targets);
        for(int i=0;i<targets->count();++i)targets->item(i)->setCheckState(targets->item(i)->data(Qt::UserRole).toInt()==8?Qt::Checked:Qt::Unchecked);
        QCOMPARE(window.options().target,jxrc::Target::jxl_scrgb);
        for(int i=0;i<targets->count();++i)targets->item(i)->setCheckState(targets->item(i)->data(Qt::UserRole).toInt()==6?Qt::Checked:Qt::Unchecked);
        window.findChild<QComboBox*>("qualityPreset")->setCurrentIndex(4);
        window.findChild<QSpinBox*>("codecQuality")->setValue(93);window.findChild<QDoubleSpinBox*>("jxlDistance")->setValue(1.25);
        window.findChild<QComboBox*>("avifDepth")->setCurrentIndex(1);window.findChild<QCheckBox*>("downsizeGainmap")->setChecked(true);
        window.findChild<QComboBox*>("rawMode")->setCurrentIndex(2);window.findChild<QDoubleSpinBox*>("rawExposure")->setValue(0.7);
        window.findChild<QComboBox*>("rawWhiteBalance")->setCurrentIndex(8);window.findChild<QSpinBox*>("rawKelvin")->setValue(6400);
        window.findChild<QDoubleSpinBox*>("rawShoulder")->setValue(0.8);window.findChild<QSpinBox*>("rawTargetNits")->setValue(2000);
        const auto o=window.options();QCOMPARE(o.mode,jxrc::Mode::process);QCOMPARE(o.target,jxrc::Target::avif_gainmap);QCOMPARE(o.quality.codec_quality,93);
        QCOMPARE(o.quality.avif_depth,12);QCOMPARE(o.quality.jxl_distance,1.25f);QVERIFY(o.quality.downsize_gainmap);
        QCOMPARE(o.raw.hdr_mode,jxrc::RawHdrMode::custom);QCOMPARE(o.raw.exposure,0.7f);QCOMPARE(o.raw.white_balance,8);QCOMPARE(o.raw.kelvin,6400);
        mode->setCurrentIndex(0);const auto preserved=window.options();QCOMPARE(preserved.mode,jxrc::Mode::preserve);QCOMPARE(preserved.raw.exposure,0.f);QCOMPARE(preserved.raw.hdr_mode,jxrc::RawHdrMode::source);
        QVERIFY(!window.findChild<QDoubleSpinBox*>("rawExposure")->isEnabled());
    }
    void jpegTranscodingControls() {
        MainWindow window;
        auto* mode=window.findChild<QComboBox*>("conversionMode");mode->setCurrentIndex(1);
        auto* targets=window.findChild<QListWidget*>("conversionTargets");
        for(int i=0;i<targets->count();++i)targets->item(i)->setCheckState(targets->item(i)->data(Qt::UserRole).toInt()==2?Qt::Checked:Qt::Unchecked);
        auto* exposure=window.findChild<QDoubleSpinBox*>("exposure");exposure->setValue(0);
        auto* edge=window.findChild<QSpinBox*>("maxEdge");edge->setValue(0);
        auto* transcode=window.findChild<QCheckBox*>("jpegTranscoding");QVERIFY(transcode);
        QVERIFY(transcode->isEnabled());transcode->setChecked(true);QVERIFY(window.options().jpeg_transcoding);
        exposure->setValue(1);QVERIFY(!transcode->isEnabled());QVERIFY(!window.options().jpeg_transcoding);
        exposure->setValue(0);QVERIFY(transcode->isEnabled());QVERIFY(transcode->isChecked());
        edge->setValue(32);QVERIFY(!transcode->isEnabled());QVERIFY(!window.options().jpeg_transcoding);
        edge->setValue(0);QVERIFY(transcode->isEnabled());QVERIFY(window.options().jpeg_transcoding);
        mode->setCurrentIndex(0);QVERIFY(transcode->isEnabled());QVERIFY(window.options().jpeg_transcoding);
        transcode->setChecked(false);
    }
    void queueParallelismFailureAndCancel() {
        std::atomic<int> active{0},peak{0},invalidBudget{0};
        ConversionQueue queue(nullptr,[&](const std::filesystem::path& input,const std::filesystem::path&,const jxrc::Options& o) {
            if(o.threads!=4)++invalidBudget;
            const int now=++active;int old=peak.load();while(now>old&&!peak.compare_exchange_weak(old,now)){}
            QThread::msleep(35);--active;if(input.string().find("bad")!=std::string::npos)throw std::runtime_error("expected failure");return jxrc::Result{};
        });
        QSignalSpy done(&queue,&ConversionQueue::batchFinished);QSignalSpy states(&queue,&ConversionQueue::statusChanged);
        QVector<Job> jobs;for(int i=0;i<7;++i)jobs.append({i==3?"bad":QString::number(i),QString("out%1").arg(i),{}});
        queue.start(jobs,3,12);QTRY_COMPARE(done.count(),1);QCOMPARE(peak.load(),3);QCOMPARE(invalidBudget.load(),0);QCOMPARE(done[0][0].toInt(),6);QCOMPARE(done[0][1].toInt(),1);QVERIFY(!queue.busy());
        queue.cancelPending();QCOMPARE(done.count(),1);
        peak=0;invalidBudget=0;jobs.removeAt(3);queue.start(jobs,2,8);queue.cancelPending();QTRY_COMPARE(done.count(),2);
        QCOMPARE(done[1][0].toInt(),2);QCOMPARE(done[1][2].toInt(),4);QVERIFY(peak.load()<=2);QCOMPARE(invalidBudget.load(),0);
        QCOMPARE(recommendedWorkers(16),4);QCOMPARE(recommendedWorkers(7),1);QCOMPARE(recommendedWorkers(2),1);QCOMPARE(recommendedWorkers(0),1);
    }
    void singleImageQueuePriority() {
        QSemaphore gate;QStringList outputs;QVector<unsigned> threads;
        ConversionQueue queue(nullptr,[&](const std::filesystem::path&,const std::filesystem::path& output,const jxrc::Options& options){
            if(output.filename()=="active")gate.acquire();
            outputs.append(QString::fromStdString(output.filename().string()));threads.append(options.threads);return jxrc::Result{};
        });
        const auto directory=root("priority");
        const Job active{"a",directory+"/active",{}},pending{"b",directory+"/pending",{}},last{"c",directory+"/last",{}};
        const Job extra{"c",directory+"/extra",{}},activeExtra{"a",directory+"/active-extra",{}};
        QSignalSpy done(&queue,&ConversionQueue::batchFinished),progress(&queue,&ConversionQueue::progress);
        queue.start({active,pending,last},1,4);
        queue.enqueueNext({last,extra,last},6,24);const int afterPromotion=queue.total();
        queue.enqueueNext({active,activeExtra},6,24);const int afterActive=queue.total();
        // Release before assertions so a failure cannot strand the worker.
        gate.release();QCOMPARE(afterPromotion,4);QCOMPARE(afterActive,5);
        QTRY_COMPARE(done.count(),1);
        QCOMPARE(outputs,QStringList({"active","active-extra","last","extra","pending"}));
        for(auto count:threads)QCOMPARE(count,4u);
        QCOMPARE(done[0][0].toInt(),5);QCOMPARE(done[0][1].toInt(),0);QCOMPARE(done[0][2].toInt(),0);
        QCOMPARE(progress.last()[0].toInt(),5);QCOMPARE(progress.last()[1].toInt(),5);
        // An idle queue starts a fresh batch, using the requested worker budget.
        queue.enqueueNext({pending},1,2);QTRY_COMPARE(done.count(),2);QCOMPARE(queue.total(),1);QCOMPARE(threads.last(),2u);
    }
    void cancelSelectedPendingOutputs() {
        QSemaphore activeGate;
        QStringList converted;
        ConversionQueue queue(nullptr,[&](const std::filesystem::path& input,const std::filesystem::path&,const jxrc::Options&) {
            const auto name=QString::fromStdString(input.string());
            if(name=="active")activeGate.acquire();
            converted.append(name);return jxrc::Result{};
        });
        QSignalSpy done(&queue,&ConversionQueue::batchFinished),states(&queue,&ConversionQueue::statusChanged),progress(&queue,&ConversionQueue::progress);
        QVector<Job> jobs={{"active",root("cancel-selected/active-1"),{}},{"active",root("cancel-selected/active-2"),{}},{"pending",root("cancel-selected/pending-1"),{}},{"pending",root("cancel-selected/pending-2"),{}},{"other",root("cancel-selected/other"),{}}};
        queue.start(jobs,1,1);
        const bool hadPending=queue.hasPendingFor({"active","pending","finished","ready"});
        queue.cancelPendingFor({"finished","ready"});
        queue.cancelPendingFor({"active","pending","finished","ready"});
        const bool stillBusy=queue.busy(),selectedPending=queue.hasPendingFor({"active","pending"}),otherPending=queue.hasPendingFor({"other"});
        // Always release the worker before assertions, including on failure.
        activeGate.release(2);
        QVERIFY(hadPending);QVERIFY(stillBusy);QVERIFY(!selectedPending);QVERIFY(otherPending);QCOMPARE(queue.total(),5);
        QTRY_COMPARE(done.count(),1);
        QCOMPARE(done[0][0].toInt(),2);QCOMPARE(done[0][1].toInt(),0);QCOMPARE(done[0][2].toInt(),3);
        QCOMPARE(converted,QStringList({"active","other"}));
        QCOMPARE(progress.last()[0].toInt(),5);QCOMPARE(progress.last()[1].toInt(),5);
        QMap<QString,QString> finalStates;for(const auto& state:states)finalStates[state[0].toString()]=state[1].toString();
        QCOMPARE(finalStates.value("active"),QString("Cancelled"));QCOMPARE(finalStates.value("pending"),QString("Cancelled"));QCOMPARE(finalStates.value("other"),QString("Done"));
        queue.cancelPendingFor({"active","pending"});QCOMPARE(done.count(),1);
    }
    void outputLayoutAndRawNames() {
        const QString a=makeImage(root("layout/a"),"same.jpg",16,12,100),b=makeImage(root("layout/b"),"same.jpg",16,12,100);
        jxrc::Options options;options.target=jxrc::Target::avif;options.overwrite=true;
        OutputLayout layout;layout.source_folder=true;layout.format_subfolders=true;layout.rename_conflicts=true;
        auto jobs=makeJobs({a,b},{},options,layout);QCOMPARE(jobs.size(),2);
        QCOMPARE(QFileInfo(jobs[0].output).absolutePath(),QFileInfo(a).absolutePath()+"/avif");
        QCOMPARE(QFileInfo(jobs[1].output).absolutePath(),QFileInfo(b).absolutePath()+"/avif");
        QVERIFY(!jobs[0].options.overwrite);QVERIFY(!jobs[1].options.overwrite);
        QDir().mkpath(QFileInfo(jobs[0].output).absolutePath());QFile existing(jobs[0].output);QVERIFY(existing.open(QIODevice::WriteOnly));existing.write("keep");existing.close();
        const auto renamed=makeJobs({a,a},{},options,layout);
        QVERIFY(renamed[0].output!=jobs[0].output);QVERIFY(renamed[0].output!=renamed[1].output);
        QVERIFY(existing.open(QIODevice::ReadOnly));QCOMPARE(existing.readAll(),QByteArray("keep"));existing.close();
        options.mode=jxrc::Mode::process;options.raw.hdr_mode=jxrc::RawHdrMode::boost;
        const QString raw=QFileInfo(a).absolutePath()+"/sensor.nef";
        auto rawJobs=makeJobs({raw,a},root("layout/output"),options);
        QVERIFY(QFileInfo(rawJobs[0].output).fileName().contains("_hdr_boost_"));QVERIFY(!rawJobs[1].output.contains("_hdr_boost_"));
        options.raw.hdr_mode=jxrc::RawHdrMode::custom;
        QVERIFY(makeJobs({raw},root("layout/output"),options)[0].output.contains("_hdr_custom_"));
        MainWindow window;auto* rename=window.findChild<QCheckBox*>("renameConflicts");auto* overwrite=window.findChild<QCheckBox*>("overwrite");
        QVERIFY(rename && overwrite);rename->setChecked(true);QVERIFY(!overwrite->isEnabled());rename->setChecked(false);QVERIFY(overwrite->isEnabled());
    }
    void existingOutputsSkipBeforeConversion() {
        const QString output=root("existing-output.avif");QFile file(output);
        QVERIFY(file.open(QIODevice::WriteOnly));file.write("keep");file.close();
        std::atomic<int> calls{0};
        ConversionQueue queue(nullptr,[&](const auto&,const auto&,const auto&){++calls;return jxrc::Result{};});
        QSignalSpy done(&queue,&ConversionQueue::batchFinished),states(&queue,&ConversionQueue::statusChanged);
        jxrc::Options options;
        queue.start({{"source",output,options}},1,1);
        QCOMPARE(done.count(),1);QCOMPARE(calls.load(),0);QCOMPARE(queue.skipped(),1);
        QCOMPARE(done[0][0].toInt(),0);QCOMPARE(done[0][1].toInt(),0);
        QCOMPARE(states.last()[1].toString(),QString("Skipped"));QVERIFY(!queue.busy());
        queue.start({{"source",output,options},{"source",root("new-output.avif"),options}},1,1);
        QTRY_COMPARE(done.count(),2);QCOMPARE(calls.load(),1);QCOMPARE(queue.skipped(),1);
        QCOMPARE(done[1][0].toInt(),1);QCOMPARE(done[1][1].toInt(),0);
        QVERIFY(states.last()[1].toString().startsWith("Done"));
        options.overwrite=true;queue.start({{"source",output,options}},1,1);
        QTRY_COMPARE(done.count(),3);QCOMPARE(calls.load(),2);QCOMPARE(queue.skipped(),0);
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("keep"));
    }
    void outputCollisionAndActualQueue() {
        const QString a=makeImage(root("jobs/a"),"same.jpg",64,48,200),b=makeImage(root("jobs/b"),"same.jpg",64,48,400);
        const QString destination=root("jobs/output");QDir().mkpath(destination);jxrc::Options o;o.target=jxrc::Target::avif;
        auto scrgbOptions=o;scrgbOptions.target=jxrc::Target::jxl_scrgb;scrgbOptions.mode=jxrc::Mode::process;
        const auto scrgbJobs=makeJobs({a},destination,scrgbOptions);
        QVERIFY(scrgbJobs[0].output.endsWith("_scrgb.jxl"));
        const auto jobs=makeJobs({a,b},destination,o);QCOMPARE(jobs.size(),2);QVERIFY(jobs[0].output!=jobs[1].output);
        ConversionQueue queue;QSignalSpy done(&queue,&ConversionQueue::batchFinished);queue.start(jobs,2,2);QTRY_COMPARE(done.count(),1);
        QCOMPARE(done[0][0].toInt(),2);QCOMPARE(done[0][1].toInt(),0);for(const auto& job:jobs)QVERIFY(QFileInfo(job.output).size()>0);
        QVector<Job> multiple;o.mode=jxrc::Mode::process;QDir().mkpath(root("jobs/multiple"));
        for(auto target:{jxrc::Target::avif,jxrc::Target::jxl,jxrc::Target::jxl_pq,jxrc::Target::jxl_linear,jxrc::Target::jxl_scrgb}){o.target=target;multiple+=makeJobs({a,b},root("jobs/multiple"),o);}
        queue.start(multiple,2,2);QTRY_COMPARE(done.count(),2);
        QCOMPARE(done[1][0].toInt(),10);QCOMPARE(done[1][1].toInt(),0);
        for(const auto& job:multiple)QVERIFY(QFileInfo(job.output).size()>0);
    }
};
int main(int argc,char** argv) {
    if(argc==3&&QByteArray(argv[1])=="--capture-exit-child") {
        QCoreApplication app(argc,argv);const auto marker=app.arguments()[2];
        for(int i=0;i<200;++i){
            if(QFile::exists(marker)){QFile done(marker+".child-done");return done.open(QIODevice::WriteOnly)?0:3;}
            QThread::msleep(50);
        }
        return 4;
    }
    if(argc==3&&QByteArray(argv[1])=="--capture-exit-probe") {
        QString marker;
        {
            QCoreApplication app(argc,argv);marker=app.arguments()[2];OutputCapture::instance();
            // Deliberately inherit stdout/stderr, reproducing the old updater
            // launch. The child retains the log pipe until capture shuts down.
            if(!QProcess::startDetached(app.applicationFilePath(),{"--capture-exit-child",marker}))return 2;
        }
        QFile done(marker);if(!done.open(QIODevice::WriteOnly))return 3;done.close();
        for(int i=0;i<100;++i){if(QFile::exists(marker+".child-done"))return 0;QThread::msleep(10);}
        return 5;
    }
    QApplication app(argc,argv);app.setAttribute(Qt::AA_Use96Dpi,true);GuiTests tests;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tests,argc,argv);
}
#include "gui.moc"
