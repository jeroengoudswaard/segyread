// Microbenchmarks for the hot paths: IBM-float decode (scalar vs AVX2),
// memory-mapped sequential read throughput, and full pyramid build
// throughput (single-threaded vs the thread pool). No external benchmark
// framework -- just wall-clock timing with enough repetitions to be stable,
// consistent with the "portable, standalone" goal of the rest of the tree.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <chrono>
#include <thread>
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>

#include "segy_format.h"
#include "segy_decode.h"
#include "segy_pyramid.h"
#include "segy_select.h"
#include "segy_mmap.h"
#include "threadpool.h"
#include "synthetic_segy.h"
#include "renderer.h"
#include "wiggle.h"

namespace {

using Clock = std::chrono::high_resolution_clock;

double elapsedSeconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void printRow(const char* name, double seconds, double bytes, double samples) {
    double mbps = (bytes / (1024.0 * 1024.0)) / seconds;
    double msps = (samples / 1.0e6) / seconds;
    std::printf("%-38s %10.3f ms   %10.1f MB/s   %10.1f Msamples/s\n",
                name, seconds * 1000.0, mbps, msps);
}

// ---------------------------------------------------------------------
void benchIbmDecode() {
    std::printf("\n== IBM float32 decode throughput ==\n");
    const size_t n = 16 * 1024 * 1024; // 16M samples = 64 MB source
    std::vector<uint8_t> src(n * 4);
    std::mt19937 rng(1);
    std::uniform_int_distribution<uint32_t> dist;
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = dist(rng);
        // Force the top exponent bit off so values stay in-range (avoid
        // saturating every sample to +-inf, which would defeat the point
        // of a realistic throughput measurement).
        v &= 0xBFFFFFFFu;
        src[i * 4 + 0] = uint8_t(v >> 24);
        src[i * 4 + 1] = uint8_t(v >> 16);
        src[i * 4 + 2] = uint8_t(v >> 8);
        src[i * 4 + 3] = uint8_t(v);
    }
    std::vector<float> dst(n);

    const int reps = 5;
    {
        auto start = Clock::now();
        for (int r = 0; r < reps; ++r) segy::ibmToIeeeScalarBlock(src.data(), dst.data(), n);
        printRow("Scalar (general bit-scan)", elapsedSeconds(start) / reps, double(src.size()), double(n));
    }
    if (segy::cpuSupportsAvx2()) {
        auto start = Clock::now();
        for (int r = 0; r < reps; ++r) segy::ibmToIeeeAvx2(src.data(), dst.data(), n);
        printRow("AVX2 (8-wide, common-case fast path)", elapsedSeconds(start) / reps, double(src.size()), double(n));
    } else {
        std::printf("AVX2 not available on this CPU -- skipped\n");
    }
}

// ---------------------------------------------------------------------
void benchMmapThroughput(const std::filesystem::path& path) {
    std::printf("\n== Memory-mapped sequential read throughput ==\n");
    segy::MappedFile file;
    std::string error;
    if (!file.open(path, &error)) {
        std::printf("  (skipped: %s)\n", error.c_str());
        return;
    }
    volatile uint64_t sink = 0;
    const uint8_t* p = file.data();
    size_t n = file.size();
    auto start = Clock::now();
    for (size_t i = 0; i < n; i += 4096) sink += p[i]; // touch each page once
    double seconds = elapsedSeconds(start);
    printRow("First touch (cold/warm page-in)", seconds, double(n), 0);
    (void)sink;
}

