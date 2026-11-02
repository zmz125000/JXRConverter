#include "catalog.hpp"
#include "jxrc/converter.hpp"
#include <exiv2/exiv2.hpp>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QSemaphoreReleaser>
#include <stdexcept>
#include <QIcon>
#include <QPixmap>
#include <QRegularExpression>
#include <QRunnable>
#include <algorithm>
#include <cmath>
#include <QLocale>

namespace jxrc::gui {
namespace {
QString key(const QString& path) {
    // Imports are already canonical. Do not stat SMB files again for model keys.
    QString canonical = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef _WIN32
    canonical = canonical.toCaseFolded();
#endif
    return canonical;
}
QString exifText(const Exiv2::ExifData& data, const char* name) {
    auto it = data.findKey(Exiv2::ExifKey(name));
    return it == data.end() ? QString{} : QString::fromStdString(it->toString());
}
double exifNumber(const Exiv2::ExifData& data, const char* name) {
    auto it = data.findKey(Exiv2::ExifKey(name));
    if (it == data.end() || !it->count()) return 0;
    try { return it->toFloat(); } catch (...) { return 0; }
}
QString dimensions(const Record& r) {
    return r.width > 0 ? QString("%1 × %2").arg(r.width).arg(r.height) : QString{};
}
}
bool supportedFile(const QString& path) {
    static const QSet<QString> extensions = {"png","webp","tif","tiff","bmp","gif","jpg","jpeg","jpe","avif","jxl","jxr","wdp","hdp","dng","cr2","cr3","crw","nef","nrw","arw","sr2","srf","raf","rw2","orf","ori","pef","ptx","rwl","srw","3fr","fff","iiq","kdc","dcr","mos","mrw","x3f","raw","erf","mef","mdc"};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}
namespace {
void checkCancelled(const std::function<bool()>& cancelled) {
    if(cancelled&&cancelled())throw std::runtime_error("Image read cancelled");
}
struct SourceBuffer {
    std::vector<uint8_t> bytes;
    QDateTime created, modified;
};
SourceBuffer readSource(const QString& path,const std::function<bool()>& cancelled,QSemaphore* sourceReadSlots,bool wantMetadata=true) {
    // Bound open source handles without serializing all input on fast storage.
    // Release the read slot before decoding so all six workers can decode.
    if(sourceReadSlots)sourceReadSlots->acquire();
    QSemaphoreReleaser releaseReadSlot(sourceReadSlots);
    checkCancelled(cancelled);
    QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error(file.errorString().toStdString());
    const qint64 length=file.size();
    if(length<=0||length>512LL*1024*1024)throw std::runtime_error("Input must be 1 byte to 512 MiB");
    SourceBuffer source;auto& bytes=source.bytes;bytes.resize(static_cast<size_t>(length));
    if(wantMetadata){source.modified=file.fileTime(QFileDevice::FileModificationTime).toLocalTime();source.created=file.fileTime(QFileDevice::FileBirthTime).toLocalTime();}
    for(size_t offset=0;offset<bytes.size();) {
        checkCancelled(cancelled);
        const auto count=file.read(reinterpret_cast<char*>(bytes.data()+offset),qint64(std::min<size_t>(256*1024,bytes.size()-offset)));
        if(count<=0)throw std::runtime_error("Cannot read image");
        offset+=size_t(count);
    }
    file.close();checkCancelled(cancelled);return source;
}
Record metadataFromBytes(const QString& path,const SourceBuffer& source,const std::function<bool()>& cancelled,const ImageInfo* sourceInfo=nullptr) {
    const auto& bytes=source.bytes;
    Record r;if(cancelled&&cancelled())return r; r.path = path; QFileInfo file(path);
    r.name = file.fileName(); r.bytes = qint64(bytes.size()); r.modified = source.modified; r.created = source.created;
    r.format = file.suffix().toUpper();
    try {
        if(cancelled&&cancelled())return r;
        auto image = Exiv2::ImageFactory::open(bytes.data(),bytes.size());
        if (image.get() != nullptr) {
            if(cancelled&&cancelled())return r;
            image->readMetadata(); r.width = image->pixelWidth(); r.height = image->pixelHeight();
            const auto& exif = image->exifData();
            for (const auto& tag : exif) r.exif.insert(QString::fromStdString(tag.key()), QString::fromStdString(tag.toString()));
            r.camera = (exifText(exif,"Exif.Image.Make") + " " + exifText(exif,"Exif.Image.Model")).simplified();
            r.lens = exifText(exif,"Exif.Photo.LensModel");
            r.iso = int(exifNumber(exif,"Exif.Photo.ISOSpeedRatings"));
            if (!r.iso) r.iso = int(exifNumber(exif,"Exif.Photo.ISOSpeed"));
            r.aperture = exifNumber(exif,"Exif.Photo.FNumber"); r.shutter = exifNumber(exif,"Exif.Photo.ExposureTime");
            r.focal = exifNumber(exif,"Exif.Photo.FocalLength");
            for(const auto* tag:{"Exif.Photo.DateTimeOriginal","Exif.Photo.DateTimeDigitized","Exif.Image.DateTime"}) {
                r.captured=QDateTime::fromString(exifText(exif,tag).left(19),"yyyy:MM:dd HH:mm:ss");
                if(r.captured.isValid())break;
            }
        }
    } catch (const std::exception& e) { r.metadataWarning = QString::fromUtf8(e.what()); }
    try {
        if(cancelled&&cancelled())return r;
        const auto info = sourceInfo?*sourceInfo:inspect_bytes(bytes);
        if(!info.width||!info.height)throw std::runtime_error("Image header information unavailable");
        r.width = info.width; r.height = info.height; r.bits = info.bits;
        r.format = QString::fromStdString(info.format); r.raw = info.raw; r.gainmap = info.gainmap; r.hdr = info.hdr;
        r.transfer = QString::fromStdString(info.transfer); r.primaries = QString::fromStdString(info.primaries);
    } catch (const std::exception& e) {
        if (r.metadataWarning.isEmpty()) r.metadataWarning = QString::fromUtf8(e.what());
        r.raw = !QSet<QString>{"PNG","WEBP","TIF","TIFF","BMP","GIF","JPG","JPEG","JPE","AVIF","JXL","JXR","WDP","HDP"}.contains(r.format);
    }
    r.inspected = true; return r;
}
}
Record readMetadata(const QString& path,const std::function<bool()>& cancelled,QSemaphore* sourceReadSlots) {
    try {return metadataFromBytes(path,readSource(path,cancelled,sourceReadSlots),cancelled);}
    catch(const std::exception& e) {
        Record result;if(cancelled&&cancelled())return result;
        result.path=path;result.name=QFileInfo(path).fileName();result.format=QFileInfo(path).suffix().toUpper();
        result.inspected=true;result.metadataWarning=QString::fromUtf8(e.what());return result;
    }
}
QImage readThumbnail(const QString& path,int edge,const std::function<bool()>& cancelled,Record* metadata,QSemaphore* sourceReadSlots) {
    const auto source=readSource(path,cancelled,sourceReadSlots,metadata!=nullptr);
    Preview image;ImageInfo info;
    try {image=preview_bytes(source.bytes,std::min(edge,768),metadata?&info:nullptr,cancelled);}
    catch(...) {
        if(metadata&&(!cancelled||!cancelled()))*metadata=metadataFromBytes(path,source,cancelled);
        throw;
    }
    if(metadata)*metadata=metadataFromBytes(path,source,cancelled,&info);
    return QImage(image.rgba.data(),int(image.width),int(image.height),int(image.width)*4,QImage::Format_RGBA8888).copy();
}
Catalog::Catalog(QObject* parent, int thumbnailCacheKiB) : QAbstractTableModel(parent) {
    cache_.setMaxCost(std::max(1,thumbnailCacheKiB));
    scans_.setMaxThreadCount(1);workers_.setMaxThreadCount(ThumbnailBudget); // Parallel decoding; sourceReadSlots_ bounds concurrent file reads.
}
Catalog::~Catalog() {
    epoch_->store(false);if(metadataToken_)metadataToken_->store(false);
    for(const auto& token:thumbnailJobs_)token->store(false);
    scans_.clear();workers_.clear();scans_.waitForDone();workers_.waitForDone();
    delete metadataTask_;qDeleteAll(thumbnailTasks_);
}
int Catalog::rowCount(const QModelIndex& parent) const { return parent.isValid()?0:records_.size(); }
int Catalog::columnCount(const QModelIndex& parent) const { return parent.isValid()?0:ColumnCount; }
QVariant Catalog::headerData(int section, Qt::Orientation orientation, int role) const {
    static const QStringList headers = {"Name","Format","Dimensions","File size","Image date","Camera","Lens","ISO","Aperture","Shutter","Focal length","Transfer","Primaries","Bit depth","Status","Created","Modified","EXIF date"};
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < headers.size()) return headers[section];
    return {};
}
QVariant Catalog::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= records_.size()) return {};
    const auto& r = records_[index.row()];
    if (role == PathRole) return r.path;
    if (role == RawRole) return r.raw;
    if (role == SummaryRole) return QStringList{dimensions(r), r.format, r.transfer}.filter(QRegularExpression(".+")).join(" · ");
    if (role == Qt::ToolTipRole) return r.path + "\n" + dimensions(r) + "\n" + r.camera + "\n" + r.imageDate().toString("yyyy-MM-dd HH:mm:ss") + "\n" + r.status + (r.error.isEmpty()?QString{}:"\n"+r.error) + (r.metadataWarning.isEmpty()?QString{}:"\nMetadata: "+r.metadataWarning);
    if ((role == Qt::DecorationRole || role == ThumbnailRole) && index.column() == Name) {
        if(role==ThumbnailRole&&thumbnailFrame_&&!visibleThumbnails_.contains(r.path))
            const_cast<Catalog*>(this)->visibleThumbnails_.append(r.path);
        if (auto* image = cache_.object(r.path)) return QIcon(QPixmap::fromImage(*image));
        return {};
    }
    if (role != Qt::DisplayRole) return {};
    switch(index.column()) {
    case Name:return r.name; case Format:return r.format; case Dimensions:return dimensions(r);
    case Size:return QLocale().formattedDataSize(r.bytes); case Captured:return r.imageDate().toString("yyyy-MM-dd HH:mm:ss");
    case Created:return r.created.toString("yyyy-MM-dd HH:mm:ss");case Modified:return r.modified.toString("yyyy-MM-dd HH:mm:ss");case ExifDate:return r.captured.toString("yyyy-MM-dd HH:mm:ss");
    case Camera:return r.camera; case Lens:return r.lens; case Iso:return r.iso?QString::number(r.iso):QString{};
    case Aperture:return r.aperture>0?QString("f/%1").arg(r.aperture,0,'f',1):QString{};
    case Shutter:return r.shutter>0?(r.shutter<1?QString("1/%1 s").arg(std::round(1/r.shutter)):QString("%1 s").arg(r.shutter)):QString{};
    case Focal:return r.focal>0?QString("%1 mm").arg(r.focal):QString{};
    case Transfer:return r.transfer; case Primaries:return r.primaries; case Bits:return r.bits?QString::number(r.bits):QString{};
    case Status:return r.status; default:return {};
    }
}
QStringList Catalog::paths() const { QStringList out; for(const auto& r:records_)out<<r.path; return out; }
int Catalog::find(const QString& path) const { for(int i=0;i<records_.size();++i)if(records_[i].path==path)return i;return -1; }
void Catalog::addPaths(const QStringList& paths, bool recursive) {
    const auto epoch=epoch_;
    ++scanning_;emit scanStateChanged(true);
    scans_.start([this,paths,recursive,epoch] {
        QStringList batch;
        auto send=[&] {
            if(batch.isEmpty()||!epoch->load())return;
            const auto ready=batch;batch.clear();
            QMetaObject::invokeMethod(this,[this,ready,epoch]{if(epoch->load())insertPaths(ready);},Qt::QueuedConnection);
        };
        for(const auto& path:paths) {
            if(!epoch->load())break;
            QFileInfo info(path);
            if(info.isDir()) {
                // Canonicalize the root once. NoSymLinks makes per-file
                // canonicalFilePath()/remote stat calls unnecessary.
                const auto root=info.canonicalFilePath();if(root.isEmpty())continue;
                QDirIterator it(root,QDir::Files|QDir::Readable|QDir::NoSymLinks,recursive?QDirIterator::Subdirectories:QDirIterator::NoIteratorFlags);
                while(epoch->load()&&it.hasNext()) {
                    const auto file=it.next();if(!epoch->load())break;
                    if(supportedFile(file))batch<<file;
                    if(batch.size()>=64)send();
                }
            } else if(info.isFile()&&supportedFile(path))batch<<info.canonicalFilePath();
            else QMetaObject::invokeMethod(this,[this,path,epoch]{if(epoch->load())emit scanError("Skipped unsupported or missing file: "+path);},Qt::QueuedConnection);
        }
        send();QMetaObject::invokeMethod(this,[this,epoch]{if(epoch->load()){--scanning_;emit scanStateChanged(scanning_>0);}},Qt::QueuedConnection);
    });
}
void Catalog::insertPaths(const QStringList& paths) {
    const auto epoch=epoch_;
    for(const auto& path:paths) {
        if(!epoch->load())break;
        const auto id=key(path);if(id.isEmpty()||known_.contains(id))continue;
        known_.insert(id);Record r;r.path=path;r.name=QFileInfo(path).fileName();r.format=QFileInfo(path).suffix().toUpper();
        if(r.format=="JPG"||r.format=="JPE")r.format="JPEG";
        r.raw=!QSet<QString>{"PNG","WEBP","TIF","TIFF","BMP","GIF","JPEG","AVIF","JXL","JXR","WDP","HDP"}.contains(r.format);
        metadataQueue_.append(path);
        beginInsertRows({},records_.size(),records_.size());records_.append(r);endInsertRows();
    }
    // In browsing mode let the first paint schedule a combined thumbnail and
    // metadata read, rather than inspecting files before their thumbnails.
    if(backgroundMetadata_)pumpMetadata();
}
bool Catalog::metadataWanted(const QString& path) const {
    if(backgroundMetadata_||visibleThumbnails_.contains(path))return true;
    // The first two images are used by automatic grid aspect detection.
    if(aspectSampling_)for(int row=0;row<std::min(2,int(records_.size()));++row)if(records_[row].path==path)return true;
    return false;
}
void Catalog::setAspectSampling(bool enabled) {aspectSampling_=enabled;setBackgroundMetadata(backgroundMetadata_);}
void Catalog::setBackgroundMetadata(bool enabled) {
    backgroundMetadata_=enabled;
    if(metadataTask_&&!metadataWanted(metadataPath_))cancelMetadata();
    pumpMetadata();
}
void Catalog::cancelMetadata() {
    if(metadataToken_)metadataToken_->store(false);
    if(metadataTask_&&workers_.tryTake(metadataTask_)) {
        delete metadataTask_;metadataTask_=nullptr;metadataToken_.reset();metadataPath_.clear();
    }
}
void Catalog::pumpMetadata() {
    if(metadataTask_||!epoch_->load())return;
    QString path;
    auto standalone=[this](int row) {
        // A visible thumbnail supplies metadata from its source buffer. Only
        // failed thumbnails need a separate metadata fallback.
        return row>=0&&!records_[row].inspected&&(!visibleThumbnails_.contains(records_[row].path)||records_[row].thumbnailFailed);
    };
    for(const auto& candidate:visibleThumbnails_) {
        const int row=find(candidate);if(standalone(row)){path=candidate;break;}
    }
    if(path.isEmpty()&&aspectSampling_)for(int row=0;row<std::min(2,int(records_.size()));++row)if(standalone(row)){path=records_[row].path;break;}
    if(path.isEmpty()&&backgroundMetadata_)while(!metadataQueue_.isEmpty()) {
        const auto candidate=metadataQueue_.takeFirst();const int row=find(candidate);
        if(standalone(row)){path=candidate;break;}
    }
    if(path.isEmpty())return;
    const auto epoch=epoch_;auto token=std::make_shared<std::atomic_bool>(true);
    metadataToken_=token;metadataPath_=path;
    metadataTask_=QRunnable::create([this,path,epoch,token] {
        const auto cancelled=[epoch,token]{return !epoch->load()||!token->load();};
        Record result;if(!cancelled())result=readMetadata(path,cancelled,&sourceReadSlots_);
        QMetaObject::invokeMethod(this,[this,path,epoch,token,result=std::move(result)]() mutable {
            delete metadataTask_;metadataTask_=nullptr;metadataToken_.reset();metadataPath_.clear();
            const int row=find(path);
            if(epoch->load()&&token->load()&&row>=0) {
                auto& old=records_[row];result.status=old.status;result.error=old.error;result.thumbnailPending=old.thumbnailPending;result.thumbnailFailed=old.thumbnailFailed;
                old=std::move(result);emit dataChanged(index(row,0),index(row,ColumnCount-1));
            }
            pumpMetadata();
        },Qt::QueuedConnection);
    });
    metadataTask_->setAutoDelete(false);workers_.start(metadataTask_,0);
}
void Catalog::clear() {
    epoch_->store(false);epoch_=std::make_shared<std::atomic_bool>(true);
    scans_.clear();scanning_=0;metadataQueue_.clear();visibleThumbnails_.clear();cancelMetadata();
    for(const auto& path:thumbnailJobs_.keys())cancelThumbnail(path);
    beginResetModel();records_.clear();known_.clear();cache_.clear();endResetModel();
    emit scanStateChanged(false);
}
void Catalog::beginThumbnailFrame(const std::function<bool(const QModelIndex&)>& visible) {
    if(thumbnailFrameDepth_++==0){thumbnailVisible_=visible;thumbnailFrame_=true;}
}
void Catalog::endThumbnailFrame() {
    if(thumbnailFrameDepth_>1){--thumbnailFrameDepth_;return;}
    // Painting can trigger layout probes too. Check geometry after layout has
    // finished, with collection disabled to avoid recursive decoration probes.
    thumbnailFrame_=false;
    for(int i=visibleThumbnails_.size()-1;i>=0;--i) {
        const auto path=visibleThumbnails_[i];const int row=find(path);
        if(row<0||!thumbnailVisible_(index(row,Name))) {
            cancelThumbnail(path);
            visibleThumbnails_.removeAt(i);
        }
    }
    thumbnailVisible_={};thumbnailFrameDepth_=0;
    if(metadataTask_&&!metadataWanted(metadataPath_))cancelMetadata();
    fitThumbnailCache();pumpThumbnails();pumpMetadata();
}
void Catalog::cancelThumbnail(const QString& path) {
    if(auto token=thumbnailJobs_.value(path))token->store(false);
    auto* task=thumbnailTasks_.value(path);
    if(task&&workers_.tryTake(task)) {
        delete task;thumbnailTasks_.remove(path);thumbnailJobs_.remove(path);
        const int row=find(path);if(row>=0)records_[row].thumbnailPending=false;
    }
}
void Catalog::fitThumbnailCache() {
    // All visible images must coexist in the cache. Otherwise completion of
    // later cells evicts early ones, and pumpThumbnails re-decodes them forever.
    // Include six in-flight images and rounded-up KiB costs in the budget.
    const qint64 count=std::max<qint64>(1,visibleThumbnails_.size()+ThumbnailBudget);
    const qint64 bytesPerImage=qint64(cache_.maxCost())*1024/count;
    thumbnailEdge_=std::clamp(int(std::sqrt(std::max<qint64>(4,bytesPerImage-1023)/4.0)),1,384);
    bool shrink=false;
    for(const auto& path:visibleThumbnails_)if(auto* image=cache_.object(path))
        shrink|=std::max(image->width(),image->height())>thumbnailEdge_;
    if(!shrink)return; // The lookups above also refresh visible LRU entries.
    QMap<QString,QImage> visible;
    for(const auto& path:visibleThumbnails_)if(auto* image=cache_.take(path)) {
        auto ready=*image;delete image;
        if(std::max(ready.width(),ready.height())>thumbnailEdge_)ready=ready.scaled(thumbnailEdge_,thumbnailEdge_,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        visible.insert(path,ready);
    }
    // Reinsert visible images last, protecting them from offscreen LRU entries.
    for(auto it=visible.cbegin();it!=visible.cend();++it)
        cache_.insert(it.key(),new QImage(it.value()),std::max<qint64>(1,(it.value().sizeInBytes()+1023)/1024));
}
void Catalog::pumpThumbnails() {
    // Six submitted thumbnail jobs total, with parallel decoding and up to two concurrent reads. Queued
    // jobs carry cancellation tokens so scrolling never decodes stale cells.
    for(const auto& path:visibleThumbnails_) {
        if(thumbnailJobs_.size()>=ThumbnailBudget)break;
        const int row=find(path);if(row<0)continue;
        auto& record=records_[row];
        if(cache_.contains(path)||record.thumbnailFailed||thumbnailJobs_.contains(path))continue;
        record.thumbnailPending=true;
        auto token=std::make_shared<std::atomic_bool>(true);thumbnailJobs_.insert(path,token);
        // Capture only the path and cancellation token. Opening the file and
        // constructing its decoder happen inside readThumbnail, after this
        // runnable starts and checks cancellation.
        const int edge=thumbnailEdge_;const bool collectMetadata=!record.inspected;
        auto* task=QRunnable::create([this,path,token,edge,collectMetadata] {
            QImage image;QString error;Record metadata;
            if(token->load()) {
                try {image=readThumbnail(path,edge,[token]{return !token->load();},collectMetadata?&metadata:nullptr,&sourceReadSlots_);}catch(const std::exception& e){error=QString::fromUtf8(e.what());}
            }
            QMetaObject::invokeMethod(this,[this,path,token,image,error,metadata=std::move(metadata)]() mutable {
                thumbnailJobs_.remove(path);
                delete thumbnailTasks_.take(path);
                const int row=find(path);
                if(row>=0) {
                    auto& r=records_[row];r.thumbnailPending=false;
                    if(token->load()) {
                        const bool updated=metadata.inspected&&!r.inspected;
                        if(updated){metadata.status=r.status;metadata.error=r.error;r=std::move(metadata);}
                        r.thumbnailFailed=image.isNull();
                        if(!image.isNull()) {
                            auto ready=image;
                            if(std::max(ready.width(),ready.height())>thumbnailEdge_)ready=ready.scaled(thumbnailEdge_,thumbnailEdge_,Qt::KeepAspectRatio,Qt::SmoothTransformation);
                            cache_.insert(path,new QImage(ready),std::max<qint64>(1,(ready.sizeInBytes()+1023)/1024));
                        }
                        else if(r.metadataWarning.isEmpty())r.metadataWarning="Thumbnail: "+error;
                        if(updated)emit dataChanged(index(row,0),index(row,ColumnCount-1));
                        else emit dataChanged(index(row,0),index(row,0),{Qt::DecorationRole,Qt::ToolTipRole});
                    }
                }
                pumpThumbnails();pumpMetadata();
            },Qt::QueuedConnection);
        });
        // Own the runnable until completion, avoiding tryTake's documented
        // pointer-reuse race with automatically deleted runnables.
        task->setAutoDelete(false);thumbnailTasks_.insert(path,task);workers_.start(task,1);
    }
}
void Catalog::removePaths(const QStringList& paths) {
    const QSet<QString> removal(paths.begin(),paths.end());
    if(!records_.isEmpty()&&std::all_of(records_.cbegin(),records_.cend(),[&](const Record& r){return removal.contains(r.path);})) {clear();return;}
    if(removal.contains(metadataPath_))cancelMetadata();
    for(int i=records_.size()-1;i>=0;--i)if(removal.contains(records_[i].path)) {
        const auto path=records_[i].path;
        visibleThumbnails_.removeAll(path);cancelThumbnail(path);
        beginRemoveRows({},i,i);records_.removeAt(i);known_.remove(key(path));cache_.remove(path);endRemoveRows();
    }
    pumpMetadata();
}
void Catalog::setStatus(const QString& path,const QString& status,const QString& error) {
    const int row=find(path);if(row<0)return;records_[row].status=status;records_[row].error=error;
    emit dataChanged(index(row,0),index(row,ColumnCount-1),{Qt::DisplayRole,Qt::ToolTipRole});
}
CatalogFilter::CatalogFilter(QObject* parent):QSortFilterProxyModel(parent) {setDynamicSortFilter(true);setSortCaseSensitivity(Qt::CaseInsensitive);}
void CatalogFilter::setFilter(const Filter& filter) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    filter_=filter;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    filter_=filter;
    invalidateFilter();
#endif
}
QString catalogFieldKey(int column) {
    static const QStringList keys={"name","format","dimensions","size","date","camera","lens","iso","aperture","shutter","focal","transfer","primaries","bits","status","created","modified","exifdate"};
    return keys.value(column);
}
bool CatalogFilter::filterAcceptsRow(int row,const QModelIndex&) const {
    const auto& r=static_cast<Catalog*>(sourceModel())->record(row);const auto& f=filter_;
    if(f.restrictPaths&&!f.paths.contains(r.path))return false;
    if(f.status==3&&!r.status.startsWith("Done")&&r.status!="Skipped")return false;
    if(f.status==4&&r.status!="Failed")return false;
    const QString haystack=r.path+" "+r.format+" "+dimensions(r)+" "+r.camera+" "+r.lens+" "+r.transfer+" "+r.primaries+" "+r.status+" "+QString::number(r.bits)+" bit "+r.exif.keys().join(' ')+" "+r.exif.values().join(' ');
    static const QRegularExpression terms(R"((?:(name|format|dimensions|size|date|camera|lens|iso|aperture|shutter|focal|transfer|primaries|bits|status|created|modified|exifdate):)?("(?:\\.|[^"\\])*"|[^\s]+))");
    auto matches=terms.globalMatch(f.search);
    while(matches.hasNext()) {
        const auto match=matches.next();const auto field=match.captured(1);auto value=match.captured(2);
        if(value.startsWith('"')&&value.endsWith('"')) {
            const auto quoted=value.mid(1,value.size()-2);value.clear();
            for(int i=0;i<quoted.size();++i){if(quoted[i]=='\\'&&i+1<quoted.size()&&(quoted[i+1]=='\\'||quoted[i+1]=='"'))++i;value+=quoted[i];}
        }
        if(!field.isEmpty()) {
            int column=0;while(column<Catalog::ColumnCount&&catalogFieldKey(column)!=field)++column;
            if(column==Catalog::ColumnCount||sourceModel()->index(row,column).data().toString().compare(value,Qt::CaseInsensitive)!=0)return false;
        } else if(!haystack.contains(value,Qt::CaseInsensitive))return false;
    }
    if(!f.format.isEmpty()) {
        bool matched=false;
        const auto formats=f.format.toLower().split(QRegularExpression("[,;\\s]+"),Qt::SkipEmptyParts);
        for(auto format:formats) {
            if(format.startsWith('.'))format.remove(0,1);
            if(format=="jpg"||format=="jpe")format="jpeg";
            if(format=="wdp"||format=="hdp")format="jxr";
            if((format=="raw"&&r.raw)||r.format.compare(format,Qt::CaseInsensitive)==0||QFileInfo(r.path).suffix().compare(format,Qt::CaseInsensitive)==0){matched=true;break;}
        }
        if(!matched)return false;
    }
    if(f.range=="RAW"&&!r.raw)return false;
    const bool hdr=r.hdr;
    if(f.range=="HDR"&&(!hdr||r.raw))return false;
    if(f.range=="SDR"&&(hdr||r.raw||r.transfer.isEmpty()))return false;
    if(r.width<f.minWidth||r.height<f.minHeight||std::max(r.width,r.height)<f.minMaxEdge||double(r.width)*r.height/1e6<f.minMegapixels)return false;
    const auto imageDate=r.imageDate().date();
    if(f.from.isValid()&&(!imageDate.isValid()||imageDate<f.from))return false;
    if(f.to.isValid()&&(!imageDate.isValid()||imageDate>f.to))return false;
    if(f.today&&imageDate!=QDate::currentDate())return false;
    if(f.literalText)return haystack.contains(f.text.trimmed(),Qt::CaseInsensitive);
    for(const auto& word:f.text.split(' ',Qt::SkipEmptyParts))if(!haystack.contains(word,Qt::CaseInsensitive))return false;
    return true;
}
bool CatalogFilter::lessThan(const QModelIndex& left,const QModelIndex& right) const {
    const auto* model=static_cast<Catalog*>(sourceModel());const auto& a=model->record(left.row());const auto& b=model->record(right.row());
    auto numeric=[&](double x,double y){return x==y?a.path<b.path:x<y;};
    switch(left.column()) {
    case Catalog::Dimensions:return numeric(double(a.width)*a.height,double(b.width)*b.height);
    case Catalog::Size:return numeric(a.bytes,b.bytes);case Catalog::Captured:return a.imageDate()==b.imageDate()?a.path<b.path:a.imageDate()<b.imageDate();
    case Catalog::Created:return a.created==b.created?a.path<b.path:a.created<b.created;
    case Catalog::Modified:return a.modified==b.modified?a.path<b.path:a.modified<b.modified;
    case Catalog::ExifDate:return a.captured==b.captured?a.path<b.path:a.captured<b.captured;
    case Catalog::Iso:return numeric(a.iso,b.iso);case Catalog::Aperture:return numeric(a.aperture,b.aperture);case Catalog::Shutter:return numeric(a.shutter,b.shutter);
    case Catalog::Focal:return numeric(a.focal,b.focal);case Catalog::Bits:return numeric(a.bits,b.bits);default: {
        const int c=QString::localeAwareCompare(sourceModel()->data(left).toString().toCaseFolded(),sourceModel()->data(right).toString().toCaseFolded());return c==0?a.path<b.path:c<0;
    }}
}
}
