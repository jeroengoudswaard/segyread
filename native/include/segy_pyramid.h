// Multi-resolution min/max/mean pyramid over trace x sample space.
//
// Design: rendering a variable-density plot of a file with millions of
// traces means the screen (a few thousand pixels wide) needs a coarse
// per-pixel summary, not every raw sample. We precompute, once per file
// load, a stack of block-reduced levels (each level's blocks cover
// `blockFactor` times more traces and samples than the level below), each
// storing min/max/mean/count per block. Rendering then picks the coarsest
// level whose block size does not exceed one screen pixel, so render cost
// is O(screen pixels) regardless of file size.
//
// The finest level is built directly from the memory-mapped file, in
// parallel across trace-block ranges (decode-then-reduce, so the full
// decoded file is never materialized in RAM -- only the much smaller
// pyramid is kept resident). Coarser levels are cheap block-of-blocks
// reductions of the level below and are built in a single pass.
#pragma once
#include <cstdint>
#include <vector>
#include <functional>
#include "segy_format.h"
#include "threadpool.h"

namespace segy {

struct PyramidLevel {
    int traceBlocks = 0;
    int sampleBlocks = 0;
    int64_t traceFactor = 1;  // raw traces represented per block, this level
    int64_t sampleFactor = 1; // raw samples represented per block, this level
    // Row-major [trace][sample], size traceBlocks*sampleBlocks.
    std::vector<float> minv;
    std::vector<float> maxv;
    std::vector<float> mean;
    std::vector<int32_t> count; // raw sample count actually reduced into the block (ragged edges have fewer)

    size_t index(int t, int s) const { return size_t(t) * size_t(sampleBlocks) + size_t(s); }
};

struct Pyramid {
    // levels.front() is the finest (built straight from the file);
    // levels.back() is the coarsest (built by repeatedly coarsening).
    std::vector<PyramidLevel> levels;
    int64_t traceCount = 0;
    int samplesPerTrace = 0;
    float globalMin = 0.0f;
    float globalMax = 0.0f;
};

struct BuildParams {
    const uint8_t* fileBase = nullptr;
    size_t fileSize = 0;
    int64_t traceCount = 0;
    int samplesPerTrace = 0;
    int16_t formatCode = 0;
    size_t traceStrideBytes = 0; // kTraceHeaderSize + samplesPerTrace * sampleFormatSizeBytes(formatCode)
    int blockFactor = 4;         // traces/samples per block at the finest level
    int maxLevels = 24;          // safety cap; geometric shrink converges long before this
    // When set, must point to `traceCount` file-trace indices: logical
    // trace i in the built pyramid reads physical file trace
    // traceIndexMap[i], instead of i itself. Lets a header-based
    // select/sort (segy_select.h) produce a pyramid over an arbitrary
    // filtered/reordered subset of a file's traces by reusing this same
    // decode-and-reduce pass, reading straight from the same mmap'd file
    // (already resident in the OS page cache from the original load) --
    // no file duplication, no extra I/O. Null (the default) is the
    // identity mapping, i.e. today's whole-file-in-order behavior, at no
    // extra cost beyond one branch per trace.
    const int64_t* traceIndexMap = nullptr;
};

// Progress callback invoked from the calling thread's caller (i.e. called
// synchronously by parallelFor workers) with a monotonically increasing
// count of finished trace-blocks and the total, so a UI can show a progress
// bar. May be called from multiple worker threads concurrently -- must be
// thread-safe (an atomic counter feeding it is the intended usage).
using ProgressFn = std::function<void(int64_t done, int64_t total)>;

Pyramid buildPyramid(const BuildParams& params, ThreadPool& pool,
                      const ProgressFn& onProgress = nullptr);

} // namespace segy
