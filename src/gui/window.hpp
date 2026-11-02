#pragma once
#include "catalog.hpp"
#include "queue.hpp"
#include <QMainWindow>
class QButtonGroup;
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
    void changeSelection(const QItemSelection&, const QItemSelection&);
    void selectVisible();
    void clearImageSelection();
    void clearCurrentTab();
    void openOutputFolder();
    void showJob(qint64 number);
    void updateJobOutput(qint64, const QString&, const QString&, const QString&);
    bool collectionView() const;
    QString browserScrollKey() const;
    void saveBrowserScroll();
    void restoreBrowserScroll();
    void loadJobs();
    void saveJobs();
    void refreshJobs();
    void recordJob(const QVector<Job>&, bool temporary);
    QString jobText(qint64 number) const;
    QList<QWidget*> persistentControls() const;
    void restoreSettings();
    void saveSettings() const;
    void updateMode();
    void updateOutputSettings();
    void updateFilter();
    void updateBackgroundMetadata();
    void updatePresentation();
    void updateAutoThumbnailAspect();
    void updateCounts();
    void showMetadata();
    void convertFiles(bool selected);
    void convertPaths(const QStringList&, const QVector<Target>&, bool exactTargets = false, bool priority = false, bool append = false);
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
    QComboBox *sort_, *range_, *mode_, *preset_, *avifDepth_, *rawMode_, *wb_;
    QListWidget* targets_ = nullptr;
    QLineEdit *search_, *instant_, *format_, *output_;
    QSlider* size_;
    QSpinBox *minImageEdge_, *quality_, *maxEdge_, *kelvin_, *nits_, *workers_;
    QDoubleSpinBox *megapixels_, *distance_, *exposure_, *rawExposure_, *highlight_, *shoulder_, *shadows_, *midtones_, *highlights_;
    QDateEdit *dateFrom_, *dateTo_;
    QCheckBox *smoothScrolling_, *loadFolder_, *loadFiles_, *acceptSdr_, *metadataFilters_, *today_, *wheelChanges_, *checkUpdates_, *dateEnabled_, *recursive_, *downsize_, *exif_, *transcode_, *formatFolders_, *dateFolders_, *jobFolders_, *sourceFolder_, *renameConflicts_, *overwrite_, *convertGamut_, *bt2020_, *cameraLook_, *linearHdr_;
    QGroupBox *custom_, *processing_, *raw_;
    QWidget *rawCustom_, *boostTarget_, *queuePanel_;
    QPushButton *thumbnailAspect_, *thumbnailFit_, *selected_, *visible_, *cancel_, *progressStop_, *remove_, *clear_;
    QLabel *counts_, *queueState_, *threadBudget_;
    QPlainTextEdit *inspector_, *outputLog_;
    QTimer* updateTimer_;
    QTimer *jobHover_, *jobSaveTimer_;
    QMenu* jobMenu_;
    QToolButton* jobButton_;
    QPushButton* running_;
    QProgressBar* progress_;
    QStringList recentFolders_, recentFiles_, lastFiles_;
    QString appliedSearch_;
    QSet<QString> loggedWarnings_, selectedImages_, rememberedImages_;
    struct OutputProgress {QString input, state;};
    struct SavedJob { qint64 number; QStringList files; QHash<QString,OutputProgress> outputs; QString folder; };
    QVector<SavedJob> savedJobs_;
    SavedJob temporaryJob_{0,{},{},{}};
    qint64 nextJob_ = 1, viewedJob_ = 0;
    QHash<QString,QPoint> scrollPositions_;
    QString scrollKey_;
    QAbstractItemView* scrollView_ = nullptr;
    int selectionModelChanges_ = 0;
    bool syncingSelection_ = false, bulkSelecting_ = false;
    bool closeWhenIdle_ = false;
    bool thumbnailAspectSampleChecked_ = false;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
};
}
