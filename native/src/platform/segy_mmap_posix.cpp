// POSIX (Linux) implementation of MappedFile. See segy_mmap.h and
// segy_mmap_win32.cpp for the Windows counterpart -- keep the two in sync
// when changing behavior (error handling, zero-length rejection, etc.).
//
// NOTE: written to mirror the Win32 implementation's semantics exactly
// (read-only mapping, reject zero-length files, same error-message shape)
// but not yet compiled/run -- see native/README.md for verification status.
#include "segy_mmap.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

namespace segy {

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        data_ = other.data_;
        size_ = other.size_;
        other.fd_ = -1;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

bool MappedFile::open(const std::filesystem::path& path, std::string* error) {
    close();

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error) *error = std::string("open() failed: ") + std::strerror(errno);
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        if (error) *error = "file is empty or fstat() failed";
        ::close(fd);
        return false;
    }

    void* view = ::mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        if (error) *error = std::string("mmap() failed: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    fd_ = fd;
    data_ = view;
    size_ = size_t(st.st_size);
    return true;
}

void MappedFile::close() {
    if (data_) {
        ::munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
}

} // namespace segy