// ---------------------------------------------------------------------
void benchPyramidBuild() {
    std::printf("\n== Pyramid build throughput (synthetic file) ==\n");
    int64_t traceCount = 20000;
    int samplesPerTrace = 1500;
    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIbmFloat32,
                                            [](int64_t t, int s) { return std::sin(double(t + s) * 0.001) * 500.0; });
    double totalBytes = double(f.bytes.size());
    double totalSamples = double(traceCount) * double(samplesPerTrace);

    segy::BuildParams bp;
    bp.fileBase = f.bytes.data();
    bp.fileSize = f.bytes.size();
    bp.traceCount = traceCount;
    bp.samplesPerTrace = samplesPerTrace;
    bp.formatCode = segy::kIbmFloat32;
    bp.traceStrideBytes = f.traceStrideBytes;
    bp.blockFactor = 4;

    {
        segy::ThreadPool pool(1);
        auto start = Clock::now();
        segy::Pyramid p = segy::buildPyramid(bp, pool);
        printRow("1 thread", elapsedSeconds(start), totalBytes, totalSamples);
        (void)p;
    }
    {
        unsigned hw = std::thread::hardware_concurrency();
        segy::ThreadPool pool(hw);
        auto start = Clock::now();
        segy::Pyramid p = segy::buildPyramid(bp, pool);
        char label[64];
        std::snprintf(label, sizeof(label), "%u threads (hardware_concurrency)", hw);
        printRow(label, elapsedSeconds(start), totalBytes, totalSamples);
        (void)p;
    }
}

// ---------------------------------------------------------------------
// Header-based select/sort (segy_select.h + segy_pyramid.h's
// traceIndexMap): same file/size as benchPyramidBuild above, so its "N
// threads" row is the direct full-file-load baseline this compares
// against. Selects ~half the file by CDP range, sorts by CDP, then rebuilds
// the pyramid over just that subset -- the claim being checked is that the
// rebuild cost tracks the SELECTED count, not the file's full trace count.
void benchHeaderSelectSort() {
    std::printf("\n== Header-based select/sort throughput (synthetic file, same size as pyramid build above) ==\n");
    int64_t traceCount = 20000;
    int samplesPerTrace = 1500;
    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIbmFloat32,
                                            [](int64_t t, int s) { return std::sin(double(t + s) * 0.001) * 500.0; });

    // makeSyntheticSegy doesn't set CDP -- give traces a spread-out,
    // easily-filtered value so selection/sorting has real work to do.
    for (int64_t t = 0; t < traceCount; ++t) {
        size_t off = segy::kHeaderTotalSize + size_t(t) * f.traceStrideBytes + 20; // CDP, bytes 21-24
        segy::test::writeI32BE(f.bytes, off, int32_t((t * 7919) % 100000));
    }

    unsigned hw = std::thread::hardware_concurrency();
    segy::ThreadPool pool(hw);

    segy::TraceHeaderColumns columns;
    {
        auto start = Clock::now();
        columns = segy::scanTraceHeaders(f.bytes.data(), traceCount, f.traceStrideBytes, pool);
        printRow("Header scan (all traces)", elapsedSeconds(start), double(traceCount) * segy::kTraceHeaderSize,
                  double(traceCount));
    }

    std::vector<segy::IdentCriterion> criteria = {{segy::IdentField::Cdp, 0, 49999, 1}};
    std::vector<int64_t> selected;
    {
        auto start = Clock::now();
        selected = segy::selectTraces(columns, criteria);
        double seconds = elapsedSeconds(start);
        std::printf("%-38s %10.3f ms   (%zu of %lld traces selected)\n", "Select (1 criterion)", seconds * 1000.0,
                    selected.size(), (long long)traceCount);
    }
    {
        auto start = Clock::now();
        segy::sortTraceIndices(selected, columns, {segy::IdentField::Cdp});
        double seconds = elapsedSeconds(start);
        std::printf("%-38s %10.3f ms   (%zu indices sorted)\n", "Sort (1 key)", seconds * 1000.0, selected.size());
    }

    segy::BuildParams bp;
    bp.fileBase = f.bytes.data();
    bp.fileSize = f.bytes.size();
    bp.traceCount = int64_t(selected.size());
    bp.samplesPerTrace = samplesPerTrace;
    bp.formatCode = segy::kIbmFloat32;
    bp.traceStrideBytes = f.traceStrideBytes;
    bp.blockFactor = 4;
    bp.traceIndexMap = selected.data();
    {
        auto start = Clock::now();
        segy::Pyramid p = segy::buildPyramid(bp, pool);
        double totalSamples = double(selected.size()) * double(samplesPerTrace);
        printRow("Pyramid rebuild over selection", elapsedSeconds(start),
                  double(selected.size()) * double(f.traceStrideBytes), totalSamples);
        (void)p;
    }
}

