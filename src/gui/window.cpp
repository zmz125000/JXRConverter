#include "window.hpp"
#include "views.hpp"
#include "style.hpp"
#include "updater.hpp"
#include "output_capture.hpp"
#include <QStandardPaths>
#include <QTextCursor>
#include <QScrollBar>
#include "settings.hpp"
#include <QStyleHints>
#include <QStyle>
#include <QStyleOption>
#include <QPainter>
#include <QMenu>
#include <QClipboard>
#include <QButtonGroup>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>
#include <QToolButton>
#include <QFile>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QAction>
#include <QActionGroup>
#include <QPersistentModelIndex>
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
#include <QFileInfo>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace jxrc::gui {
namespace {
class ToolbarSeparator : public QWidget {
public:
    ToolbarSeparator(){setFixedWidth(8);setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Expanding);}
protected:
    void paintEvent(QPaintEvent*) override {QStyleOption option;option.initFrom(this);option.state|=QStyle::State_Horizontal;QPainter painter(this);style()->drawPrimitive(QStyle::PE_IndicatorToolBarSeparator,&option,&painter,this);}
};
// Use the same InstantPopup behavior as Recent folders, while drawing the
// regular push-button control used by the neighboring actions.
class PopupButton : public QToolButton {
    QStyleOptionButton buttonOption() const {
        QStyleOptionButton option;option.initFrom(this);option.text=text();option.icon=icon();option.iconSize=iconSize();
        if(isDown())option.state|=QStyle::State_Sunken;else option.state|=QStyle::State_Raised;
        if(isChecked())option.state|=QStyle::State_On;
        return option;
    }
public:
    PopupButton(){setPopupMode(QToolButton::InstantPopup);setToolButtonStyle(Qt::ToolButtonTextOnly);setProperty("pushAppearance",true);}
    QSize sizeHint() const override {auto option=buttonOption();return style()->sizeFromContents(QStyle::CT_PushButton,&option,fontMetrics().size(Qt::TextShowMnemonic,text()),this);}
protected:
    void paintEvent(QPaintEvent*) override {auto option=buttonOption();QPainter painter(this);style()->drawControl(QStyle::CE_PushButton,&option,&painter,this);}
};
const QStringList aspectNames={"16:9","4:3","1:1","9:16 (portrait)","3:4 (portrait)"};
const double aspectRatios[]={16.0/9,4.0/3,1,9.0/16,3.0/4};
class WidthMatchedWidget : public QWidget {
    QWidget* reference_;
public:
    explicit WidthMatchedWidget(QWidget* reference):reference_(reference){reference->installEventFilter(this);setFixedWidth(reference->width());}
protected:
    bool eventFilter(QObject* object,QEvent* event) override {if(object==reference_&&event->type()==QEvent::Resize)setFixedWidth(reference_->width());return QWidget::eventFilter(object,event);}
};
class FloatingClearButton : public QPushButton {
    QPlainTextEdit* edit_;
    void anchor(){adjustSize();const auto rect=edit_->viewport()->geometry();move(rect.right()-width()-7,rect.top()+8);raise();}
public:
    explicit FloatingClearButton(QPlainTextEdit* edit):QPushButton("Clear",edit),edit_(edit){edit->installEventFilter(this);edit->viewport()->installEventFilter(this);anchor();}
protected:
    bool eventFilter(QObject* object,QEvent* event) override {if(event->type()==QEvent::Resize||event->type()==QEvent::Show||event->type()==QEvent::StyleChange)anchor();return QPushButton::eventFilter(object,event);}
};
QComboBox* combo(const QStringList& values,const char* name,QWidget* parent=nullptr) {auto* w=new QComboBox(parent);w->addItems(values);w->setObjectName(name);return w;}
QSpinBox* spin(int low,int high,int initial,const char* name) {auto* w=new QSpinBox;w->setRange(low,high);w->setValue(initial);w->setObjectName(name);w->setKeyboardTracking(false);return w;}
QDoubleSpinBox* real(double low,double high,double initial,double step,const char* name) {auto* w=new QDoubleSpinBox;w->setRange(low,high);w->setDecimals(2);w->setSingleStep(step);w->setValue(initial);w->setObjectName(name);w->setKeyboardTracking(false);return w;}
QCheckBox* check(const QString& text,const char* name,bool initial=false) {auto* w=new QCheckBox(text);w->setObjectName(name);w->setChecked(initial);return w;}
QString quotedSearchValue(QString value) {value.replace('\\',"\\\\");value.replace('"',"\\\"");return "\""+value+"\"";}
QStringList urlPaths(const QMimeData* mime) {QStringList out;for(const auto& url:mime->urls())if(url.isLocalFile())out<<url.toLocalFile();return out;}
}
MainWindow::MainWindow(QWidget* parent):QMainWindow(parent),catalog_(new Catalog(this)),proxy_(new CatalogFilter(this)),queue_(new ConversionQueue(this)) {
    OutputCapture::instance();
    setWindowTitle("JXRConverter v" + QString::fromStdString(jxrc::version()));setObjectName("converterWindow");resize(1440,920);setMinimumSize(980,640);setAcceptDrops(true);
    proxy_->setSourceModel(catalog_);proxy_->sort(Catalog::Name);
    // Register structural guards before the view selection model, whose own
    // removal handlers emit selectionChanged as filtered rows disappear.
    connect(proxy_,&QAbstractItemModel::rowsAboutToBeRemoved,this,[this]{++selectionModelChanges_;});
    connect(proxy_,&QAbstractItemModel::modelAboutToBeReset,this,[this]{++selectionModelChanges_;});
    connect(proxy_,&QAbstractItemModel::layoutAboutToBeChanged,this,[this]{++selectionModelChanges_;});
    selection_=new QItemSelectionModel(proxy_,this);
    auto modelChanged=[this]{--selectionModelChanges_;QTimer::singleShot(0,this,&MainWindow::syncSelection);};
    connect(proxy_,&QAbstractItemModel::rowsRemoved,this,modelChanged);
    connect(proxy_,&QAbstractItemModel::modelReset,this,modelChanged);
    connect(proxy_,&QAbstractItemModel::layoutChanged,this,modelChanged);
    catalog_->setBackgroundMetadata(false);
    auto* toolbar=addToolBar("Library");toolbar->setMovable(false);
    auto* files=toolbar->addAction("Add files…");connect(files,&QAction::triggered,this,[this]{addPaths(QFileDialog::getOpenFileNames(this,"Add images",{},"Images (*.png *.webp *.tif *.tiff *.bmp *.gif *.jpg *.jpeg *.avif *.jxl *.jxr *.wdp *.hdp *.dng *.cr2 *.cr3 *.nef *.arw *.raf *.rw2 *.orf *.pef *.raw);;All files (*)"));});
    auto* folder=toolbar->addAction("Open folder…");connect(folder,&QAction::triggered,this,[this]{const auto path=QFileDialog::getExistingDirectory(this,"Open image folder");if(!path.isEmpty())addPaths({path});});
    loadHistory();
    auto* recent=new QToolButton;recent->setObjectName("recentFolders");recent->setText("Recent folders");recent->setPopupMode(QToolButton::InstantPopup);auto* recentMenu=new QMenu(recent);recent->setMenu(recentMenu);toolbar->addWidget(recent);
    connect(recentMenu,&QMenu::aboutToShow,this,[this,recentMenu]{recentMenu->clear();for(const auto& path:recentFolders_){auto* action=recentMenu->addAction(path);connect(action,&QAction::triggered,this,[this,path]{addPaths({path});});}
        auto* filesMenu=recentMenu->addMenu("Recent dropped files");for(const auto& path:recentFiles_){auto* action=filesMenu->addAction(path);connect(action,&QAction::triggered,this,[this,path]{addPaths({path});});}
    });
    loadFolder_=check("Load last folder","loadLastFolder");loadFiles_=check("Load last files","loadLastFiles");toolbar->addWidget(loadFolder_);toolbar->addWidget(loadFiles_);
    loadJobs_=check("Load jobs","loadPreviousJobs");loadJobs_->setToolTip("Restore saved job lists on startup. Job images load only when you select a job.");toolbar->addWidget(loadJobs_);
    clearJobs_=toolbar->addAction("Clear jobs");clearJobs_->setObjectName("clearJobs");clearJobs_->setToolTip("Clear saved jobs and reset job numbering. Images already in the library stay available.");connect(clearJobs_,&QAction::triggered,this,&MainWindow::clearJobs);
    auto* clearHistory=toolbar->addAction("Clear history");connect(clearHistory,&QAction::triggered,this,[this]{recentFolders_.clear();recentFiles_.clear();lastFiles_.clear();saveHistory();});
    recursive_=check("Include subfolders","recursive",true);toolbar->addWidget(recursive_);
    toolbar->addSeparator();wheelChanges_=check("Wheel changes controls","wheelChanges");
    wheelChanges_->setToolTip("Allow the mouse wheel to change dropdowns and numeric inputs. Off lets you scroll past controls without changing conversion settings.");toolbar->addWidget(wheelChanges_);
    checkUpdates_=check("Check for updates","checkForUpdates",true);checkUpdates_->setToolTip("Check at startup and on every hour. Updates are installed only when you click Update.");toolbar->addWidget(checkUpdates_);
    auto* splitter=new QSplitter;setCentralWidget(splitter);
    auto* browser=new QWidget;auto* layout=new QVBoxLayout(browser);layout->setContentsMargins(16,16,16,10);layout->setSpacing(10);
    auto* searchRow=new QHBoxLayout;search_=new QLineEdit;search_->setObjectName("search");search_->setPlaceholderText("Search files and metadata · press Enter");search_->setClearButtonEnabled(true);auto* searchButton=new QPushButton("Search");searchRow->addWidget(search_,1);searchRow->addWidget(searchButton);layout->addLayout(searchRow);
    connect(search_,&QLineEdit::returnPressed,this,&MainWindow::applySearch);connect(searchButton,&QPushButton::clicked,this,&MainWindow::applySearch);
    connect(search_,&QLineEdit::textChanged,this,[this](const QString& text){if(text.isEmpty())applySearch();});
    auto* filters=new QHBoxLayout;instant_=new QLineEdit;instant_->setObjectName("instantFilter");instant_->setPlaceholderText("Instant filter within search results…");instant_->setClearButtonEnabled(true);filters->addWidget(instant_,1);
    metadataFilters_=check("Filters","metadataFilters");filters->addWidget(metadataFilters_);
    today_=check("Today","todayFilter");today_->setToolTip("Image date is today: EXIF date, falling back to file modification date, then creation date.");filters->addWidget(today_);
    format_=new QLineEdit;format_->setObjectName("formatFilter");format_->setPlaceholderText("e.g. jpg, avif, raw");format_->setClearButtonEnabled(true);
    format_->setToolTip("Enter one or more formats/extensions separated by commas or spaces. JPEG/JPG and RAW camera extensions are supported.");
    layout->addLayout(filters);
    auto* advanced=new QGroupBox("Filters");auto* advancedLayout=new QHBoxLayout(advanced);advancedLayout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* types=new QHBoxLayout;types->addWidget(new QLabel("Format"));types->addWidget(format_);range_=combo({"All ranges","SDR","HDR","RAW"},"rangeFilter");types->addWidget(range_);advancedLayout->addLayout(types);
    auto* imageLimits=new QHBoxLayout;advancedLayout->addLayout(imageLimits);
    minImageEdge_=spin(0,100000,0,"minImageEdge");minImageEdge_->setPrefix("Max edge ≥ ");minImageEdge_->setSuffix(" px");minImageEdge_->setSpecialValueText("Any max edge");
    minImageEdge_->setToolTip("Minimum longest edge, independent of portrait/landscape. Combined with the minimum MP filter; both must match.");
    megapixels_=real(0,1000,0,1,"minMegapixels");megapixels_->setSuffix(" MP+");megapixels_->setSpecialValueText("Any MP");
    for(auto* w:QList<QWidget*>{minImageEdge_,megapixels_}){w->ensurePolished();w->setMinimumWidth(w->sizeHint().width());imageLimits->addWidget(w);}
    auto* filterScroll=new QScrollArea;filterScroll->setWidgetResizable(true);filterScroll->setFrameShape(QFrame::NoFrame);filterScroll->setWidget(advanced);filterScroll->setFixedHeight(90);layout->addWidget(filterScroll);filterScroll->hide();
    auto* dates=new QHBoxLayout;advancedLayout->addLayout(dates);dateEnabled_=check("Capture date","dateFilter");dateFrom_=new QDateEdit(QDate::currentDate().addDays(-1));dateTo_=new QDateEdit(QDate::currentDate());dateFrom_->setObjectName("dateFrom");dateTo_->setObjectName("dateTo");
    for(auto* date:{dateFrom_,dateTo_}){date->setDisplayFormat("yyyy-MM-dd");date->setCalendarPopup(true);date->setEnabled(false);}
    dates->addWidget(dateEnabled_);dates->addWidget(dateFrom_);dates->addWidget(new QLabel("to"));dates->addWidget(dateTo_);
    auto* reset=new QPushButton("Reset filters");filters->addWidget(reset);dates->addStretch();
    auto* viewBar=new QHBoxLayout;viewMode_=new QButtonGroup(this);viewMode_->setObjectName("viewMode");viewMode_->setExclusive(true);
    const QStringList viewNames={"Grid","List","Details"};for(int i=0;i<viewNames.size();++i){auto* button=new QPushButton(viewNames[i]);button->setCheckable(true);viewMode_->addButton(button,i);viewBar->addWidget(button);}viewMode_->button(0)->setChecked(true);sort_=new QComboBox;sort_->setObjectName("sortField");
    for(int col=0;col<Catalog::ColumnCount;++col)sort_->addItem(catalog_->headerData(col,Qt::Horizontal,Qt::DisplayRole).toString(),col);
    auto* direction=new QPushButton("Ascending");direction->setObjectName("sortDirection");viewBar->addWidget(new QLabel("Sort"));viewBar->addWidget(sort_);viewBar->addWidget(direction);viewBar->addStretch();
    smoothScrolling_=new QCheckBox("Smooth scrolling");smoothScrolling_->setObjectName("smoothScrolling");
    smoothScrolling_->setToolTip("Animate scrolling in the image grid, list and details views.");viewBar->addWidget(smoothScrolling_);
    autoAspect_=check("Auto","autoThumbnailAspect",true);autoAspect_->setToolTip("Sample two images on import and in the viewport’s first row when scrolling. Ratio ranges choose the shape; disagreement keeps the current shape.");viewBar->addWidget(autoAspect_);
    portraitAspect_=check("Portrait","portraitThumbnailAspect");portraitAspect_->setToolTip("Allow Auto to choose portrait cells. Off limits automatic cell shapes to landscape or 1:1; manual ratio choices remain available.");viewBar->addWidget(portraitAspect_);
    thumbnailAspect_=new PopupButton;thumbnailAspect_->setObjectName("thumbnailAspect");thumbnailAspect_->setText("1:1");
    auto* aspectMenu=new QMenu(thumbnailAspect_);thumbnailAspect_->setMenu(aspectMenu);auto* aspectActions=new QActionGroup(this);aspectActions->setExclusive(true);
    for(int i=0;i<aspectNames.size();++i){auto* action=aspectMenu->addAction(aspectNames[i]);action->setData(i);action->setCheckable(true);action->setChecked(i==aspectIndex_);aspectActions->addAction(action);connect(action,&QAction::triggered,this,[this,i]{setThumbnailAspect(i);});}
    thumbnailAspect_->setToolTip("Thumbnail cell aspect ratio; images retain their original proportions");viewBar->addWidget(thumbnailAspect_);

    aspectTimer_=new QTimer(this);aspectTimer_->setSingleShot(true);aspectTimer_->setInterval(50);connect(aspectTimer_,&QTimer::timeout,this,&MainWindow::updateAutoThumbnailAspect);
    thumbnailFit_=new QPushButton("Fit");thumbnailFit_->setObjectName("thumbnailFit");thumbnailFit_->setCheckable(true);
    thumbnailFit_->setToolTip("Fit shows the whole image. Fill crops the image to fill its thumbnail cell. Click to switch.");viewBar->addWidget(thumbnailFit_);
    viewBar->addWidget(new QLabel("Minimum size"));
    size_=new QSlider(Qt::Horizontal);size_->setObjectName("thumbnailSize");size_->setRange(100,500);size_->setSingleStep(10);size_->setPageStep(10);size_->setValue(200);size_->setFixedWidth(140);
    size_->setToolTip("Minimum grid cell width in dp (100–500). Like LocalViewer, columns adapt to this minimum plus fixed gaps, and cells stretch evenly to fill the available width.");viewBar->addWidget(size_);
    auto* sizeValue=new QLabel("200 dp");sizeValue->setMinimumWidth(48);viewBar->addWidget(sizeValue);
    connect(size_,&QSlider::valueChanged,this,[this,sizeValue](int value){const int snapped=((value+5)/10)*10;if(value!=snapped){size_->setValue(snapped);return;}sizeValue->setText(QString::number(value)+" dp");});layout->addLayout(viewBar);
    views_=new QStackedWidget;list_=new FileListView;table_=new FileTableView;delegate_=new ThumbnailDelegate(list_);list_->setItemDelegate(delegate_);
    list_->setModel(proxy_);table_->setModel(proxy_);list_->setSelectionModel(selection_);table_->setSelectionModel(selection_);
    table_->verticalHeader()->hide();table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);table_->setColumnWidth(Catalog::Name,260);table_->setColumnWidth(Catalog::Camera,190);for(int column:{Catalog::Captured,Catalog::Created,Catalog::Modified,Catalog::ExifDate})table_->setColumnWidth(column,170);table_->sortByColumn(Catalog::Name,Qt::AscendingOrder);
    views_->addWidget(list_);views_->addWidget(table_);layout->addWidget(views_,1);
    counts_=new QLabel("Drop files or folders here, or choose Open folder.");counts_->setObjectName("catalogCounts");
    auto* libraryActions=new QWidget;libraryActions->setObjectName("libraryActions");auto* actionsLayout=new QHBoxLayout(libraryActions);actionsLayout->setContentsMargins(0,0,0,0);actionsLayout->addWidget(counts_);actionsLayout->addStretch();
    statusFilter_=new QButtonGroup(this);statusFilter_->setObjectName("statusFilter");statusFilter_->setExclusive(true);
    auto separator=[actionsLayout](const char* name){auto* line=new ToolbarSeparator;line->setObjectName(name);actionsLayout->addWidget(line);};
    auto tab=[this,actionsLayout](const QString& text,int id,const char* name){auto* button=new QPushButton(text);button->setObjectName(name);button->setCheckable(true);statusFilter_->addButton(button,id);actionsLayout->addWidget(button);return button;};
    cancelJob_=new QPushButton("Cancel job");cancelJob_->setObjectName("cancelJob");cancelJob_->setToolTip("Cancel this job’s pending outputs; active conversions finish");actionsLayout->addWidget(cancelJob_);cancelJob_->hide();connect(cancelJob_,&QPushButton::clicked,this,[this]{queue_->cancelBatch(viewedJob_);updateCounts();});
    jobButton_=new PopupButton;jobButton_->setObjectName("statusJob");jobButton_->setCheckable(true);statusFilter_->addButton(jobButton_,2);actionsLayout->addWidget(jobButton_);
    jobMenu_=new QMenu(jobButton_);jobMenu_->setObjectName("jobMenu");jobActions_=new QActionGroup(jobMenu_);jobActions_->setExclusive(true);jobButton_->setMenu(jobMenu_);jobButton_->setToolTip("Click to choose a job. Counts: finished / all / skipped / failed (images).");
    running_=new QPushButton("Running");running_->setObjectName("runningJob");actionsLayout->addWidget(running_);connect(running_,&QPushButton::clicked,this,[this]{const auto batch=queue_->runningBatch();if(batch>=0)showJob(batch);});
    tab("Remember",5,"statusRemember")->setToolTip("Images you clicked or individually selected; Select all does not add images");
    tab("Selected",1,"statusSelected")->setToolTip("Your selection across all views; image clicks leave it unchanged");
    separator("collectionSeparator");
    tab("All",0,"statusAll")->setChecked(true);tab("Finished",3,"statusFinished");tab("Failed",4,"statusFailed");
    separator("selectionSeparator");
    connect(statusFilter_,&QButtonGroup::idClicked,this,&MainWindow::updateFilter);
    jobSaveTimer_=new QTimer(this);jobSaveTimer_->setSingleShot(true);jobSaveTimer_->setInterval(500);connect(jobSaveTimer_,&QTimer::timeout,this,&MainWindow::saveJobs);
    nextJob_=std::clamp(appSettings().value("jobs/nextNumber",1).toLongLong(),qint64(1),qint64(1000000001));
    if(appSettings().value("controls/loadPreviousJobs",false).toBool()){loadJobs();jobHistoryLoaded_=true;}
    refreshJobs();
    auto* selectAll=new QPushButton("Select all");selectAll->setObjectName("selectAllImages");actionsLayout->addWidget(selectAll);
    connect(selectAll,&QPushButton::clicked,this,&MainWindow::selectVisible);
    auto* deselect=new QPushButton("Deselect");deselect->setObjectName("deselectImages");actionsLayout->addWidget(deselect);connect(deselect,&QPushButton::clicked,this,&MainWindow::clearImageSelection);
    remove_=new QPushButton("Remove selected");remove_->setObjectName("removeSelected");actionsLayout->addWidget(remove_);connect(remove_,&QPushButton::clicked,this,[this]{catalog_->removePaths(selectedPaths());});
    clear_=new QPushButton("Clear");clear_->setObjectName("clearLibrary");actionsLayout->addWidget(clear_);connect(clear_,&QPushButton::clicked,this,&MainWindow::clearCurrentTab);auto* footerScroll=new QScrollArea;footerScroll->setObjectName("libraryFooterScroll");footerScroll->setFrameShape(QFrame::NoFrame);footerScroll->setWidgetResizable(true);footerScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);footerScroll->setWidget(libraryActions);footerScroll->setFixedHeight(libraryActions->sizeHint().height()+style()->pixelMetric(QStyle::PM_ScrollBarExtent)+2);layout->addWidget(footerScroll);

    auto* sidebar=new QTabWidget;sidebar->setObjectName("settingsTabs");sidebar->setMinimumWidth(330);sidebar->setMaximumWidth(460);
    auto* scroll=new QScrollArea;scroll->setObjectName("conversionScroll");scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(buildControls());scroll->setMinimumHeight(150);
    sidebar->addTab(scroll,"Conversion");
    inspector_=new QPlainTextEdit;inspector_->setObjectName("metadataInspector");inspector_->setReadOnly(true);sidebar->addTab(inspector_,"Metadata");
    auto* logPanel=new QWidget;auto* logLayout=new QVBoxLayout(logPanel);logLayout->setContentsMargins(0,0,0,0);
    outputLog_=new QPlainTextEdit;outputLog_->setObjectName("outputLog");outputLog_->setReadOnly(true);outputLog_->setMaximumBlockCount(5000);outputLog_->setLineWrapMode(QPlainTextEdit::NoWrap);logLayout->addWidget(outputLog_,1);sidebar->addTab(logPanel,"Log");
    auto* clearLog=new FloatingClearButton(outputLog_);clearLog->setObjectName("clearLog");connect(clearLog,&QPushButton::clicked,outputLog_,&QPlainTextEdit::clear);
    connect(OutputCapture::instance(),&OutputCapture::textReady,this,[this](const QString& text){auto* bar=outputLog_->verticalScrollBar();const bool follow=bar->value()==bar->maximum();QTextCursor cursor(outputLog_->document());cursor.movePosition(QTextCursor::End);cursor.insertText(text);if(follow){outputLog_->setTextCursor(cursor);outputLog_->ensureCursorVisible();}});
    logMessage("JXRConverter "+QString::fromStdString(jxrc::version()));
    auto* sidePanel=new QWidget;sidePanel->setMinimumWidth(330);sidePanel->setMaximumWidth(460);auto* sideLayout=new QVBoxLayout(sidePanel);sideLayout->setContentsMargins(0,0,0,0);sideLayout->addWidget(sidebar,1);
    auto* outputToggle=new QPushButton("▾ Output queue");outputToggle->setObjectName("toggleOutput");outputToggle->setCheckable(true);outputToggle->setChecked(true);auto* outputHeader=new WidthMatchedWidget(scroll->parentWidget());auto* headerLayout=new QHBoxLayout(outputHeader);headerLayout->setContentsMargins(14,0,14+scroll->verticalScrollBar()->sizeHint().width(),0);headerLayout->addWidget(outputToggle);sideLayout->addWidget(outputHeader,0,Qt::AlignHCenter);
    auto* outputPanel=new WidthMatchedWidget(scroll->parentWidget());outputPanel->setObjectName("outputPanel");auto* outputLayout=new QVBoxLayout(outputPanel);outputLayout->setContentsMargins(0,0,0,0);
    auto* startButtons=new QHBoxLayout;startButtons->setContentsMargins(12,0,12,0);
    auto* openQueueOutput=new QPushButton;openQueueOutput->setObjectName("openQueueOutputFolder");openQueueOutput->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));openQueueOutput->setToolTip("Open output folder");startButtons->addWidget(openQueueOutput);startButtons->addWidget(visible_,1);connect(openQueueOutput,&QPushButton::clicked,this,&MainWindow::openOutputFolder);
    startButtons->addWidget(selected_,1);outputLayout->addLayout(startButtons);
    auto* outputScroll=new QScrollArea;outputScroll->setObjectName("outputQueueScroll");outputScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);outputScroll->setWidgetResizable(true);outputScroll->setFrameShape(QFrame::NoFrame);outputScroll->setWidget(queuePanel_);outputScroll->setFixedHeight(330);outputLayout->addWidget(outputScroll);
    auto* progressFrame=new QWidget;auto* progressLayout=new QVBoxLayout(progressFrame);progressLayout->setContentsMargins(14,6,0,12);auto* progressRow=new QHBoxLayout;progressRow->setSpacing(0);progressRow->addWidget(progress_,1);
    progressStop_=new QPushButton("×");progressStop_->setObjectName("stopProgress");progressStop_->setToolTip("Stop queue; active files finish first");progressStop_->setFixedWidth(28);progressStop_->hide();
    // Match the queue group's side padding and scrollbar gutter. Keep the
    // stop-button slot reserved so the bar keeps these edges while running.
    auto* stopSlot=new QWidget;stopSlot->setFixedWidth(14+outputScroll->verticalScrollBar()->sizeHint().width());auto* stopLayout=new QHBoxLayout(stopSlot);stopLayout->setContentsMargins(0,0,0,0);stopLayout->addWidget(progressStop_,0,Qt::AlignRight);progressRow->addWidget(stopSlot);progressLayout->addLayout(progressRow);
    connect(progressStop_,&QPushButton::clicked,queue_,&ConversionQueue::cancelPending);outputLayout->addWidget(progressFrame);sideLayout->addWidget(outputPanel,0,Qt::AlignHCenter);
    connect(outputToggle,&QPushButton::toggled,this,[outputScroll,outputToggle](bool on){outputScroll->setVisible(on);outputToggle->setText(on?"▾ Output queue":"▸ Output queue");});
    splitter->addWidget(browser);splitter->addWidget(sidePanel);splitter->setStretchFactor(0,1);splitter->setSizes({1050,370});
    auto* spacer=new QWidget;spacer->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);toolbar->addWidget(spacer);
    updater_=new Updater(this);auto* updateButton=new QPushButton("Update");updateButton->setObjectName("updateApplication");toolbar->addWidget(updateButton);
    connect(updateButton,&QPushButton::clicked,this,[this]{if(!queue_->busy())updater_->start();});
    connect(updater_,&Updater::statusChanged,updateButton,&QPushButton::setText);
    connect(updater_,&Updater::message,this,&MainWindow::logMessage);
    connect(updater_,&Updater::updateAvailableChanged,this,[updateButton](bool available){updateButton->setProperty("primary",available);updateButton->style()->unpolish(updateButton);updateButton->style()->polish(updateButton);updateButton->update();});
    updateTimer_=new QTimer(this);updateTimer_->setObjectName("hourlyUpdateCheck");updateTimer_->setSingleShot(true);updateTimer_->setTimerType(Qt::PreciseTimer);
    connect(updateTimer_,&QTimer::timeout,this,&MainWindow::checkForUpdates);
    connect(checkUpdates_,&QCheckBox::toggled,this,[this](bool on){if(on)checkForUpdates();else updateTimer_->stop();});
    connect(updater_,&Updater::busyChanged,this,[this,updateButton](bool busy){updateButton->setEnabled(!busy);updateCounts();});
    auto* theme=combo({"Auto","Light","Dark"},"theme");toolbar->addWidget(theme);
    theme->setCurrentIndex(appSettings().value("appearance/theme",0).toInt());applyDesktopStyle(theme->currentIndex());
    connect(theme,&QComboBox::currentIndexChanged,this,[theme]{appSettings().setValue("appearance/theme",theme->currentIndex());applyDesktopStyle(theme->currentIndex());});
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(qApp->styleHints(),&QStyleHints::colorSchemeChanged,this,[theme]{if(theme->currentIndex()==0)applyDesktopStyle(0);});
#endif
    auto openImage=[this](const QModelIndex& index){if(index.isValid())QDesktopServices::openUrl(QUrl::fromLocalFile(index.data(Catalog::PathRole).toString()));};
    connect(list_,&QListView::doubleClicked,this,openImage);connect(table_,&QTableView::doubleClicked,this,openImage);
    for(auto* view:QList<QAbstractItemView*>{list_,table_}) {
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(view,&QWidget::customContextMenuRequested,this,[this,view,openImage](const QPoint& point){
            const auto fieldIndex=view->indexAt(point);
            if(!fieldIndex.isValid()){QMenu menu;auto* reset=menu.addAction("Reset filters");reset->setObjectName("resetFilters");if(menu.exec(view->viewport()->mapToGlobal(point))==reset)resetFilters();return;}
            const int fieldColumn=fieldIndex.column();const QString fieldValue=fieldIndex.data().toString();
            auto index=fieldIndex.siblingAtColumn(0);
            if(!collectionView()&&!selection_->isRowSelected(index.row(),{}))selection_->setCurrentIndex(index,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
            QMenu menu;QAction* addField=nullptr;
            if(view==table_){addField=menu.addAction("Add this to filters");addField->setObjectName("addFieldToFilters");addField->setEnabled(!fieldValue.isEmpty());addField->setToolTip(catalog_->headerData(fieldColumn,Qt::Horizontal,Qt::DisplayRole).toString()+": "+fieldValue);}
            auto* reset=menu.addAction("Reset filters");reset->setObjectName("resetFilters");menu.addSeparator();
            auto* open=menu.addAction("Open image");auto* folder=menu.addAction("Open folder");auto* camera=menu.addAction("Add camera to search");auto* format=menu.addAction("Add format to search");auto* bits=menu.addAction("Add bit depth to search");auto* date=menu.addAction("Set date");date->setObjectName("setImageDate");date->setToolTip("Filter to this image’s day");auto* start=menu.addAction("Set start date");auto* end=menu.addAction("Set end date");
            const QString currentImage=index.data(Catalog::PathRole).toString();rememberedImages_.insert(currentImage);
            auto* convertImage=menu.addAction("Convert image");convertImage->setObjectName("convertImage");convertImage->setEnabled(!updater_->installing()&&!selectedTargets().isEmpty());
            auto* convertTo=menu.addMenu("Convert to…");convertTo->setObjectName("convertTo");convertTo->setEnabled(!updater_->installing());
            for(int i=0;i<targets_->count();++i){const auto* item=targets_->item(i);auto* action=convertTo->addAction(item->text());action->setObjectName("convertTarget"+item->data(Qt::UserRole).toString());action->setData(item->data(Qt::UserRole));action->setEnabled(item->flags()&Qt::ItemIsEnabled);}
            const auto& record=catalog_->record(proxy_->mapToSource(index).row());const QDate captured=record.imageDate().date();const QString cameraName=record.camera,formatName=record.format;const int depth=record.bits;camera->setEnabled(!cameraName.isEmpty());bits->setEnabled(depth>0);date->setEnabled(captured.isValid());start->setEnabled(captured.isValid());end->setEnabled(captured.isValid());
            const auto queueSelection=selectedPaths();
            auto* sendJob=menu.addAction("Send selected to new job");sendJob->setObjectName("sendSelectedToJob");sendJob->setEnabled(!selectedPaths().isEmpty()&&!updater_->installing());
            auto* cancelSelected=menu.addAction("Cancel selected");cancelSelected->setObjectName("cancelSelectedQueue");cancelSelected->setToolTip("Cancel pending outputs for selected images; active conversions finish");cancelSelected->setEnabled(queue_->hasPendingFor(queueSelection));
            auto* remove=menu.addAction("Remove selected");remove->setEnabled(!queue_->busy());
            const auto action=menu.exec(view->viewport()->mapToGlobal(point));
            if(action==reset)resetFilters();
            else if(addField&&action==addField){
                const auto key=catalogFieldKey(fieldColumn);const auto term=key+":"+quotedSearchValue(fieldValue);
                search_->setText((search_->text()+" "+term).trimmed());applySearch();
            }
            else if(action==convertImage){convertPaths({currentImage},selectedTargets(),false,true);}
            else if(action&&action->parent()==convertTo){convertPaths({currentImage},{Target(action->data().toInt())},true,true);}
            else if(action==sendJob){createDraftJob(queueSelection);}
            else if(action==cancelSelected){queue_->cancelPendingFor(queueSelection);}
            else if(action==camera||action==format||action==bits){
                const QString term=action==camera?"camera:"+quotedSearchValue(cameraName):action==format?"format:"+quotedSearchValue(formatName):"bits:"+QString::number(depth);
                search_->setText((search_->text()+" "+term).trimmed());applySearch();
            }else if(action==date||action==start||action==end){
                if(action==date){dateFrom_->setDate(captured);dateTo_->setDate(captured);}
                else if(action==start){dateFrom_->setDate(captured);if(dateTo_->date()<captured)dateTo_->setDate(captured);}
                else{dateTo_->setDate(captured);if(dateFrom_->date()>captured)dateFrom_->setDate(captured);}
                today_->setChecked(false);metadataFilters_->setChecked(true);dateEnabled_->setChecked(true);updateFilter();
            }else if(action==open)openImage(index);else if(action==folder)QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(index.data(Catalog::PathRole).toString()).absolutePath()));else if(action==remove)catalog_->removePaths(selectedPaths());
        });
    }
    connect(list_,&FileListView::pathsDropped,this,&MainWindow::addDroppedPaths);connect(table_,&FileTableView::pathsDropped,this,&MainWindow::addDroppedPaths);
    connect(instant_,&QLineEdit::textChanged,this,&MainWindow::updateFilter);
    connect(range_,&QComboBox::currentIndexChanged,this,&MainWindow::updateFilter);
    connect(format_,&QLineEdit::textChanged,this,&MainWindow::updateFilter);

    connect(minImageEdge_,&QSpinBox::valueChanged,this,&MainWindow::updateFilter);
    connect(megapixels_,&QDoubleSpinBox::valueChanged,this,&MainWindow::updateFilter);
    connect(dateEnabled_,&QCheckBox::toggled,this,[this](bool on){dateFrom_->setEnabled(on);dateTo_->setEnabled(on);updateFilter();});
    for(auto* d:{dateFrom_,dateTo_})connect(d,&QDateEdit::dateChanged,this,&MainWindow::updateFilter);
    connect(today_,&QCheckBox::toggled,this,&MainWindow::updateFilter);
    connect(metadataFilters_,&QCheckBox::toggled,this,[this,filterScroll](bool on){filterScroll->setVisible(on);instant_->setPlaceholderText(on?"Search filenames, camera, lens, EXIF tags or values…":"Instant filter · e.g. sunset");updateFilter();});
    connect(reset,&QPushButton::clicked,this,&MainWindow::resetFilters);
    // Refresh Today across local midnight, including when the application stays open.
    auto* dateRefresh=new QTimer(this);dateRefresh->setInterval(30000);connect(dateRefresh,&QTimer::timeout,this,[this]{if(today_->isChecked())updateFilter();});dateRefresh->start();
    connect(sort_,&QComboBox::currentIndexChanged,this,[this]{proxy_->sort(sort_->currentData().toInt(),sortOrder_);updateBackgroundMetadata();});
    connect(direction,&QPushButton::clicked,this,[this,direction]{sortOrder_=sortOrder_==Qt::AscendingOrder?Qt::DescendingOrder:Qt::AscendingOrder;direction->setText(sortOrder_==Qt::AscendingOrder?"Ascending":"Descending");proxy_->sort(sort_->currentData().toInt(),sortOrder_);});
    connect(table_->horizontalHeader(),&QHeaderView::sortIndicatorChanged,this,[this,direction](int column,Qt::SortOrder order){sortOrder_=order;sort_->setCurrentIndex(column);direction->setText(order==Qt::AscendingOrder?"Ascending":"Descending");});
    connect(smoothScrolling_,&QCheckBox::toggled,this,&MainWindow::updatePresentation);
    connect(autoAspect_,&QCheckBox::toggled,this,[this](bool on){catalog_->setAspectSampling(on);if(on){if(!portraitAspect_->isChecked()&&aspectIndex_>=3)setThumbnailAspect(2);requestAspectSample(list_->verticalScrollBar()->value()>0);}else{aspectSamplePending_=false;aspectTimer_->stop();}});
    connect(portraitAspect_,&QCheckBox::toggled,this,[this](bool on){if(!autoAspect_->isChecked())return;if(!on&&aspectIndex_>=3)setThumbnailAspect(2);requestAspectSample(list_->verticalScrollBar()->value()>0);});
    connect(list_,&FileListView::viewportChanged,this,[this]{if(!changingAspect_&&viewMode_->checkedId()==0&&!(aspectSamplePending_&&!aspectViewportSample_))requestAspectSample(true);});
    connect(thumbnailFit_,&QPushButton::toggled,this,&MainWindow::updatePresentation);
    connect(viewMode_,&QButtonGroup::idClicked,this,&MainWindow::updatePresentation);connect(size_,&QSlider::valueChanged,this,&MainWindow::updatePresentation);
    connect(list_->verticalScrollBar(),&QScrollBar::valueChanged,this,[this]{if(!changingAspect_&&viewMode_->checkedId()==0)requestAspectSample(true);});
    connect(list_,&FileListView::bulkSelectionChanged,this,[this](bool selecting){bulkSelecting_=selecting;});
    connect(table_,&FileTableView::bulkSelectionChanged,this,[this](bool selecting){bulkSelecting_=selecting;});
    connect(selection_,&QItemSelectionModel::selectionChanged,this,&MainWindow::changeSelection);
    for(auto* view:QList<QAbstractItemView*>{list_,table_})connect(view,&QAbstractItemView::clicked,this,[this](const QModelIndex& index){rememberedImages_.insert(index.data(Catalog::PathRole).toString());showMetadata();});
    connect(proxy_,&QAbstractItemModel::rowsInserted,this,[this]{syncSelection();updateCounts();});connect(proxy_,&QAbstractItemModel::rowsRemoved,this,&MainWindow::updateCounts);
    connect(catalog_,&QAbstractItemModel::rowsInserted,this,[this]{requestAspectSample(false);});
    connect(catalog_,&QAbstractItemModel::rowsAboutToBeRemoved,this,[this](const QModelIndex&,int first,int last){for(int row=first;row<=last;++row){const auto path=catalog_->record(row).path;selectedImages_.remove(path);rememberedImages_.remove(path);for(auto& selected:jobSelections_)selected.remove(path);}});
    connect(catalog_,&QAbstractItemModel::rowsRemoved,this,[this]{if(!catalog_->rowCount())aspectSamplePending_=false;updateFilter();});
    connect(catalog_,&QAbstractItemModel::modelReset,this,[this]{aspectSamplePending_=false;loggedWarnings_.clear();selectedImages_.clear();jobSelections_.clear();rememberedImages_.clear();updateFilter();showMetadata();});
    connect(catalog_,&QAbstractItemModel::dataChanged,this,[this](const QModelIndex& first,const QModelIndex& last){
        updateCounts();showMetadata();if(autoAspect_->isChecked()&&aspectSamplePending_)aspectTimer_->start();
        for(int row=first.row();row<=last.row();++row){const auto& record=catalog_->record(row);if(record.metadataWarning.isEmpty())continue;const auto warning=record.path+"\n"+record.metadataWarning;if(!loggedWarnings_.contains(warning)){loggedWarnings_.insert(warning);logMessage("Warning: "+warning);}}
    });
    connect(catalog_,&Catalog::scanStateChanged,this,[this](bool scanning){statusBar()->showMessage(scanning?"Scanning folders…":"Folder scan complete",scanning?0:4000);updateCounts();});
    connect(catalog_,&Catalog::scanError,this,[this](const QString& error){statusBar()->showMessage(error,6000);logMessage("Scan error: "+error);});
    connect(queue_,&ConversionQueue::outputStarted,this,[this](qint64 batch,const QString& input,const QString& output){updateJobOutput(batch,input,output,"Running");});
    connect(queue_,&ConversionQueue::outputFinished,this,[this](qint64 batch,const QString& input,const QString& output,const QString& state){updateJobOutput(batch,input,output,state);});
    connect(queue_,&ConversionQueue::statusChanged,catalog_,&Catalog::setStatus);
    connect(queue_,&ConversionQueue::statusChanged,this,[this](const QString& path,const QString& state,const QString& detail){logMessage(state+": "+path+(detail.isEmpty()?QString{}:"\n"+detail));});
    connect(queue_,&ConversionQueue::progress,this,[this](int done,int total,int active){progress_->setRange(0,total);progress_->setValue(done);queueState_->setText(QString("%1 / %2 outputs finished · %3 active").arg(done).arg(total).arg(active));refreshJobs();updateCounts();});
    connect(queue_,&ConversionQueue::batchFinished,this,[this](int success,int failed,int cancelled){saveJobs();queueState_->setText(QString("%1 done · %2 skipped · %3 failed · %4 cancelled").arg(success).arg(queue_->skipped()).arg(failed).arg(cancelled));logMessage(queueState_->text());updateCounts();if(closeWhenIdle_)QTimer::singleShot(0,this,&QWidget::close);});
    auto* deleteShortcut=new QShortcut(QKeySequence::Delete,this);connect(deleteShortcut,&QShortcut::activated,this,[this]{if(!queue_->busy()&&(list_->hasFocus()||table_->hasFocus()))catalog_->removePaths(selectedPaths());});
    auto* allShortcut=new QShortcut(QKeySequence::SelectAll,this);connect(allShortcut,&QShortcut::activated,this,[this]{if(list_->hasFocus()||table_->hasFocus())selectVisible();});
    for(auto* view:QList<QAbstractItemView*>{list_,table_}) {
        auto* copy=new QShortcut(QKeySequence::Copy,view);copy->setContext(Qt::WidgetWithChildrenShortcut);
        connect(copy,&QShortcut::activated,this,[this]{QList<QUrl> urls;for(const auto& path:selectedPaths())urls.append(QUrl::fromLocalFile(path));if(urls.isEmpty())return;auto* mime=new QMimeData;mime->setUrls(urls);mime->setData("application/x-kde-cutselection","0");qApp->clipboard()->setMimeData(mime);});
    }
    for(auto* control:findChildren<QComboBox*>())control->installEventFilter(this);
    for(auto* control:findChildren<QAbstractSpinBox*>())control->installEventFilter(this);
    auto settings=appSettings();restoreGeometry(settings.value("window/geometry").toByteArray());viewMode_->button(std::clamp(settings.value("browser/view",0).toInt(),0,2))->setChecked(true);size_->setValue(settings.value("browser/minCellDp",200).toInt());
    wheelChanges_->setChecked(settings.value("browser/wheelChanges",false).toBool());
    output_->setText(settings.value("conversion/output",QDir::homePath()+"/Pictures/Converted").toString());recursive_->setChecked(settings.value("browser/recursive",true).toBool());
    restoreSettings();appliedSearch_=settings.value("search/applied",search_->text()).toString();direction->setText(sortOrder_==Qt::AscendingOrder?"Ascending":"Descending");proxy_->sort(sort_->currentData().toInt(),sortOrder_);filterScroll->setVisible(metadataFilters_->isChecked());outputScroll->setVisible(outputToggle->isChecked());outputToggle->setText(outputToggle->isChecked()?"▾ Output queue":"▸ Output queue");
    catalog_->setAspectSampling(autoAspect_->isChecked());updateMode();updateFilter();updatePresentation();updateCounts();requestAspectSample(false);
    QTimer::singleShot(0,this,&MainWindow::checkForUpdates);

    QTimer::singleShot(0,this,[this]{if(loadFolder_->isChecked()&&!recentFolders_.isEmpty())addPaths({recentFolders_.first()});if(loadFiles_->isChecked())addPaths(lastFiles_);});
}
MainWindow::~MainWindow() {
    saveJobs();saveSettings();
    auto settings=appSettings();settings.setValue("window/geometry",saveGeometry());settings.setValue("browser/view",viewMode_->checkedId());settings.setValue("browser/minCellDp",size_->value());settings.setValue("browser/wheelChanges",wheelChanges_->isChecked());settings.setValue("browser/recursive",recursive_->isChecked());settings.setValue("conversion/output",output_->text());
}
namespace {
const char* settingProperty(QWidget* widget) {
    if(qobject_cast<QComboBox*>(widget))return "currentIndex";
    if(qobject_cast<QDateEdit*>(widget))return "date";
    if(qobject_cast<QCheckBox*>(widget)||qobject_cast<QPushButton*>(widget))return "checked";
    if(qobject_cast<QLineEdit*>(widget))return "text";
    if(qobject_cast<QTabWidget*>(widget))return "currentIndex";
    return "value";
}
}
QList<QWidget*> MainWindow::persistentControls() const {
    return {checkUpdates_,smoothScrolling_,autoAspect_,portraitAspect_,thumbnailFit_,mode_,preset_,avifDepth_,rawMode_,wb_,quality_,distance_,downsize_,exif_,transcode_,
        exposure_,maxEdge_,bt2020_,convertGamut_,rawExposure_,kelvin_,highlight_,shoulder_,nits_,shadows_,midtones_,highlights_,cameraLook_,linearHdr_,
        formatFolders_,dateFolders_,jobFolders_,sourceFolder_,renameConflicts_,overwrite_,workers_,loadFolder_,loadFiles_,loadJobs_,acceptSdr_,metadataFilters_,today_,dateEnabled_,dateFrom_,dateTo_,search_,instant_,format_,range_,minImageEdge_,megapixels_,sort_,
        findChild<QPushButton*>("toggleOutput"),findChild<QTabWidget*>("settingsTabs")};
}
void MainWindow::restoreSettings() {
    auto settings=appSettings();
    for(auto* widget:persistentControls()) {
        if(!widget)continue;
        const auto key="controls/"+widget->objectName();
        if(settings.contains(key)){const QSignalBlocker blocker(widget);widget->setProperty(settingProperty(widget),settings.value(key));}
    }
    if(settings.contains("conversion/targets")) {
        const auto saved=settings.value("conversion/targets").toStringList();const QSignalBlocker blocker(targets_);
        for(int i=0;i<targets_->count();++i){auto* item=targets_->item(i);item->setCheckState(saved.contains(item->data(Qt::UserRole).toString())?Qt::Checked:Qt::Unchecked);}
    }
    aspectIndex_=std::clamp(settings.value("browser/aspectRatio",2).toInt(),0,4);
    sortOrder_=settings.value("browser/descending",false).toBool()?Qt::DescendingOrder:Qt::AscendingOrder;
    findChild<QSlider*>("rawTargetNitsSlider")->setValue(nits_->value());
    findChild<QSlider*>("rawShoulderSlider")->setValue(qRound(shoulder_->value()*100));
}
void MainWindow::saveSettings() const {
    auto settings=appSettings();
    for(auto* widget:persistentControls())if(widget)settings.setValue("controls/"+widget->objectName(),widget->property(settingProperty(widget)));
    QStringList saved;for(const auto target:selectedTargets())saved.append(QString::number(int(target)));
    settings.setValue("jobs/nextNumber",nextJob_);settings.setValue("browser/aspectRatio",aspectIndex_);settings.setValue("search/applied",appliedSearch_);settings.setValue("conversion/targets",saved);settings.setValue("browser/descending",sortOrder_==Qt::DescendingOrder);settings.sync();
}
QWidget* MainWindow::buildControls() {
    auto* panel=new QWidget;panel->setObjectName("conversionControls");auto* layout=new QVBoxLayout(panel);layout->setContentsMargins(14,16,14,16);layout->setSpacing(14);
    auto* general=new QFormLayout;mode_=combo({"Preserve source values","Process pixels"},"conversionMode");mode_->setCurrentIndex(1);general->addRow("Mode",mode_);layout->addLayout(general);
    auto* outputFormats=new QGroupBox("Output formats · select one or more");outputFormats->setObjectName("outputFormats");auto* outputFormatsLayout=new QVBoxLayout(outputFormats);
    targets_=new QListWidget;targets_->setObjectName("conversionTargets");targets_->setSelectionMode(QAbstractItemView::NoSelection);targets_->setMinimumHeight(155);targets_->setMaximumHeight(210);
    const QStringList targetNames={"JPEG · SDR","JPEG · gain map","JXL · SDR","JXL · linear HDR","AVIF · SDR / source","AVIF · PQ","AVIF · gain map","JXL · PQ","JXL · scRGB HDR"};
    for(int i=0;i<targetNames.size();++i){auto* item=new QListWidgetItem(targetNames[i],targets_);item->setData(Qt::UserRole,i);item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable);item->setCheckState(i==4?Qt::Checked:Qt::Unchecked);}
    targets_->insertItem(4,targets_->takeItem(7));
    targets_->insertItem(4,targets_->takeItem(8));
    targets_->viewport()->installEventFilter(this);
    outputFormatsLayout->addWidget(targets_);
    auto* targetActions=new QHBoxLayout;
    acceptSdr_=check("Accept SDR formats","acceptSdr",appSettings().value("conversion/acceptSdr",false).toBool());
    acceptSdr_->setToolTip("For HDR targets, include SDR-source filenames using the SDR output equivalent. The scRGB HDR target converts them to scRGB. Like LocalViewer, JXL, JXR and RAW filenames are in the HDR group; JPEG, AVIF, PNG, WebP, TIFF, BMP and GIF are in the SDR group regardless of their contents.");
    connect(acceptSdr_,&QCheckBox::toggled,this,[](bool on){appSettings().setValue("conversion/acceptSdr",on);});
    targetActions->addWidget(acceptSdr_);targetActions->addStretch();
    auto* clearTargets=new QPushButton("None");clearTargets->setObjectName("clearTargets");clearTargets->setToolTip("Deselect all output formats.");targetActions->addWidget(clearTargets);
    auto* allTargets=new QPushButton("All");allTargets->setObjectName("allTargets");allTargets->setToolTip("Select all output formats available in the current conversion mode.");targetActions->addWidget(allTargets);
    connect(clearTargets,&QPushButton::clicked,this,[this]{const QSignalBlocker blocker(targets_);for(int i=0;i<targets_->count();++i)targets_->item(i)->setCheckState(Qt::Unchecked);updateMode();});
    connect(allTargets,&QPushButton::clicked,this,[this]{const QSignalBlocker blocker(targets_);for(int i=0;i<targets_->count();++i){auto* item=targets_->item(i);if(item->flags()&Qt::ItemIsEnabled)item->setCheckState(Qt::Checked);}updateMode();});
    outputFormatsLayout->addLayout(targetActions);layout->addWidget(outputFormats);
    auto* qualityGroup=new QGroupBox("Quality");auto* qualityLayout=new QFormLayout(qualityGroup);preset_=combo({"Lossless","High","Medium","Low","Custom"},"qualityPreset");preset_->setCurrentIndex(1);qualityLayout->addRow("Preset",preset_);
    auto* warning=new QLabel("JPEG remains lossy with every preset.");warning->setWordWrap(true);qualityLayout->addRow(warning);
    custom_=new QGroupBox("Custom quality");auto* customForm=new QFormLayout(custom_);quality_=spin(1,100,90,"codecQuality");distance_=real(0,3,0.5,0.05,"jxlDistance");avifDepth_=combo({"10 bit","12 bit"},"avifDepth");downsize_=check("Halve gain-map dimensions","downsizeGainmap");
    customForm->addRow("JPEG / AVIF",quality_);customForm->addRow("JXL distance",distance_);avifDepth_->setToolTip("Maximum AVIF depth. 8-bit sources remain 8-bit; AVIF supports up to 12 bits per channel.");customForm->addRow("AVIF max depth",avifDepth_);customForm->addRow(downsize_);qualityLayout->addRow(custom_);layout->addWidget(qualityGroup);
    exif_=check("Keep EXIF","keepExif",true);exif_->setToolTip("Retain photographic EXIF, including camera, capture date, exposure and GPS, when present. Processed exports update orientation and dimensions; source raster offsets, thumbnails and maker notes are excluded.");transcode_=check("JXL lossless JPEG transcoding","jpegTranscoding");
    transcode_->setToolTip("Store eligible SDR JPEG coefficients directly in JXL without RGB decoding or additional image loss. Ignores the quality preset for these JPEGs; other inputs use their normal conversion path. Exposure or resizing disables transcoding. Enable Keep EXIF to allow original JPEG byte reconstruction.");
    layout->addWidget(exif_);layout->addWidget(transcode_);
    processing_=new QGroupBox("Pixel processing");auto* processingForm=new QFormLayout(processing_);exposure_=real(-3,3,0,0.1,"exposure");exposure_->setSuffix(" EV");
    exposure_->setToolTip("0 EV leaves exposure unchanged. −3 EV multiplies linear RGB by 1/8; +3 EV multiplies it by 8. For RAW gain-map output, use Custom RAW exposure instead.");
    maxEdge_=spin(0,65535,0,"maxEdge");maxEdge_->setSpecialValueText("Original size");bt2020_=check("Extended BT.2020 · JXL decoding","extendedBt2020",true);
    bt2020_->setToolTip("When decoding floating-point linear-sRGB JXL, convert extended RGB values to BT.2020 before clipping negative channels. This preserves more wide-gamut color. Convert scRGB gamut to BT.2020 takes priority for scRGB sources. Other input formats are unaffected; it does not select the output transfer function.");
    convertGamut_=check("Convert scRGB gamut to BT.2020","convertGamut",true);
    convertGamut_->setToolTip("Convert scRGB to BT.2020 in Process pixels mode. Uncheck to retain scRGB for untouched linear HDR JXL and sRGB/BT.709 primaries for other outputs.");
    processingForm->addRow(convertGamut_);
    processingForm->addRow("Exposure (EV)",exposure_);processingForm->addRow("Max edge (px)",maxEdge_);processingForm->addRow(bt2020_);layout->addWidget(processing_);
    raw_=new QGroupBox("RAW rendering");auto* rawLayout=new QFormLayout(raw_);rawMode_=combo({"Source headroom","Boost highlights","Custom"},"rawMode");rawMode_->setCurrentIndex(1);rawLayout->addRow("HDR mode",rawMode_);
    rawCustom_=new QWidget;auto* rawForm=new QFormLayout(rawCustom_);rawForm->setContentsMargins(0,0,0,0);
    rawExposure_=real(-3,3,0,0.1,"rawExposure");wb_=combo({"Camera","Auto","Daylight","Cloudy","Shade","Tungsten","Fluorescent","Flash","Kelvin"},"rawWhiteBalance");kelvin_=spin(2000,12000,5200,"rawKelvin");kelvin_->setSingleStep(100);
    highlight_=real(0,3,0,0.1,"rawHighlightStops");shoulder_=real(0,1,0.5,0.05,"rawShoulder");nits_=spin(1000,10000,1500,"rawTargetNits");nits_->setSingleStep(100);
    shadows_=real(-1,1,0,0.05,"rawShadows");midtones_=real(-1,1,0,0.05,"rawMidtones");highlights_=real(-1,1,0,0.05,"rawHighlights");cameraLook_=check("Camera look (SDR)","rawCameraLook",true);linearHdr_=check("Linear HDR · bypass camera look","rawLinearHdr");
    rawForm->addRow(cameraLook_);rawForm->addRow(linearHdr_);rawForm->addRow("Exposure (EV)",rawExposure_);rawForm->addRow("White balance",wb_);rawForm->addRow("Temperature (K)",kelvin_);rawForm->addRow("Highlight stops",highlight_);rawForm->addRow("Shadows",shadows_);rawForm->addRow("Midtones",midtones_);rawForm->addRow("Highlights",highlights_);auto* shoulderRow=new QWidget;auto* shoulderLayout=new QHBoxLayout(shoulderRow);shoulderLayout->setContentsMargins(0,0,0,0);
    auto* shoulderSlider=new QSlider(Qt::Horizontal);shoulderSlider->setObjectName("rawShoulderSlider");shoulderSlider->setRange(0,100);shoulderSlider->setValue(50);shoulderLayout->addWidget(shoulderSlider,1);shoulderLayout->addWidget(shoulder_);
    connect(shoulderSlider,&QSlider::valueChanged,this,[this](int value){shoulder_->setValue(value/100.0);});connect(shoulder_,&QDoubleSpinBox::valueChanged,this,[shoulderSlider](double value){shoulderSlider->setValue(qRound(value*100));});
    shoulderRow->setToolTip("HDR highlight lift: 0 retains the base render; 1 opens highlights toward source headroom or the boost target. No effect on SDR output.");boostTarget_=new QWidget;boostTarget_->setObjectName("rawBoostTarget");auto* targetLayout=new QHBoxLayout(boostTarget_);targetLayout->setContentsMargins(0,0,0,0);
    auto* targetSlider=new QSlider(Qt::Horizontal);targetSlider->setObjectName("rawTargetNitsSlider");targetSlider->setRange(1000,10000);targetSlider->setSingleStep(100);targetSlider->setPageStep(500);targetSlider->setValue(1500);
    nits_->setSuffix(" nits");targetLayout->addWidget(targetSlider,1);targetLayout->addWidget(nits_);
    connect(targetSlider,&QSlider::valueChanged,nits_,&QSpinBox::setValue);connect(nits_,&QSpinBox::valueChanged,targetSlider,&QSlider::setValue);
    boostTarget_->setToolTip("HDR highlight target for Boost highlights and Custom modes. Shoulder controls how strongly highlights approach this target. Source headroom mode uses the source's headroom instead.");
    rawLayout->addRow("HDR boost target",boostTarget_);rawLayout->addRow("HDR shoulder",shoulderRow);rawLayout->addRow(rawCustom_);layout->addWidget(raw_);
    for(auto* widget:raw_->findChildren<QWidget*>())if(qobject_cast<QComboBox*>(widget)||qobject_cast<QAbstractSpinBox*>(widget)||qobject_cast<QCheckBox*>(widget)||qobject_cast<QSlider*>(widget))widget->setProperty("rawDefault",widget->property(settingProperty(widget)));
    auto* resetRaw=new QPushButton("Reset RAW rendering");resetRaw->setObjectName("resetRaw");rawLayout->addRow(resetRaw);
    connect(resetRaw,&QPushButton::clicked,this,[this]{for(auto* widget:raw_->findChildren<QWidget*>())if(widget->property("rawDefault").isValid())widget->setProperty(settingProperty(widget),widget->property("rawDefault"));updateMode();});
    queuePanel_=new QWidget;auto* queueLayout=new QVBoxLayout(queuePanel_);queueLayout->setContentsMargins(14,0,14,10);queueLayout->setSpacing(8);
    auto* destination=new QGroupBox("Output queue");destination->setObjectName("outputDestination");auto* destinationLayout=new QVBoxLayout(destination);output_=new QLineEdit;output_->setObjectName("outputDirectory");output_->setPlaceholderText("Output folder");destinationLayout->addWidget(output_);
    auto* folderButtons=new QHBoxLayout;
    auto* browse=new QPushButton("Choose folder…");browse->setObjectName("chooseOutputFolder");connect(browse,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getExistingDirectory(this,"Output folder",output_->text());if(!path.isEmpty())output_->setText(path);});folderButtons->addWidget(browse);
    auto* openOutput=new QPushButton("Open output folder");openOutput->setObjectName("openOutputFolder");folderButtons->addWidget(openOutput);destinationLayout->addLayout(folderButtons);
    connect(openOutput,&QPushButton::clicked,this,&MainWindow::openOutputFolder);
    formatFolders_=check("Create format subfolders","formatSubfolders");destinationLayout->addWidget(formatFolders_);
    formatFolders_->setToolTip("Group outputs by format: avif/, jxl/ and jpg/ inside the destination folder.");
    dateFolders_=check("Create date subfolder","dateSubfolders");destinationLayout->addWidget(dateFolders_);
    dateFolders_->setToolTip("Create today’s folder using the system’s short date format. Slashes and other filename separators become hyphens. Format subfolders go inside it.");
    jobFolders_=check("Create job subfolder","jobSubfolders");destinationLayout->addWidget(jobFolders_);jobFolders_->setToolTip("Save in YYYY-MM-DD-Job-N (or YYYY-MM-DD-Temporary). This replaces the separate date subfolder for this batch.");
    sourceFolder_=check("Save to original folder","saveToSourceFolder");destinationLayout->addWidget(sourceFolder_);
    sourceFolder_->setToolTip("Save beside each source image. Format subfolders, when enabled, are created there.");
    renameConflicts_=check("Rename if conflict","renameConflicts");destinationLayout->addWidget(renameConflicts_);
    renameConflicts_->setToolTip("Choose an unused numbered filename instead of replacing existing output files.");
    overwrite_=check("Replace existing output files","overwrite");destinationLayout->addWidget(overwrite_);
    connect(sourceFolder_,&QCheckBox::toggled,this,&MainWindow::updateOutputSettings);
    connect(renameConflicts_,&QCheckBox::toggled,this,&MainWindow::updateOutputSettings);
    const int cores=coreCount();workers_=spin(1,cores,recommendedWorkers(cores),"parallelJobs");auto* workerForm=new QFormLayout;workerForm->addRow("Parallel jobs",workers_);destinationLayout->addLayout(workerForm);
    threadBudget_=new QLabel;threadBudget_->setWordWrap(true);auto updateBudget=[this,cores]{threadBudget_->setText(QString("%1 CPU cores · up to %2 codec threads/job. Default jobs: CPU cores / 4 (minimum 1). Reduce jobs for large RAW files.").arg(cores).arg(std::max(1,cores/workers_->value())));};connect(workers_,&QSpinBox::valueChanged,this,updateBudget);updateBudget();destinationLayout->addWidget(threadBudget_);queueLayout->addWidget(destination);
    selected_=new QPushButton("Convert selected");selected_->setObjectName("convertSelected");selected_->setProperty("primary",true);visible_=new QPushButton("Convert all");visible_->setObjectName("convertVisible");cancel_=new QPushButton("Stop queue · finish active files");cancel_->setObjectName("cancelQueue");
    visible_->setToolTip("Convert all visible files");selected_->setToolTip("Convert selected files");
    // Start actions are placed outside the collapsible output settings.
    queueLayout->addWidget(cancel_);progress_=new QProgressBar;progress_->setObjectName("conversionProgress");progress_->setFormat("%p% (%v/%m)");progress_->setRange(0,1);progress_->setValue(0);queueLayout->addWidget(progress_);queueState_=new QLabel("Ready");queueState_->setWordWrap(true);queueLayout->addWidget(queueState_);layout->addStretch();
    connect(mode_,&QComboBox::currentIndexChanged,this,&MainWindow::updateMode);connect(targets_,&QListWidget::itemChanged,this,&MainWindow::updateMode);connect(preset_,&QComboBox::currentIndexChanged,this,&MainWindow::updateMode);connect(rawMode_,&QComboBox::currentIndexChanged,this,&MainWindow::updateMode);connect(wb_,&QComboBox::currentIndexChanged,this,&MainWindow::updateMode);
    connect(exposure_,&QDoubleSpinBox::valueChanged,this,&MainWindow::updateMode);connect(maxEdge_,&QSpinBox::valueChanged,this,&MainWindow::updateMode);
    connect(selected_,&QPushButton::clicked,this,[this]{convertFiles(true);});connect(visible_,&QPushButton::clicked,this,[this]{convertFiles(false);});connect(cancel_,&QPushButton::clicked,queue_,&ConversionQueue::cancelPending);
    return panel;
}
void MainWindow::updateMode() {
    const bool process=mode_->currentIndex()==1;
    const QSignalBlocker blocker(targets_);bool removed=false;
    for(int i=0;i<targets_->count();++i) {
        auto* item=targets_->item(i);
        const int target=item->data(Qt::UserRole).toInt();
        if(target!=3&&target!=5&&target!=7&&target!=8)continue;
        if(!process&&item->checkState()==Qt::Checked){item->setCheckState(Qt::Unchecked);removed=true;}
        item->setFlags(process?Qt::ItemIsEnabled|Qt::ItemIsUserCheckable:Qt::ItemFlags{});
    }
    if(removed&&selectedTargets().isEmpty())for(int i=0;i<targets_->count();++i)if(targets_->item(i)->data(Qt::UserRole).toInt()==4)targets_->item(i)->setCheckState(Qt::Checked);
    processing_->setEnabled(process);raw_->setEnabled(process);rawCustom_->setEnabled(rawMode_->currentIndex()==2);boostTarget_->setEnabled(rawMode_->currentIndex()!=0);kelvin_->setEnabled(wb_->currentIndex()==8);
    custom_->setEnabled(preset_->currentIndex()==4);transcode_->setEnabled(selectedTargets().contains(Target::jxl)&&(!process||(!exposure_->value()&&!maxEdge_->value())));
    updateOutputSettings();updateCounts();
}
void MainWindow::updateOutputSettings() {
    overwrite_->setEnabled(!renameConflicts_->isChecked());
    output_->setEnabled(!sourceFolder_->isChecked());
    findChild<QPushButton*>("chooseOutputFolder")->setEnabled(!sourceFolder_->isChecked());
}
QVector<Target> MainWindow::selectedTargets() const {
    QVector<Target> result;
    for(int i=0;i<targets_->count();++i)if(targets_->item(i)->checkState()==Qt::Checked&&(targets_->item(i)->flags()&Qt::ItemIsEnabled))result.append(Target(targets_->item(i)->data(Qt::UserRole).toInt()));
    return result;
}
Options MainWindow::options() const {
    const auto targets=selectedTargets();
    if(targets.isEmpty())throw std::invalid_argument("Select at least one output format");
    return optionsForTarget(targets.first());
}
Options MainWindow::optionsForTarget(Target target) const {
    Options o;o.mode=mode_->currentIndex()?Mode::process:Mode::preserve;o.target=target;o.quality.preset=Preset(preset_->currentIndex());
    o.quality.codec_quality=quality_->value();o.quality.jxl_distance=static_cast<float>(distance_->value());o.quality.avif_depth=avifDepth_->currentIndex()?12:10;o.quality.downsize_gainmap=downsize_->isChecked();
    o.keep_exif=exif_->isChecked();o.overwrite=overwrite_->isEnabled()&&overwrite_->isChecked();o.jpeg_transcoding=transcode_->isChecked()&&target==Target::jxl&&(o.mode==Mode::preserve||(!exposure_->value()&&!maxEdge_->value()));
    if(o.mode==Mode::process) {
        o.exposure=static_cast<float>(exposure_->value());o.max_edge=maxEdge_->value();o.extended_bt2020=bt2020_->isChecked();o.convert_gamut=convertGamut_->isChecked();o.raw.hdr_mode=RawHdrMode(rawMode_->currentIndex());o.raw.shoulder=static_cast<float>(shoulder_->value());
        if(o.raw.hdr_mode!=RawHdrMode::source)o.raw.target_nits=static_cast<float>(nits_->value());
        if(o.raw.hdr_mode==RawHdrMode::custom){o.raw.exposure=static_cast<float>(rawExposure_->value());o.raw.white_balance=wb_->currentIndex();o.raw.kelvin=kelvin_->value();o.raw.highlight_stops=static_cast<float>(highlight_->value());o.raw.shoulder=static_cast<float>(shoulder_->value());o.raw.target_nits=static_cast<float>(nits_->value());o.raw.shadows=static_cast<float>(shadows_->value());o.raw.midtones=static_cast<float>(midtones_->value());o.raw.highlights=static_cast<float>(highlights_->value());o.raw.camera_look=cameraLook_->isChecked();o.raw.linear_hdr=linearHdr_->isChecked();}
    }
    validate(o);return o;
}
bool MainWindow::collectionView() const {return statusFilter_->checkedId()==1;}
QSet<QString>& MainWindow::imageSelection() {return statusFilter_->checkedId()==2?jobSelections_[viewedJob_]:selectedImages_;}
const QSet<QString>& MainWindow::imageSelection() const {
    if(statusFilter_->checkedId()!=2)return selectedImages_;
    const auto it=jobSelections_.constFind(viewedJob_);static const QSet<QString> empty;
    return it==jobSelections_.cend()?empty:it.value();
}
void MainWindow::syncSelection() {
    if(selectionModelChanges_||!selection_)return;
    const bool previous=syncingSelection_;syncingSelection_=true;
    QItemSelection visible;
    if(!imageSelection().isEmpty())for(int row=0;row<proxy_->rowCount();++row){const auto index=proxy_->index(row,0);if(imageSelection().contains(index.data(Catalog::PathRole).toString()))visible.select(index,index.siblingAtColumn(Catalog::ColumnCount-1));}
    selection_->select(visible,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
    syncingSelection_=previous;
}
void MainWindow::changeSelection(const QItemSelection& selected,const QItemSelection& deselected) {
    if(syncingSelection_||selectionModelChanges_||collectionView())return;
    for(const auto& index:deselected.indexes())if(index.column()==0)imageSelection().remove(index.data(Catalog::PathRole).toString());
    for(const auto& index:selected.indexes())if(index.column()==0){const auto path=index.data(Catalog::PathRole).toString();imageSelection().insert(path);if(!bulkSelecting_)rememberedImages_.insert(path);}
    updateCounts();showMetadata();
}
void MainWindow::selectVisible() {
    for(int row=0;row<proxy_->rowCount();++row){const auto path=proxy_->index(row,0).data(Catalog::PathRole).toString();imageSelection().insert(path);}
    updateFilter();showMetadata();
}
void MainWindow::clearImageSelection() {imageSelection().clear();updateFilter();showMetadata();}
QString MainWindow::browserScrollKey() const {return QString::number(statusFilter_->checkedId())+":"+(statusFilter_->checkedId()==2?QString::number(viewedJob_):QString{})+":"+QString::number(viewMode_->checkedId());}
void MainWindow::saveBrowserScroll() {
    if(scrollView_&&!scrollKey_.isEmpty())scrollPositions_[scrollKey_]=QPoint(scrollView_->horizontalScrollBar()->value(),scrollView_->verticalScrollBar()->value());
}
void MainWindow::restoreBrowserScroll() {
    const auto key=scrollKey_;const auto position=scrollPositions_.value(key);auto* view=scrollView_;
    QTimer::singleShot(0,this,[this,key,position,view]{if(key!=scrollKey_||view!=scrollView_)return;view->doItemsLayout();view->horizontalScrollBar()->setValue(position.x());view->verticalScrollBar()->setValue(position.y());});
}
void MainWindow::loadJobs() {
    QFile file(configDirectory()+"/jobs.json");if(!file.open(QIODevice::ReadOnly)||file.size()>16*1024*1024)return;
    const auto data=QJsonDocument::fromJson(file.readAll()).object();
    auto files=[](const QJsonValue& value){QStringList paths;for(const auto& entry:value.toArray())if(entry.isString()&&!entry.toString().isEmpty())paths.append(entry.toString());paths.removeDuplicates();return paths;};
    auto outputs=[](const QJsonValue& value){QHash<QString,OutputProgress> result;const auto object=value.toObject();for(auto it=object.begin();it!=object.end();++it){const auto entry=it.value().toObject();auto state=entry.value("state").toString();if(state=="Queued"||state=="Running")state="Interrupted";result.insert(it.key(),{entry.value("input").toString(),state});}return result;};
    temporaryJob_.files=files(data.value("temporary"));temporaryJob_.outputs=outputs(data.value("temporaryOutputs"));temporaryJob_.folder=data.value("temporaryFolder").toString();
    QSet<qint64> ids;
    for(const auto& value:data.value("jobs").toArray()){
        const auto object=value.toObject();const auto number=object.value("number").toVariant().toLongLong();
        if(number<1||ids.contains(number)||number>1000000000)continue;
        savedJobs_.append({number,files(object.value("files")),outputs(object.value("outputs")),object.value("folder").toString()});ids.insert(number);nextJob_=std::max(nextJob_,number+1);
    }
    const auto selected=data.value("viewedJob").toVariant().toLongLong();if(ids.contains(selected))viewedJob_=selected;
}
void MainWindow::saveJobs() {
    auto settings=appSettings();settings.setValue("jobs/nextNumber",nextJob_);settings.sync();
    if(jobSaveTimer_)jobSaveTimer_->stop();
    // Merely starting with history restore off must not erase saved lists.
    if(!jobHistoryLoaded_&&!jobHistoryDirty_)return;
    auto outputs=[](const SavedJob& job){QJsonObject result;for(auto it=job.outputs.cbegin();it!=job.outputs.cend();++it){QJsonObject value;value.insert("input",it->input);value.insert("state",it->state);result.insert(it.key(),value);}return result;};
    QJsonArray jobs;for(const auto& job:savedJobs_){QJsonObject object;object.insert("number",QString::number(job.number));object.insert("files",QJsonArray::fromStringList(job.files));object.insert("outputs",outputs(job));object.insert("folder",job.folder);jobs.append(object);}
    QJsonObject object;object.insert("version",2);object.insert("temporary",QJsonArray::fromStringList(temporaryJob_.files));object.insert("temporaryOutputs",outputs(temporaryJob_));object.insert("temporaryFolder",temporaryJob_.folder);object.insert("jobs",jobs);object.insert("viewedJob",QString::number(viewedJob_));
    QSaveFile file(configDirectory()+"/jobs.json");const auto bytes=QJsonDocument(object).toJson();
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())logMessage("Warning: cannot save job file lists");
}
QString MainWindow::jobText(qint64 number) const {
    const SavedJob* job=&temporaryJob_;for(const auto& saved:savedJobs_)if(saved.number==number)job=&saved;
    struct Counts {int total=0,terminal=0,finished=0,skipped=0,failed=0;};QHash<QString,Counts> counts;
    for(const auto& output:job->outputs){auto& c=counts[output.input];++c.total;if(output.state=="Finished"||output.state=="Skipped"||output.state=="Failed"||output.state=="Cancelled"||output.state=="Interrupted")++c.terminal;if(output.state=="Finished")++c.finished;else if(output.state=="Skipped")++c.skipped;else if(output.state=="Failed")++c.failed;}
    int finished=0,skipped=0,failed=0;
    for(const auto& input:job->files){const auto c=counts.value(input);if(c.total&&c.terminal==c.total){if(c.failed)++failed;else if(c.skipped==c.total)++skipped;else if(c.finished+c.skipped==c.total)++finished;}}
    return (number?QString("Job #%1").arg(number):QString("Temporary"))+QString(" (%1/%2/%3/%4)").arg(finished).arg(job->files.size()).arg(skipped).arg(failed)+(queue_->hasRunningBatch(number)?" *":isDraftJob(number)?" (?)":"");
}
void MainWindow::refreshJobs() {
    QVector<qint64> ids{0};for(auto it=savedJobs_.crbegin();it!=savedJobs_.crend();++it)ids.append(it->number);
    for(int i=0;i<ids.size();++i){const auto number=ids[i];QAction* action=nullptr;for(auto* candidate:jobMenu_->actions())if(candidate->data().toLongLong()==number){action=candidate;break;}
        if(!action){action=new QAction(jobMenu_);action->setData(number);action->setCheckable(true);jobActions_->addAction(action);jobMenu_->insertAction(jobMenu_->actions().value(i),action);connect(action,&QAction::triggered,this,[this,number]{showJob(number);});}
        action->setText(jobText(number));action->setChecked(number==viewedJob_);
    }
    jobButton_->setText(statusFilter_->checkedId()==2?jobText(viewedJob_):QString("Jobs"));
}
void MainWindow::showJob(qint64 number) {
    viewedJob_=number;statusFilter_->button(2)->setChecked(true);refreshJobs();saveJobs();updateFilter();
    const QStringList* files=&temporaryJob_.files;for(const auto& job:savedJobs_)if(job.number==number)files=&job.files;
    const auto current=catalog_->paths();const QSet<QString> present(current.cbegin(),current.cend());QStringList missing;
    for(const auto& path:*files)if(!present.contains(path))missing.append(path);
    if(!missing.isEmpty())catalog_->addPaths(missing,false);
}
void MainWindow::clearJobs() {
    if(queue_->busy())return;
    savedJobs_.clear();temporaryJob_={0,{},{},{}};jobSelections_.clear();viewedJob_=0;nextJob_=1;
    jobHistoryLoaded_=true;jobHistoryDirty_=true;jobMenu_->clear();
    if(statusFilter_->checkedId()==2)statusFilter_->button(0)->setChecked(true);
    refreshJobs();saveJobs();updateFilter();
    for(auto it=scrollPositions_.begin();it!=scrollPositions_.end();)if(it.key().startsWith("2:"))it=scrollPositions_.erase(it);else ++it;
}
bool MainWindow::isDraftJob(qint64 number) const {
    const SavedJob* saved=&temporaryJob_;for(const auto& job:savedJobs_)if(job.number==number)saved=&job;
    return !saved->files.isEmpty()&&saved->outputs.isEmpty();
}
void MainWindow::createDraftJob(const QStringList& files) {
    if(files.isEmpty())return;
    QStringList unique=files;unique.removeDuplicates();
    const auto number=nextJob_++;savedJobs_.append({number,unique,{},{}});jobHistoryDirty_=true;showJob(number);
}
void MainWindow::recordJob(const QVector<Job>& jobs,bool temporary,bool reuse) {
    jobHistoryDirty_=true;
    SavedJob* saved=&temporaryJob_;
    if(reuse){for(auto& job:savedJobs_)if(job.number==viewedJob_)saved=&job;}
    else if(!temporary){savedJobs_.append({nextJob_++,{},{},{}});saved=&savedJobs_.last();}
    for(const auto& job:jobs){if(!saved->files.contains(job.input))saved->files.append(job.input);saved->outputs.insert(job.output,{job.input,"Queued"});}
    if(!jobs.isEmpty())saved->folder=QFileInfo(jobs.first().output).absolutePath();
    showJob(saved->number);
}
void MainWindow::updateJobOutput(qint64 batch,const QString& input,const QString& output,const QString& state) {
    auto update=[&](SavedJob& job){auto it=job.outputs.find(output);if(it!=job.outputs.end()&&it->input==input&&(it->state=="Queued"||it->state=="Running"))it->state=state;};
    for(auto& job:savedJobs_)if(job.number==batch)update(job);
    // A single-image request may promote an already queued output belonging
    // to a numbered job; the temporary job observes that output's result too.
    update(temporaryJob_);refreshJobs();jobSaveTimer_->start();
}
void MainWindow::clearCurrentTab() {
    const int tab=statusFilter_->checkedId();
    if(tab==2)return;
    if(tab==1){clearImageSelection();return;}
    if(tab==5){rememberedImages_.clear();updateFilter();return;}
    QStringList paths;for(int row=0;row<proxy_->rowCount();++row)paths.append(proxy_->index(row,0).data(Catalog::PathRole).toString());
    if(tab==0&&catalog_->scanning())catalog_->clear();else catalog_->removePaths(paths);
    updateFilter();
}
void MainWindow::openOutputFolder() {
    QString path;
    if(statusFilter_->checkedId()==2){if(viewedJob_==0)path=temporaryJob_.folder;else for(const auto& job:savedJobs_)if(job.number==viewedJob_)path=job.folder;}
    if(path.isEmpty()){
        path=output_->text().trimmed();
        if(sourceFolder_->isChecked()){auto paths=selectedPaths();if(paths.isEmpty()&&proxy_->rowCount())paths.append(proxy_->index(0,0).data(Catalog::PathRole).toString());if(paths.isEmpty()){QMessageBox::warning(this,"Cannot open output folder","Select an image to open its source folder.");return;}path=QFileInfo(paths.first()).absolutePath();}
        if(jobFolders_->isChecked())path=QDir(path).filePath(QDate::currentDate().toString(Qt::ISODate)+(viewedJob_?"-Job-"+QString::number(viewedJob_):"-Temporary"));
        else if(dateFolders_->isChecked()&&!path.isEmpty())path=QDir(path).filePath(outputDateFolder());
    }
    if(path.isEmpty()||!QDir().mkpath(path)){QMessageBox::warning(this,"Cannot open output folder","Choose a writable output folder.");return;}
    if(!QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(path).absolutePath())))QMessageBox::warning(this,"Cannot open output folder","The system file manager could not open this folder.");
}
void MainWindow::applySearch() {appliedSearch_=search_->text();updateFilter();}
void MainWindow::resetFilters() {
    search_->clear();appliedSearch_.clear();instant_->clear();format_->clear();range_->setCurrentIndex(0);
    minImageEdge_->setValue(0);megapixels_->setValue(0);dateEnabled_->setChecked(false);today_->setChecked(false);
    dateFrom_->setDate(QDate::currentDate().addDays(-1));dateTo_->setDate(QDate::currentDate());
    statusFilter_->button(0)->setChecked(true);updateFilter();
}
void MainWindow::loadHistory() {
    QFile file(configDirectory()+"/history.json");
    if(!file.open(QIODevice::ReadOnly))return;
    const auto object=QJsonDocument::fromJson(file.readAll()).object();
    auto strings=[](const QJsonValue& value){QStringList result;for(const auto& entry:value.toArray())if(entry.isString()&&!entry.toString().isEmpty())result.append(entry.toString());return result;};
    recentFolders_=strings(object.value("folders"));recentFiles_=strings(object.value("files"));lastFiles_=strings(object.value("lastFiles"));
}
void MainWindow::saveHistory() {
    const auto directory=configDirectory();
    QDir().mkpath(directory);QSaveFile file(directory+"/history.json");
    QJsonObject object;object.insert("version",1);object.insert("folders",QJsonArray::fromStringList(recentFolders_));object.insert("files",QJsonArray::fromStringList(recentFiles_));object.insert("lastFiles",QJsonArray::fromStringList(lastFiles_));
    if(!file.open(QIODevice::WriteOnly)){statusBar()->showMessage("Cannot save recent history",5000);return;}
    const auto data=QJsonDocument(object).toJson();if(file.write(data)!=data.size()||!file.commit())statusBar()->showMessage("Cannot save recent history",5000);
}
void MainWindow::addDroppedPaths(const QStringList& paths) {
    QStringList files;
    for(const auto& path:paths){const QFileInfo info(path);if(info.isFile()&&supportedFile(path)){const auto canonical=info.canonicalFilePath();if(!files.contains(canonical))files.append(canonical);}}
    if(!files.isEmpty()) {
        lastFiles_=files;
        for(auto it=files.crbegin();it!=files.crend();++it){recentFiles_.removeAll(*it);recentFiles_.prepend(*it);}
        while(recentFiles_.size()>1000)recentFiles_.removeLast();
        saveHistory();
    }
    addPaths(paths);
}
void MainWindow::addPaths(const QStringList& paths) {
    bool changed=false;
    for(const auto& path:paths){const QFileInfo info(path);if(info.isDir()){const auto canonical=info.canonicalFilePath();recentFolders_.removeAll(canonical);recentFolders_.prepend(canonical);changed=true;}}
    if(changed){while(recentFolders_.size()>30)recentFolders_.removeLast();saveHistory();}
    if(!paths.isEmpty()){statusFilter_->button(0)->setChecked(true);updateFilter();catalog_->addPaths(paths,recursive_->isChecked());}
}

