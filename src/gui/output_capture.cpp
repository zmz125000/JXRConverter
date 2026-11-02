#include "output_capture.hpp"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QStringConverter>
#include <cstdio>
#include <atomic>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif
namespace jxrc::gui {
namespace {
std::atomic<OutputCapture*> active{nullptr};
#ifdef _WIN32
int duplicate(int fd){return _dup(fd);}int redirect(int from,int to){return _dup2(from,to);}void closeFd(int fd){if(fd>=0)_close(fd);}
int readFd(int fd,char* buffer,int size){return _read(fd,buffer,size);}int writeFd(int fd,const char* buffer,int size){return _write(fd,buffer,size);}
int streamFd(FILE* file){return _fileno(file);}
#else
int duplicate(int fd){const int copy=dup(fd);if(copy>=0)fcntl(copy,F_SETFD,FD_CLOEXEC);return copy;}
int redirect(int from,int to){return dup2(from,to);}void closeFd(int fd){if(fd>=0)close(fd);}
int readFd(int fd,char* buffer,int size){int n;do{n=int(read(fd,buffer,size));}while(n<0&&errno==EINTR);return n;}
int writeFd(int fd,const char* buffer,int size){int n;do{n=int(write(fd,buffer,size));}while(n<0&&errno==EINTR);return n;}
int streamFd(FILE* file){return fileno(file);}
#endif
}
OutputCapture* OutputCapture::instance() {
    static auto* capture=new OutputCapture(QCoreApplication::instance());return capture;
}
OutputCapture::OutputCapture(QObject* parent):QObject(parent) {
#ifdef _WIN32
    // WIN32 GUI executables normally have no CRT stdout/stderr descriptors.
    FILE* ignored=nullptr;
    if(streamFd(stdout)<0)freopen_s(&ignored,"NUL","w",stdout);
    if(streamFd(stderr)<0)freopen_s(&ignored,"NUL","w",stderr);
#endif
    std::fflush(stdout);std::fflush(stderr);out_=streamFd(stdout);err_=streamFd(stderr);
    if(out_<0||err_<0)return;
    savedOut_=duplicate(out_);savedErr_=duplicate(err_);int channel[2]={-1,-1};
#ifdef _WIN32
    const bool opened=_pipe(channel,16384,_O_BINARY|_O_NOINHERIT)==0;
#else
    const bool opened=pipe(channel)==0;
    if(opened){fcntl(channel[0],F_SETFD,FD_CLOEXEC);fcntl(channel[1],F_SETFD,FD_CLOEXEC);}
#endif
    if(!opened||savedOut_<0||savedErr_<0){closeFd(channel[0]);closeFd(channel[1]);return;}
    if(redirect(channel[1],out_)<0||redirect(channel[1],err_)<0) {
        redirect(savedOut_,out_);redirect(savedErr_,err_);closeFd(channel[0]);closeFd(channel[1]);return;
    }
    try {
        reader_=std::thread([this,fd=channel[0]] {
            QStringDecoder decode(QStringDecoder::Utf8);char buffer[8192];int count;
            while((count=readFd(fd,buffer,sizeof(buffer)))>0) {
                // Preserve terminal/debug-console diagnostics as well as the tab.
                for(int offset=0;offset<count;) {const int n=writeFd(savedErr_,buffer+offset,count-offset);if(n<=0)break;offset+=n;}
                const QString text=decode(QByteArrayView(buffer,count));
                QMetaObject::invokeMethod(this,[this,text]{emit textReady(text);},Qt::QueuedConnection);
            }
            closeFd(fd);
        });
    }catch(...) {redirect(savedOut_,out_);redirect(savedErr_,err_);closeFd(channel[0]);closeFd(channel[1]);return;}
    closeFd(channel[1]);
    if(!QStandardPaths::isTestModeEnabled()){active.store(this);previous_=qInstallMessageHandler(qtMessage);}
}
void OutputCapture::qtMessage(QtMsgType type,const QMessageLogContext& context,const QString& message) {
    auto* capture=active.load();
    if(capture&&capture->previous_){capture->previous_(type,context,message);return;}
    const auto text=(qFormatLogMessage(type,context,message)+'\n').toUtf8();
    std::fwrite(text.constData(),1,size_t(text.size()),stderr);std::fflush(stderr);
}
OutputCapture::~OutputCapture() {
    if(active.load()==this){qInstallMessageHandler(previous_);active.store(nullptr);}
    std::fflush(stdout);std::fflush(stderr);
    if(savedOut_>=0)redirect(savedOut_,out_);if(savedErr_>=0)redirect(savedErr_,err_);
    if(reader_.joinable())reader_.join();closeFd(savedOut_);closeFd(savedErr_);
}
}
