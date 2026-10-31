#pragma once
#include <QListView>
#include <QStyledItemDelegate>
#include <QTableView>

namespace jxrc::gui {
class FileListView : public QListView {
    Q_OBJECT
public:
    explicit FileListView(QWidget* parent = nullptr);
    void setPresentation(bool grid, int size);
signals:
    void pathsDropped(const QStringList& paths);
protected:
    void resizeEvent(QResizeEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    void updateCellSize();
    bool grid_ = true;
    int minimumCell_ = 100;
};
class FileTableView : public QTableView {
    Q_OBJECT
public:
    explicit FileTableView(QWidget* parent = nullptr);
signals:
    void pathsDropped(const QStringList& paths);
protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
};
class ThumbnailDelegate : public QStyledItemDelegate {
public:
    explicit ThumbnailDelegate(QObject* parent = nullptr):QStyledItemDelegate(parent) {}
    bool grid = true;
    int edge = 144;
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
};
}
