#include "window.hpp"
#include "views.hpp"
#include <QAction>
#include <QApplication>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateEdit>
#include <QDir>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QUrl>
#include <algorithm>
#include <stdexcept>

namespace jxrc::gui {
namespace {
QComboBox* combo(const QStringList& values,const char* name,QWidget* parent=nullptr) {auto* w=new QComboBox(parent);w->addItems(values);w->setObjectName(name);return w;}
QSpinBox* spin(int low,int high,int initial,const char* name) {auto* w=new QSpinBox;w->setRange(low,high);w->setValue(initial);w->setObjectName(name);w->setKeyboardTracking(false);return w;}
QDoubleSpinBox* real(double low,double high,double initial,double step,const char* name) {auto* w=new QDoubleSpinBox;w->setRange(low,high);w->setDecimals(2);w->setSingleStep(step);w->setValue(initial);w->setObjectName(name);w->setKeyboardTracking(false);return w;}
QCheckBox* check(const QString& text,const char* name,bool initial=false) {auto* w=new QCheckBox(text);w->setObjectName(name);w->setChecked(initial);return w;}
QStringList urlPaths(const QMimeData* mime) {QStringList out;for(const auto& url:mime->urls())if(url.isLocalFile())out<<url.toLocalFile();return out;}
}
MainWindow::MainWindow(QWidget* parent):QMainWindow(parent),catalog_(new Catalog(this)),proxy_(new CatalogFilter(this)),queue_(new ConversionQueue(this)) {
    setWindowTitle("JXRConverter " + QString::fromStdString(jxrc::version()));setObjectName("converterWindow");resize(1440,920);setMinimumSize(980,640);setAcceptDrops(true);
    proxy_->setSourceModel(catalog_);proxy_->sort(Catalog::Name);selection_=new QItemSelectionModel(proxy_,this);
    auto* toolbar=addToolBar("Library");toolbar->setMovable(false);
    auto* files=toolbar->addAction("Add files…");connect(files,&QAction::triggered,this,[this]{addPaths(QFileDialog::getOpenFileNames(this,"Add images",{},"Images (*.jpg *.jpeg *.avif *.jxl *.jxr *.wdp *.hdp *.dng *.cr2 *.cr3 *.nef *.arw *.raf *.rw2 *.orf *.pef *.raw);;All files (*)"));});
    auto* folder=toolbar->addAction("Open folder…");connect(folder,&QAction::triggered,this,[this]{const auto path=QFileDialog::getExistingDirectory(this,"Open image folder");if(!path.isEmpty())addPaths({path});});
    recursive_=check("Include subfolders","recursive",true);toolbar->addWidget(recursive_);toolbar->addSeparator();
    auto* selectAll=toolbar->addAction("Select all");connect(selectAll,&QAction::triggered,this,[this]{if(views_->currentWidget()==table_)table_->selectAll();else list_->selectAll();});
    remove_=new QPushButton("Remove selected");remove_->setObjectName("removeSelected");toolbar->addWidget(remove_);
    connect(remove_,&QPushButton::clicked,this,[this]{catalog_->removePaths(selectedPaths());});
    clear_=new QPushButton("Clear");toolbar->addWidget(clear_);connect(clear_,&QPushButton::clicked,this,[this]{catalog_->removePaths(catalog_->paths());});
    toolbar->addSeparator();wheelChanges_=check("Wheel changes controls","wheelChanges");
    wheelChanges_->setToolTip("Allow the mouse wheel to change dropdowns and numeric inputs. Off lets you scroll past controls without changing conversion settings.");toolbar->addWidget(wheelChanges_);
    auto* splitter=new QSplitter;setCentralWidget(splitter);
    auto* browser=new QWidget;auto* layout=new QVBoxLayout(browser);layout->setContentsMargins(16,16,16,10);layout->setSpacing(10);
    auto* heading=new QLabel("Image library");QFont title=heading->font();title.setPointSize(18);title.setBold(true);heading->setFont(title);layout->addWidget(heading);
    auto* filters=new QHBoxLayout;search_=new QLineEdit;search_->setObjectName("search");search_->setPlaceholderText("Instant filter · e.g. sunset");search_->setClearButtonEnabled(true);filters->addWidget(search_,1);
    metadataFilters_=check("Metadata filters","metadataFilters");filters->addWidget(metadataFilters_);
    today_=check("Today","todayFilter");today_->setToolTip("Captured today; uses file modification date when no capture date is available.");filters->addWidget(today_);
    format_=combo({"All formats","JPEG","AVIF","JXL","JXR","RAW"},"formatFilter");range_=combo({"All ranges","SDR","HDR","RAW"},"rangeFilter");layout->addLayout(filters);
    auto* advanced=new QGroupBox("Image & EXIF filters");auto* advancedLayout=new QVBoxLayout(advanced);
    auto* types=new QHBoxLayout;types->addWidget(new QLabel("Format"));types->addWidget(format_);types->addWidget(new QLabel("Range"));types->addWidget(range_);types->addStretch();advancedLayout->addLayout(types);
    auto* imageLimits=new QHBoxLayout;advancedLayout->addLayout(imageLimits);
    minImageEdge_=spin(0,100000,0,"minImageEdge");minImageEdge_->setPrefix("Max edge ≥ ");minImageEdge_->setSuffix(" px");minImageEdge_->setSpecialValueText("Any max edge");
    minImageEdge_->setToolTip("Minimum longest edge, independent of portrait/landscape. Combined with the minimum MP filter; both must match.");
    megapixels_=real(0,1000,0,1,"minMegapixels");megapixels_->setSuffix(" MP+");megapixels_->setSpecialValueText("Any MP");
    minIso_=spin(0,10000000,0,"minIso");minIso_->setPrefix("ISO ≥ ");minIso_->setSpecialValueText("Any ISO min");
    maxIso_=spin(0,10000000,0,"maxIso");maxIso_->setPrefix("ISO ≤ ");maxIso_->setSpecialValueText("Any ISO max");
    for(auto* w:QList<QWidget*>{minImageEdge_,megapixels_,minIso_,maxIso_})imageLimits->addWidget(w);
    layout->addWidget(advanced);advanced->hide();
    auto* datePanel=new QWidget;auto* dates=new QHBoxLayout(datePanel);dates->setContentsMargins(0,0,0,0);dateEnabled_=check("Capture date","dateFilter");dateFrom_=new QDateEdit(QDate(1970,1,1));dateTo_=new QDateEdit(QDate::currentDate());dateFrom_->setObjectName("dateFrom");dateTo_->setObjectName("dateTo");
    for(auto* date:{dateFrom_,dateTo_}){date->setDisplayFormat("yyyy-MM-dd");date->setCalendarPopup(true);date->setEnabled(false);}
    dates->addWidget(dateEnabled_);dates->addWidget(dateFrom_);dates->addWidget(new QLabel("to"));dates->addWidget(dateTo_);
    auto* reset=new QPushButton("Reset filters");filters->addWidget(reset);dates->addStretch();layout->addWidget(datePanel);datePanel->hide();
    auto* viewBar=new QHBoxLayout;viewMode_=combo({"Grid","List","Details"},"viewMode");sort_=new QComboBox;sort_->setObjectName("sortField");
    for(int col=0;col<Catalog::ColumnCount;++col)sort_->addItem(catalog_->headerData(col,Qt::Horizontal,Qt::DisplayRole).toString(),col);
    auto* direction=new QPushButton("Ascending");direction->setObjectName("sortDirection");viewBar->addWidget(viewMode_);viewBar->addWidget(new QLabel("Sort"));viewBar->addWidget(sort_);viewBar->addWidget(direction);viewBar->addStretch();viewBar->addWidget(new QLabel("Minimum size"));
    size_=new QSlider(Qt::Horizontal);size_->setObjectName("thumbnailSize");size_->setRange(50,200);size_->setSingleStep(10);size_->setPageStep(10);size_->setValue(100);size_->setFixedWidth(140);
    size_->setToolTip("Minimum grid cell width in dp (50–200). Columns grow to fill the available width.");viewBar->addWidget(size_);
    auto* sizeValue=new QLabel("100 dp");sizeValue->setMinimumWidth(48);viewBar->addWidget(sizeValue);
    connect(size_,&QSlider::valueChanged,this,[this,sizeValue](int value){const int snapped=((value+5)/10)*10;if(value!=snapped){size_->setValue(snapped);return;}sizeValue->setText(QString::number(value)+" dp");});layout->addLayout(viewBar);
    views_=new QStackedWidget;list_=new FileListView;table_=new FileTableView;delegate_=new ThumbnailDelegate(list_);list_->setItemDelegate(delegate_);
    list_->setModel(proxy_);table_->setModel(proxy_);list_->setSelectionModel(selection_);table_->setSelectionModel(selection_);
    table_->verticalHeader()->hide();table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);table_->setColumnWidth(Catalog::Name,260);table_->setColumnWidth(Catalog::Camera,190);table_->sortByColumn(Catalog::Name,Qt::AscendingOrder);
    views_->addWidget(list_);views_->addWidget(table_);layout->addWidget(views_,1);
    counts_=new QLabel("Drop files or folders here, or choose Open folder.");counts_->setObjectName("catalogCounts");layout->addWidget(counts_);
    auto* sidebar=new QTabWidget;sidebar->setMinimumWidth(330);sidebar->setMaximumWidth(460);
    auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(buildControls());scroll->setMinimumHeight(150);
    auto* conversionPage=new QWidget;auto* conversionLayout=new QVBoxLayout(conversionPage);conversionLayout->setContentsMargins(0,0,0,0);conversionLayout->setSpacing(0);conversionLayout->addWidget(scroll,1);conversionLayout->addWidget(queuePanel_);sidebar->addTab(conversionPage,"Conversion");
    inspector_=new QPlainTextEdit;inspector_->setObjectName("metadataInspector");inspector_->setReadOnly(true);inspector_->setPlaceholderText("Select an image to inspect its EXIF and image information.");sidebar->addTab(inspector_,"Metadata");
    splitter->addWidget(browser);splitter->addWidget(sidebar);splitter->setStretchFactor(0,1);splitter->setStretchFactor(1,0);splitter->setSizes({1050,370});
    connect(list_,&FileListView::pathsDropped,this,&MainWindow::addPaths);connect(table_,&FileTableView::pathsDropped,this,&MainWindow::addPaths);
    connect(search_,&QLineEdit::textChanged,this,&MainWindow::updateFilter);
    for(auto* c:{format_,range_})connect(c,&QComboBox::currentIndexChanged,this,&MainWindow::updateFilter);
    for(auto* s:{minImageEdge_,minIso_,maxIso_})connect(s,&QSpinBox::valueChanged,this,&MainWindow::updateFilter);
    connect(megapixels_,&QDoubleSpinBox::valueChanged,this,&MainWindow::updateFilter);
    connect(dateEnabled_,&QCheckBox::toggled,this,[this](bool on){dateFrom_->setEnabled(on);dateTo_->setEnabled(on);updateFilter();});
    for(auto* d:{dateFrom_,dateTo_})connect(d,&QDateEdit::dateChanged,this,&MainWindow::updateFilter);
    connect(today_,&QCheckBox::toggled,this,&MainWindow::updateFilter);
    connect(metadataFilters_,&QCheckBox::toggled,this,[this,advanced,datePanel](bool on){advanced->setVisible(on);datePanel->setVisible(on);search_->setPlaceholderText(on?"Search filenames, camera, lens, EXIF tags or values…":"Instant filter · e.g. sunset");updateFilter();});
    connect(reset,&QPushButton::clicked,this,[this]{search_->clear();format_->setCurrentIndex(0);range_->setCurrentIndex(0);minImageEdge_->setValue(0);megapixels_->setValue(0);minIso_->setValue(0);maxIso_->setValue(0);dateEnabled_->setChecked(false);today_->setChecked(false);});
    // Refresh Today across local midnight, including when the application stays open.
    auto* dateRefresh=new QTimer(this);dateRefresh->setInterval(30000);connect(dateRefresh,&QTimer::timeout,this,[this]{if(today_->isChecked())updateFilter();});dateRefresh->start();
    connect(sort_,&QComboBox::currentIndexChanged,this,[this]{proxy_->sort(sort_->currentData().toInt(),sortOrder_);});
    connect(direction,&QPushButton::clicked,this,[this,direction]{sortOrder_=sortOrder_==Qt::AscendingOrder?Qt::DescendingOrder:Qt::AscendingOrder;direction->setText(sortOrder_==Qt::AscendingOrder?"Ascending":"Descending");proxy_->sort(sort_->currentData().toInt(),sortOrder_);});
    connect(table_->horizontalHeader(),&QHeaderView::sortIndicatorChanged,this,[this,direction](int column,Qt::SortOrder order){sortOrder_=order;sort_->setCurrentIndex(column);direction->setText(order==Qt::AscendingOrder?"Ascending":"Descending");});
    connect(viewMode_,&QComboBox::currentIndexChanged,this,&MainWindow::updatePresentation);connect(size_,&QSlider::valueChanged,this,&MainWindow::updatePresentation);
    connect(selection_,&QItemSelectionModel::selectionChanged,this,[this]{updateCounts();showMetadata();});
    connect(proxy_,&QAbstractItemModel::rowsInserted,this,&MainWindow::updateCounts);connect(proxy_,&QAbstractItemModel::rowsRemoved,this,&MainWindow::updateCounts);
    connect(catalog_,&QAbstractItemModel::dataChanged,this,[this]{updateCounts();showMetadata();});
    connect(catalog_,&Catalog::scanStateChanged,this,[this](bool scanning){statusBar()->showMessage(scanning?"Scanning folders…":"Folder scan complete",scanning?0:4000);updateCounts();});
    connect(catalog_,&Catalog::scanError,this,[this](const QString& error){statusBar()->showMessage(error,6000);});
    connect(queue_,&ConversionQueue::statusChanged,catalog_,&Catalog::setStatus);
    connect(queue_,&ConversionQueue::progress,this,[this](int done,int total,int active){progress_->setRange(0,total);progress_->setValue(done);queueState_->setText(QString("%1 / %2 finished · %3 active").arg(done).arg(total).arg(active));updateCounts();});
    connect(queue_,&ConversionQueue::batchFinished,this,[this](int success,int failed,int cancelled){queueState_->setText(QString("%1 done · %2 failed · %3 cancelled").arg(success).arg(failed).arg(cancelled));updateCounts();if(closeWhenIdle_)QTimer::singleShot(0,this,&QWidget::close);});
    auto* deleteShortcut=new QShortcut(QKeySequence::Delete,this);connect(deleteShortcut,&QShortcut::activated,this,[this]{if(!queue_->busy()&&(list_->hasFocus()||table_->hasFocus()))catalog_->removePaths(selectedPaths());});
    auto* allShortcut=new QShortcut(QKeySequence::SelectAll,this);connect(allShortcut,&QShortcut::activated,this,[this]{if(list_->hasFocus())list_->selectAll();else if(table_->hasFocus())table_->selectAll();});
    for(auto* control:findChildren<QComboBox*>())control->installEventFilter(this);
    for(auto* control:findChildren<QAbstractSpinBox*>())control->installEventFilter(this);
    QSettings settings;restoreGeometry(settings.value("window/geometry").toByteArray());viewMode_->setCurrentIndex(settings.value("browser/view",0).toInt());size_->setValue(settings.value("browser/minCellDp",100).toInt());
    wheelChanges_->setChecked(settings.value("browser/wheelChanges",false).toBool());
    output_->setText(settings.value("conversion/output",QDir::homePath()+"/Pictures/Converted").toString());recursive_->setChecked(settings.value("browser/recursive",true).toBool());
    updateMode();updateFilter();updatePresentation();updateCounts();
}
MainWindow::~MainWindow() {
    QSettings settings;settings.setValue("window/geometry",saveGeometry());settings.setValue("browser/view",viewMode_->currentIndex());settings.setValue("browser/minCellDp",size_->value());settings.setValue("browser/wheelChanges",wheelChanges_->isChecked());settings.setValue("browser/recursive",recursive_->isChecked());settings.setValue("conversion/output",output_->text());
}
QWidget* MainWindow::buildControls() {
    auto* panel=new QWidget;auto* layout=new QVBoxLayout(panel);layout->setContentsMargins(14,16,14,16);layout->setSpacing(14);
    auto* title=new QLabel("Convert images");QFont font=title->font();font.setPointSize(16);font.setBold(true);title->setFont(font);layout->addWidget(title);
    auto* general=new QFormLayout;mode_=combo({"Preserve source values","Process pixels"},"conversionMode");target_=combo({"JPEG · SDR","JPEG · gain map","JXL · SDR","JXL · linear HDR","AVIF · SDR / source","AVIF · PQ","AVIF · gain map","JXL · PQ"},"conversionTarget");target_->setCurrentIndex(4);
    general->addRow("Mode",mode_);general->addRow("Output",target_);layout->addLayout(general);
    auto* qualityGroup=new QGroupBox("Quality");auto* qualityLayout=new QFormLayout(qualityGroup);preset_=combo({"Lossless","High","Medium","Low","Custom"},"qualityPreset");preset_->setCurrentIndex(1);qualityLayout->addRow("Preset",preset_);
    auto* warning=new QLabel("JPEG remains lossy with every preset.");warning->setWordWrap(true);qualityLayout->addRow(warning);
    custom_=new QGroupBox("Custom quality");auto* customForm=new QFormLayout(custom_);quality_=spin(1,100,90,"codecQuality");distance_=real(0,3,0.5,0.05,"jxlDistance");avifDepth_=combo({"10 bit","12 bit"},"avifDepth");downsize_=check("Halve gain-map dimensions","downsizeGainmap");
    customForm->addRow("JPEG / AVIF",quality_);customForm->addRow("JXL distance",distance_);customForm->addRow("AVIF depth",avifDepth_);customForm->addRow(downsize_);qualityLayout->addRow(custom_);layout->addWidget(qualityGroup);
    exif_=check("Keep EXIF","keepExif");exif_->setToolTip("Processed RAW and JXR EXIF retention is not supported by the library yet.");transcode_=check("JPEG coefficients → JXL","jpegTranscoding");
    layout->addWidget(exif_);layout->addWidget(transcode_);
    processing_=new QGroupBox("Pixel processing");auto* processingForm=new QFormLayout(processing_);exposure_=real(-3,3,0,0.1,"exposure");exposure_->setSuffix(" EV");
    exposure_->setToolTip("0 EV leaves exposure unchanged. −3 EV multiplies linear RGB by 1/8; +3 EV multiplies it by 8. For RAW gain-map output, use Custom RAW exposure instead.");
    maxEdge_=spin(0,65535,0,"maxEdge");maxEdge_->setSpecialValueText("Original size");bt2020_=check("Extended BT.2020 · JXL decoding","extendedBt2020",true);
    bt2020_->setToolTip("When decoding floating-point linear-sRGB JXL, convert extended RGB values to BT.2020 before clipping negative channels. This preserves more wide-gamut color. Other input formats are unaffected; it does not select the output transfer function.");
    processingForm->addRow("Exposure (EV)",exposure_);processingForm->addRow("Max edge (px)",maxEdge_);processingForm->addRow(bt2020_);layout->addWidget(processing_);
    raw_=new QGroupBox("RAW rendering");auto* rawLayout=new QFormLayout(raw_);rawMode_=combo({"Source headroom","Boost highlights","Custom"},"rawMode");rawLayout->addRow("HDR mode",rawMode_);
    rawCustom_=new QWidget;auto* rawForm=new QFormLayout(rawCustom_);rawForm->setContentsMargins(0,0,0,0);
    rawExposure_=real(-3,3,0,0.1,"rawExposure");wb_=combo({"Camera","Auto","Daylight","Cloudy","Shade","Tungsten","Fluorescent","Flash","Kelvin"},"rawWhiteBalance");kelvin_=spin(2000,12000,5200,"rawKelvin");kelvin_->setSingleStep(100);
    highlight_=real(0,3,0,0.1,"rawHighlightStops");shoulder_=real(0,1,0.5,0.05,"rawShoulder");nits_=spin(1000,10000,1500,"rawTargetNits");nits_->setSingleStep(100);
    shadows_=real(-1,1,0,0.05,"rawShadows");midtones_=real(-1,1,0,0.05,"rawMidtones");highlights_=real(-1,1,0,0.05,"rawHighlights");cameraLook_=check("Camera look (SDR)","rawCameraLook",true);linearHdr_=check("Linear HDR · bypass camera look","rawLinearHdr");
    rawForm->addRow(cameraLook_);rawForm->addRow(linearHdr_);rawForm->addRow("Exposure (EV)",rawExposure_);rawForm->addRow("White balance",wb_);rawForm->addRow("Temperature (K)",kelvin_);rawForm->addRow("Highlight stops",highlight_);rawForm->addRow("Shoulder",shoulder_);rawForm->addRow("Target nits",nits_);rawForm->addRow("Shadows",shadows_);rawForm->addRow("Midtones",midtones_);rawForm->addRow("Highlights",highlights_);rawLayout->addRow(rawCustom_);layout->addWidget(raw_);
    queuePanel_=new QWidget;auto* queueLayout=new QVBoxLayout(queuePanel_);queueLayout->setContentsMargins(14,0,14,10);queueLayout->setSpacing(8);
    auto* destination=new QGroupBox("Destination & queue");auto* destinationLayout=new QVBoxLayout(destination);output_=new QLineEdit;output_->setObjectName("outputDirectory");output_->setPlaceholderText("Output folder");destinationLayout->addWidget(output_);
    auto* folderButtons=new QHBoxLayout;
    auto* browse=new QPushButton("Choose folder…");connect(browse,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getExistingDirectory(this,"Output folder",output_->text());if(!path.isEmpty())output_->setText(path);});folderButtons->addWidget(browse);
    auto* openOutput=new QPushButton("Open output folder");openOutput->setObjectName("openOutputFolder");folderButtons->addWidget(openOutput);destinationLayout->addLayout(folderButtons);
    connect(openOutput,&QPushButton::clicked,this,[this]{
        const auto path=output_->text().trimmed();
        if(path.isEmpty()||!QDir().mkpath(path)){QMessageBox::warning(this,"Cannot open output folder","Choose a writable output folder.");return;}
        if(!QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(path).absolutePath())))QMessageBox::warning(this,"Cannot open output folder","The system file manager could not open this folder.");
    });
    overwrite_=check("Replace existing output files","overwrite");destinationLayout->addWidget(overwrite_);
    const int cores=coreCount();workers_=spin(1,cores,recommendedWorkers(cores),"parallelJobs");auto* workerForm=new QFormLayout;workerForm->addRow("Parallel jobs",workers_);destinationLayout->addLayout(workerForm);
    threadBudget_=new QLabel;threadBudget_->setWordWrap(true);auto updateBudget=[this,cores]{threadBudget_->setText(QString("%1 CPU cores · up to %2 codec threads/job. Default jobs: CPU cores / 4 (minimum 1). Reduce jobs for large RAW files.").arg(cores).arg(std::max(1,cores/workers_->value())));};connect(workers_,&QSpinBox::valueChanged,this,updateBudget);updateBudget();destinationLayout->addWidget(threadBudget_);queueLayout->addWidget(destination);
    selected_=new QPushButton("Convert selected");selected_->setObjectName("convertSelected");selected_->setProperty("primary",true);visible_=new QPushButton("Convert visible files");visible_->setObjectName("convertVisible");cancel_=new QPushButton("Stop queue · finish active files");cancel_->setObjectName("cancelQueue");
    queueLayout->addWidget(selected_);queueLayout->addWidget(visible_);queueLayout->addWidget(cancel_);progress_=new QProgressBar;progress_->setRange(0,1);progress_->setValue(0);queueLayout->addWidget(progress_);queueState_=new QLabel("Ready");queueState_->setWordWrap(true);queueLayout->addWidget(queueState_);layout->addStretch();
    connect(mode_,&QComboBox::currentIndexChanged,this,&MainWindow::updateMode);connect(target_,&QComboBox::currentIndexChanged,this,&MainWindow::updateMode);connect(preset_,&QComboBox::currentIndexChanged,this,&MainWindow::updateMode);connect(rawMode_,&QComboBox::currentIndexChanged,this,&MainWindow::updateMode);connect(wb_,&QComboBox::currentIndexChanged,this,&MainWindow::updateMode);
    connect(selected_,&QPushButton::clicked,this,[this]{convertFiles(true);});connect(visible_,&QPushButton::clicked,this,[this]{convertFiles(false);});connect(cancel_,&QPushButton::clicked,queue_,&ConversionQueue::cancelPending);
    return panel;
}
void MainWindow::updateMode() {
    const bool process=mode_->currentIndex()==1;auto* targets=qobject_cast<QStandardItemModel*>(target_->model());
    for(int i:{3,5,7})targets->item(i)->setEnabled(process);
    if(!process&&(target_->currentIndex()==3||target_->currentIndex()==5||target_->currentIndex()==7))target_->setCurrentIndex(4);
    processing_->setEnabled(process);raw_->setEnabled(process);rawCustom_->setEnabled(rawMode_->currentIndex()==2);kelvin_->setEnabled(wb_->currentIndex()==8);
    custom_->setEnabled(preset_->currentIndex()==4);transcode_->setEnabled(!process&&target_->currentIndex()==2);
    if(!transcode_->isEnabled())transcode_->setChecked(false);
}
Options MainWindow::options() const {
    Options o;o.mode=mode_->currentIndex()?Mode::process:Mode::preserve;o.target=Target(target_->currentIndex());o.quality.preset=Preset(preset_->currentIndex());
    o.quality.codec_quality=quality_->value();o.quality.jxl_distance=static_cast<float>(distance_->value());o.quality.avif_depth=avifDepth_->currentIndex()?12:10;o.quality.downsize_gainmap=downsize_->isChecked();
    o.keep_exif=exif_->isChecked();o.overwrite=overwrite_->isChecked();o.jpeg_transcoding=transcode_->isEnabled()&&transcode_->isChecked();
    if(o.mode==Mode::process) {
        o.exposure=static_cast<float>(exposure_->value());o.max_edge=maxEdge_->value();o.extended_bt2020=bt2020_->isChecked();o.raw.hdr_mode=RawHdrMode(rawMode_->currentIndex());
        if(o.raw.hdr_mode==RawHdrMode::custom){o.raw.exposure=static_cast<float>(rawExposure_->value());o.raw.white_balance=wb_->currentIndex();o.raw.kelvin=kelvin_->value();o.raw.highlight_stops=static_cast<float>(highlight_->value());o.raw.shoulder=static_cast<float>(shoulder_->value());o.raw.target_nits=static_cast<float>(nits_->value());o.raw.shadows=static_cast<float>(shadows_->value());o.raw.midtones=static_cast<float>(midtones_->value());o.raw.highlights=static_cast<float>(highlights_->value());o.raw.camera_look=cameraLook_->isChecked();o.raw.linear_hdr=linearHdr_->isChecked();}
    }
    validate(o);return o;
}
void MainWindow::addPaths(const QStringList& paths) {if(!paths.isEmpty())catalog_->addPaths(paths,recursive_->isChecked());}
void MainWindow::updateFilter() {
    Filter f;f.text=search_->text();f.literalText=!metadataFilters_->isChecked();f.today=today_->isChecked();
    if(metadataFilters_->isChecked()) {
        if(format_->currentIndex())f.format=format_->currentText();if(range_->currentIndex())f.range=range_->currentText();
        f.minMaxEdge=minImageEdge_->value();f.minMegapixels=megapixels_->value();f.minIso=minIso_->value();f.maxIso=maxIso_->value();
        if(dateEnabled_->isChecked()&&!f.today){f.from=dateFrom_->date();f.to=dateTo_->date();}
    }
    const bool dates=metadataFilters_->isChecked()&&dateEnabled_->isChecked()&&!f.today;
    dateEnabled_->setEnabled(!f.today);dateFrom_->setEnabled(dates);dateTo_->setEnabled(dates);
    proxy_->setFilter(f);updateCounts();
}
void MainWindow::updatePresentation() {
    const bool details=viewMode_->currentIndex()==2,grid=viewMode_->currentIndex()==0;
    views_->setCurrentWidget(details?static_cast<QWidget*>(table_):list_);list_->setPresentation(grid,size_->value());
    table_->setIconSize(QSize(size_->value()/2,size_->value()/2));table_->verticalHeader()->setDefaultSectionSize(std::max(32,size_->value()/2+8));
}
QStringList MainWindow::selectedPaths() const {QStringList out;for(const auto& index:selection_->selectedRows(0))out<<index.data(Catalog::PathRole).toString();return out;}
void MainWindow::updateCounts() {
    const int selected=selection_->selectedRows(0).size();const bool busy=queue_->busy();
    counts_->setText(QString("%1 visible / %2 files · %3 selected%4").arg(proxy_->rowCount()).arg(catalog_->rowCount()).arg(selected).arg(catalog_->scanning()?" · scanning…":""));
    selected_->setEnabled(selected>0&&!busy);visible_->setEnabled(proxy_->rowCount()>0&&!busy);cancel_->setEnabled(busy);remove_->setEnabled(selected>0&&!busy);clear_->setEnabled(catalog_->rowCount()>0&&!busy&&!catalog_->scanning());workers_->setEnabled(!busy);
}
void MainWindow::showMetadata() {
    const auto selected=selection_->selectedRows(0);if(selected.isEmpty()){inspector_->clear();return;}
    const auto& r=catalog_->record(proxy_->mapToSource(selected.first()).row());
    QString text=r.path+"\n\n"+QString("%1 × %2 · %3 · %4 bit\n%5 · %6\nStatus: %7\n").arg(r.width).arg(r.height).arg(r.format).arg(r.bits).arg(r.transfer,r.primaries,r.status);
    if(!r.error.isEmpty())text+="\n"+r.error+"\n";
    if(!r.metadataWarning.isEmpty())text+="\nMetadata note: "+r.metadataWarning+"\n";
    text+="\nEXIF\n";for(auto it=r.exif.cbegin();it!=r.exif.cend();++it)text+=it.key()+" = "+it.value()+"\n";inspector_->setPlainText(text);
}
void MainWindow::convertFiles(bool selected) {
    QStringList paths;if(selected)paths=selectedPaths();else for(int row=0;row<proxy_->rowCount();++row)paths<<proxy_->index(row,0).data(Catalog::PathRole).toString();
    if(paths.isEmpty()||queue_->busy())return;
    try {
        const auto o=options();const QString directory=output_->text().trimmed();if(directory.isEmpty())throw std::invalid_argument("Choose an output folder");
        auto jobs=makeJobs(paths,directory,o);if(!QDir().mkpath(directory))throw std::runtime_error("Cannot create output folder");
        queue_->start(std::move(jobs),workers_->value());updateCounts();
    } catch(const std::exception& e){QMessageBox::warning(this,"Cannot start conversion",QString::fromUtf8(e.what()));}
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if(queue_->busy()){closeWhenIdle_=true;queue_->cancelPending();queueState_->setText("Closing after active conversions finish…");event->ignore();return;}event->accept();
}
bool MainWindow::eventFilter(QObject* watched,QEvent* event) {
    if(event->type()==QEvent::Wheel&&!wheelChanges_->isChecked()&&
       (qobject_cast<QComboBox*>(watched)||qobject_cast<QAbstractSpinBox*>(watched))) {
        event->ignore();return true;
    }
    return QMainWindow::eventFilter(watched,event);
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event){if(event->mimeData()->hasUrls())event->acceptProposedAction();}
void MainWindow::dropEvent(QDropEvent* event){if(event->mimeData()->hasUrls()){addPaths(urlPaths(event->mimeData()));event->acceptProposedAction();}}
}
