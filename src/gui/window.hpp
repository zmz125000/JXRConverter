#pragma once
#include "catalog.hpp"
#include "queue.hpp"
#include <QMainWindow>
#include <QVariantMap>
class QButtonGroup;
class QAction;
class QActionGroup;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QGroupBox;
class QItemSelectionModel;
class QItemSelection;
class QAbstractItemView;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QMenu;
class QToolButton;
class QWidget;
namespace jxrc::gui {
class Updater;
class FileListView;
class FileTableView;
class ThumbnailDelegate;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void addPaths(const QStringList& paths);
    Options options() const;
    Catalog* catalog() const { return catalog_; }
    CatalogFilter* filterModel() const { return proxy_; }
    ConversionQueue* queue() const { return queue_; }
protected:
    bool eventFilter(QObject*, QEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    QWidget* buildControls();
    void addDroppedPaths(const QStringList& paths);
    void loadHistory();
    void saveHistory();
    void applySearch();
    void resetFilters();
    void syncSelection();
    QSet<QString>& imageSelection();
    const QSet<QString>& imageSelection() const;
    void changeSelection(const QItemSelection&, const QItemSelection&);
    void selectVisible();
    void clearImageSelection();
    void removeImageSelection();
    void clearCurrentTab();
    void openOutputFolder();
    OutputLayout outputLayout(qint64 batch, bool quick = false) const;
    void showJob(qint64 number);
    void updateJobOutput(qint64, const QString&, const QString&, const QString&);
    QString browserScrollKey() const;
    void saveBrowserScroll();
    QList<QWidget*> browserControls() const;
    void switchBrowserState();
    void applyPresentation();
    void restoreBrowserScroll();
    void loadJobs();
    void clearJobs();
    void saveJobs();
    void refreshJobs();
    void createDraftJob(const QStringList& files);
    bool isDraftJob(qint64 number) const;
    void recordJob(const QVector<Job>&, bool temporary, bool reuse = false, bool quick = false);
    QString jobText(qint64 number) const;
    QList<QWidget*> persistentControls() const;
    QList<QWidget*> filterControls() const;
    void restoreSettings();
    void saveSettings() const;
    void updateMode();
    void updateOutputSettings();
    void updateFilter();
    void updateBackgroundMetadata();
    void updatePresentation();
    void updateAutoThumbnailAspect();
    void requestAspectSample(bool viewport);
    void setThumbnailAspect(int index);
    void updateCounts();
    void showLibraryMessage(const QString& message, int timeout = 0);
    void showMetadata();
    void convertFiles(bool selected);
    void convertPaths(const QStringList&, const QVector<Target>&, bool exactTargets = false, bool priority = false, bool append = false, bool reuseJob = false, bool quick = false);
    void checkForUpdates();
    void logMessage(const QString&);
    QVector<Target> selectedTargets() const;
    Options optionsForTarget(Target target) const;
    QStringList selectedPaths() const;
    Catalog* catalog_;
    CatalogFilter* proxy_;
    ConversionQueue* queue_;
    Updater* updater_=nullptr;
    QItemSelectionModel* selection_;
    FileListView* list_;
    FileTableView* table_;
    ThumbnailDelegate* delegate_;
    QStackedWidget* views_;
    QButtonGroup *viewMode_, *statusFilter_;
    QComboBox *sort_, *range_, *mode_, *preset_, *avifDepth_, *pngDepth_, *rawMode_, *wb_;
    QListWidget* targets_ = nullptr;
    QLineEdit *search_, *instant_, *format_, *output_;
    QSlider* size_;
    QSpinBox *minImageEdge_, *quality_, *maxEdge_, *kelvin_, *nits_, *workers_;
    QDoubleSpinBox *megapixels_, *distance_, *exposure_, *rawExposure_, *highlight_, *shoulder_, *shadows_, *midtones_, *highlights_;
    QDateEdit *dateFrom_, *dateTo_;
    QCheckBox *autoAspect_, *portraitAspect_, *smoothScrolling_, *loadFolder_, *loadFiles_, *loadJobs_, *loadFilters_, *acceptSdr_, *metadataFilters_, *today_, *wheelChanges_, *checkUpdates_, *dateEnabled_, *recursive_, *downsize_, *exif_, *transcode_, *formatFolders_, *convertedFolder_, *parentFolders_, *flattenFolders_, *dateFolders_, *jobFolders_, *jumpToJob_, *sourceFolder_, *renameConflicts_, *overwrite_, *convertGamut_, *srgbClamp_, *bt2020_, *cameraLook_, *linearHdr_, *disableRawBoost_, *rawAutoExposure_, *rawProtectHighlights_, *autoBitDepth_;
    QGroupBox *custom_, *processing_, *raw_;
    QWidget *rawCustom_, *boostTarget_, *queuePanel_;
    QPushButton *thumbnailFit_, *selected_, *visible_, *cancel_, *progressStop_, *remove_, *clear_, *cancelJob_;
    QLabel *counts_, *queueState_, *threadBudget_;
    QPlainTextEdit *inspector_, *outputLog_;
    QTimer* updateTimer_;
    QTimer* libraryMessageTimer_;
    QString libraryMessage_;
    QTimer* jobSaveTimer_;
    QAction* clearJobs_;
    QActionGroup* jobActions_;
    double jobWheelSteps_=0;
    QMenu* jobMenu_;
    QToolButton *jobButton_, *thumbnailAspect_;
    QTimer* aspectTimer_;
    int aspectIndex_ = 2;
    QPushButton* running_;
    QProgressBar* progress_;
    QStringList recentFolders_, recentFiles_, lastFiles_;
    QString appliedSearch_;
    QSet<QString> loggedWarnings_, selectedImages_, selectedTabImages_, rememberedImages_;
    QHash<qint64,QSet<QString>> jobSelections_;
    struct OutputProgress {QString input, state;};
    struct SavedJob { qint64 number; QStringList files; QHash<QString,OutputProgress> outputs; QString folder; bool quick = false; };
    QVector<SavedJob> savedJobs_;
    SavedJob temporaryJob_{0,{},{},{}};
    qint64 nextJob_ = 1, viewedJob_ = 0, quickJob_ = -1;
    struct BrowserState {
        QVariantMap controls;
        QString search;
        int mode = 0, aspect = 2;
        Qt::SortOrder order = Qt::AscendingOrder;
        QByteArray header;
    };
    struct ViewportState {
        QPoint scroll;
        QString currentPath;
        int currentColumn = 0;
    };
    QHash<QString,BrowserState> browserStates_;
    QString browserTabKey_;
    QHash<QString,ViewportState> scrollPositions_;
    quint64 scrollRestoreGeneration_ = 0;
    bool restoringBrowserScroll_ = false;
    QString scrollKey_;
    QAbstractItemView* scrollView_ = nullptr;
    int selectionModelChanges_ = 0;
    bool syncingSelection_ = false, bulkSelecting_ = false;
    bool jobHistoryLoaded_ = false, jobHistoryDirty_ = false;
    bool closeWhenIdle_ = false;
    bool aspectSamplePending_ = false, aspectViewportSample_ = false, changingAspect_ = false;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
};
}