void MainWindow::updateFilter() {
    jobButton_->setText(statusFilter_->checkedId()==2?jobText(viewedJob_):QString("Jobs"));
    const auto key=browserScrollKey();const bool changed=key!=scrollKey_;if(changed)saveBrowserScroll();
    Filter f;const int tab=statusFilter_->checkedId();f.status=(tab==3||tab==4)?tab:0;
    f.restrictPaths=tab==1||tab==2||tab==5;
    if(tab==1)f.paths=selectedImages_;
    else if(tab==5)f.paths=rememberedImages_;
    else if(tab==2){const QStringList* files=&temporaryJob_.files;for(const auto& job:savedJobs_)if(job.number==viewedJob_)files=&job.files;f.paths=QSet<QString>(files->cbegin(),files->cend());}
    f.search=appliedSearch_;f.text=instant_->text();f.literalText=!metadataFilters_->isChecked();f.today=today_->isChecked();
    if(metadataFilters_->isChecked()) {
        f.format=format_->text().trimmed();if(range_->currentIndex())f.range=range_->currentText();
        f.minMaxEdge=minImageEdge_->value();f.minMegapixels=megapixels_->value();
        if(dateEnabled_->isChecked()&&!f.today){f.from=dateFrom_->date();f.to=dateTo_->date();}
    }
    const bool dates=metadataFilters_->isChecked()&&dateEnabled_->isChecked()&&!f.today;
    dateEnabled_->setEnabled(!f.today);dateFrom_->setEnabled(dates);dateTo_->setEnabled(dates);
    const bool wasSyncing=syncingSelection_;syncingSelection_=true;proxy_->setFilter(f);
    list_->setSelectionMode(collectionView()?QAbstractItemView::NoSelection:QAbstractItemView::ExtendedSelection);
    table_->setSelectionMode(collectionView()?QAbstractItemView::NoSelection:QAbstractItemView::ExtendedSelection);
    syncSelection();syncingSelection_=wasSyncing;updateBackgroundMetadata();updateCounts();
    if(changed){scrollKey_=key;scrollView_=qobject_cast<QAbstractItemView*>(views_->currentWidget());restoreBrowserScroll();}
}
void MainWindow::updateBackgroundMetadata() {
    const int sort=sort_->currentData().toInt();
    const bool metadataSort=sort!=Catalog::Name&&sort!=Catalog::Format&&sort!=Catalog::Status;
    const bool metadataFilter=metadataFilters_->isChecked()&&(!format_->text().trimmed().isEmpty()||range_->currentIndex()!=0||minImageEdge_->value()>0||megapixels_->value()>0||dateEnabled_->isChecked());
    catalog_->setBackgroundMetadata(metadataSort||metadataFilter||today_->isChecked()||!appliedSearch_.trimmed().isEmpty()||!instant_->text().trimmed().isEmpty());
}
void MainWindow::updatePresentation() {
    saveBrowserScroll();
    const bool details=viewMode_->checkedId()==2,grid=viewMode_->checkedId()==0;
    views_->setCurrentWidget(details?static_cast<QWidget*>(table_):list_);list_->setPresentation(grid,size_->value(),aspectRatios[aspectIndex_]);
    list_->setSmoothScrolling(smoothScrolling_->isChecked());table_->setSmoothScrolling(smoothScrolling_->isChecked());
    thumbnailAspect_->setText(aspectNames[aspectIndex_].section(' ',0,0));
    for(auto* action:thumbnailAspect_->menu()->actions())action->setChecked(action->data().toInt()==aspectIndex_);
    const bool fill=thumbnailFit_->isChecked();thumbnailFit_->setText(fill?"Fill":"Fit");delegate_->fill=fill;table_->setFillThumbnails(fill);
    table_->setIconSize(QSize(size_->value()/2,size_->value()/2));table_->verticalHeader()->setDefaultSectionSize(std::max(32,size_->value()/2+8));
    updateFilter();
}
void MainWindow::requestAspectSample(bool viewport) {
    if(!autoAspect_->isChecked()||changingAspect_)return;
    aspectSamplePending_=true;aspectViewportSample_=viewport;aspectTimer_->start();
}
void MainWindow::setThumbnailAspect(int index) {
    index=std::clamp(index,0,4);if(index==aspectIndex_)return;
    const auto visible=list_->firstVisibleRow(1);const QPersistentModelIndex anchor=visible.isEmpty()?QModelIndex{}:visible.first();
    const double offset=anchor.isValid()?double(list_->visualRect(anchor).top())/std::max(1,list_->gridSize().height()):0;
    changingAspect_=true;aspectIndex_=index;updatePresentation();
    if(anchor.isValid()&&viewMode_->checkedId()==0){list_->scrollTo(anchor,QAbstractItemView::PositionAtTop);list_->verticalScrollBar()->setValue(list_->verticalScrollBar()->value()-qRound(offset*list_->gridSize().height()));}
    changingAspect_=false;
}
void MainWindow::updateAutoThumbnailAspect() {
    if(!autoAspect_->isChecked()||!aspectSamplePending_)return;
    QVector<int> rows;
    if(aspectViewportSample_){if(viewMode_->checkedId()!=0)return;for(const auto& index:list_->firstVisibleRow())rows.append(proxy_->mapToSource(index).row());}
    else for(int row=0;row<std::min(2,catalog_->rowCount());++row)rows.append(row);
    if(rows.isEmpty())return;
    int choice=-1;
    for(const auto row:rows){const auto& record=catalog_->record(row);if(!record.inspected)return;
        if(record.width<=0||record.height<=0){aspectSamplePending_=false;return;}
        const double ratio=double(record.width)/record.height;
        // Near-square images use square cells. Landscape and portrait ranges
        // are mirrored, with 1.6 separating 4:3 and 16:9 landscape cells.
        int match=ratio>=1.6?0:ratio>1.1?1:ratio<1.0/1.6?3:ratio<1.0/1.1?4:2;
        if(!portraitAspect_->isChecked()&&match>=3)match=2;
        if(choice<0)choice=match;else if(choice!=match){aspectSamplePending_=false;return;}
    }
    aspectSamplePending_=false;setThumbnailAspect(choice);
}
QStringList MainWindow::selectedPaths() const {QStringList out;for(const auto& path:catalog_->paths())if(imageSelection().contains(path))out.append(path);return out;}
void MainWindow::updateCounts() {
    const int selected=imageSelection().size();const bool busy=queue_->busy()||(updater_&&updater_->installing()),hasTargets=!selectedTargets().isEmpty();
    if(auto* button=findChild<QPushButton*>("updateApplication"))button->setEnabled(!busy&&!updater_->busy());
    counts_->setText(QString("%1 visible / %2 files · %3 selected%4").arg(proxy_->rowCount()).arg(catalog_->rowCount()).arg(selected).arg(catalog_->scanning()?" · scanning…":""));
    auto stateStyle=[](QPushButton* button,const char* name,bool value){if(button->property(name).isValid()&&button->property(name).toBool()==value)return;button->setProperty(name,value);button->style()->unpolish(button);button->style()->polish(button);button->update();};
    stateStyle(selected_,"noSelection",selected==0);stateStyle(cancel_,"running",queue_->busy());
    progressStop_->setVisible(queue_->busy());
    selected_->setEnabled(selected>0&&hasTargets&&!(updater_&&updater_->installing())&&(!queue_->busy()||statusFilter_->checkedId()==2));visible_->setEnabled(proxy_->rowCount()>0&&hasTargets&&!(updater_&&updater_->installing())&&(!queue_->busy()||(statusFilter_->checkedId()==2&&isDraftJob(viewedJob_))));cancel_->setEnabled(queue_->busy());remove_->setEnabled(selected>0&&!busy);cancelJob_->setVisible(statusFilter_->checkedId()==2);cancelJob_->setEnabled(queue_->hasPendingBatch(viewedJob_));
    clear_->setToolTip("Clear the current tab");
    clear_->setEnabled(statusFilter_->checkedId()==2?false:statusFilter_->checkedId()==1?!selectedImages_.isEmpty():statusFilter_->checkedId()==5?!rememberedImages_.isEmpty():!busy&&(proxy_->rowCount()>0||catalog_->scanning()));
    running_->setEnabled(queue_->busy());clearJobs_->setEnabled(!busy);workers_->setEnabled(!busy);
}
void MainWindow::showMetadata() {
    QModelIndex index=selection_->currentIndex();const auto selected=selection_->selectedRows(0);
    if(!index.isValid()&&!selected.isEmpty())index=selected.first();
    if(!index.isValid()){inspector_->clear();return;}
    const auto& r=catalog_->record(proxy_->mapToSource(index).row());
    QString text=r.path+"\n\n"+QString("%1 × %2 · %3 · %4 bit\n%5 · %6\nStatus: %7\n").arg(r.width).arg(r.height).arg(r.format).arg(r.bits).arg(r.transfer,r.primaries,r.status);
    text+="\nCreated: "+r.created.toString("yyyy-MM-dd HH:mm:ss")+"\nModified: "+r.modified.toString("yyyy-MM-dd HH:mm:ss")+"\nEXIF date: "+r.captured.toString("yyyy-MM-dd HH:mm:ss")+"\n";
    if(!r.error.isEmpty())text+="\n"+r.error+"\n";
    if(!r.metadataWarning.isEmpty())text+="\nMetadata note: "+r.metadataWarning+"\n";
    text+="\nEXIF\n";for(auto it=r.exif.cbegin();it!=r.exif.cend();++it)text+=it.key()+" = "+it.value()+"\n";inspector_->setPlainText(text);
}
void MainWindow::convertFiles(bool selected) {
    QStringList paths;if(selected)paths=selectedPaths();else for(int row=0;row<proxy_->rowCount();++row)paths<<proxy_->index(row,0).data(Catalog::PathRole).toString();
    convertPaths(paths,selectedTargets(),false,false,false,statusFilter_->checkedId()==2&&(selected||isDraftJob(viewedJob_)));
}
void MainWindow::convertPaths(const QStringList& paths,const QVector<Target>& targets,bool exactTargets,bool priority,bool append,bool reuseJob) {
    if(paths.isEmpty()||(!priority&&!append&&!reuseJob&&queue_->busy())||(updater_&&updater_->installing()))return;
    try {
        if(targets.isEmpty())throw std::invalid_argument("Select at least one output format");
        const QString directory=output_->text().trimmed();if(directory.isEmpty()&&!sourceFolder_->isChecked())throw std::invalid_argument("Choose an output folder");
        QMap<int,QStringList> inputsByTarget;
        for(const auto& path:paths) {
            const auto extension=QFileInfo(path).suffix().toLower();
            const bool hdrGroup=extension!="jpg"&&extension!="jpeg"&&extension!="jpe"&&extension!="avif"&&extension!="png"&&extension!="webp"&&extension!="tif"&&extension!="tiff"&&extension!="bmp"&&extension!="gif";
            for(auto target:targets) {
                const bool hdrTarget=target==Target::jpeg_gainmap||target==Target::jxl_linear||target==Target::jxl_scrgb||target==Target::jxl_pq||target==Target::avif_pq||target==Target::avif_gainmap;
                if(hdrTarget&&!hdrGroup&&!exactTargets) {
                    if(!acceptSdr_->isChecked())continue;
                    if(target==Target::jpeg_gainmap)target=Target::jpeg;
                    else if(target==Target::jxl_linear||target==Target::jxl_pq)target=Target::jxl;
                    else if(target!=Target::jxl_scrgb)target=Target::avif;
                }
                auto& inputs=inputsByTarget[int(target)];if(!inputs.contains(path))inputs.append(path);
            }
        }
        const qint64 batch=reuseJob?viewedJob_:priority?0:nextJob_;
        OutputLayout layout;if(jobFolders_->isChecked())layout.job_subfolder=QDate::currentDate().toString(Qt::ISODate)+(batch==0?"-Temporary":"-Job-"+QString::number(batch));layout.format_subfolders=formatFolders_->isChecked();layout.date_subfolders=dateFolders_->isChecked();layout.source_folder=sourceFolder_->isChecked();layout.rename_conflicts=renameConflicts_->isChecked();
        for(int row=0;row<catalog_->rowCount();++row){const auto& record=catalog_->record(row);if(record.raw)layout.raw_inputs.insert(record.path);}
        QVector<Job> jobs;for(auto it=inputsByTarget.cbegin();it!=inputsByTarget.cend();++it)jobs+=makeJobs(it.value(),directory,optionsForTarget(Target(it.key())),layout);
        if(jobs.isEmpty())throw std::invalid_argument("No inputs match the selected targets. Enable Accept SDR formats to include SDR sources with SDR output equivalents.");
        QSet<QString> inputs;for(const auto& path:paths)inputs.insert(QFileInfo(path).absoluteFilePath().toCaseFolded());
        for(const auto& job:jobs) {
            if(inputs.contains(QFileInfo(job.output).absoluteFilePath().toCaseFolded()))throw std::invalid_argument("Output would replace an input file");
            if(!QDir().mkpath(QFileInfo(job.output).absolutePath()))throw std::runtime_error("Cannot create output folder");
        }
        for(auto& job:jobs)job.batch=batch;
        recordJob(jobs,priority,reuseJob);
        if(priority||reuseJob)queue_->enqueueNext(std::move(jobs),workers_->value());
        else if(append)queue_->enqueue(std::move(jobs),workers_->value());
        else queue_->start(std::move(jobs),workers_->value());
        updateCounts();
    } catch(const std::exception& e){logMessage("Cannot start conversion: "+QString::fromUtf8(e.what()));QMessageBox::warning(this,"Cannot start conversion",QString::fromUtf8(e.what()));}
}
void MainWindow::logMessage(const QString& message) {outputLog_->appendPlainText("["+QTime::currentTime().toString("HH:mm:ss")+"] "+message);}
void MainWindow::checkForUpdates() {
    if(!checkUpdates_->isChecked()){updateTimer_->stop();return;}
    // Test windows retain the scheduler but never contact the real release API.
    if(!QStandardPaths::isTestModeEnabled())updater_->check();
    const auto now=QTime::currentTime();updateTimer_->start(3600000-now.minute()*60000-now.second()*1000-now.msec());
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if(queue_->busy()){closeWhenIdle_=true;queue_->cancelPending();queueState_->setText("Closing after active conversions finish…");event->ignore();return;}event->accept();
}
bool MainWindow::eventFilter(QObject* watched,QEvent* event) {


    if(targets_&&watched==targets_->viewport()&&event->type()==QEvent::MouseButtonRelease) {
        auto* mouse=static_cast<QMouseEvent*>(event);
        if(mouse->button()==Qt::LeftButton) {
            auto* item=targets_->itemAt(mouse->position().toPoint());
            if(item&&(item->flags()&Qt::ItemIsEnabled))item->setCheckState(item->checkState()==Qt::Checked?Qt::Unchecked:Qt::Checked);
            return true;
        }
    }
    if(event->type()==QEvent::Wheel&&!wheelChanges_->isChecked()&&
       (qobject_cast<QComboBox*>(watched)||qobject_cast<QAbstractSpinBox*>(watched))) {
        event->ignore();return true;
    }
    return QMainWindow::eventFilter(watched,event);
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event){if(event->mimeData()->hasUrls())event->acceptProposedAction();}
void MainWindow::dropEvent(QDropEvent* event){if(event->mimeData()->hasUrls()){addDroppedPaths(urlPaths(event->mimeData()));event->acceptProposedAction();}}
}
