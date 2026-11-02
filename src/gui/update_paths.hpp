#pragma once
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jxrc_update {
namespace fs=std::filesystem;
inline std::string lower(std::string value) {
    for(auto& c:value)c=char(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
inline bool endsWith(const std::string& value,const std::string& suffix) {
    return value.size()>=suffix.size()&&value.compare(value.size()-suffix.size(),suffix.size(),suffix)==0;
}
inline bool startsWith(const std::string& value,const std::string& prefix) {return value.rfind(prefix,0)==0;}
inline bool local(const fs::path& path) {
    if(path.empty()||path.is_absolute()||path.has_root_name())return false;
    const auto name=path.generic_u8string();
    if(name.back()=='/'||name.find("//")!=std::string::npos)return false;
    for(unsigned char c:name)if(c<32||c==127||std::string("\\:*?\"<>|").find(char(c))!=std::string::npos)return false;
    for(const auto& part:path) {
        const auto component=part.u8string();
        if(component.empty()||component=="."||component==".."||component.back()=='.'||component.back()==' ')return false;
        const auto stem=lower(component.substr(0,component.find('.')));
        if(stem=="con"||stem=="prn"||stem=="aux"||stem=="nul"||stem=="conin$"||stem=="conout$"||
           (stem.size()==4&&(startsWith(stem,"com")||startsWith(stem,"lpt"))&&stem[3]>='1'&&stem[3]<='9'))return false;
    }
    return true;
}
inline bool packagePath(const fs::path& path,bool appOnly=false) {
    if(!local(path))return false;
    const auto name=lower(path.generic_u8string());
    // Never interpret user data as package inventory, even in an app bundle.
    for(const auto& component:path) {
        const auto part=lower(component.u8string());
        if(part=="config"||part==".git"||startsWith(part,".jxrc-"))return false;
    }
    if(startsWith(name,"jxrconverter.app/contents/")||(appOnly&&startsWith(name,"contents/")))return true;
    if(startsWith(name,"share/jxrconverter/"))return true;
    if(name=="share/icons/hicolor/scalable/apps/jxrconverter.svg")return true;
    const auto slash=name.find('/');
    const auto first=name.substr(0,slash);
    const auto filename=lower(path.filename().u8string());
    const bool library=endsWith(filename,".dll")||endsWith(filename,".dylib")||endsWith(filename,".so")||filename.find(".so.")!=std::string::npos;
    if(slash!=std::string::npos) {
        // Runtime libraries and Qt plugins, including older bin/ layouts.
        static const std::set<std::string> runtimeDirs={"lib","lib64","plugins","platforms","styles","tls","iconengines","imageformats","platformthemes","platforminputcontexts","xcbglintegrations","egldeviceintegrations","networkinformation","generic"};
        if(runtimeDirs.count(first)&&library)return true;
        if(first=="bin")return packagePath(fs::u8path(name.substr(slash+1)));
        return false;
    }
    if(name=="jxrconverter"||name=="jxrconverter.exe"||name=="jxrconvert"||name=="jxrconvert.exe"||name=="jxrc-update"||name=="jxrc-update.exe"||name=="qt.conf")return true;
    if(name=="vc_redist.x64.exe"||name=="vc_redist.arm64.exe"||name=="vc_redist.x86.exe")return true;
    if(!library)return false;
    static const std::set<std::string> legacyRuntime={"opengl32sw.dll","libegl.dll","libglesv2.dll","d3dcompiler_43.dll","libgcc_s_seh-1.dll","libstdc++-6.dll","libwinpthread-1.dll"};
    if(legacyRuntime.count(name))return true;
    return startsWith(name,"qt6")||startsWith(name,"libqt6")||startsWith(name,"libjxrc")||name=="jxrc.dll"||startsWith(name,"icu")||startsWith(name,"libicu")||startsWith(name,"libssl-")||startsWith(name,"libcrypto-")||startsWith(name,"vcruntime")||startsWith(name,"msvcp")||startsWith(name,"concrt")||startsWith(name,"api-ms-win-")||name=="ucrtbase.dll"||name=="dxcompiler.dll"||name=="dxil.dll"||name=="d3dcompiler_47.dll";
}
inline void safeParent(const fs::path& root,const fs::path& relative) {
    fs::path at=root;
    for(const auto& part:relative.parent_path()) {
        at/=part;
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(at.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Install path contains a directory reparse point");
#endif
        if(fs::is_symlink(fs::symlink_status(at)))throw std::runtime_error("Install path contains a directory symlink");
    }
}
inline std::set<fs::path> readManifest(const fs::path& root,const fs::path& manifestPath) {
    const auto inventoryPath=manifestPath.generic_u8string();
    if(inventoryPath!="share/JXRConverter/package-files.txt"&&inventoryPath!="Contents/Resources/share/JXRConverter/package-files.txt")throw std::runtime_error("Invalid package inventory location");
    safeParent(root,manifestPath);
    const auto status=fs::symlink_status(root/manifestPath);
    if(status.type()==fs::file_type::not_found)return {}; // Older releases had no inventory.
    if(!fs::is_regular_file(status))throw std::runtime_error("Package inventory must be a regular file");
    if(fs::file_size(root/manifestPath)>4*1024*1024)throw std::runtime_error("Package inventory is too large");
    std::ifstream input(root/manifestPath);
    if(!input)throw std::runtime_error("Cannot read package inventory");
    std::set<fs::path> entries;std::set<std::string> seen;std::string line;
    while(std::getline(input,line)) {
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        const auto relative=fs::u8path(line);
        if(relative.generic_u8string()!=line||line.find('\\')!=std::string::npos||!packagePath(relative,startsWith(lower(manifestPath.generic_u8string()),"contents/")))throw std::runtime_error("Unsafe package inventory entry: "+line);
        safeParent(root,relative);
        const auto target=fs::symlink_status(root/relative);
        if(fs::is_directory(target))throw std::runtime_error("Package inventory cannot contain directories");
        if(fs::exists(target)&&!fs::is_regular_file(target)&&!fs::is_symlink(target))throw std::runtime_error("Package inventory contains a special file");
#if defined(_WIN32) || defined(__APPLE__)
        const auto identity=lower(line);
#else
        const auto& identity=line;
#endif
        if(!seen.insert(identity).second)throw std::runtime_error("Duplicate package inventory entry");
        entries.insert(relative);
    }
    if(input.bad())throw std::runtime_error("Cannot read package inventory");
    return entries;
}
inline bool samePath(const fs::path& a,const fs::path& b) {
#if defined(_WIN32) || defined(__APPLE__)
    return lower(a.generic_u8string())==lower(b.generic_u8string());
#else
    return a==b;
#endif
}
inline bool inside(const fs::path& child,const fs::path& parent) {
    auto a=child.begin(),b=parent.begin();
    for(;b!=parent.end();++a,++b)if(a==child.end()||!samePath(*a,*b))return false;
    return true;
}
inline void validateRoots(const fs::path& root,const fs::path& source,const fs::path& temp) {
    const auto install=fs::canonical(root),payload=fs::canonical(source),workspace=fs::canonical(temp);
    if(install==install.root_path())throw std::runtime_error("Updating from a drive root is not allowed; use a dedicated app folder");
    if(workspace==workspace.root_path()||!startsWith(workspace.filename().u8string(),"jxrc-update-")||
       !samePath(workspace.parent_path(),fs::canonical(fs::temp_directory_path()))||
       samePath(payload,workspace)||!inside(payload,workspace)||inside(install,workspace)||inside(workspace,install))
        throw std::runtime_error("Unsafe update workspace");
}
}
