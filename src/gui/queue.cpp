#include "queue.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif
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
int recommendedWorkers(int cores, quint64 bytes) {
    constexpr quint64 perJob = 768ull*1024*1024;
    return std::max(1,std::min(std::max(1,cores),int(std::max<quint64>(1,bytes/perJob))));
}
quint64 availableMemory() {
    quint64 result=2ull*1024*1024*1024;
#ifdef _WIN32
    MEMORYSTATUSEX status{};status.dwLength=sizeof(status);if(GlobalMemoryStatusEx(&status))result=status.ullAvailPhys;
#elif defined(__APPLE__)
    vm_statistics64_data_t stats{};mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;vm_size_t page=0;
    if(host_page_size(mach_host_self(),&page)==KERN_SUCCESS&&host_statistics64(mach_host_self(),HOST_VM_INFO64,reinterpret_cast<host_info64_t>(&stats),&count)==KERN_SUCCESS)result=quint64(stats.free_count+stats.inactive_count)*page;
#else
    const long pages=sysconf(_SC_AVPHYS_PAGES),page=sysconf(_SC_PAGESIZE);if(pages>0&&page>0)result=quint64(pages)*page;
    QFile maximum("/sys/fs/cgroup/memory.max"),current("/sys/fs/cgroup/memory.current");
    if(maximum.open(QIODevice::ReadOnly)&&current.open(QIODevice::ReadOnly)) {
        bool ok=false;const quint64 cap=maximum.readAll().trimmed().toULongLong(&ok),used=current.readAll().trimmed().toULongLong();
        if(ok)result=std::min(result,cap>used?cap-used:quint64(0));
    }
#endif
    return result;
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
    for(auto& job:jobs_){job.options.threads=budget;emit statusChanged(job.input,"Queued",job.output);}
    pump();
}
void ConversionQueue::cancelPending() {
    if(!busy())return;
    while(next_<jobs_.size()){const auto& job=jobs_[next_++];++cancelled_;++finished_;emit statusChanged(job.input,"Cancelled","Not started");}
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
                if(error.isEmpty()){++succeeded_;emit statusChanged(job.input,"Done",job.output);}
                else{++failed_;emit statusChanged(job.input,"Failed",error);}
                pump();
            },Qt::QueuedConnection);
        });
    }
    emit progress(finished_,jobs_.size(),active_);
    if(!busy())emit batchFinished(succeeded_,failed_,cancelled_);
}
}
