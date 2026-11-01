#include "window.hpp"
#include "views.hpp"
#include "style.hpp"
#include "updater.hpp"
#include "settings.hpp"
#include <QStyleHints>
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
    auto* files=toolbar->addAction("Add files…");connect(files,&QAction::triggered,this,[this]{addPaths(QFileDialog::getOpenFileNames(this,"Add images",{},"Images (*.png *.webp *.tif *.tiff *.bmp *.gif *.jpg *.jpeg *.avif *.jxl *.jxr *.wdp *.hdp *.dng *.cr2 *.cr3 *.nef *.arw *.raf *.rw2 *.orf *.pef *.raw);;All files (*)"));});
    auto* folder=toolbar->addAction("Open folder…");connect(folder,&QAction::triggered,this,[this]{const auto path=QFileDialog::getExistingDirectory(this,"Open image folder");if(!path.isEmpty())addPaths({path});});
    loadHistory();
    auto* recent=new QToolButton;recent->setText("Recent folders");recent->setPopupMode(QToolButton::InstantPopup);auto* recentMenu=new QMenu(recent);recent->setMenu(recentMenu);toolbar->addWidget(recent);
    connect(recentMenu,&QMenu::aboutToShow,this,[this,recentMenu]{recentMenu->clear();for(const auto& path:recentFolders_){auto* action=recentMenu->addAction(path);connect(action,&QAction::triggered,this,[this,path]{addPaths({path});});}
        auto* filesMenu=recentMenu->addMenu("Recent dropped files");for(const auto& path:recentFiles_){auto* action=filesMenu->addAction(path);connect(action,&QAction::triggered,this,[this,path]{addPaths({path});});}
    });
    loadFolder_=check("Load last folder","loadLastFolder");loadFiles_=check("Load last files","loadLastFiles");toolbar->addWidget(loadFolder_);toolbar->addWidget(loadFiles_);
    auto* clearHistory=toolbar->addAction("Clear history");connect(clearHistory,&QAction::triggered,this,[this]{recentFolders_.clear();recentFiles_.clear();lastFiles_.clear();saveHistory();});
    recursive_=check("Include subfolders","recursive",true);toolbar->addWidget(recursive_);toolbar->addSeparator();
    auto* selectAll=toolbar->addAction("Select all");connect(selectAll,&QAction::triggered,this,[this]{if(views_->currentWidget()==table_)table_->selectAll();else list_->selectAll();});
    remove_=new QPushButton("Remove selected");remove_->setObjectName("removeSelected");toolbar->addWidget(remove_);
    connect(remove_,&QPushButton::clicked,this,[this]{catalog_->removePaths(selectedPaths());});
    clear_=new QPushButton("Clear");toolbar->addWidget(clear_);connect(clear_,&QPushButton::clicked,this,[this]{catalog_->removePaths(catalog_->paths());});
    toolbar->addSeparator();wheelChanges_=check("Wheel changes controls","wheelChanges");
    wheelChanges_->setToolTip("Allow the mouse wheel to change dropdowns and numeric inputs. Off lets you scroll past controls without changing conversion settings.");toolbar->addWidget(wheelChanges_);
    auto* splitter=new QSplitter;setCentralWidget(splitter);
    auto* browser=new QWidget;auto* layout=new QVBoxLayout(browser);layout->setContentsMargins(16,16,16,10);layout->setSpacing(10);
    auto* searchRow=new QHBoxLayout;search_=new QLineEdit;search_->setObjectName("search");search_->setPlaceholderText("Search files and metadata · press Enter");search_->setClearButtonEnabled(true);auto* searchButton=new QPushButton("Search");searchRow->addWidget(search_,1);searchRow->addWidget(searchButton);layout->addLayout(searchRow);
    connect(search_,&QLineEdit::returnPressed,this,&MainWindow::applySearch);connect(searchButton,&QPushButton::clicked,this,&MainWindow::applySearch);
    connect(search_,&QLineEdit::textChanged,this,[this](const QString& text){if(text.isEmpty())applySearch();});
    auto* filters=new QHBoxLayout;instant_=new QLineEdit;instant_->setObjectName("instantFilter");instant_->setPlaceholderText("Instant filter within search results…");instant_->setClearButtonEnabled(true);filters->addWidget(instant_,1);
    metadataFilters_=check("Filters","metadataFilters");filters->addWidget(metadataFilters_);
    today_=check("Today","todayFilter");today_->setToolTip("Captured today; uses file modification date when no capture date is available.");filters->addWidget(today_);
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
    thumbnailAspect_=new QPushButton;thumbnailAspect_->setObjectName("thumbnailAspect");thumbnailAspect_->setCheckable(true);
    thumbnailAspect_->setToolTip("Toggle thumbnail cells between square and 16:9. Images retain their original proportions.");viewBar->addWidget(thumbnailAspect_);
    thumbnailFit_=new QPushButton("Fit");thumbnailFit_->setObjectName("thumbnailFit");thumbnailFit_->setCheckable(true);
    thumbnailFit_->setToolTip("Fit shows the whole image. Fill crops the image to fill its thumbnail cell. Click to switch.");viewBar->addWidget(thumbnailFit_);
    viewBar->addWidget(new QLabel("Minimum size"));
    size_=new QSlider(Qt::Horizontal);size_->setObjectName("thumbnailSize");size_->setRange(100,500);size_->setSingleStep(10);size_->setPageStep(10);size_->setValue(200);size_->setFixedWidth(140);
    size_->setToolTip("Minimum grid cell width in dp (100–500). Columns grow to fill the available width.");viewBar->addWidget(size_);
    auto* sizeValue=new QLabel("200 dp");sizeValue->setMinimumWidth(48);viewBar->addWidget(sizeValue);
    connect(size_,&QSlider::valueChanged,this,[this,sizeValue](int value){const int snapped=((value+5)/10)*10;if(value!=snapped){size_->setValue(snapped);return;}sizeValue->setText(QString::number(value)+" dp");});layout->addLayout(viewBar);
    views_=new QStackedWidget;list_=new FileListView;table_=new FileTableView;delegate_=new ThumbnailDelegate(list_);list_->setItemDelegate(delegate_);
    list_->setModel(proxy_);table_->setModel(proxy_);list_->setSelectionModel(selection_);table_->setSelectionModel(selection_);
    table_->verticalHeader()->hide();table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);table_->setColumnWidth(Catalog::Name,260);table_->setColumnWidth(Catalog::Camera,190);table_->sortByColumn(Catalog::Name,Qt::AscendingOrder);
    views_->addWidget(list_);views_->addWidget(table_);layout->addWidget(views_,1);
    counts_=new QLabel("Drop files or folders here, or choose Open folder.");counts_->setObjectName("catalogCounts");layout->addWidget(counts_);
    auto* sidebar=new QTabWidget;sidebar->setObjectName("settingsTabs");sidebar->setMinimumWidth(330);sidebar->setMaximumWidth(460);
    auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(buildControls());scroll->setMinimumHeight(150);
    sidebar->addTab(scroll,"Conversion");
    inspector_=new QPlainTextEdit;inspector_->setObjectName("metadataInspector");inspector_->setReadOnly(true);sidebar->addTab(inspector_,"Metadata");
    auto* sidePanel=new QWidget;sidePanel->setMinimumWidth(330);sidePanel->setMaximumWidth(460);auto* sideLayout=new QVBoxLayout(sidePanel);sideLayout->setContentsMargins(0,0,0,0);sideLayout->addWidget(sidebar,1);
    auto* outputToggle=new QPushButton("▾ Output queue");outputToggle->setObjectName("toggleOutput");outputToggle->setCheckable(true);outputToggle->setChecked(true);sideLayout->addWidget(outputToggle);
    auto* outputScroll=new QScrollArea;outputScroll->setWidgetResizable(true);outputScroll->setFrameShape(QFrame::NoFrame);outputScroll->setWidget(queuePanel_);outputScroll->setFixedHeight(330);sideLayout->addWidget(outputScroll);
    auto* progressFrame=new QWidget;auto* progressLayout=new QVBoxLayout(progressFrame);progressLayout->setContentsMargins(12,6,12,12);progressLayout->addWidget(progress_);sideLayout->addWidget(progressFrame);
    connect(outputToggle,&QPushButton::toggled,this,[outputScroll,outputToggle](bool on){outputScroll->setVisible(on);outputToggle->setText(on?"▾ Output queue":"▸ Output queue");});
    splitter->addWidget(browser);splitter->addWidget(sidePanel);splitter->setStretchFactor(0,1);splitter->setSizes({1050,370});
    auto* spacer=new QWidget;spacer->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);toolbar->addWidget(spacer);
    updater_=new Updater(this);auto* updateButton=new QPushButton("Update");updateButton->setObjectName("updateApplication");toolbar->addWidget(updateButton);
    connect(updateButton,&QPushButton::clicked,this,[this]{if(!queue_->busy())updater_->start();});
    connect(updater_,&Updater::statusChanged,updateButton,&QPushButton::setText);
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
            auto index=view->indexAt(point);if(!index.isValid())return;index=index.siblingAtColumn(0);
            if(!selection_->isRowSelected(index.row(),{}))selection_->setCurrentIndex(index,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
            QMenu menu;auto* open=menu.addAction("Open image");auto* folder=menu.addAction("Open folder");auto* camera=menu.addAction("Add camera to search");auto* format=menu.addAction("Add format to search");auto* bits=menu.addAction("Add bit depth to search");auto* start=menu.addAction("Set start date");auto* end=menu.addAction("Set end date");
            const auto& record=catalog_->record(proxy_->mapToSource(index).row());const QDate captured=record.captured.date();const QString cameraName=record.camera,formatName=record.format;const int depth=record.bits;camera->setEnabled(!cameraName.isEmpty());bits->setEnabled(depth>0);start->setEnabled(captured.isValid());end->setEnabled(captured.isValid());
            auto* remove=menu.addAction("Remove selected");remove->setEnabled(!queue_->busy());
            const auto action=menu.exec(view->viewport()->mapToGlobal(point));
            if(action==camera||action==format||action==bits){
                QString term=action==camera?"camera:\""+QString(cameraName).replace('"', ' ')+"\"":action==format?"format:\""+formatName+"\"":"bits:"+QString::number(depth);
                search_->setText((search_->text()+" "+term).trimmed());applySearch();
            }else if(action==start||action==end){
                if(action==start){dateFrom_->setDate(captured);if(dateTo_->date()<captured)dateTo_->setDate(captured);}
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
    connect(reset,&QPushButton::clicked,this,[this]{instant_->clear();format_->clear();range_->setCurrentIndex(0);minImageEdge_->setValue(0);megapixels_->setValue(0);dateEnabled_->setChecked(false);today_->setChecked(false);dateFrom_->setDate(QDate::currentDate().addDays(-1));dateTo_->setDate(QDate::currentDate());});
    // Refresh Today across local midnight, including when the application stays open.
    auto* dateRefresh=new QTimer(this);dateRefresh->setInterval(30000);connect(dateRefresh,&QTimer::timeout,this,[this]{if(today_->isChecked())updateFilter();});dateRefresh->start();
    connect(sort_,&QComboBox::currentIndexChanged,this,[this]{proxy_->sort(sort_->currentData().toInt(),sortOrder_);});
    connect(direction,&QPushButton::clicked,this,[this,direction]{sortOrder_=sortOrder_==Qt::AscendingOrder?Qt::DescendingOrder:Qt::AscendingOrder;direction->setText(sortOrder_==Qt::AscendingOrder?"Ascending":"Descending");proxy_->sort(sort_->currentData().toInt(),sortOrder_);});
    connect(table_->horizontalHeader(),&QHeaderView::sortIndicatorChanged,this,[this,direction](int column,Qt::SortOrder order){sortOrder_=order;sort_->setCurrentIndex(column);direction->setText(order==Qt::AscendingOrder?"Ascending":"Descending");});
    connect(smoothScrolling_,&QCheckBox::toggled,this,&MainWindow::updatePresentation);connect(thumbnailAspect_,&QPushButton::toggled,this,&MainWindow::updatePresentation);
    connect(thumbnailFit_,&QPushButton::toggled,this,&MainWindow::updatePresentation);
    connect(viewMode_,&QButtonGroup::idClicked,this,&MainWindow::updatePresentation);connect(size_,&QSlider::valueChanged,this,&MainWindow::updatePresentation);
    connect(selection_,&QItemSelectionModel::selectionChanged,this,[this]{updateCounts();showMetadata();});
    connect(proxy_,&QAbstractItemModel::rowsInserted,this,&MainWindow::updateCounts);connect(proxy_,&QAbstractItemModel::rowsRemoved,this,&MainWindow::updateCounts);
    connect(catalog_,&QAbstractItemModel::rowsInserted,this,[this](const QModelIndex&,int first,int last){
        for(int row=first;row<=last;++row){const auto ext=QFileInfo(catalog_->record(row).path).suffix().toLower();if(ext=="jxr"||ext=="wdp"||ext=="hdp"){thumbnailAspect_->setChecked(true);break;}}
        updateAutoThumbnailAspect();
    });
    connect(catalog_,&QAbstractItemModel::rowsRemoved,this,[this]{if(!catalog_->rowCount())thumbnailAspectSampleChecked_=false;});
    connect(catalog_,&QAbstractItemModel::dataChanged,this,[this]{updateCounts();showMetadata();updateAutoThumbnailAspect();});
    connect(catalog_,&Catalog::scanStateChanged,this,[this](bool scanning){statusBar()->showMessage(scanning?"Scanning folders…":"Folder scan complete",scanning?0:4000);updateCounts();});
    connect(catalog_,&Catalog::scanError,this,[this](const QString& error){statusBar()->showMessage(error,6000);});
    connect(queue_,&ConversionQueue::statusChanged,catalog_,&Catalog::setStatus);
    connect(queue_,&ConversionQueue::progress,this,[this](int done,int total,int active){progress_->setRange(0,total);progress_->setValue(done);queueState_->setText(QString("%1 / %2 outputs finished · %3 active").arg(done).arg(total).arg(active));updateCounts();});
    connect(queue_,&ConversionQueue::batchFinished,this,[this](int success,int failed,int cancelled){queueState_->setText(QString("%1 done · %2 skipped · %3 failed · %4 cancelled").arg(success).arg(queue_->skipped()).arg(failed).arg(cancelled));updateCounts();if(closeWhenIdle_)QTimer::singleShot(0,this,&QWidget::close);});
    auto* deleteShortcut=new QShortcut(QKeySequence::Delete,this);connect(deleteShortcut,&QShortcut::activated,this,[this]{if(!queue_->busy()&&(list_->hasFocus()||table_->hasFocus()))catalog_->removePaths(selectedPaths());});
    auto* allShortcut=new QShortcut(QKeySequence::SelectAll,this);connect(allShortcut,&QShortcut::activated,this,[this]{if(list_->hasFocus())list_->selectAll();else if(table_->hasFocus())table_->selectAll();});
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
    updateMode();updateFilter();updatePresentation();updateCounts();
    QTimer::singleShot(0,this,[this]{if(loadFolder_->isChecked()&&!recentFolders_.isEmpty())addPaths({recentFolders_.first()});if(loadFiles_->isChecked())addPaths(lastFiles_);});
}
MainWindow::~MainWindow() {
    saveSettings();
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
    return {smoothScrolling_,thumbnailAspect_,thumbnailFit_,mode_,preset_,avifDepth_,rawMode_,wb_,quality_,distance_,downsize_,exif_,transcode_,
        exposure_,maxEdge_,bt2020_,convertGamut_,rawExposure_,kelvin_,highlight_,shoulder_,nits_,shadows_,midtones_,highlights_,cameraLook_,linearHdr_,
        formatFolders_,sourceFolder_,renameConflicts_,overwrite_,workers_,loadFolder_,loadFiles_,acceptSdr_,metadataFilters_,today_,dateEnabled_,dateFrom_,dateTo_,search_,instant_,format_,range_,minImageEdge_,megapixels_,sort_,
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
    sortOrder_=settings.value("browser/descending",false).toBool()?Qt::DescendingOrder:Qt::AscendingOrder;
    findChild<QSlider*>("rawTargetNitsSlider")->setValue(nits_->value());
    findChild<QSlider*>("rawShoulderSlider")->setValue(qRound(shoulder_->value()*100));
}
void MainWindow::saveSettings() const {
    auto settings=appSettings();
    for(auto* widget:persistentControls())if(widget)settings.setValue("controls/"+widget->objectName(),widget->property(settingProperty(widget)));
    QStringList saved;for(const auto target:selectedTargets())saved.append(QString::number(int(target)));
    settings.setValue("search/applied",appliedSearch_);settings.setValue("conversion/targets",saved);settings.setValue("browser/descending",sortOrder_==Qt::DescendingOrder);settings.sync();
}
QWidget* MainWindow::buildControls() {
    auto* panel=new QWidget;auto* layout=new QVBoxLayout(panel);layout->setContentsMargins(14,16,14,16);layout->setSpacing(14);
    auto* general=new QFormLayout;mode_=combo({"Preserve source values","Process pixels"},"conversionMode");mode_->setCurrentIndex(1);general->addRow("Mode",mode_);layout->addLayout(general);
    auto* outputFormats=new QGroupBox("Output formats · select one or more");auto* outputFormatsLayout=new QVBoxLayout(outputFormats);
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
    exif_=check("Keep EXIF","keepExif");exif_->setToolTip("Processed RAW and JXR EXIF retention is not supported by the library yet.");transcode_=check("JXL lossless JPEG transcoding","jpegTranscoding");
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
    raw_=new QGroupBox("RAW rendering");auto* rawLayout=new QFormLayout(raw_);rawMode_=combo({"Source headroom","Boost highlights","Custom"},"rawMode");rawLayout->addRow("HDR mode",rawMode_);
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
    queuePanel_=new QWidget;auto* queueLayout=new QVBoxLayout(queuePanel_);queueLayout->setContentsMargins(14,0,14,10);queueLayout->setSpacing(8);
    auto* destination=new QGroupBox("Output queue");auto* destinationLayout=new QVBoxLayout(destination);output_=new QLineEdit;output_->setObjectName("outputDirectory");output_->setPlaceholderText("Output folder");destinationLayout->addWidget(output_);
    auto* folderButtons=new QHBoxLayout;
    auto* browse=new QPushButton("Choose folder…");browse->setObjectName("chooseOutputFolder");connect(browse,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getExistingDirectory(this,"Output folder",output_->text());if(!path.isEmpty())output_->setText(path);});folderButtons->addWidget(browse);
    auto* openOutput=new QPushButton("Open output folder");openOutput->setObjectName("openOutputFolder");folderButtons->addWidget(openOutput);destinationLayout->addLayout(folderButtons);
    connect(openOutput,&QPushButton::clicked,this,[this]{
        QString path=output_->text().trimmed();
        if(sourceFolder_->isChecked()) {
            auto paths=selectedPaths();if(paths.isEmpty()&&proxy_->rowCount())paths.append(proxy_->index(0,0).data(Catalog::PathRole).toString());
            if(paths.isEmpty()){QMessageBox::warning(this,"Cannot open output folder","Select an image to open its source folder.");return;}
            path=QFileInfo(paths.first()).absolutePath();
        }
        if(path.isEmpty()||!QDir().mkpath(path)){QMessageBox::warning(this,"Cannot open output folder","Choose a writable output folder.");return;}
        if(!QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(path).absolutePath())))QMessageBox::warning(this,"Cannot open output folder","The system file manager could not open this folder.");
    });
    formatFolders_=check("Create format subfolders","formatSubfolders");destinationLayout->addWidget(formatFolders_);
    formatFolders_->setToolTip("Group outputs by format: avif/, jxl/ and jpg/ inside the destination folder.");
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
    auto* startButtons=new QHBoxLayout;startButtons->addWidget(visible_,1);startButtons->addWidget(selected_,1);queueLayout->insertLayout(0,startButtons);
    queueLayout->addWidget(cancel_);progress_=new QProgressBar;progress_->setRange(0,1);progress_->setValue(0);queueLayout->addWidget(progress_);queueState_=new QLabel("Ready");queueState_->setWordWrap(true);queueLayout->addWidget(queueState_);layout->addStretch();
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
    o.keep_exif=exif_->isChecked();o.overwrite=overwrite_->isEnabled()&&overwrite_->isChecked();o.jpeg_transcoding=transcode_->isEnabled()&&transcode_->isChecked()&&target==Target::jxl;
    if(o.mode==Mode::process) {
        o.exposure=static_cast<float>(exposure_->value());o.max_edge=maxEdge_->value();o.extended_bt2020=bt2020_->isChecked();o.convert_gamut=convertGamut_->isChecked();o.raw.hdr_mode=RawHdrMode(rawMode_->currentIndex());o.raw.shoulder=static_cast<float>(shoulder_->value());
        if(o.raw.hdr_mode!=RawHdrMode::source)o.raw.target_nits=static_cast<float>(nits_->value());
        if(o.raw.hdr_mode==RawHdrMode::custom){o.raw.exposure=static_cast<float>(rawExposure_->value());o.raw.white_balance=wb_->currentIndex();o.raw.kelvin=kelvin_->value();o.raw.highlight_stops=static_cast<float>(highlight_->value());o.raw.shoulder=static_cast<float>(shoulder_->value());o.raw.target_nits=static_cast<float>(nits_->value());o.raw.shadows=static_cast<float>(shadows_->value());o.raw.midtones=static_cast<float>(midtones_->value());o.raw.highlights=static_cast<float>(highlights_->value());o.raw.camera_look=cameraLook_->isChecked();o.raw.linear_hdr=linearHdr_->isChecked();}
    }
    validate(o);return o;
}
void MainWindow::applySearch() {appliedSearch_=search_->text();updateFilter();}
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
    if(!paths.isEmpty())catalog_->addPaths(paths,recursive_->isChecked());
}

