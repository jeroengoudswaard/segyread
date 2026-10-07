// Platform-independent SEG-Y file loading: open + mmap, parse/validate the
// binary header, and build the pyramid. Synchronous and blocking -- each
// platform shell runs this on its own background thread and marshals
// progress/completion back to the UI thread however that platform does it
// (PostMessage on Windows; a self-pipe + XSendEvent wakeup on X11).
#pragma once
#include <filesystem>
#include <string>
#include "segy_format.h"
#include "segy_pyramid.h"
#include "segy_mmap.h"
#include "threadpool.h"

namespace segy {

struct LoadResult {
    bool ok = false;
    std::string error;
    MappedFile file; // moved out to the caller on success; the pyramid and
                      // any later exact-decode rendering both read from it,
                      // so its lifetime must outlive the loaded document.
    BinaryHeader binHeader{};
    int64_t traceCount = 0;
    size_t traceStrideBytes = 0;
    Pyramid pyramid;
};

// Opens and validates `path`, then builds its pyramid using `pool`.
// `onProgress(done, total)` may be called from multiple pool worker threads
// concurrently (it's fed straight from buildPyramid's own progress
// callback) -- must be thread-safe.
LoadResult loadSegyFile(const std::filesystem::path& path, ThreadPool& pool, const ProgressFn& onProgress = nullptr);

} // namespace segy
