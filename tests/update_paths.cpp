#include "../src/gui/update_paths.hpp"
#include <chrono>
#include <iostream>
#include <vector>
namespace fs=std::filesystem;
using namespace jxrc_update;
void require(bool condition,const std::string& message) {if(!condition)throw std::runtime_error(message);}
template<class Work> void rejects(Work work,const std::string& label) {
    bool rejected=false;try{work();}catch(const std::exception&){rejected=true;}
    require(rejected,"Accepted unsafe input: "+label);
}
void write(const fs::path& path,const std::string& text) {
    fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out<<text;
    require(bool(out),"Cannot create fixture");
}
struct Directory {
    fs::path path;
    explicit Directory(fs::path value):path(std::move(value)){require(fs::create_directory(path),"Cannot create test directory");}
    ~Directory(){std::error_code ignored;fs::remove_all(path,ignored);}
};
int main(int argc,char** argv) {
    try {
        const auto stamp=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        Directory fixture(fs::temp_directory_path()/("jxrc-policy-test-"+stamp));
        Directory workspace(fs::temp_directory_path()/("jxrc-update-"+stamp));
        const auto root=fixture.path/"app",source=workspace.path/"payload";
        fs::create_directories(root);fs::create_directories(source);
        const fs::path manifest="share/JXRConverter/package-files.txt";
        write(root/"personal/photo.jpg","personal photo");write(root/"config/settings.ini","user settings");
        validateRoots(root,source,workspace.path);
        rejects([&]{validateRoots(root.root_path(),source,workspace.path);},"drive root");
        rejects([&]{validateRoots(root,source,root.root_path());},"cleanup of drive root");
        rejects([&]{validateRoots(root,root,workspace.path);},"payload outside workspace");
        rejects([&]{validateRoots(workspace.path,source,workspace.path);},"installation inside workspace");
        rejects([&]{validateRoots(root,source,fixture.path);},"unrecognized cleanup directory");
        require(readManifest(root,manifest).empty(),"Missing old inventory should be supported");
        rejects([&]{readManifest(root,"config/settings.ini");},"inventory in user config");
        rejects([&]{readManifest(root,fs::temp_directory_path()/"package-files.txt");},"absolute inventory path");
        const std::vector<std::string> valid={"jxrconverter","jxrconvert.exe","jxrc-update","qt.conf","Qt6Core.dll","vcruntime140.dll","lib/libjxrc.so.0","lib/libQt6Core.so.6","platforms/qwindows.dll","plugins/platforms/libqxcb.so","bin/platforms/qwindows.dll","JXRConverter.app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore","share/JXRConverter/licenses/libraw/LICENSE","share/icons/hicolor/scalable/apps/jxrconverter.svg"};
        std::string listing;for(const auto& name:valid){require(packagePath(fs::u8path(name)),"Rejected package path: "+name);listing+=name+"\r\n";}
        write(root/manifest,listing);require(readManifest(root,manifest).size()==valid.size(),"Valid inventory was not accepted");
        require(packagePath("Contents/MacOS/JXRConverter",true),"Rejected app-only Mac package");
        const std::vector<std::string> invalid={"",".","..","../personal/photo.jpg","/","/home/user/photo.jpg","C:/Users/user/photo.jpg","C:photo.jpg","//server/share/photo.jpg","lib/../config/settings.ini","lib/./libjxrc.so","lib//libjxrc.so","lib/","lib/*.so","lib/?.so","lib/libjxrc.so:stream","lib/nul.dll","lib/COM1.dll","lib/foo.dll.","lib/foo.dll ","lib\\libjxrc.so","config/settings.ini","CONFIG/history.json","personal/photo.jpg","Contents/MacOS/JXRConverter",".git/config","share/JXRConverter/config/settings.ini","lib/.jxrc-update-backup-user/file.so",std::string("lib/a\0.so",9),"lib/a\t.so"};
        for(const auto& name:invalid) {
            write(root/manifest,"jxrconvert\n"+name+"\n");
            rejects([&]{readManifest(root,manifest);},name);
            require(fs::exists(root/"personal/photo.jpg")&&fs::exists(root/"config/settings.ini"),"User files changed during validation");
        }
        write(root/manifest,"jxrconvert\njxrconvert\n");rejects([&]{readManifest(root,manifest);},"duplicate rules");
        fs::create_directories(root/"lib/libjxrc.so");write(root/manifest,"lib/libjxrc.so\n");rejects([&]{readManifest(root,manifest);},"directory deletion");
        fs::remove(root/"lib/libjxrc.so");fs::remove(root/"lib");
        std::error_code ec;fs::create_directory_symlink(fixture.path,root/"lib",ec);
        if(!ec){write(root/manifest,"lib/other.so\n");rejects([&]{readManifest(root,manifest);},"directory symlink escape");fs::remove(root/"lib");}
        write(root/manifest,std::string(4*1024*1024+1,'a'));rejects([&]{readManifest(root,manifest);},"oversized inventory");
        for(int i=1;i<argc;++i) {
            const fs::path package=fs::u8path(argv[i]);
            require(!readManifest(package,manifest).empty(),"Existing release inventory was empty");
        }
        std::cout<<"Update path and manifest checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
