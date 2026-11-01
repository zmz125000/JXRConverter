#pragma once
#include <QAbstractTableModel>
#include <QCache>
#include <QDateTime>
#include <QImage>
#include <QMap>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <memory>
class QRunnable;

namespace jxrc::gui {
struct Record {
    QString path, name, format, camera, lens, transfer, primaries;
    QString status = "Ready", error, metadataWarning;
    qint64 bytes = 0;
    int width = 0, height = 0, bits = 0, iso = 0;
    double aperture = 0, shutter = 0, focal = 0;
    bool raw = false, gainmap = false, hdr = false, inspected = false, thumbnailPending = false, thumbnailFailed = false;
    QDateTime captured, modified;
    QMap<QString, QString> exif;
};
bool supportedFile(const QString& path);
Record readMetadata(const QString& path);
QImage readThumbnail(const QString& path, int edge = 384);

class Catalog : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Format, Dimensions, Size, Captured, Camera, Lens, Iso, Aperture, Shutter, Focal, Transfer, Primaries, Bits, Status, ColumnCount };
    enum Role { PathRole = Qt::UserRole + 1, SummaryRole, RawRole, ThumbnailRole };
    explicit Catalog(QObject* parent = nullptr, int thumbnailCacheKiB = 65536);
    ~Catalog() override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    const Record& record(int row) const { return records_.at(row); }
    QStringList paths() const;
    void addPaths(const QStringList& paths, bool recursive = true);
    void removePaths(const QStringList& paths);
    void setStatus(const QString& path, const QString& status, const QString& error = {});
    // Layout probes never enqueue work; views bracket actual painting instead.
    void beginThumbnailFrame(const std::function<bool(const QModelIndex&)>& visible);
    void endThumbnailFrame();
    int thumbnailJobCount() const { return thumbnailJobs_.size(); }
    int thumbnailCacheCost() const { return cache_.totalCost(); }
    bool scanning() const { return scanning_ > 0; }
signals:
    void scanStateChanged(bool scanning);
    void scanError(const QString& error);
private:
    void insertPaths(const QStringList& paths);
    void pumpThumbnails();
    void cancelThumbnail(const QString& path);
    void fitThumbnailCache();
    int find(const QString& path) const;
    QVector<Record> records_;
    QSet<QString> known_;
    QThreadPool scans_, metadata_, thumbnails_;
    QCache<QString, QImage> cache_{65536}; // KiB; bounded independently of catalog size.
    QStringList visibleThumbnails_;
    QMap<QString, std::shared_ptr<std::atomic_bool>> thumbnailJobs_;
    QMap<QString, QRunnable*> thumbnailTasks_;
    int thumbnailEdge_ = 384;
    bool thumbnailFrame_ = false;
    int thumbnailFrameDepth_ = 0;
    std::function<bool(const QModelIndex&)> thumbnailVisible_;
    int scanning_ = 0;
};
struct Filter {
    QString search, text, format, range;
    int minWidth = 0, minHeight = 0, minMaxEdge = 0;
    double minMegapixels = 0;
    bool literalText = false, today = false;
    QDate from, to;
};
class CatalogFilter : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit CatalogFilter(QObject* parent = nullptr);
    void setFilter(const Filter& filter);
    const Filter& filter() const { return filter_; }
protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
private:
    Filter filter_;
};
}
