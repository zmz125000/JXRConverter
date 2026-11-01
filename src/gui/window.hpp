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
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QWidget;
namespace jxrc::gui {
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
    QList<QWidget*> persistentControls() const;
    void restoreSettings();
    void saveSettings() const;
    void updateMode();
    void updateFilter();
    void updatePresentation();
    void updateCounts();
    void showMetadata();
    void convertFiles(bool selected);
    QVector<Target> selectedTargets() const;
    Options optionsForTarget(Target target) const;
    QStringList selectedPaths() const;
    Catalog* catalog_;
    CatalogFilter* proxy_;
    ConversionQueue* queue_;
    QItemSelectionModel* selection_;
    FileListView* list_;
    FileTableView* table_;
    ThumbnailDelegate* delegate_;
    QStackedWidget* views_;
    QButtonGroup* viewMode_;
    QComboBox *sort_, *range_, *mode_, *preset_, *avifDepth_, *rawMode_, *wb_;
    QListWidget* targets_;
    QLineEdit *search_, *instant_, *format_, *output_;
    QSlider* size_;
    QSpinBox *minImageEdge_, *quality_, *maxEdge_, *kelvin_, *nits_, *workers_;
    QDoubleSpinBox *megapixels_, *distance_, *exposure_, *rawExposure_, *highlight_, *shoulder_, *shadows_, *midtones_, *highlights_;
    QDateEdit *dateFrom_, *dateTo_;
    QCheckBox *loadFolder_, *loadFiles_, *acceptSdr_, *metadataFilters_, *today_, *wheelChanges_, *dateEnabled_, *recursive_, *downsize_, *exif_, *transcode_, *overwrite_, *bt2020_, *cameraLook_, *linearHdr_;
    QGroupBox *custom_, *processing_, *raw_;
    QWidget *rawCustom_, *boostTarget_, *queuePanel_;
    QPushButton *selected_, *visible_, *cancel_, *remove_, *clear_;
    QLabel *counts_, *queueState_, *threadBudget_;
    QPlainTextEdit* inspector_;
    QProgressBar* progress_;
    QStringList recentFolders_, recentFiles_, lastFiles_;
    QString appliedSearch_;
    bool closeWhenIdle_ = false;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
};
}
