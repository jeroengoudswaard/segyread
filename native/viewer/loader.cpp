#include "loader.h"

namespace segy {

LoadResult loadSegyFile(const std::filesystem::path& path, ThreadPool& pool, const ProgressFn& onProgress) {
    LoadResult result;

    std::string error;
    if (!result.file.open(path, &error)) {
        result.error = "Could not open file: " + error;
        return result;
    }
    if (result.file.size() < kHeaderTotalSize) {
        result.error = "File is smaller than a SEG-Y header (3600 bytes).";
        return result;
    }

    BinaryHeader bh = parseBinaryHeader(result.file.data() + kTextHeaderSize);
    if (bh.samplesPerTrace <= 0) {
        result.error = "Binary header reports zero samples per trace.";
        return result;
    }
    if (!isSampleFormatSupported(bh.formatCode)) {
        result.error = "Unsupported sample format code: " + std::to_string(bh.formatCode);
        return result;
    }

    size_t traceStride = kTraceHeaderSize + size_t(bh.samplesPerTrace) * size_t(sampleFormatSizeBytes(bh.formatCode));
    int64_t traceCount = int64_t((result.file.size() - kHeaderTotalSize) / traceStride);
    if (traceCount <= 0) {
        result.error = "File contains no complete traces.";
        return result;
    }

    BuildParams bp;
    bp.fileBase = result.file.data();
    bp.fileSize = result.file.size();
    bp.traceCount = traceCount;
    bp.samplesPerTrace = bh.samplesPerTrace;
    bp.formatCode = bh.formatCode;
    bp.traceStrideBytes = traceStride;
    bp.blockFactor = 4;

    result.pyramid = buildPyramid(bp, pool, onProgress);
    result.binHeader = bh;
    result.traceCount = traceCount;
    result.traceStrideBytes = traceStride;
    result.ok = true;
    return result;
}

} // namespace segy
