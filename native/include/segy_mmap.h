// RAII wrapper around a read-only memory-mapped file view.
//
// Rationale: SEG-Y files are read far more than they are written, and a
// viewer needs random access to arbitrary trace offsets. Memory-mapping
// lets the OS page cache do the I/O scheduling/caching for us with zero
// copy into user buffers, instead of hand-rolled buffered reads.
//
// This header is shared between platforms; the resource handles a mapping
// actually needs differ (Win32 HANDLE pair vs. a POSIX file descriptor), so
// those few members are the one place in the shared headers that branches
// on platform. The public interface (open/close/data/size) is identical on
// both; implementations live in src/platform/segy_mmap_win32.cpp and
// src/platform/segy_mmap_posix.cpp -- exactly one of which is compiled in,
// selected by build.bat (Windows) or build.sh (Linux).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <filesystem>

namespace segy {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { close(); }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }
    MappedFile& operator=(MappedFile&& other) noexcept;

    // Opens and maps `path` read-only. Returns false and fills `error` on
    // failure (file missing, locked, zero-length, mapping failure, etc.).
    // std::filesystem::path (rather than std::string/std::wstring) so
    // callers don't need to know the platform's native path encoding.
    bool open(const std::filesystem::path& path, std::string* error = nullptr);
    void close();

    bool isOpen() const { return data_ != nullptr; }
    const uint8_t* data() const { return static_cast<const uint8_t*>(data_); }
    size_t size() const { return size_; }

private:
    void* data_ = nullptr;
    size_t size_ = 0;
#if defined(_WIN32)
    void* fileHandle_ = nullptr;    // HANDLE, void* to avoid pulling <windows.h> into every TU
    void* mappingHandle_ = nullptr; // HANDLE
#else
    int fd_ = -1;
#endif
};

} // namespace segy
