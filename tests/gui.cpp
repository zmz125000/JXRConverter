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
#include <QListWidget>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <exiv2/exiv2.hpp>
#include <atomic>
#include "window.hpp"
#include "style.hpp"
#include "views.hpp"
using namespace jxrc::gui;

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
private slots:
    void initTestCase() {
        applyDesktopStyle();
        QVERIFY(temp_.isValid());QStandardPaths::setTestModeEnabled(true);QCoreApplication::setOrganizationName("JxrcTests");QCoreApplication::setApplicationName("DesktopTests");QSettings().clear();
    }
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
        f={};proxy.setFilter(f);auto decoration=model.data(model.index(0,0),Qt::DecorationRole);Q_UNUSED(decoration);
        QTRY_VERIFY(!qvariant_cast<QIcon>(model.data(model.index(0,0),Qt::DecorationRole)).isNull());
        model.removePaths({b});QCOMPARE(model.rowCount(),2);QCOMPARE(proxy.rowCount(),2);
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
        auto* list=window.findChild<FileListView*>("fileList");auto* modes=window.findChild<QButtonGroup*>("viewMode");auto* slider=window.findChild<QSlider*>("thumbnailSize");QVERIFY(list&&modes&&slider);
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
    void controlsSnapshot() {
        MainWindow window;
        auto* mode=window.findChild<QComboBox*>("conversionMode");mode->setCurrentIndex(1);
        auto* targets=window.findChild<QListWidget*>("conversionTargets");QVERIFY(targets);
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
    void outputCollisionAndActualQueue() {
        const QString a=makeImage(root("jobs/a"),"same.jpg",64,48,200),b=makeImage(root("jobs/b"),"same.jpg",64,48,400);
        const QString destination=root("jobs/output");QDir().mkpath(destination);jxrc::Options o;o.target=jxrc::Target::avif;
        const auto jobs=makeJobs({a,b},destination,o);QCOMPARE(jobs.size(),2);QVERIFY(jobs[0].output!=jobs[1].output);
        ConversionQueue queue;QSignalSpy done(&queue,&ConversionQueue::batchFinished);queue.start(jobs,2,2);QTRY_COMPARE(done.count(),1);
        QCOMPARE(done[0][0].toInt(),2);QCOMPARE(done[0][1].toInt(),0);for(const auto& job:jobs)QVERIFY(QFileInfo(job.output).size()>0);
    }
};
QTEST_MAIN(GuiTests)
#include "gui.moc"
