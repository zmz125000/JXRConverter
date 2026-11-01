#pragma once
#include <QListView>
#include <QStyledItemDelegate>
#include <QTableView>
class QPropertyAnimation;

namespace jxrc::gui {
class FileListView : public QListView {
    Q_OBJECT
public:
    explicit FileListView(QWidget* parent = nullptr);
    void setPresentation(bool grid, int size, bool wide = false);
    void setSmoothScrolling(bool enabled);
signals:
    void pathsDropped(const QStringList& paths);
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
    bool wide_ = false, smooth_ = false;
    QPropertyAnimation* scrollAnimation_ = nullptr;
    double wheelRemainder_ = 0;
};
class FileTableView : public QTableView {
    Q_OBJECT
public:
    explicit FileTableView(QWidget* parent = nullptr);
    void setSmoothScrolling(bool enabled);
    void setFillThumbnails(bool fill);
signals:
    void pathsDropped(const QStringList& paths);
protected:
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    bool smooth_ = false;
    QPropertyAnimation* scrollAnimation_ = nullptr;
    double wheelRemainder_ = 0;
};
class ThumbnailDelegate : public QStyledItemDelegate {
public:
    explicit ThumbnailDelegate(QObject* parent = nullptr):QStyledItemDelegate(parent) {}
    bool grid = true;
    int edge = 200;
    bool wide = false;
    bool fill = false;
    int thumbnailHeight() const { return wide ? qRound(edge*9.0/16) : edge; }
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
};
}