void MainWindow::updateFilter() {
    Filter f;f.search=appliedSearch_;f.text=instant_->text();f.literalText=!metadataFilters_->isChecked();f.today=today_->isChecked();
    if(metadataFilters_->isChecked()) {
        f.format=format_->text().trimmed();if(range_->currentIndex())f.range=range_->currentText();
        f.minMaxEdge=minImageEdge_->value();f.minMegapixels=megapixels_->value();
        if(dateEnabled_->isChecked()&&!f.today){f.from=dateFrom_->date();f.to=dateTo_->date();}
    }
    const bool dates=metadataFilters_->isChecked()&&dateEnabled_->isChecked()&&!f.today;
    dateEnabled_->setEnabled(!f.today);dateFrom_->setEnabled(dates);dateTo_->setEnabled(dates);
    proxy_->setFilter(f);updateCounts();
}
void MainWindow::updatePresentation() {
    const bool details=viewMode_->checkedId()==2,grid=viewMode_->checkedId()==0;
    views_->setCurrentWidget(details?static_cast<QWidget*>(table_):list_);list_->setPresentation(grid,size_->value(),thumbnailAspect_->isChecked());
    list_->setSmoothScrolling(smoothScrolling_->isChecked());table_->setSmoothScrolling(smoothScrolling_->isChecked());
    const bool wide=thumbnailAspect_->isChecked();thumbnailAspect_->setText(wide?"16:9":"1:1");
    thumbnailAspect_->setMinimumWidth(thumbnailAspect_->sizeHint().width());
    const bool fill=thumbnailFit_->isChecked();thumbnailFit_->setText(fill?"Fill":"Fit");delegate_->fill=fill;table_->setFillThumbnails(fill);
    table_->setIconSize(QSize(size_->value()/2,size_->value()/2));table_->verticalHeader()->setDefaultSectionSize(std::max(32,size_->value()/2+8));
}
void MainWindow::updateAutoThumbnailAspect() {
    if(thumbnailAspectSampleChecked_||catalog_->rowCount()<3)return;
    bool wide=true;
    for(int row=0;row<3;++row) {
        const auto& record=catalog_->record(row);
        if(!record.inspected)return;
        // Allow rounded pixel dimensions, without mistaking other landscape
        // ratios for 16:9. Read the display dimensions, including orientation.
        wide=wide&&record.height>0&&std::abs(double(record.width)/record.height-16.0/9)<0.02;
    }
    // Sample once per library so later metadata/thumbnail completions cannot
    // undo a manual choice of cell aspect ratio.
    thumbnailAspectSampleChecked_=true;
    if(wide)thumbnailAspect_->setChecked(true);
}
QStringList MainWindow::selectedPaths() const {QStringList out;for(const auto& index:selection_->selectedRows(0))out<<index.data(Catalog::PathRole).toString();return out;}
void MainWindow::updateCounts() {
    const int selected=selection_->selectedRows(0).size();const bool busy=queue_->busy()||(updater_&&updater_->busy()),hasTargets=!selectedTargets().isEmpty();
    if(auto* button=findChild<QPushButton*>("updateApplication"))button->setEnabled(!busy);
    counts_->setText(QString("%1 visible / %2 files · %3 selected%4").arg(proxy_->rowCount()).arg(catalog_->rowCount()).arg(selected).arg(catalog_->scanning()?" · scanning…":""));
    selected_->setEnabled(selected>0&&hasTargets&&!busy);visible_->setEnabled(proxy_->rowCount()>0&&hasTargets&&!busy);cancel_->setEnabled(busy);remove_->setEnabled(selected>0&&!busy);clear_->setEnabled(catalog_->rowCount()>0&&!busy&&!catalog_->scanning());workers_->setEnabled(!busy);
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
    if(paths.isEmpty()||queue_->busy()||(updater_&&updater_->busy()))return;
    try {
        const auto targets=selectedTargets();if(targets.isEmpty())throw std::invalid_argument("Select at least one output format");
        const QString directory=output_->text().trimmed();if(directory.isEmpty()&&!sourceFolder_->isChecked())throw std::invalid_argument("Choose an output folder");
        QMap<int,QStringList> inputsByTarget;
        for(const auto& path:paths) {
            const auto extension=QFileInfo(path).suffix().toLower();
            const bool hdrGroup=extension!="jpg"&&extension!="jpeg"&&extension!="jpe"&&extension!="avif"&&extension!="png"&&extension!="webp"&&extension!="tif"&&extension!="tiff"&&extension!="bmp"&&extension!="gif";
            for(auto target:targets) {
                const bool hdrTarget=target==Target::jpeg_gainmap||target==Target::jxl_linear||target==Target::jxl_scrgb||target==Target::jxl_pq||target==Target::avif_pq||target==Target::avif_gainmap;
                if(hdrTarget&&!hdrGroup) {
                    if(!acceptSdr_->isChecked())continue;
                    if(target==Target::jpeg_gainmap)target=Target::jpeg;
                    else if(target==Target::jxl_linear||target==Target::jxl_pq)target=Target::jxl;
                    else if(target!=Target::jxl_scrgb)target=Target::avif;
                }
                auto& inputs=inputsByTarget[int(target)];if(!inputs.contains(path))inputs.append(path);
            }
        }
        OutputLayout layout;layout.format_subfolders=formatFolders_->isChecked();layout.source_folder=sourceFolder_->isChecked();layout.rename_conflicts=renameConflicts_->isChecked();
        for(int row=0;row<catalog_->rowCount();++row){const auto& record=catalog_->record(row);if(record.raw)layout.raw_inputs.insert(record.path);}
        QVector<Job> jobs;for(auto it=inputsByTarget.cbegin();it!=inputsByTarget.cend();++it)jobs+=makeJobs(it.value(),directory,optionsForTarget(Target(it.key())),layout);
        if(jobs.isEmpty())throw std::invalid_argument("No inputs match the selected targets. Enable Accept SDR formats to include SDR sources with SDR output equivalents.");
        QSet<QString> inputs;for(const auto& path:paths)inputs.insert(QFileInfo(path).absoluteFilePath().toCaseFolded());
        for(const auto& job:jobs) {
            if(inputs.contains(QFileInfo(job.output).absoluteFilePath().toCaseFolded()))throw std::invalid_argument("Output would replace an input file");
            if(!QDir().mkpath(QFileInfo(job.output).absolutePath()))throw std::runtime_error("Cannot create output folder");
        }
        queue_->start(std::move(jobs),workers_->value());updateCounts();
    } catch(const std::exception& e){QMessageBox::warning(this,"Cannot start conversion",QString::fromUtf8(e.what()));}
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if(queue_->busy()){closeWhenIdle_=true;queue_->cancelPending();queueState_->setText("Closing after active conversions finish…");event->ignore();return;}event->accept();
}
bool MainWindow::eventFilter(QObject* watched,QEvent* event) {
    if(watched==targets_->viewport()&&event->type()==QEvent::MouseButtonRelease) {
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
