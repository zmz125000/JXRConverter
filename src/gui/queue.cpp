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
QVector<Job> makeJobs(const QStringList& inputs,const QString& directory,const Options& options) {
    validate(options);QVector<Job> result;QSet<QString> outputs;
    const int target=int(options.target);
    const QStringList suffixes={"sdr.jpg","gainmap.jpg","sdr.jxl","linear.jxl","sdr.avif","pq.avif","gainmap.avif","pq.jxl"};
    for(const auto& input:inputs) {
        const QString stem=QFileInfo(input).completeBaseName();
        QString filename=stem+"_"+suffixes[target];int n=2;
        while(outputs.contains(filename.toCaseFolded()))filename=stem+"_"+QString::number(n++)+"_"+suffixes[target];
        outputs.insert(filename.toCaseFolded());
        const QString output=QDir(directory).absoluteFilePath(filename);
        if(QFileInfo(input).absoluteFilePath()==output)throw std::invalid_argument("Output would replace an input file");
        result.append({input,output,options});
    }
    return result;
}
ConversionQueue::ConversionQueue(QObject* parent,Convert converter):QObject(parent),converter_(std::move(converter)) {}
ConversionQueue::~ConversionQueue() { pool_.clear();pool_.waitForDone(); }
void ConversionQueue::start(QVector<Job> jobs,int parallelism,int cores) {
    if(busy())throw std::logic_error("A conversion batch is already running");
    if(jobs.isEmpty())return;
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
        const Job job=jobs_[next_++];++active_;emit statusChanged(job.input,"Converting",job.output);
        pool_.start([this,job] {
            QString error;
            try {converter_(nativePath(job.input),nativePath(job.output),job.options);}catch(const std::exception& e){error=QString::fromUtf8(e.what());}catch(...){error="Unknown conversion failure";}
            error.replace("--mode process","Process pixels mode");error.replace("--raw-exposure","RAW exposure");
            QMetaObject::invokeMethod(this,[this,job,error] {
                --active_;++finished_;
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
    const auto errors=fileErrors_.value(job.input);
    if(!errors.isEmpty())emit statusChanged(job.input,"Failed",errors);
    else if(fileCancelled_.value(job.input))emit statusChanged(job.input,"Cancelled",QString("%1 output(s) not started").arg(fileCancelled_.value(job.input)));
    else emit statusChanged(job.input,"Done",job.output);
}
}
