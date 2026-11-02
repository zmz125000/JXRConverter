// Standalone helper: no Qt or codec DLLs are held open while replacing them.
#include <filesystem>
#include "update_paths.hpp"
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <cerrno>
#endif
namespace fs=std::filesystem;
namespace {
using jxrc_update::local;
using jxrc_update::safeParent;
void waitForParent(unsigned long pid) {
#ifdef _WIN32
    HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if(!process){if(GetLastError()==ERROR_INVALID_PARAMETER)return;throw std::runtime_error("Cannot wait for application to close");}
    const DWORD result=WaitForSingleObject(process,60000);CloseHandle(process);
    if(result!=WAIT_OBJECT_0)throw std::runtime_error("Application did not close within 60 seconds");
#else
    for(int n=0;n<600;++n){if(kill(static_cast<pid_t>(pid),0)<0&&errno==ESRCH)return;std::this_thread::sleep_for(std::chrono::milliseconds(100));}
    throw std::runtime_error("Application did not close within 60 seconds");
#endif
}
bool launch(const fs::path& exe,const fs::path& root) {
    // Portable updates must resolve the newly installed Qt/engine, rather than
    // paths inherited from an SDK or an earlier external library installation.
    for(const char* name:{"QT_PLUGIN_PATH","QML2_IMPORT_PATH","QTDIR","LD_LIBRARY_PATH","DYLD_LIBRARY_PATH"}) {
#ifdef _WIN32
        SetEnvironmentVariableA(name,nullptr);
#else
        unsetenv(name);
#endif
    }
#ifdef _WIN32
    std::wstring command=L"\""+exe.wstring()+L"\"";STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION process{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,root.c_str(),&start,&process))return false;
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return true;
#else
    // The close-on-exec pipe distinguishes a successful exec from a failed fork child.
    int channel[2];if(pipe(channel))return false;
    if(fcntl(channel[1],F_SETFD,FD_CLOEXEC)<0){close(channel[0]);close(channel[1]);return false;}
    const pid_t child=fork();if(child<0){close(channel[0]);close(channel[1]);return false;}
    if(child==0) {
        close(channel[0]);
        if(chdir(root.c_str())==0)execl(exe.c_str(),exe.c_str(),static_cast<char*>(nullptr));
        char failed=1;const auto written=write(channel[1],&failed,1);(void)written;_exit(127);
    }
    close(channel[1]);char failed=0;ssize_t count;
    do {count=read(channel[0],&failed,1);} while(count<0&&errno==EINTR);
    close(channel[0]);return count==0;
#endif
}
struct Change {fs::path relative;bool hadOld=false,installed=false;};
int apply(unsigned long pid,const fs::path& root,const fs::path& source,const fs::path& executable,const fs::path& temp,const fs::path& manifestPath) {
    if(!local(executable)||!local(manifestPath)||!fs::is_directory(root)||!fs::is_directory(source)||!fs::is_regular_file(source/executable))return 2;
    jxrc_update::validateRoots(root,source,temp);
    const fs::path backup=root/(".jxrc-update-backup-"+std::to_string(pid));
    std::vector<Change> changes;bool createdBackup=false,parentExited=false;
    try {
        waitForParent(pid);parentExited=true;
        // Validate the entire inventory and payload before changing files.
        const auto inventory=jxrc_update::readManifest(root,manifestPath);
        std::set<fs::path> incoming,obsolete;
        for(const auto& entry:fs::recursive_directory_iterator(source))
            if(!entry.is_directory()||entry.is_symlink()) {
                const auto relative=entry.path().lexically_relative(source);
                if(!jxrc_update::packagePath(relative,jxrc_update::startsWith(jxrc_update::lower(manifestPath.generic_u8string()),"contents/")))throw std::runtime_error("Unsafe package path");
                const auto sourceStatus=entry.symlink_status();
                if(!fs::is_regular_file(sourceStatus)&&!fs::is_symlink(sourceStatus))throw std::runtime_error("Package contains a special file");
                if(fs::is_symlink(sourceStatus)) {
                    const auto link=fs::read_symlink(entry.path());
                    if(link.is_absolute()||!local((relative.parent_path()/link).lexically_normal()))throw std::runtime_error("Unsafe package symlink");
                }
                safeParent(root,relative);
                const auto targetStatus=fs::symlink_status(root/relative);
                if(fs::exists(targetStatus)&&!fs::is_regular_file(targetStatus)&&!fs::is_symlink(targetStatus))throw std::runtime_error("A directory or special file conflicts with an update file");
                incoming.insert(relative);
            }
        for(const auto& relative:inventory)if(!incoming.count(relative))obsolete.insert(relative);
        if(fs::exists(backup))throw std::runtime_error("An earlier update backup must be recovered first");
        if(!fs::create_directory(backup))throw std::runtime_error("Cannot create the update backup");
        createdBackup=true;
        // Older portable releases predate the package manifest.
        for(const char* name:{"vc_redist.x64.exe","vc_redist.arm64.exe","vc_redist.x86.exe","dxcompiler.dll","dxil.dll","d3dcompiler_47.dll"})
            if(!incoming.count(name)&&fs::is_regular_file(root/name))obsolete.insert(name);
        auto replace=[&](const fs::path& relative,bool install) {
            if(!local(relative))throw std::runtime_error("Invalid update path");
            safeParent(root,relative);
            const fs::path target=root/relative,old=backup/relative;
            fs::create_directories(target.parent_path());changes.push_back({relative});auto& change=changes.back();
            const auto status=fs::symlink_status(target);
            if(fs::exists(status)) {
                if(fs::is_directory(status))throw std::runtime_error("A directory conflicts with an update file");
                fs::create_directories(old.parent_path());
                std::error_code error;
                for(int n=0;n<100;++n){fs::rename(target,old,error);if(!error)break;std::this_thread::sleep_for(std::chrono::milliseconds(100));}
                if(error)throw fs::filesystem_error("Cannot replace a file; close other converter processes",target,error);
                change.hadOld=true;
            }
            if(install) {
                change.installed=true;
                if(fs::is_symlink(fs::symlink_status(source/relative))) {
                    const auto link=fs::read_symlink(source/relative);
                    if(link.is_absolute()||!local((relative.parent_path()/link).lexically_normal()))throw std::runtime_error("Unsafe package symlink");
                    fs::copy_symlink(source/relative,target);
                } else fs::copy_file(source/relative,target);
                change.installed=true;
            }
        };
        for(const auto& relative:obsolete)replace(relative,false);
        for(const auto& relative:incoming)replace(relative,true);
        if(!launch(root/executable,root))throw std::runtime_error("Could not relaunch the updated application");
        std::error_code ignored;fs::remove_all(backup,ignored);fs::remove_all(temp,ignored);return 0;
    } catch(const std::exception& error) {
        bool restored=true;
        for(auto it=changes.rbegin();it!=changes.rend();++it) {
            std::error_code ec;const fs::path target=root/it->relative;
            if(it->installed)fs::remove(target,ec);
            if(it->hadOld){ec.clear();fs::rename(backup/it->relative,target,ec);if(ec)restored=false;}
        }
        std::ofstream report(root/".jxrc-update-error.txt");report<<error.what();
        if(!restored)report<<"\nSome files could not be restored. Recover them from "<<backup.u8string();
        report.close();
        if(restored){std::error_code ignored;if(createdBackup)fs::remove_all(backup,ignored);if(parentExited)launch(root/executable,root);}
        return 1;
    }
}
}
// Helper protocol v1: parent PID, installation, payload, executable, workspace, manifest.
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    if(argc!=7)return 2;
    try{return apply(std::stoul(argv[1]),fs::path(argv[2]),fs::path(argv[3]),fs::path(argv[4]),fs::path(argv[5]),fs::path(argv[6]));}catch(...){return 2;}
}
#else
int main(int argc,char** argv) {
    if(argc!=7)return 2;
    try{return apply(std::stoul(argv[1]),fs::u8path(argv[2]),fs::u8path(argv[3]),fs::u8path(argv[4]),fs::u8path(argv[5]),fs::u8path(argv[6]));}catch(...){return 2;}
}
#endif
