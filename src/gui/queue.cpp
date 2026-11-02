#include "queue.hpp"
#include <QDir>
#include <QDate>
#include <QLocale>
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
QString jobKey(const Job& job) {return QString::number(job.batch)+QChar(0)+job.input+QChar(0)+job.output;}
void orderJobs(QVector<Job>& jobs) {
    // Keep each source's targets together to bound prepared-pixel memory by
    // worker count. RAW has SDR, HDR, and paired HDR/SDR render families.
    QHash<QString,int> order;for(const auto& job:jobs)if(!order.contains(job.input))order.insert(job.input,order.size());
    auto family=[](Target t){return t==Target::jpeg_gainmap||t==Target::avif_gainmap?2:t==Target::jpeg||t==Target::jxl||t==Target::avif?0:1;};
    std::stable_sort(jobs.begin(),jobs.end(),[&](const Job& a,const Job& b){
        return a.input==b.input?family(a.options.target)<family(b.options.target):order[a.input]<order[b.input];
    });
}

}
QString outputDateFolder() {
    QString name=QLocale().toString(QDate::currentDate(),QLocale::ShortFormat);
    // Locale date separators such as '/' cannot be used in a single folder name.
    for(auto& ch:name)if(ch.unicode()<32||QStringLiteral("/\\:*?\"<>|").contains(ch))ch=QLatin1Char('-');
    while(name.endsWith(' ')||name.endsWith('.'))name.chop(1);
    return name;
}
int coreCount() { return std::max(1,QThread::idealThreadCount()); }
int recommendedWorkers(int cores) {
    return std::max(1,cores/4);
}
QVector<Job> makeJobs(const QStringList& inputs,const QString& directory,const Options& options,const OutputLayout& layout) {
    validate(options);QVector<Job> result;QSet<QString> outputs;
    const int target=int(options.target);
    const QStringList suffixes={"sdr.jpg","gainmap.jpg","sdr.jxl","linear.jxl","sdr.avif","pq.avif","gainmap.avif","pq.jxl","scrgb.jxl"};
    const QString dateFolder=layout.date_subfolders?outputDateFolder():QString{};
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
        if(!layout.job_subfolder.isEmpty()) {
            if(layout.job_subfolder=="."||layout.job_subfolder==".."||layout.job_subfolder.contains('/')||layout.job_subfolder.contains('\\')||layout.job_subfolder.contains(':'))throw std::invalid_argument("Invalid job folder name");
            folder=QDir(folder).filePath(layout.job_subfolder);
        } else if(layout.date_subfolders)folder=QDir(folder).filePath(dateFolder);
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
    orderJobs(jobs);
    sessions_.clear();activeInputs_.clear();activeOutputs_.clear();activeBatches_.clear();batchAliases_.clear();
    jobs_=std::move(jobs);next_=active_=finished_=succeeded_=failed_=cancelled_=skipped_=0;
    limit_=std::clamp(parallelism,1,std::max(1,cores));pool_.setMaxThreadCount(limit_);
    threadBudget_=std::max(1,cores/limit_);
    remaining_.clear();fileCancelled_.clear();fileSkipped_.clear();fileSucceeded_.clear();fileErrors_.clear();
    for(const auto& job:jobs_)++remaining_[job.input];
    for(auto& job:jobs_){job.options.threads=threadBudget_;emit statusChanged(job.input,"Queued",job.output);}
    pump();
}
void ConversionQueue::enqueue(QVector<Job> jobs,int parallelism,int cores) {
    if(!busy()){start(std::move(jobs),parallelism,cores);return;}
    orderJobs(jobs);
    for(auto& job:jobs){job.options.threads=threadBudget_;if(!remaining_.value(job.input)){fileErrors_.remove(job.input);fileCancelled_.remove(job.input);fileSkipped_.remove(job.input);fileSucceeded_.remove(job.input);}++remaining_[job.input];}
    jobs_+=jobs;
    for(const auto& job:jobs)if(!activeInputs_.contains(job.input))emit statusChanged(job.input,"Queued",job.output);
    pump();
}
qint64 ConversionQueue::runningBatch() const {
    qint64 current=-1;for(auto it=activeBatches_.cbegin();it!=activeBatches_.cend();++it){const auto batch=it.value();if(batch==0||batchAliases_.value(jobKey({it.key(),activeOutputs_.value(it.key()),{},batch})).contains(0))return 0;current=std::max(current,batch);}
    return current>=0?current:next_<jobs_.size()?jobs_[next_].batch:-1;
}
bool ConversionQueue::hasRunningBatch(qint64 batch) const {
    for(auto it=activeBatches_.cbegin();it!=activeBatches_.cend();++it)
        if(it.value()==batch||batchAliases_.value(jobKey({it.key(),activeOutputs_.value(it.key()),{},it.value()})).contains(batch))return true;
    return false;
}
bool ConversionQueue::hasPendingBatch(qint64 batch) const {return std::any_of(jobs_.cbegin()+next_,jobs_.cend(),[this,batch](const Job& job){return job.batch==batch||batchAliases_.value(jobKey(job)).contains(batch);});}
void ConversionQueue::cancelBatch(qint64 batch) {cancelPendingMatching(nullptr,batch);}
void ConversionQueue::enqueueNext(QVector<Job> jobs,int parallelism,int cores) {
    if(!busy()){start(std::move(jobs),parallelism,cores);return;}
    if(jobs.isEmpty())return;
    orderJobs(jobs);
    QSet<QString> inputs;for(const auto& job:jobs)inputs.insert(job.input);
    QVector<Job> priority,rest;
    // Promote existing outputs rather than submitting them twice. Running
    // outputs finish in place; new targets reuse the same source session.
    for(int i=next_;i<jobs_.size();++i)
        (inputs.contains(jobs_[i].input)?priority:rest).append(jobs_[i]);
    QVector<Job> added;
    for(auto& job:jobs) {
        if(activeOutputs_.value(job.input)==job.output){batchAliases_[jobKey({job.input,job.output,{},activeBatches_.value(job.input)})].insert(job.batch);continue;}
        const auto pending=std::find_if(priority.cbegin(),priority.cend(),[&](const Job& other){return other.input==job.input&&other.output==job.output;});
        if(pending!=priority.cend()){batchAliases_[jobKey(*pending)].insert(job.batch);continue;}
        job.options.threads=threadBudget_;
        if(!remaining_.value(job.input)){fileErrors_.remove(job.input);fileCancelled_.remove(job.input);fileSkipped_.remove(job.input);fileSucceeded_.remove(job.input);}
        priority.append(job);added.append(job);++remaining_[job.input];
    }
    orderJobs(priority);
    jobs_.resize(next_);jobs_+=priority;jobs_+=rest;
    for(const auto& job:added)if(!activeInputs_.contains(job.input))emit statusChanged(job.input,"Queued",job.output);
    pump();
}
bool ConversionQueue::hasPendingFor(const QStringList& inputs) const {
    const QSet<QString> selected(inputs.cbegin(),inputs.cend());
    return std::any_of(jobs_.cbegin()+next_,jobs_.cend(),[&](const Job& job){return selected.contains(job.input);});
}
void ConversionQueue::cancelPending() { cancelPendingMatching(nullptr); }
void ConversionQueue::cancelPendingFor(const QStringList& inputs) {
    const QSet<QString> selected(inputs.cbegin(),inputs.cend());
    cancelPendingMatching(&selected);
}
void ConversionQueue::cancelPendingMatching(const QSet<QString>* inputs,qint64 batch) {
    if(!busy())return;
    // Keep total job count and the order of unselected pending outputs intact.
    const auto end=std::stable_partition(jobs_.begin()+next_,jobs_.end(),[&](const Job& job){return (batch<0||job.batch==batch||batchAliases_.value(jobKey(job)).contains(batch))&&(!inputs||inputs->contains(job.input));});
    const int stop=int(end-jobs_.begin());
    if(stop==next_)return;
    while(next_<stop){const auto& job=jobs_[next_++];++cancelled_;++finished_;finishFile(job,{},true);}
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
        const Job job=jobs_[next_++];
        std::error_code outputError;
        const auto outputStatus=std::filesystem::symlink_status(nativePath(job.output),outputError);
        if(!job.options.overwrite && std::filesystem::exists(outputStatus)) {
            ++skipped_;++finished_;finishFile(job,{},false,true);continue;
        }
        ++active_;activeInputs_.insert(job.input);activeOutputs_.insert(job.input,job.output);activeBatches_.insert(job.input,job.batch);
        std::shared_ptr<ConversionSession> session;
        if(!converter_) {
            auto& stored=sessions_[job.input];
            if(!stored)stored=std::make_shared<ConversionSession>(nativePath(job.input));
            session=stored;
        }
        emit outputStarted(job.batch,job.input,job.output);
        emit statusChanged(job.input,"Converting",job.output);
        pool_.start([this,job,session] {
            QString error;bool skipped=false;
            try {const auto result=session?session->convert(nativePath(job.output),job.options):converter_(nativePath(job.input),nativePath(job.output),job.options);skipped=result.skipped;}catch(const std::exception& e){error=QString::fromUtf8(e.what());}catch(...){error="Unknown conversion failure";}
            error.replace("--mode process","Process pixels mode");error.replace("--raw-exposure","RAW exposure");
            QMetaObject::invokeMethod(this,[this,job,error,skipped] {
                --active_;activeInputs_.remove(job.input);activeOutputs_.remove(job.input);activeBatches_.remove(job.input);++finished_;
                if(!error.isEmpty())++failed_;else if(skipped)++skipped_;else ++succeeded_;
                finishFile(job,error,false,skipped);
                pump();
            },Qt::QueuedConnection);
        });
    }
    emit progress(finished_,jobs_.size(),active_);
    if(!busy())emit batchFinished(succeeded_,failed_,cancelled_);
}
void ConversionQueue::finishFile(const Job& job,const QString& error,bool cancelled,bool skipped) {
    const QString outcome=cancelled?"Cancelled":!error.isEmpty()?"Failed":skipped?"Skipped":"Finished";
    const auto aliases=batchAliases_.take(jobKey(job));
    emit outputFinished(job.batch,job.input,job.output,outcome);
    for(auto alias:aliases)if(alias!=job.batch)emit outputFinished(alias,job.input,job.output,outcome);
    if(!error.isEmpty())fileErrors_[job.input]+=job.output+": "+error+"\n";
    if(cancelled)++fileCancelled_[job.input];
    if(skipped)++fileSkipped_[job.input];
    if(error.isEmpty()&&!cancelled&&!skipped)++fileSucceeded_[job.input];
    const int left=--remaining_[job.input];
    // With multiple outputs, a later success must not hide an earlier failure.
    if(left>0){emit statusChanged(job.input,QString("In progress · %1 outputs remaining").arg(left),fileErrors_.value(job.input));return;}
    sessions_.remove(job.input);
    const auto errors=fileErrors_.value(job.input);
    if(!errors.isEmpty())emit statusChanged(job.input,"Failed",errors);
    else if(fileCancelled_.value(job.input))emit statusChanged(job.input,"Cancelled",QString("%1 output(s) not started").arg(fileCancelled_.value(job.input)));
    else if(fileSkipped_.value(job.input))emit statusChanged(job.input,fileSucceeded_.value(job.input)?"Done · skipped existing outputs":"Skipped",QString("%1 existing output(s) skipped\n%2").arg(fileSkipped_.value(job.input)).arg(job.output));
    else emit statusChanged(job.input,"Done",job.output);
}
}
