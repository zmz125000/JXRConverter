#include "views.hpp"
#include "catalog.hpp"
#include <QApplication>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QPainter>
#include <QResizeEvent>
#include <QHideEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QPropertyAnimation>
#include <QAbstractProxyModel>
#include <QUrl>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#endif
#include <algorithm>

namespace jxrc::gui {
void revealFile(const QString& path) {
    const auto absolute=QFileInfo(path).absoluteFilePath();
    auto fallback=[absolute]{QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(absolute).absolutePath()));};
#if defined(Q_OS_WIN)
    if(!QProcess::startDetached("explorer.exe",{"/select,",QDir::toNativeSeparators(absolute)}))fallback();
#elif defined(Q_OS_MACOS)
    if(!QProcess::startDetached("/usr/bin/open",{"-R",absolute}))fallback();
#else
    auto message=QDBusMessage::createMethodCall("org.freedesktop.FileManager1","/org/freedesktop/FileManager1","org.freedesktop.FileManager1","ShowItems");
    message<<QStringList{QUrl::fromLocalFile(absolute).toString(QUrl::FullyEncoded)}<<QString{};
    auto* watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message),qApp);
    QObject::connect(watcher,&QDBusPendingCallWatcher::finished,qApp,[fallback](QDBusPendingCallWatcher* call){
        if(call->reply().type()==QDBusMessage::ErrorMessage)fallback();
        call->deleteLater();
    });
#endif
}
void FileListView::selectAll(){emit bulkSelectionChanged(true);QListView::selectAll();emit bulkSelectionChanged(false);}
void FileTableView::selectAll(){emit bulkSelectionChanged(true);QTableView::selectAll();emit bulkSelectionChanged(false);}

