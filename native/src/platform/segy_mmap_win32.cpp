// Windows implementation of MappedFile. See segy_mmap.h and
// segy_mmap_posix.cpp for the POSIX counterpart -- keep the two in sync
// when changing behavior (error handling, zero-length rejection, etc.).
#include "segy_mmap.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace segy {

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        fileHandle_ = other.fileHandle_;
        mappingHandle_ = other.mappingHandle_;
        data_ = other.data_;
        size_ = other.size_;
        other.fileHandle_ = nullptr;
        other.mappingHandle_ = nullptr;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

bool MappedFile::open(const std::filesystem::path& path, std::string* error) {
    close();

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "CreateFileW failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0) {
        if (error) *error = "file is empty or GetFileSizeEx failed";
        CloseHandle(file);
        return false;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        if (error) *error = "CreateFileMappingW failed (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(file);
        return false;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        if (error) *error = "MapViewOfFile failed (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }

    fileHandle_ = file;
    mappingHandle_ = mapping;
    data_ = view;
    size_ = size_t(fileSize.QuadPart);
    return true;
}

void MappedFile::close() {
    if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (mappingHandle_) {
        CloseHandle(static_cast<HANDLE>(mappingHandle_));
        mappingHandle_ = nullptr;
    }
    if (fileHandle_) {
        CloseHandle(static_cast<HANDLE>(fileHandle_));
        fileHandle_ = nullptr;
    }
    size_ = 0;
}

} // namespace segy
