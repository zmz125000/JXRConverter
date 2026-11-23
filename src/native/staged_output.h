#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace jxrc {
struct Staging {
    std::filesystem::path directory, file;
    explicit Staging(const std::filesystem::path& output) {
        static std::atomic<uint64_t> sequence{0};
        auto parent=output.parent_path();if(parent.empty())parent=".";
        for(unsigned i=0;i<100;++i) {
            directory=parent/(".jxrc-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++));
            if(std::filesystem::create_directory(directory)){file=directory/"output";return;}
        }
        throw std::runtime_error("Cannot create conversion staging directory");
    }
    ~Staging() {std::error_code ec;std::filesystem::remove_all(directory,ec);}
    void publish(const std::filesystem::path& output,bool overwrite) {
        if(!overwrite) {std::filesystem::create_hard_link(file,output);return;}
#ifdef _WIN32
        if(!MoveFileExW(file.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot publish output");
#else
        std::filesystem::rename(file,output);
#endif
    }
};
}
