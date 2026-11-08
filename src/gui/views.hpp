#pragma once
#include <QListView>
#include <QStyledItemDelegate>
#include <QTableView>
class QPropertyAnimation;

namespace jxrc::gui {
void revealFile(const QString& path);
class FileListView : public QListView {
    Q_OBJECT
public:
    explicit FileListView(QWidget* parent = nullptr);
    void setPresentation(bool grid, int size, double aspect = 1.0);
    void setSmoothScrolling(bool enabled);
    void selectAll() override;
    QModelIndexList firstVisibleRow(int count = 2) const;
signals:
    void pathsDropped(const QStringList& paths);
    void bulkSelectionChanged(bool selecting);
    void viewportChanged();
protected:
    void wheelEvent(QWheelEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    void updateCellSize();
    bool grid_ = true;
    int minimumCell_ = 200;
    double aspect_ = 1.0;
    bool smooth_ = false;
    QPropertyAnimation* scrollAnimation_ = nullptr;
    double wheelRemainder_ = 0;
};
class FileTableView : public QTableView {
    Q_OBJECT
public:
    explicit FileTableView(QWidget* parent = nullptr);
    void setSmoothScrolling(bool enabled);
    void selectAll() override;
    void setFillThumbnails(bool fill);
    void setModel(QAbstractItemModel*) override;
    void setSelectionModel(QItemSelectionModel*) override;
    QRect visualRect(const QModelIndex&) const override;
    QModelIndex indexAt(const QPoint&) const override;
    void scrollTo(const QModelIndex&, ScrollHint hint = EnsureVisible) override;
signals:
    void pathsDropped(const QStringList& paths);
    void bulkSelectionChanged(bool selecting);
protected:
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    void updateFrozenColumn();
    QTableView* frozen_;
    bool smooth_ = false;
    QPropertyAnimation* scrollAnimation_ = nullptr;
    double wheelRemainder_ = 0;
};
class ThumbnailDelegate : public QStyledItemDelegate {
public:
    explicit ThumbnailDelegate(QObject* parent = nullptr):QStyledItemDelegate(parent) {}
    bool grid = true;
    int edge = 200;
    double aspect = 1.0;
    bool fill = false;
    int thumbnailHeight() const { return qRound(edge/aspect); }
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
};
}
