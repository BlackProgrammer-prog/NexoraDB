#pragma once
#include <filesystem>
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace nexora::graph::detail {
class DirectoryLease {
    int fd_=-1;
public:
    explicit DirectoryLease(const std::filesystem::path& directory) {
#ifdef _WIN32
        throw std::runtime_error("graph directory leases require POSIX backend");
#else
        std::filesystem::create_directories(directory);
        fd_=::open((directory/".nexora-admin.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd_<0) throw std::runtime_error("cannot open graph administration lease");
        if(::flock(fd_,LOCK_SH|LOCK_NB)!=0) {
            ::close(fd_); fd_=-1;
            throw std::runtime_error("graph directory is held by offline administration");
        }
#endif
    }
    ~DirectoryLease() {
#ifndef _WIN32
        if(fd_>=0) ::close(fd_);
#endif
    }
    DirectoryLease(const DirectoryLease&)=delete;
    DirectoryLease& operator=(const DirectoryLease&)=delete;
};
}