// ---------------------------------------------------------------------
// Wiggle layout computation (viewer/wiggle.cpp): the performance-critical
// claim being checked is that this stays cheap *regardless of file size*,
// since it decimates both which traces are drawn and how many points each
// gets -- see wiggle.h. Uses a real temp file (computeWiggleLayout always
// exact-decodes from a real mmap, same reason test_main.cpp's wiggle tests
// need one) rather than the in-memory byte buffer synthesizeSegy() returns.
void benchWiggleLayout() {
    std::printf("\n== Wiggle layout computation throughput (synthetic file, fully zoomed out) ==\n");
    int64_t traceCount = 50000;
    int samplesPerTrace = 2000;
    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIbmFloat32,
                                            [](int64_t t, int s) { return std::sin(double(t + s) * 0.001) * 500.0; });

    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "segybench_wiggle_tmp.sgy";
    {
        FILE* out = std::fopen(tmp.string().c_str(), "wb");
        std::fwrite(f.bytes.data(), 1, f.bytes.size(), out);
        std::fclose(out);
    }
    segy::MappedFile file;
    std::string error;
    if (!file.open(tmp, &error)) {
        std::printf("  (skipped: %s)\n", error.c_str());
        return;
    }

    segy::BuildParams bp;
    bp.fileBase = file.data();
    bp.fileSize = file.size();
    bp.traceCount = traceCount;
    bp.samplesPerTrace = samplesPerTrace;
    bp.formatCode = segy::kIbmFloat32;
    bp.traceStrideBytes = f.traceStrideBytes;
    bp.blockFactor = 4;
    segy::ThreadPool pool(std::thread::hardware_concurrency());
    segy::Pyramid pyramid = segy::buildPyramid(bp, pool);

    segy::RenderContext ctx;
    ctx.file = &file;
    ctx.pyramid = &pyramid;
    ctx.binHeader.samplesPerTrace = int16_t(samplesPerTrace);
    ctx.binHeader.sampleIntervalUs = 1000;
    ctx.binHeader.formatCode = segy::kIbmFloat32;
    ctx.traceCount = traceCount;
    ctx.traceStrideBytes = f.traceStrideBytes;
    ctx.pool = &pool;

    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = double(traceCount);
    view.sampleStart = 0;
    view.sampleEnd = double(samplesPerTrace);
    int plotWidth = 1600, plotHeight = 900; // a large, typical window size

    const int reps = 20;
    size_t totalPoints = 0;
    auto start = Clock::now();
    for (int r = 0; r < reps; ++r) {
        segy::WiggleLayout layout = segy::computeWiggleLayout(ctx, view, plotWidth, plotHeight);
        for (const auto& tr : layout.traces) totalPoints += tr.linePoints.size();
    }
    double ms = elapsedSeconds(start) / reps * 1000.0;
    std::printf("%-38s %10.3f ms   (%d traces drawn out of %lld in file, ~%zu points/frame)\n",
                "Wiggle layout", ms, plotWidth / segy::kMinPixelsPerTrace, (long long)traceCount,
                totalPoints / size_t(reps));

    file.close();
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

} // namespace

int main(int argc, char** argv) {
    std::printf("SEG-Y native rewrite -- microbenchmarks\n");
    std::printf("AVX2 available: %s\n", segy::cpuSupportsAvx2() ? "yes" : "no");
    std::printf("hardware_concurrency: %u\n", std::thread::hardware_concurrency());

    benchIbmDecode();
    benchPyramidBuild();
    benchHeaderSelectSort();
    benchWiggleLayout();

    if (argc > 1) {
        benchMmapThroughput(std::filesystem::path(argv[1]));
    } else {
        std::printf("\n(pass a SEG-Y file path as argv[1] to also benchmark mmap read throughput on a real file)\n");
    }
    return 0;
}
