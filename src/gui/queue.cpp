#include "queue.hpp"
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <stdexcept>
namespace jxrc::gui {
namespace {
std::filesystem::path nativePath(const QString& value) {
#ifdef _WIN32
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::u8path(value.toUtf8().constData());
#endif
}
}
int coreCount() { return std::max(1,QThread::idealThreadCount()); }
int recommendedWorkers(int cores) {
    return std::max(1,cores/4);
}
QVector<Job> makeJobs(const QStringList& inputs,const QString& directory,const Options& options,const OutputLayout& layout) {
    validate(options);QVector<Job> result;QSet<QString> outputs;
    const int target=int(options.target);
    const QStringList suffixes={"sdr.jpg","gainmap.jpg","sdr.jxl","linear.jxl","sdr.avif","pq.avif","gainmap.avif","pq.jxl"};
    static const QSet<QString> rawExtensions={"dng","cr2","cr3","crw","nef","nrw","arw","sr2","srf","raf","rw2","orf","ori","pef","ptx","rwl","srw","3fr","fff","iiq","kdc","dcr","mos","mrw","x3f","raw","erf","mef","mdc"};
    auto pathKey=[](const QString& path){return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();};
    QSet<QString> sources;for(const auto& input:inputs)sources.insert(pathKey(input));
    for(const auto& input:inputs) {
        const QFileInfo source(input);QString stem=source.completeBaseName();
        const bool raw=layout.raw_inputs.contains(input)||rawExtensions.contains(source.suffix().toLower());
        if(raw && options.mode==Mode::process) {
            if(options.raw.hdr_mode==RawHdrMode::boost)stem+="_hdr_boost";
            else if(options.raw.hdr_mode==RawHdrMode::custom)stem+="_hdr_custom";
        }
        QString folder=layout.source_folder?source.absolutePath():directory;
        if(folder.trimmed().isEmpty())throw std::invalid_argument("Choose an output folder");
        if(layout.format_subfolders)folder=QDir(folder).filePath(suffixes[target].section('.',-1));
        QString filename=stem+"_"+suffixes[target];int n=2;
        auto outputPath=[&]{return QDir(folder).absoluteFilePath(filename);};
        while(outputs.contains(pathKey(outputPath())) ||
              (layout.rename_conflicts && (QFileInfo::exists(outputPath()) || sources.contains(pathKey(outputPath())))))
            filename=stem+"_"+QString::number(n++)+"_"+suffixes[target];
        const QString output=outputPath();outputs.insert(pathKey(output));
        if(sources.contains(pathKey(output)))throw std::invalid_argument("Output would replace an input file");
        Options snapshot=options;if(layout.rename_conflicts)snapshot.overwrite=false;
        result.append({input,output,snapshot});
    }
    return result;
}
ConversionQueue::ConversionQueue(QObject* parent,Convert converter):QObject(parent),converter_(std::move(converter)) {}
ConversionQueue::~ConversionQueue() { pool_.clear();pool_.waitForDone(); }
void ConversionQueue::start(QVector<Job> jobs,int parallelism,int cores) {
    if(busy())throw std::logic_error("A conversion batch is already running");
    if(jobs.isEmpty())return;
    // Keep each source's targets together to bound prepared-pixel memory by
    // worker count. RAW has SDR, HDR, and paired HDR/SDR render families.
    QHash<QString,int> order;for(const auto& job:jobs)if(!order.contains(job.input))order.insert(job.input,order.size());
    auto family=[](Target t){return t==Target::jpeg_gainmap||t==Target::avif_gainmap?2:t==Target::jpeg||t==Target::jxl||t==Target::avif?0:1;};
    std::stable_sort(jobs.begin(),jobs.end(),[&](const Job& a,const Job& b){
        return a.input==b.input?family(a.options.target)<family(b.options.target):order[a.input]<order[b.input];
    });
    sessions_.clear();activeInputs_.clear();
    jobs_=std::move(jobs);next_=active_=finished_=succeeded_=failed_=cancelled_=0;
    limit_=std::clamp(parallelism,1,std::max(1,cores));pool_.setMaxThreadCount(limit_);
    const unsigned budget=std::max(1,cores/limit_);
    remaining_.clear();fileCancelled_.clear();fileErrors_.clear();
    for(const auto& job:jobs_)++remaining_[job.input];
    for(auto& job:jobs_){job.options.threads=budget;emit statusChanged(job.input,"Queued",job.output);}
    pump();
}
void ConversionQueue::cancelPending() {
    if(!busy())return;
    while(next_<jobs_.size()){const auto& job=jobs_[next_++];++cancelled_;++finished_;finishFile(job,{},true);}
    emit progress(finished_,jobs_.size(),active_);
    if(!busy())emit batchFinished(succeeded_,failed_,cancelled_);
}
void ConversionQueue::pump() {
    while(active_<limit_&&next_<jobs_.size()) {
        int candidate=next_;
        while(candidate<jobs_.size()&&activeInputs_.contains(jobs_[candidate].input))++candidate;
        if(candidate==jobs_.size())break;
        // Rotate rather than swap so each source retains its render-family order.
        std::rotate(jobs_.begin()+next_,jobs_.begin()+candidate,jobs_.begin()+candidate+1);
        const Job job=jobs_[next_++];++active_;activeInputs_.insert(job.input);
        std::shared_ptr<ConversionSession> session;
        if(!converter_) {
            auto& stored=sessions_[job.input];
            if(!stored)stored=std::make_shared<ConversionSession>(nativePath(job.input));
            session=stored;
        }
        emit statusChanged(job.input,"Converting",job.output);
        pool_.start([this,job,session] {
            QString error;
            try {if(session)session->convert(nativePath(job.output),job.options);else converter_(nativePath(job.input),nativePath(job.output),job.options);}catch(const std::exception& e){error=QString::fromUtf8(e.what());}catch(...){error="Unknown conversion failure";}
            error.replace("--mode process","Process pixels mode");error.replace("--raw-exposure","RAW exposure");
            QMetaObject::invokeMethod(this,[this,job,error] {
                --active_;activeInputs_.remove(job.input);++finished_;
                if(error.isEmpty())++succeeded_;else ++failed_;
                finishFile(job,error,false);
                pump();
            },Qt::QueuedConnection);
        });
    }
    emit progress(finished_,jobs_.size(),active_);
    if(!busy())emit batchFinished(succeeded_,failed_,cancelled_);
}
void ConversionQueue::finishFile(const Job& job,const QString& error,bool cancelled) {
    if(!error.isEmpty())fileErrors_[job.input]+=job.output+": "+error+"\n";
    if(cancelled)++fileCancelled_[job.input];
    const int left=--remaining_[job.input];
    // With multiple outputs, a later success must not hide an earlier failure.
    if(left>0){emit statusChanged(job.input,QString("In progress · %1 outputs remaining").arg(left),fileErrors_.value(job.input));return;}
    sessions_.remove(job.input);
    const auto errors=fileErrors_.value(job.input);
    if(!errors.isEmpty())emit statusChanged(job.input,"Failed",errors);
    else if(fileCancelled_.value(job.input))emit statusChanged(job.input,"Cancelled",QString("%1 output(s) not started").arg(fileCancelled_.value(job.input)));
    else emit statusChanged(job.input,"Done",job.output);
}
}