namespace {
void paintThumbnail(QPainter* painter,const QRect& rect,const QIcon& icon,bool fill) {
    const auto pixmap=icon.pixmap(icon.availableSizes().value(0,QSize(384,384)));
    if(pixmap.isNull()||rect.isEmpty())return;
    QRectF source(pixmap.rect()),target(rect);
    if(fill) {
        const double ratio=double(rect.width())/rect.height();
        if(source.width()/source.height()>ratio){const double width=source.height()*ratio;source.setLeft((source.width()-width)/2);source.setWidth(width);}
        else{const double height=source.width()/ratio;source.setTop((source.height()-height)/2);source.setHeight(height);}
    } else {
        const auto size=source.size().scaled(target.size(),Qt::KeepAspectRatio);
        target=QRectF(QPointF(target.center().x()-size.width()/2,target.center().y()-size.height()/2),size);
    }
    painter->save();painter->setRenderHint(QPainter::SmoothPixmapTransform);painter->drawPixmap(target,pixmap,source);painter->restore();
}
// Only decoration lookups made while painting can request thumbnails. Keep
// previously painted cells during partial repaints, pruning them by geometry.
class TableThumbnailDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    bool fill = false;
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const override {
        if(index.column()==Catalog::Name)index.data(Catalog::ThumbnailRole);
        if(fill&&index.column()==Catalog::Name) {
            QStyleOptionViewItem styled(option);initStyleOption(&styled,index);
            const auto icon=styled.icon;
            if(!icon.isNull()) {
                if(option.decorationSize.isValid())styled.decorationSize=option.decorationSize;
                auto* style=styled.widget?styled.widget->style():QApplication::style();
                const auto rect=style->subElementRect(QStyle::SE_ItemViewItemDecoration,&styled,styled.widget);
                // Retain decoration layout and text spacing while painting
                // the actual thumbnail ourselves with a centered crop.
                QPixmap placeholder(styled.decorationSize);placeholder.fill(Qt::transparent);styled.icon=QIcon(placeholder);
                style->drawControl(QStyle::CE_ItemViewItem,&styled,painter,styled.widget);
                paintThumbnail(painter,rect,icon,true);return;
            }
        }
        QStyledItemDelegate::paint(painter,option,index);
    }
};
class ThumbnailFrame {
    Catalog* catalog_ = nullptr;
public:
    explicit ThumbnailFrame(QAbstractItemView* view, bool hidden = false) {
        auto* model=view->model();QList<QAbstractProxyModel*> proxies;
        while(auto* proxy=qobject_cast<QAbstractProxyModel*>(model)){proxies.prepend(proxy);model=proxy->sourceModel();}
        catalog_=qobject_cast<Catalog*>(model);if(!catalog_)return;
        catalog_->beginThumbnailFrame([view,hidden,proxies](const QModelIndex& source) {
            if(hidden||!view->isVisible())return false;
            auto index=source;for(auto* proxy:proxies)index=proxy->mapFromSource(index);
            return index.isValid()&&view->visualRect(index).intersects(view->viewport()->rect());
        });
    }
    ~ThumbnailFrame(){if(catalog_)catalog_->endThumbnailFrame();}
};
bool scrollWheel(QAbstractItemView* view,QWheelEvent* event,bool smooth,QPropertyAnimation*& animation,double& remainder) {
    const int pixel=event->pixelDelta().y(),angle=event->angleDelta().y();
    if(!pixel&&!angle)return false;
    auto* bar=view->verticalScrollBar();
    if(!animation){animation=new QPropertyAnimation(bar,"value",view);animation->setDuration(140);animation->setEasingCurve(QEasingCurve::OutCubic);}
    const double delta=(pixel?double(pixel):double(angle)/120*QApplication::wheelScrollLines()*bar->singleStep())/2+remainder;
    const int movement=qRound(delta);remainder=delta-movement;
    const int start=animation->state()==QAbstractAnimation::Running?animation->endValue().toInt():bar->value();
    const int target=std::clamp(start-movement,bar->minimum(),bar->maximum());
    animation->stop();
    if(smooth){animation->setStartValue(bar->value());animation->setEndValue(target);animation->start();}
    else bar->setValue(target);
    event->accept();return true;
}
QStringList paths(const QMimeData* mime) {QStringList out;for(const auto& url:mime->urls())if(url.isLocalFile())out<<url.toLocalFile();return out;}
}
FileListView::FileListView(QWidget* parent):QListView(parent) {
    setObjectName("fileList");setSelectionMode(ExtendedSelection);setSelectionBehavior(SelectRows);setSelectionRectVisible(true);setDragEnabled(false);
    setAcceptDrops(true);setDropIndicatorShown(false);setEditTriggers(NoEditTriggers);setUniformItemSizes(true);
    setVerticalScrollMode(ScrollPerPixel);setHorizontalScrollMode(ScrollPerPixel);
    setResizeMode(Adjust);setLayoutMode(Batched);setBatchSize(128);setSpacing(5);
    // Stable viewport width keeps adaptive columns from oscillating as the
    // scrollbar appears or disappears near a column/row boundary.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    connect(verticalScrollBar(),&QScrollBar::valueChanged,this,[this]{ThumbnailFrame frame(this);emit viewportChanged();});
}
void FileListView::paintEvent(QPaintEvent* event) {
    ThumbnailFrame frame(this);
    QListView::paintEvent(event);
    if(model()&&model()->rowCount()==0) {
        QPainter painter(viewport());painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(viewport()->rect().adjusted(24,24,-24,-24),Qt::AlignCenter|Qt::TextWordWrap,
            "Drop image files or folders here\n\nOpen folder to browse your collection.\nDrag from blank space to box-select files.");
    }
}
void FileListView::setPresentation(bool grid,int size,double aspect) {
    grid_=grid;minimumCell_=size;aspect_=std::clamp(aspect,0.25,4.0);
    setViewMode(grid?IconMode:ListMode);setMovement(Static);setWrapping(grid);setFlow(grid?LeftToRight:TopToBottom);
    setVerticalScrollMode(ScrollPerPixel);setSelectionRectVisible(true);setSpacing(grid?0:5);updateCellSize();
}
void FileListView::resizeEvent(QResizeEvent* event) {
    QListView::resizeEvent(event);updateCellSize();emit viewportChanged();
}
void FileListView::updateCellSize() {
    // LocalViewer 535800129: GridCells.Adaptive chooses columns from the
    // minimum cell width plus fixed gaps, then stretches the cells to fill
    // the content width. Qt geometry is already in logical pixels (dp).
    constexpr int gutter=10;
    constexpr int margin=5;
    const int available=std::max(1,viewport()->width()-2*margin);
    const int columns=std::max(1,(available+gutter)/(std::max(1,minimumCell_)+gutter));
    // QListView compares the next slot against QRect::right() (inclusive).
    // Reserve one pixel in width allocation so the final column does not wrap.
    const int cell=std::max(1,(available-gutter*(columns-1)-1)/columns);
    // QListView slots include the gutter; the delegate paints a 5 dp inset
    // on both sides, leaving the fitted cell width and a fixed 10 dp gap.
    const int slot=cell+gutter;
    const int edge=grid_?std::max(1,cell-20):minimumCell_;
    if(auto* delegate=dynamic_cast<ThumbnailDelegate*>(itemDelegate())){delegate->grid=grid_;delegate->edge=edge;delegate->aspect=aspect_;}
    setGridSize(grid_?QSize(slot,qRound(edge/aspect_)+94):QSize{});
    setIconSize(QSize(edge,edge));doItemsLayout();viewport()->update();
}
QModelIndexList FileListView::firstVisibleRow(int count) const {
    QModelIndexList result;if(!model()||count<1)return result;
    const int slot=grid_?std::max(1,gridSize().width()):std::max(1,viewport()->width());
    const int limit=std::min(viewport()->height(),grid_?gridSize().height()+8:sizeHintForRow(0)+8);
    for(int y=0;y<limit;y+=4){
        for(int x=std::min(slot/2,viewport()->width()/2);x<viewport()->width()&&result.size()<count;x+=slot){const auto index=indexAt(QPoint(x,y));if(index.isValid()&&!result.contains(index))result.append(index);}
        if(!result.isEmpty())break;
    }
    return result;
}
void FileListView::setSmoothScrolling(bool enabled){smooth_=enabled;if(!enabled&&scrollAnimation_)scrollAnimation_->stop();}
void FileListView::wheelEvent(QWheelEvent* e){if(!scrollWheel(this,e,smooth_,scrollAnimation_,wheelRemainder_))QListView::wheelEvent(e);}
void FileListView::hideEvent(QHideEvent* e){QListView::hideEvent(e);ThumbnailFrame frame(this,true);if(scrollAnimation_)scrollAnimation_->stop();}
void FileListView::dragEnterEvent(QDragEnterEvent* e){if(e->mimeData()->hasUrls())e->acceptProposedAction();else e->ignore();}
void FileListView::dragMoveEvent(QDragMoveEvent* e){if(e->mimeData()->hasUrls())e->acceptProposedAction();else e->ignore();}
void FileListView::dropEvent(QDropEvent* e){if(e->mimeData()->hasUrls()){emit pathsDropped(paths(e->mimeData()));e->acceptProposedAction();}}
FileTableView::FileTableView(QWidget* parent):QTableView(parent) {
    setItemDelegate(new TableThumbnailDelegate(this));
    setObjectName("fileDetails");setSelectionMode(ExtendedSelection);setSelectionBehavior(SelectRows);setDragEnabled(false);setAcceptDrops(true);setDropIndicatorShown(false);
    setVerticalScrollMode(ScrollPerPixel);setHorizontalScrollMode(ScrollPerPixel);
    setEditTriggers(NoEditTriggers);setSortingEnabled(true);setAlternatingRowColors(true);setShowGrid(false);
    connect(verticalScrollBar(),&QScrollBar::valueChanged,this,[this]{ThumbnailFrame frame(this);});
}
void FileTableView::setSmoothScrolling(bool enabled){smooth_=enabled;if(!enabled&&scrollAnimation_)scrollAnimation_->stop();}
void FileTableView::setFillThumbnails(bool fill){static_cast<TableThumbnailDelegate*>(itemDelegate())->fill=fill;viewport()->update();}
void FileTableView::wheelEvent(QWheelEvent* e){if(!scrollWheel(this,e,smooth_,scrollAnimation_,wheelRemainder_))QTableView::wheelEvent(e);}
void FileTableView::paintEvent(QPaintEvent* e){ThumbnailFrame frame(this);QTableView::paintEvent(e);}
void FileTableView::hideEvent(QHideEvent* e){QTableView::hideEvent(e);ThumbnailFrame frame(this,true);if(scrollAnimation_)scrollAnimation_->stop();}
void FileTableView::dragEnterEvent(QDragEnterEvent* e){if(e->mimeData()->hasUrls())e->acceptProposedAction();else e->ignore();}
void FileTableView::dragMoveEvent(QDragMoveEvent* e){if(e->mimeData()->hasUrls())e->acceptProposedAction();else e->ignore();}
void FileTableView::dropEvent(QDropEvent* e){if(e->mimeData()->hasUrls()){emit pathsDropped(paths(e->mimeData()));e->acceptProposedAction();}}
QSize ThumbnailDelegate::sizeHint(const QStyleOptionViewItem&,const QModelIndex&) const {return grid?QSize(edge+30,thumbnailHeight()+94):QSize(240,std::clamp(edge/2+20,52,156));}
void ThumbnailDelegate::paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const {
    painter->save();painter->setRenderHint(QPainter::Antialiasing);
    const bool selected=option.state&QStyle::State_Selected;
    const QRect tile=option.rect.adjusted(grid?5:2,grid?5:2,grid?-5:-2,grid?-5:-2);
    painter->setPen(selected?QPen(QColor("#2e8069"),2):QPen(option.palette.mid().color(),1));
    painter->setBrush(selected?option.palette.highlight().color():option.palette.base().color());painter->drawRoundedRect(tile,7,7);
    const int thumbEdge=grid?edge:std::min(tile.height()-12,edge);
    QRect thumb=grid?QRect(tile.left()+10,tile.top()+8,tile.width()-20,thumbnailHeight()):QRect(tile.left()+8,tile.top()+6,thumbEdge,thumbEdge);
    painter->fillRect(thumb,option.palette.alternateBase().color());
    const auto icon=qvariant_cast<QIcon>(index.data(Catalog::ThumbnailRole));
    if(!icon.isNull())paintThumbnail(painter,thumb,icon,fill);
    else{painter->setPen(QColor("#88958f"));painter->drawText(thumb,Qt::AlignCenter,index.siblingAtColumn(Catalog::Format).data().toString());}
    QRect text=grid?QRect(tile.left()+10,thumb.bottom()+8,tile.width()-20,20):QRect(thumb.right()+14,tile.top()+8,tile.width()-thumb.width()-32,20);
    painter->setPen(option.palette.text().color());QFont font=option.font;font.setBold(true);painter->setFont(font);
    painter->drawText(text,Qt::AlignVCenter,painter->fontMetrics().elidedText(index.data().toString(),Qt::ElideMiddle,text.width()));
    font.setBold(false);font.setPointSizeF(std::max(8.0,font.pointSizeF()-1));painter->setFont(font);painter->setPen(option.palette.color(QPalette::PlaceholderText));
    text.translate(0,21);painter->drawText(text,Qt::AlignVCenter,painter->fontMetrics().elidedText(index.data(Catalog::SummaryRole).toString(),Qt::ElideRight,text.width()));
    if(grid||tile.height()>75){text.translate(0,19);const QString state=index.siblingAtColumn(Catalog::Status).data().toString();painter->setPen(state=="Failed"?QColor("#ad3434"):state=="Done"?QColor("#26755e"):option.palette.color(QPalette::PlaceholderText));painter->drawText(text,Qt::AlignVCenter,state);}
    if(option.state&QStyle::State_HasFocus){painter->setPen(QPen(QColor("#2e8069"),1,Qt::DotLine));painter->setBrush(Qt::NoBrush);painter->drawRoundedRect(tile.adjusted(4,4,-4,-4),5,5);}
    painter->restore();
}
}
