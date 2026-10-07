// Minimal self-contained test harness (no external framework dependency --
// keeps the "portable, standalone" property of the rest of this project).
// Exit code is the number of failed checks (0 = success), so it plugs into
// any CI runner without extra glue.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <algorithm>
#include <random>
#include <vector>
#include <string>
#include <filesystem>

#include "segy_format.h"
#include "segy_decode.h"
#include "segy_pyramid.h"
#include "segy_select.h"
#include "segy_mmap.h"
#include "threadpool.h"
#include "synthetic_segy.h"
#include "renderer.h"
#include "chrome.h"
#include "axis_ticks.h"
#include "colormap.h"
#include "wiggle.h"
#include "analysis.h"
#include "bandpass.h"
#include "segy_writer.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_NEAR(a, b, eps) do { \
    ++g_checks; \
    double _a = (a), _b = (b), _eps = (eps); \
    if (!(std::fabs(_a - _b) <= _eps)) { \
        ++g_failures; \
        std::printf("  FAIL %s:%d: %s ~= %s : %.9g vs %.9g (eps %.3g)\n", __FILE__, __LINE__, #a, #b, _a, _b, _eps); \
    } \
} while (0)

void section(const char* name) { std::printf("-- %s\n", name); }

// ---------------------------------------------------------------------
void testIbmFloatKnownValues() {
    section("IBM float: known value round-trips");
    struct Case { uint32_t bits; double expected; };
    const Case cases[] = {
        {0x00000000u, 0.0},
        {0x80000000u, -0.0},
        {0x42100000u, 16.0},            // derivation worked example (see segy_decode.cpp)
        {0xC2100000u, -16.0},
        {0x41100000u, 1.0},             // 0x100000/2^24 * 16^(65-64) = 0.0625*16 = 1.0
    };
    for (const auto& c : cases) {
        float got = segy::ibmToIeeeScalar(c.bits);
        CHECK_NEAR(got, c.expected, std::max(1e-2, std::fabs(c.expected) * 1e-4));
    }
}

void testIbmFloatRoundTripViaEncoder() {
    section("IBM float: encode-then-decode round trip over a value sweep");
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> dist(-1.0e6, 1.0e6);
    for (int i = 0; i < 20000; ++i) {
        double v = dist(rng);
        uint32_t bits = segy::test::ieeeToIbmBitsReference(v);
        float got = segy::ibmToIeeeScalar(bits);
        // IBM hex float has ~6.5 decimal digits of precision (24-bit mantissa,
        // radix 16); allow generous relative tolerance.
        double tol = std::max(1e-3, std::fabs(v) * 2e-6);
        CHECK_NEAR(got, v, tol);
    }
}

void testAvx2MatchesScalar() {
    section("AVX2 IBM decode matches scalar reference exactly");
    if (!segy::cpuSupportsAvx2()) {
        std::printf("  SKIP: AVX2 not available on this CPU\n");
        return;
    }
    std::mt19937 rng(999);
    std::uniform_real_distribution<double> dist(-5.0e5, 5.0e5);
    for (int trial = 0; trial < 50; ++trial) {
        size_t n = 1 + (rng() % 4096); // exercise both full-vector and tail paths
        std::vector<uint8_t> be(n * 4);
        std::vector<float> expected(n), actual(n);
        for (size_t i = 0; i < n; ++i) {
            double v = (i % 97 == 0) ? 0.0 : dist(rng); // sprinkle exact zeros
            uint32_t bits = segy::test::ieeeToIbmBitsReference(v);
            be[i * 4 + 0] = uint8_t(bits >> 24);
            be[i * 4 + 1] = uint8_t(bits >> 16);
            be[i * 4 + 2] = uint8_t(bits >> 8);
            be[i * 4 + 3] = uint8_t(bits);
            expected[i] = segy::ibmToIeeeScalar(bits);
        }
        segy::ibmToIeeeAvx2(be.data(), actual.data(), n);
        bool allMatch = true;
        for (size_t i = 0; i < n; ++i) {
            uint32_t eb, ab;
            std::memcpy(&eb, &expected[i], 4);
            std::memcpy(&ab, &actual[i], 4);
            if (eb != ab) { allMatch = false; break; }
        }
        CHECK(allMatch);
    }
}

void testHeaderParsing() {
    section("Binary/trace header field parsing at correct byte offsets");
    auto file = segy::test::makeSyntheticSegy(3, 10, segy::kIeeeFloat32,
                                               [](int64_t, int) { return 0.0; });
    segy::BinaryHeader bh = segy::parseBinaryHeader(file.bytes.data() + segy::kTextHeaderSize);
    CHECK(bh.samplesPerTrace == 10);
    CHECK(bh.formatCode == segy::kIeeeFloat32);
    CHECK(bh.sampleIntervalUs == 1000);

    for (int64_t t = 0; t < file.traceCount; ++t) {
        const uint8_t* traceBase = file.bytes.data() + segy::kHeaderTotalSize + size_t(t) * file.traceStrideBytes;
        segy::TraceHeader th = segy::parseTraceHeader(traceBase);
        CHECK(th.traceSequenceLine == t + 1);
        CHECK(th.traceNumber == t + 1);
    }
}

void testEbcdicDecode() {
    section("EBCDIC text header decode");
    std::vector<uint8_t> text(segy::kTextHeaderSize, 0x40); // EBCDIC space
    // "C1" line: EBCDIC 'C'=0xC3, '1'=0xF1 -> ASCII 'C','1'
    text[0] = 0xC3; text[1] = 0xF1;
    std::string decoded = segy::decodeEbcdicText(text.data());
    CHECK(decoded.size() == segy::kTextHeaderSize);
    CHECK(decoded[0] == 'C');
    CHECK(decoded[1] == '1');
    CHECK(decoded[2] == ' ');
}

void testEbcdicDecodeDetectsAlreadyAsciiHeader() {
    section("EBCDIC text header decode: auto-detects an already-ASCII header instead of mangling it");
    // Some SEG-Y writers use plain ASCII text headers despite the format's
    // EBCDIC convention -- mostly space-padded (0x20, unlike EBCDIC's 0x40)
    // with a real line of readable text at the start.
    std::string line = "C 1 CLIENT Example Co.   COMPANY Example Seismic   LINE No. 1234";
    std::vector<uint8_t> text(segy::kTextHeaderSize, ' ');
    std::memcpy(text.data(), line.data(), line.size());
    std::string decoded = segy::decodeEbcdicText(text.data());
    CHECK(decoded.size() == segy::kTextHeaderSize);
    CHECK(decoded.compare(0, line.size(), line) == 0);
    CHECK(decoded[line.size()] == ' ');
}

void testDecodeSamplesAllFormats() {
    section("decodeSamples: IEEE float32 / int32 / int16 / int8");
    {
        auto f = segy::test::makeSyntheticSegy(1, 5, segy::kIeeeFloat32,
                                                [](int64_t, int s) { return s - 2.5; });
        std::vector<float> out(5);
        const uint8_t* sampleBase = f.bytes.data() + segy::kHeaderTotalSize + segy::kTraceHeaderSize;
        segy::decodeSamples(sampleBase, out.data(), 5, segy::kIeeeFloat32);
        for (int s = 0; s < 5; ++s) CHECK_NEAR(out[s], s - 2.5, 1e-6);
    }
    {
        auto f = segy::test::makeSyntheticSegy(1, 4, segy::kInt32,
                                                [](int64_t, int s) { return (s - 2) * 1000; });
        std::vector<float> out(4);
        const uint8_t* sampleBase = f.bytes.data() + segy::kHeaderTotalSize + segy::kTraceHeaderSize;
        segy::decodeSamples(sampleBase, out.data(), 4, segy::kInt32);
        for (int s = 0; s < 4; ++s) CHECK_NEAR(out[s], (s - 2) * 1000, 1e-6);
    }
    {
        auto f = segy::test::makeSyntheticSegy(1, 4, segy::kInt16,
                                                [](int64_t, int s) { return (s - 2) * 100; });
        std::vector<float> out(4);
        const uint8_t* sampleBase = f.bytes.data() + segy::kHeaderTotalSize + segy::kTraceHeaderSize;
        segy::decodeSamples(sampleBase, out.data(), 4, segy::kInt16);
        for (int s = 0; s < 4; ++s) CHECK_NEAR(out[s], (s - 2) * 100, 1e-6);
    }
    {
        auto f = segy::test::makeSyntheticSegy(1, 4, segy::kInt8,
                                                [](int64_t, int s) { return s - 2; });
        std::vector<float> out(4);
        const uint8_t* sampleBase = f.bytes.data() + segy::kHeaderTotalSize + segy::kTraceHeaderSize;
        segy::decodeSamples(sampleBase, out.data(), 4, segy::kInt8);
        for (int s = 0; s < 4; ++s) CHECK_NEAR(out[s], s - 2, 1e-6);
    }
}

// Brute-force ground truth for a pyramid block, decoding straight from the
// synthetic file bytes independent of the production decode path's
// internals (it still calls decodeSamples, which is itself covered by the
// tests above, so this checks the *reduction* logic, not decoding).
void bruteForceBlockStats(const segy::test::SyntheticFile& f, int64_t traceStart, int64_t traceEnd,
                           int sampleStart, int sampleEnd, float* mn, float* mx, float* mean, int32_t* count) {
    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();
    double sum = 0.0;
    int32_t cnt = 0;
    std::vector<float> buf(size_t(f.samplesPerTrace));
    for (int64_t t = traceStart; t < traceEnd; ++t) {
        const uint8_t* sampleBase = f.bytes.data() + segy::kHeaderTotalSize + size_t(t) * f.traceStrideBytes + segy::kTraceHeaderSize;
        segy::decodeSamples(sampleBase, buf.data(), size_t(f.samplesPerTrace), f.formatCode);
        for (int s = sampleStart; s < sampleEnd; ++s) {
            lo = std::min(lo, buf[size_t(s)]);
            hi = std::max(hi, buf[size_t(s)]);
            sum += buf[size_t(s)];
            ++cnt;
        }
    }
    *mn = lo; *mx = hi; *count = cnt;
    *mean = cnt > 0 ? float(sum / cnt) : 0.0f;
}

void testPyramidFinestLevel(bool ragged) {
    section(ragged ? "Pyramid: finest level matches brute force (ragged edges)"
                    : "Pyramid: finest level matches brute force (exact multiples)");
    int64_t traceCount = ragged ? 37 : 32;
    int samplesPerTrace = ragged ? 53 : 64;
    int blockFactor = 8;

    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIbmFloat32,
                                            [](int64_t t, int s) { return std::sin(double(t) * 0.3) * 1000.0 + s; });

    segy::ThreadPool pool(4);
    segy::BuildParams bp;
    bp.fileBase = f.bytes.data();
    bp.fileSize = f.bytes.size();
    bp.traceCount = traceCount;
    bp.samplesPerTrace = samplesPerTrace;
    bp.formatCode = segy::kIbmFloat32;
    bp.traceStrideBytes = f.traceStrideBytes;
    bp.blockFactor = blockFactor;

    segy::Pyramid pyramid = segy::buildPyramid(bp, pool);
    CHECK(!pyramid.levels.empty());
    const segy::PyramidLevel& level0 = pyramid.levels.front();
    CHECK(level0.traceBlocks == int((traceCount + blockFactor - 1) / blockFactor));
    CHECK(level0.sampleBlocks == int((samplesPerTrace + blockFactor - 1) / blockFactor));

    for (int tb = 0; tb < level0.traceBlocks; ++tb) {
        for (int sb = 0; sb < level0.sampleBlocks; ++sb) {
            int64_t traceStart = int64_t(tb) * blockFactor;
            int64_t traceEnd = std::min<int64_t>(traceCount, traceStart + blockFactor);
            int sampleStart = sb * blockFactor;
            int sampleEnd = std::min(samplesPerTrace, sampleStart + blockFactor);
            float mn, mx, mean; int32_t cnt;
            bruteForceBlockStats(f, traceStart, traceEnd, sampleStart, sampleEnd, &mn, &mx, &mean, &cnt);
            size_t idx = level0.index(tb, sb);
            CHECK(level0.count[idx] == cnt);
            CHECK_NEAR(level0.minv[idx], mn, 1e-2);
            CHECK_NEAR(level0.maxv[idx], mx, 1e-2);
            CHECK_NEAR(level0.mean[idx], mean, 1e-2);
        }
    }
}

void testPyramidCoarseLevelsExact() {
    section("Pyramid: coarser levels' min/max/mean are exact vs. brute force over raw data");
    int64_t traceCount = 130; // not a power of the block factor -> exercises ragged coarsening too
    int samplesPerTrace = 70;
    int blockFactor = 4;

    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIeeeFloat32,
                                            [](int64_t t, int s) { return double((t * 31 + s * 7) % 2001) - 1000.0; });

    segy::ThreadPool pool(4);
    segy::BuildParams bp;
    bp.fileBase = f.bytes.data();
    bp.fileSize = f.bytes.size();
    bp.traceCount = traceCount;
    bp.samplesPerTrace = samplesPerTrace;
    bp.formatCode = segy::kIeeeFloat32;
    bp.traceStrideBytes = f.traceStrideBytes;
    bp.blockFactor = blockFactor;

    segy::Pyramid pyramid = segy::buildPyramid(bp, pool);
    CHECK(pyramid.levels.size() >= 2);

    // Check level 1 (second-finest) directly against brute force over raw
    // trace/sample ranges -- this validates that coarsening composes
    // correctly (min-of-mins, max-of-maxes, exact weighted mean) and that
    // ragged edges at the coarser level are handled.
    const segy::PyramidLevel& level1 = pyramid.levels[1];
    int64_t rawTraceFactor = level1.traceFactor;
    int64_t rawSampleFactor = level1.sampleFactor;
    for (int tb = 0; tb < level1.traceBlocks; ++tb) {
        for (int sb = 0; sb < level1.sampleBlocks; ++sb) {
            int64_t traceStart = int64_t(tb) * rawTraceFactor;
            int64_t traceEnd = std::min<int64_t>(traceCount, traceStart + rawTraceFactor);
            int sampleStart = int(sb * rawSampleFactor);
            int sampleEnd = int(std::min<int64_t>(samplesPerTrace, sampleStart + rawSampleFactor));
            float mn, mx, mean; int32_t cnt;
            bruteForceBlockStats(f, traceStart, traceEnd, sampleStart, sampleEnd, &mn, &mx, &mean, &cnt);
            size_t idx = level1.index(tb, sb);
            CHECK(level1.count[idx] == cnt);
            CHECK_NEAR(level1.minv[idx], mn, 1e-3);
            CHECK_NEAR(level1.maxv[idx], mx, 1e-3);
            CHECK_NEAR(level1.mean[idx], mean, 1e-3);
        }
    }

    // Global min/max (from the coarsest level) must equal the true global
    // min/max of the whole file.
    float trueMin, trueMax, trueMean; int32_t trueCnt;
    bruteForceBlockStats(f, 0, traceCount, 0, samplesPerTrace, &trueMin, &trueMax, &trueMean, &trueCnt);
    CHECK_NEAR(pyramid.globalMin, trueMin, 1e-3);
    CHECK_NEAR(pyramid.globalMax, trueMax, 1e-3);
}

void testEmptyAndDegenerateInputs() {
    section("Degenerate inputs: zero traces / zero samples don't crash");
    segy::ThreadPool pool(2);
    segy::BuildParams bp;
    bp.traceCount = 0;
    bp.samplesPerTrace = 0;
    segy::Pyramid p = segy::buildPyramid(bp, pool);
    CHECK(p.levels.empty());
}

// ---------------------------------------------------------------------
// Header-based select/sort (segy_select.h): scanTraceHeaders/selectTraces/
// sortTraceIndices, plus the segy_pyramid.h traceIndexMap seam they feed
// into -- see native/README.md for the design rationale (why this is a
// pyramid-rebuild-over-an-index-list rather than a second copy of the
// file).

// Overwrites the CDP field (bytes 21-24) makeSyntheticSegy itself doesn't
// touch, with a distinct, easily-invertible value per trace -- lets tests
// pick an exact expected subset/order from the formula alone.
void setSyntheticCdp(segy::test::SyntheticFile& f, int64_t traceCount, const std::function<int32_t(int64_t)>& cdpFor) {
    for (int64_t t = 0; t < traceCount; ++t) {
        size_t off = segy::kHeaderTotalSize + size_t(t) * f.traceStrideBytes + 20; // CDP, bytes 21-24
        segy::test::writeI32BE(f.bytes, off, cdpFor(t));
    }
}

void testSelectScanTraceHeadersMatchesParseTraceHeader() {
    section("Select: scanTraceHeaders matches parseTraceHeader for every trace");
    int64_t traceCount = 53; // deliberately not a multiple of any thread count
    int samplesPerTrace = 10;
    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIeeeFloat32,
                                            [](int64_t, int s) { return double(s); });
    setSyntheticCdp(f, traceCount, [](int64_t t) { return int32_t(1000 + t * 3); });

    segy::ThreadPool pool(4);
    segy::TraceHeaderColumns columns = segy::scanTraceHeaders(f.bytes.data(), traceCount, f.traceStrideBytes, pool);
    CHECK(columns.traceCount() == traceCount);

    for (int64_t t = 0; t < traceCount; ++t) {
        const uint8_t* headerBase = f.bytes.data() + segy::kHeaderTotalSize + size_t(t) * f.traceStrideBytes;
        segy::TraceHeader expected = segy::parseTraceHeader(headerBase);
        CHECK(columns.traceSequenceLine[size_t(t)] == expected.traceSequenceLine);
        CHECK(columns.traceNumber[size_t(t)] == expected.traceNumber);
        CHECK(columns.cdp[size_t(t)] == expected.cdp);
        CHECK(columns.cdp[size_t(t)] == 1000 + t * 3);
    }
}

void testSelectTracesFiltersByRangeAndIncrement() {
    section("Select: min/max/inc filtering, and AND across multiple criteria");
    int64_t traceCount = 40;
    int samplesPerTrace = 4;
    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIeeeFloat32,
                                            [](int64_t, int) { return 0.0; });
    // cdp cycles 0,10,20,...,390; traceNumber (already written by
    // makeSyntheticSegy) is t+1 -- two independent, verifiable idents.
    setSyntheticCdp(f, traceCount, [](int64_t t) { return int32_t(t * 10); });

    segy::ThreadPool pool(4);
    segy::TraceHeaderColumns columns = segy::scanTraceHeaders(f.bytes.data(), traceCount, f.traceStrideBytes, pool);

    // Empty criteria: every trace, in file order.
    std::vector<int64_t> all = segy::selectTraces(columns, {});
    CHECK(all.size() == size_t(traceCount));
    for (int64_t t = 0; t < traceCount; ++t) CHECK(all[size_t(t)] == t);

    // cdp in [100, 300], stride 20 over cdp's own value domain (min=100) ->
    // cdp in {100,120,140,...,300} -> t in {10,12,14,...,30}.
    std::vector<segy::IdentCriterion> incCriteria = {{segy::IdentField::Cdp, 100, 300, 20}};
    std::vector<int64_t> incSelected = segy::selectTraces(columns, incCriteria);
    std::vector<int64_t> expectedInc;
    for (int64_t t = 10; t <= 30; t += 2) expectedInc.push_back(t);
    CHECK(incSelected == expectedInc);

    // Two criteria AND'd: cdp in [0,150] (t <= 15) AND traceNumber (t+1) in
    // [5,10] (t in [4,9]) -> t in [4,9].
    std::vector<segy::IdentCriterion> andCriteria = {
        {segy::IdentField::Cdp, 0, 150, 1},
        {segy::IdentField::TraceNumber, 5, 10, 1},
    };
    std::vector<int64_t> andSelected = segy::selectTraces(columns, andCriteria);
    std::vector<int64_t> expectedAnd = {4, 5, 6, 7, 8, 9};
    CHECK(andSelected == expectedAnd);
}

void testSortTraceIndicesMultiKeyStable() {
    section("Select: sortTraceIndices -- multi-key priority order, stable on ties");
    int64_t traceCount = 6;
    int samplesPerTrace = 2;
    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIeeeFloat32,
                                            [](int64_t, int) { return 0.0; });
    // cdp: two traces share each value, so sorting by cdp alone leaves real
    // ties for traceNumber (t+1, already unique/ascending) to break.
    setSyntheticCdp(f, traceCount, [](int64_t t) { return int32_t(t / 2); }); // 0,0,1,1,2,2

    segy::ThreadPool pool(2);
    segy::TraceHeaderColumns columns = segy::scanTraceHeaders(f.bytes.data(), traceCount, f.traceStrideBytes, pool);

    // Start deliberately out of order so a no-op sort would be detectable.
    std::vector<int64_t> indices = {5, 3, 1, 4, 2, 0};
    segy::sortTraceIndices(indices, columns, {segy::IdentField::Cdp, segy::IdentField::TraceNumber});
    // cdp ascending (0,0,1,1,2,2), ties broken by traceNumber ascending ->
    // original file order, since that's exactly how cdp/traceNumber were
    // constructed here.
    std::vector<int64_t> expected = {0, 1, 2, 3, 4, 5};
    CHECK(indices == expected);

    // Empty sort key list is a documented no-op.
    std::vector<int64_t> untouched = {5, 3, 1, 4, 2, 0};
    segy::sortTraceIndices(untouched, columns, {});
    CHECK(untouched == std::vector<int64_t>({5, 3, 1, 4, 2, 0}));
}

void testPyramidTraceIndexMapMatchesDirectSubsetBuild() {
    section("Pyramid: traceIndexMap over a filtered/reordered subset matches building that subset directly");
    int64_t sourceTraceCount = 24;
    int samplesPerTrace = 16;
    int blockFactor = 4;
    auto formula = [](int64_t t, int s) { return std::sin(double(t) * 0.7 + double(s) * 0.05) * 750.0; };
    auto source = segy::test::makeSyntheticSegy(sourceTraceCount, samplesPerTrace, segy::kIbmFloat32, formula);

    // An arbitrary, non-contiguous, non-monotonic permutation/subset of the
    // source file's traces -- deliberately not "every trace in order" and
    // not "a contiguous range", to actually exercise the indirection.
    std::vector<int64_t> selected = {21, 3, 3, 17, 0, 9, 14};

    // The independently-built "expected" file: trace t's samples are
    // exactly source trace selected[t]'s samples, so a normal (no map)
    // pyramid build over THIS file is the ground truth for what
    // traceIndexMap is supposed to produce from `source` directly.
    auto expectedFile = segy::test::makeSyntheticSegy(
        int64_t(selected.size()), samplesPerTrace, segy::kIbmFloat32,
        [&](int64_t t, int s) { return formula(selected[size_t(t)], s); });

    segy::ThreadPool pool(4);

    segy::BuildParams mappedParams;
    mappedParams.fileBase = source.bytes.data();
    mappedParams.fileSize = source.bytes.size();
    mappedParams.traceCount = int64_t(selected.size());
    mappedParams.samplesPerTrace = samplesPerTrace;
    mappedParams.formatCode = segy::kIbmFloat32;
    mappedParams.traceStrideBytes = source.traceStrideBytes;
    mappedParams.blockFactor = blockFactor;
    mappedParams.traceIndexMap = selected.data();
    segy::Pyramid mappedPyramid = segy::buildPyramid(mappedParams, pool);

    segy::BuildParams directParams;
    directParams.fileBase = expectedFile.bytes.data();
    directParams.fileSize = expectedFile.bytes.size();
    directParams.traceCount = int64_t(selected.size());
    directParams.samplesPerTrace = samplesPerTrace;
    directParams.formatCode = segy::kIbmFloat32;
    directParams.traceStrideBytes = expectedFile.traceStrideBytes;
    directParams.blockFactor = blockFactor;
    segy::Pyramid directPyramid = segy::buildPyramid(directParams, pool);

    CHECK(mappedPyramid.levels.size() == directPyramid.levels.size());
    for (size_t lvl = 0; lvl < mappedPyramid.levels.size(); ++lvl) {
        const segy::PyramidLevel& a = mappedPyramid.levels[lvl];
        const segy::PyramidLevel& b = directPyramid.levels[lvl];
        CHECK(a.traceBlocks == b.traceBlocks);
        CHECK(a.sampleBlocks == b.sampleBlocks);
        for (size_t i = 0; i < a.minv.size(); ++i) {
            CHECK(a.count[i] == b.count[i]);
            CHECK_NEAR(a.minv[i], b.minv[i], 1e-3);
            CHECK_NEAR(a.maxv[i], b.maxv[i], 1e-3);
            CHECK_NEAR(a.mean[i], b.mean[i], 1e-3);
        }
    }
    CHECK_NEAR(mappedPyramid.globalMin, directPyramid.globalMin, 1e-3);
    CHECK_NEAR(mappedPyramid.globalMax, directPyramid.globalMax, 1e-3);

    // A null map (the default) must still behave exactly as before --
    // regression guard for the one added branch in the hot loop.
    segy::BuildParams identityParams = directParams;
    identityParams.fileBase = source.bytes.data();
    identityParams.fileSize = source.bytes.size();
    identityParams.traceCount = sourceTraceCount;
    identityParams.traceStrideBytes = source.traceStrideBytes;
    segy::Pyramid identityPyramid = segy::buildPyramid(identityParams, pool);
    CHECK(identityPyramid.traceCount == sourceTraceCount);
    CHECK(!identityPyramid.levels.empty());
}

// ---------------------------------------------------------------------
// Generic "nice" axis ticks (axis_ticks.h) -- the shared building block
// chrome.cpp's own Time-axis tests below exercise indirectly (and
// exhaustively: max line count, overlap avoidance, unit switching) through
// renderFrameWithChrome()'s full contract. This is a direct, minimal check
// of the function itself, now that it's also called straight from the
// Histogram/Spectrum plot widgets, not just from chrome.cpp.
void testAxisTicksBasicContract() {
    section("Axis ticks: max line count, value range, and pixel-position mapping");
    auto noOverlapCheck = [](const std::string&) { return 0; }; // widest label never overlaps
    std::vector<segy::AxisTick> ticks = segy::computeNiceAxisTicks(-42.0, 137.0, 800, 11, noOverlapCheck);
    CHECK(!ticks.empty());
    CHECK(ticks.size() <= 11);
    for (const segy::AxisTick& tk : ticks) {
        CHECK(tk.value >= -42.0 - 1e-6 && tk.value <= 137.0 + 1e-6);
        CHECK(tk.pixelPos >= 0 && tk.pixelPos <= 800);
    }
    // Consecutive ticks are evenly spaced (same "nice" interval throughout).
    for (size_t i = 2; i < ticks.size(); ++i) {
        double gapA = ticks[i - 1].value - ticks[i - 2].value;
        double gapB = ticks[i].value - ticks[i - 1].value;
        CHECK_NEAR(gapA, gapB, 1e-9);
    }

    // Degenerate inputs return empty rather than crashing.
    CHECK(segy::computeNiceAxisTicks(5.0, 5.0, 800, 11, nullptr).empty());  // zero range
    CHECK(segy::computeNiceAxisTicks(0.0, 100.0, 0, 11, nullptr).empty()); // zero pixel span
    CHECK(segy::computeNiceAxisTicks(0.0, 100.0, 800, 0, nullptr).empty()); // zero max lines

    // A measureWidth that always reports overlap forces maximal coarsening
    // down to a single-digit line count, not an infinite/unbounded interval.
    auto alwaysOverlap = [](const std::string&) { return 100000; };
    std::vector<segy::AxisTick> coarsened = segy::computeNiceAxisTicks(0.0, 100.0, 800, 11, alwaysOverlap);
    CHECK(coarsened.size() <= 2);
}

// ---------------------------------------------------------------------
// Color scales (colormap.h): the amplitude scale legend's right-click menu
// (viewer_qt.cpp) picks one of these by AppState::display.colorScale;
// renderer.cpp/chrome.cpp both go through applyColorScale() instead of
// calling a specific one directly.
void testColorScalesEndpointsAndZero() {
    section("Color scales: correct endpoint/zero colors for every scale, via the shared dispatcher");
    uint8_t r, g, b;

    // Red-White-Blue (the default/original): red at +1, white at 0, blue at -1.
    segy::applyColorScale(segy::ColorScale::RedWhiteBlue, 1.0f, &b, &g, &r);
    CHECK(r == 255 && g == 0 && b == 0);
    segy::applyColorScale(segy::ColorScale::RedWhiteBlue, 0.0f, &b, &g, &r);
    CHECK(r == 255 && g == 255 && b == 255);
    segy::applyColorScale(segy::ColorScale::RedWhiteBlue, -1.0f, &b, &g, &r);
    CHECK(r == 0 && g == 0 && b == 255);

    // Red-White-Black: same positive arm, black instead of blue negative.
    segy::applyColorScale(segy::ColorScale::RedWhiteBlack, 1.0f, &b, &g, &r);
    CHECK(r == 255 && g == 0 && b == 0);
    segy::applyColorScale(segy::ColorScale::RedWhiteBlack, 0.0f, &b, &g, &r);
    CHECK(r == 255 && g == 255 && b == 255);
    segy::applyColorScale(segy::ColorScale::RedWhiteBlack, -1.0f, &b, &g, &r);
    CHECK(r == 0 && g == 0 && b == 0);

    // Grayscale White->Black: linear across the whole range, not diverging
    // around a white zero -- midpoint is mid-gray, not white.
    segy::applyColorScale(segy::ColorScale::GrayscaleWhiteToBlack, -1.0f, &b, &g, &r);
    CHECK(r == 255 && g == 255 && b == 255);
    segy::applyColorScale(segy::ColorScale::GrayscaleWhiteToBlack, 1.0f, &b, &g, &r);
    CHECK(r == 0 && g == 0 && b == 0);
    segy::applyColorScale(segy::ColorScale::GrayscaleWhiteToBlack, 0.0f, &b, &g, &r);
    CHECK_NEAR(double(r), 127.5, 1.0);
    CHECK(r == g && g == b);

    // Grayscale Black->White: the exact reverse.
    segy::applyColorScale(segy::ColorScale::GrayscaleBlackToWhite, -1.0f, &b, &g, &r);
    CHECK(r == 0 && g == 0 && b == 0);
    segy::applyColorScale(segy::ColorScale::GrayscaleBlackToWhite, 1.0f, &b, &g, &r);
    CHECK(r == 255 && g == 255 && b == 255);

    // Yellow-Red-White-Black-Cyan: 5 stops, evenly spaced -- check all 5
    // exactly (t = -1, -0.5, 0, 0.5, 1).
    segy::applyColorScale(segy::ColorScale::YellowRedWhiteBlackCyan, -1.0f, &b, &g, &r);
    CHECK(r == 255 && g == 255 && b == 0); // yellow
    segy::applyColorScale(segy::ColorScale::YellowRedWhiteBlackCyan, -0.5f, &b, &g, &r);
    CHECK(r == 255 && g == 0 && b == 0); // red
    segy::applyColorScale(segy::ColorScale::YellowRedWhiteBlackCyan, 0.0f, &b, &g, &r);
    CHECK(r == 255 && g == 255 && b == 255); // white
    segy::applyColorScale(segy::ColorScale::YellowRedWhiteBlackCyan, 0.5f, &b, &g, &r);
    CHECK(r == 0 && g == 0 && b == 0); // black
    segy::applyColorScale(segy::ColorScale::YellowRedWhiteBlackCyan, 1.0f, &b, &g, &r);
    CHECK(r == 0 && g == 255 && b == 255); // cyan

    // Out-of-range input clamps rather than extrapolating/undefined-behaving.
    segy::applyColorScale(segy::ColorScale::RedWhiteBlue, 5.0f, &b, &g, &r);
    CHECK(r == 255 && g == 0 && b == 0);
    segy::applyColorScale(segy::ColorScale::RedWhiteBlue, -5.0f, &b, &g, &r);
    CHECK(r == 0 && g == 0 && b == 255);
}

// ---------------------------------------------------------------------
// Chrome (time scale / color bar / grid lines): builds a small synthetic
// file+pyramid and exercises renderFrameWithChrome() through its public
// contract -- tick count, unit selection, overlap avoidance, and the
// color bar's polarity -- rather than reaching into its internal "nice
// interval" sequence generator directly. A crude but proportional
// measureWidth mock (7px/char) is enough to exercise the overlap-avoidance
// branch meaningfully without needing a real font.
segy::RenderContext makeChromeTestContext(segy::Pyramid& pyramidOut, int64_t traceCount, int samplesPerTrace,
                                           int16_t sampleIntervalUs, segy::ThreadPool& pool) {
    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIeeeFloat32,
                                            [](int64_t t, int s) { return std::sin(double(t + s) * 0.05) * 1000.0; });
    segy::BuildParams bp;
    bp.fileBase = f.bytes.data();
    bp.fileSize = f.bytes.size();
    bp.traceCount = traceCount;
    bp.samplesPerTrace = samplesPerTrace;
    bp.formatCode = segy::kIeeeFloat32;
    bp.traceStrideBytes = f.traceStrideBytes;
    bp.blockFactor = 4;
    pyramidOut = segy::buildPyramid(bp, pool);

    segy::RenderContext ctx;
    ctx.file = nullptr; // forces the pyramid path regardless of visible-sample budget -- fine for testing chrome layout
    ctx.pyramid = &pyramidOut;
    ctx.binHeader.samplesPerTrace = int16_t(samplesPerTrace);
    ctx.binHeader.sampleIntervalUs = sampleIntervalUs;
    ctx.binHeader.formatCode = segy::kIeeeFloat32;
    ctx.traceCount = traceCount;
    ctx.traceStrideBytes = f.traceStrideBytes;
    ctx.pool = &pool;
    return ctx;
}

segy::TextMetrics crudeMetrics() {
    segy::TextMetrics m;
    m.fontHeightPx = 12;
    m.measureWidth = [](const std::string& s) { return int(s.size()) * 7; };
    return m;
}

void testChromeTickCountNeverExceedsMax() {
    section("Chrome: never more than 11 grid lines/ticks, across many view sizes");
    segy::ThreadPool pool(2);
    segy::Pyramid pyramid;
    segy::RenderContext ctx = makeChromeTestContext(pyramid, 500, 2000, 1000, pool); // 1000us -> 1ms/sample, 2000ms record
    segy::DisplaySettings settings;
    std::vector<uint32_t> canvas(400 * 600);

    for (int canvasHeight : {50, 100, 300, 600, 900}) {
        for (double sampleSpan : {50.0, 200.0, 500.0, 2000.0}) {
            segy::ViewRange view;
            view.traceStart = 0;
            view.traceEnd = 400;
            view.sampleStart = 0;
            view.sampleEnd = sampleSpan;
            canvas.assign(size_t(300) * size_t(canvasHeight), 0);
            segy::ChromeLayout chrome =
                segy::renderFrameWithChrome(ctx, view, settings, crudeMetrics(), canvas.data(), 300, canvasHeight);
            CHECK(chrome.ticks.size() <= 11);
        }
    }
}

void testChromeOverlapAvoidance() {
    section("Chrome: coarsens the interval rather than let rotated labels overlap");
    segy::ThreadPool pool(2);
    segy::Pyramid pyramid;
    segy::RenderContext ctx = makeChromeTestContext(pyramid, 500, 2000, 1000, pool);
    segy::DisplaySettings settings;
    segy::TextMetrics metrics = crudeMetrics();

    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = 400;
    view.sampleStart = 0;
    view.sampleEnd = 2000; // 2000ms visible

    // A very short canvas forces tight tick spacing -- the finest interval
    // that would satisfy "<=11 lines" alone would put labels closer
    // together than their own rendered width, so the algorithm must back
    // off to a coarser interval.
    int canvasHeight = 40;
    std::vector<uint32_t> canvas(size_t(300) * size_t(canvasHeight), 0);
    segy::ChromeLayout chrome =
        segy::renderFrameWithChrome(ctx, view, settings, metrics, canvas.data(), 300, canvasHeight);

    for (size_t i = 0; i + 1 < chrome.ticks.size(); ++i) {
        int gapPx = std::abs(chrome.ticks[i + 1].pixelY - chrome.ticks[i].pixelY);
        int neededPx = metrics.measureWidth(chrome.ticks[i].label) + 4;
        CHECK(gapPx >= neededPx);
    }
}

void testChromeTimeUnitSelection() {
    section("Chrome: time axis unit switches ms -> s for very long records, ms -> \xC2\xB5s for very short ones");
    segy::ThreadPool pool(2);
    segy::DisplaySettings settings;
    segy::TextMetrics metrics = crudeMetrics();
    std::vector<uint32_t> canvas(size_t(300) * 200, 0);
    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = 10;

    {
        // Both samplesPerTrace and sampleIntervalUs are int16_t in a real
        // SEG-Y binary header (max 32767 each), so this -- not an
        // arbitrarily large sample count -- is the realistic way to reach
        // a multi-digit-second record: 32767 samples * 32767us/sample ~=
        // 1,073,676 ms, just over the 1,000,000ms/7-digit threshold.
        segy::Pyramid pyramid;
        segy::RenderContext ctx = makeChromeTestContext(pyramid, 10, 32767, 32767, pool);
        view.sampleStart = 0;
        view.sampleEnd = 32767;
        auto chrome = segy::renderFrameWithChrome(ctx, view, settings, metrics, canvas.data(), 300, 200);
        CHECK(chrome.axisTitle == "Time (s)");
    }
    {  // 100 samples * 2us = 200us total record -- well under 1ms
        segy::Pyramid pyramid;
        segy::RenderContext ctx = makeChromeTestContext(pyramid, 10, 100, 2, pool);
        view.sampleStart = 0;
        view.sampleEnd = 100;
        auto chrome = segy::renderFrameWithChrome(ctx, view, settings, metrics, canvas.data(), 300, 200);
        CHECK(chrome.axisTitle == "Time (\xC2\xB5s)");
    }
    {  // 1000 samples * 1000us = 1,000,000us = 1000ms -- ordinary range, stays in ms
        segy::Pyramid pyramid;
        segy::RenderContext ctx = makeChromeTestContext(pyramid, 10, 1000, 1000, pool);
        view.sampleStart = 0;
        view.sampleEnd = 1000;
        auto chrome = segy::renderFrameWithChrome(ctx, view, settings, metrics, canvas.data(), 300, 200);
        CHECK(chrome.axisTitle == "Time (ms)");
    }
}

void testChromeColorBarPolarity() {
    section("Chrome: color bar is positive/red at the top, negative/blue at the bottom");
    segy::ThreadPool pool(2);
    segy::Pyramid pyramid;
    segy::RenderContext ctx = makeChromeTestContext(pyramid, 200, 500, 1000, pool);
    segy::DisplaySettings settings;
    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = 200;
    view.sampleStart = 0;
    view.sampleEnd = 500;

    int canvasWidth = 300, canvasHeight = 200;
    std::vector<uint32_t> canvas(size_t(canvasWidth) * size_t(canvasHeight), 0);
    segy::ChromeLayout chrome =
        segy::renderFrameWithChrome(ctx, view, settings, crudeMetrics(), canvas.data(), canvasWidth, canvasHeight);
    CHECK(chrome.colorBarWidth > 0);

    uint32_t topPixel = canvas[0];
    uint32_t bottomPixel = canvas[size_t(canvasHeight - 1) * size_t(canvasWidth)];
    int topR = int(uint8_t(topPixel >> 16)), topB = int(uint8_t(topPixel));
    int botR = int(uint8_t(bottomPixel >> 16)), botB = int(uint8_t(bottomPixel));
    CHECK(topR > topB);  // top of the color bar: positive amplitude -> red-dominant
    CHECK(botB > botR);  // bottom: negative amplitude -> blue-dominant
}

void testChromeSettingsToggleOffMargins() {
    section("Chrome: disabling color bar / time scale collapses their margins to zero width");
    segy::ThreadPool pool(2);
    segy::Pyramid pyramid;
    segy::RenderContext ctx = makeChromeTestContext(pyramid, 100, 300, 1000, pool);
    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = 100;
    view.sampleStart = 0;
    view.sampleEnd = 300;

    segy::DisplaySettings allOff;
    allOff.showColorBar = false;
    allOff.showTimeScale = false;
    allOff.showGridLines = false;
    std::vector<uint32_t> canvas(size_t(300) * 200, 0);
    segy::ChromeLayout chrome =
        segy::renderFrameWithChrome(ctx, view, allOff, crudeMetrics(), canvas.data(), 300, 200);
    CHECK(chrome.colorBarWidth == 0);
    CHECK(chrome.leftScaleWidth == 0);
    CHECK(chrome.rightScaleWidth == 0);
    CHECK(chrome.plotX == 0);
    CHECK(chrome.plotWidth == 300);
}

// ---------------------------------------------------------------------
// Wiggle display geometry (viewer/wiggle.cpp). Unlike the chrome tests
// above, computeWiggleLayout() always exact-decodes from a real mmap'd
// file (it never uses the pyramid for its own data), so this needs an
// actual MappedFile -- built by writing a synthetic file to a real temp
// file and opening it, rather than the ctx.file=nullptr shortcut the
// chrome tests use.
// Self-referential (ctx.file points at this same struct's file member), so
// it must always be constructed in place -- copying or moving it would
// leave ctx.file dangling (pointing at the old object's address, not the
// new one's). Deleted explicitly rather than left as an easy-to-hit trap.
struct WiggleTestFixture {
    segy::MappedFile file;
    segy::Pyramid pyramid;
    segy::RenderContext ctx;
    std::filesystem::path tempPath;

    WiggleTestFixture() = default;
    WiggleTestFixture(const WiggleTestFixture&) = delete;
    WiggleTestFixture(WiggleTestFixture&&) = delete;

    ~WiggleTestFixture() {
        file.close();
        std::error_code ec;
        std::filesystem::remove(tempPath, ec); // best-effort; Windows can't delete a still-mapped file
    }
};

void makeWiggleTestContext(WiggleTestFixture& fx, int64_t traceCount, int samplesPerTrace, segy::ThreadPool& pool,
                            const std::function<double(int64_t, int)>& sampleValue) {
    static int counter = 0;
    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIeeeFloat32, sampleValue);

    fx.tempPath =
        std::filesystem::temp_directory_path() / ("segytest_wiggle_" + std::to_string(counter++) + ".sgy");
    FILE* out = std::fopen(fx.tempPath.string().c_str(), "wb");
    std::fwrite(f.bytes.data(), 1, f.bytes.size(), out);
    std::fclose(out);

    std::string err;
    fx.file.open(fx.tempPath, &err);

    segy::BuildParams bp;
    bp.fileBase = fx.file.data();
    bp.fileSize = fx.file.size();
    bp.traceCount = traceCount;
    bp.samplesPerTrace = samplesPerTrace;
    bp.formatCode = segy::kIeeeFloat32;
    bp.traceStrideBytes = f.traceStrideBytes;
    bp.blockFactor = 4;
    fx.pyramid = segy::buildPyramid(bp, pool);

    fx.ctx.file = &fx.file;
    fx.ctx.pyramid = &fx.pyramid;
    fx.ctx.binHeader.samplesPerTrace = int16_t(samplesPerTrace);
    fx.ctx.binHeader.sampleIntervalUs = 1000;
    fx.ctx.binHeader.formatCode = segy::kIeeeFloat32;
    fx.ctx.traceCount = traceCount;
    fx.ctx.traceStrideBytes = f.traceStrideBytes;
    fx.ctx.pool = &pool;
}

void testWiggleTraceCountBounded() {
    section("Wiggle: selected trace count never exceeds plotWidth/kMinPixelsPerTrace");
    segy::ThreadPool pool(2);
    WiggleTestFixture fx;
    makeWiggleTestContext(fx, 5000, 200, pool,
                           [](int64_t t, int s) { return std::sin(double(t + s) * 0.05) * 500.0; });
    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = 5000;
    view.sampleStart = 0;
    view.sampleEnd = 200;
    for (int plotWidth : {50, 137, 400, 1000}) {
        segy::WiggleLayout layout = segy::computeWiggleLayout(fx.ctx, view, plotWidth, 300);
        int maxAllowed = std::max(1, plotWidth / segy::kMinPixelsPerTrace);
        CHECK(int(layout.traces.size()) <= maxAllowed);
        CHECK(!layout.traces.empty());
    }
}

void testWiggleEnvelopePointCountBounded() {
    section("Wiggle: per-trace point count stays bounded even with a huge visible sample range");
    segy::ThreadPool pool(2);
    WiggleTestFixture fx;
    makeWiggleTestContext(fx, 50, 20000, pool,
                           [](int64_t t, int s) { return std::sin(double(s) * 0.01 + double(t)) * 500.0; });
    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = 50;
    view.sampleStart = 0;
    view.sampleEnd = 20000; // way more samples than plotHeight
    int plotHeight = 400;
    segy::WiggleLayout layout = segy::computeWiggleLayout(fx.ctx, view, 200, plotHeight);
    CHECK(!layout.traces.empty());
    for (const auto& tr : layout.traces) {
        CHECK(int(tr.linePoints.size()) <= plotHeight * 2 + 4); // envelope: 2 vertices/row, small slack
    }
}

void testWiggleNoEnvelopeGivesOnePointPerSample() {
    section("Wiggle: not-oversampled view gives exactly one vertex per raw sample");
    segy::ThreadPool pool(2);
    WiggleTestFixture fx;
    makeWiggleTestContext(fx, 20, 150, pool,
                           [](int64_t t, int s) { return std::sin(double(s) * 0.1 + double(t)) * 300.0; });
    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = 20;
    view.sampleStart = 0;
    view.sampleEnd = 150; // 150 samples visible, plotHeight=400 -> not oversampled
    segy::WiggleLayout layout = segy::computeWiggleLayout(fx.ctx, view, 100, 400);
    CHECK(!layout.traces.empty());
    for (const auto& tr : layout.traces) {
        CHECK(int(tr.linePoints.size()) == 150);
    }
}

void testWigglePointsWithinPlotBounds() {
    section("Wiggle: all emitted points stay within plot-local pixel bounds");
    segy::ThreadPool pool(2);
    WiggleTestFixture fx;
    makeWiggleTestContext(fx, 300, 400, pool,
                           [](int64_t t, int s) { return std::sin(double(t + s) * 0.03) * 1000.0; });
    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = 300;
    view.sampleStart = 0;
    view.sampleEnd = 400;
    int plotWidth = 250, plotHeight = 300;
    segy::WiggleLayout layout = segy::computeWiggleLayout(fx.ctx, view, plotWidth, plotHeight);
    CHECK(!layout.traces.empty());
    for (const auto& tr : layout.traces) {
        for (const auto& p : tr.linePoints) {
            CHECK(p.y >= -1.0f && p.y <= float(plotHeight) + 1.0f);
            // A wiggle trace is allowed to overlap its neighbors' lanes for
            // large excursions (same as any real wiggle plot); this is a
            // sanity bound against a runaway/garbage coordinate, not a
            // tight one.
            CHECK(std::fabs(p.x - tr.baselineX) < float(plotWidth));
        }
    }
}

void testWiggleEnvelopeMatchesBruteForceMinMax() {
    section("Wiggle: envelope reduction preserves true min/max per row vs. brute force");
    segy::ThreadPool pool(2);
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-1000.0, 1000.0);
    std::vector<double> raw(3000);
    for (auto& v : raw) v = dist(rng);
    int64_t traceCount = 1;
    int samplesPerTrace = int(raw.size());
    WiggleTestFixture fx;
    makeWiggleTestContext(fx, traceCount, samplesPerTrace, pool, [&](int64_t, int s) { return raw[size_t(s)]; });
    segy::ViewRange view;
    view.traceStart = 0;
    view.traceEnd = double(traceCount);
    view.sampleStart = 0;
    view.sampleEnd = double(samplesPerTrace);
    int plotWidth = 60;
    int plotHeight = 100;
    segy::WiggleLayout layout = segy::computeWiggleLayout(fx.ctx, view, plotWidth, plotHeight);
    CHECK(layout.traces.size() == 1);
    const segy::WiggleTrace& tr = layout.traces.front();
    CHECK(int(tr.linePoints.size()) == plotHeight * 2);

    float clip = std::max(std::fabs(fx.pyramid.globalMin), std::fabs(fx.pyramid.globalMax));
    float halfLane = float(plotWidth) / 2.0f; // exactly 1 selected trace
    double sampleSpan = view.sampleEnd - view.sampleStart;
    for (int y = 0; y < plotHeight; ++y) {
        double rowStart = view.sampleStart + double(y) / plotHeight * sampleSpan;
        double rowEnd = view.sampleStart + double(y + 1) / plotHeight * sampleSpan;
        int rs0 = std::clamp(int(std::floor(rowStart)), 0, samplesPerTrace - 1);
        int rs1 = std::clamp(int(std::ceil(rowEnd)), rs0, samplesPerTrace - 1);
        double lo = raw[size_t(rs0)], hi = raw[size_t(rs0)];
        for (int s = rs0; s <= rs1; ++s) {
            lo = std::min(lo, raw[size_t(s)]);
            hi = std::max(hi, raw[size_t(s)]);
        }
        float expLo = tr.baselineX + float(lo / clip) * halfLane;
        float expHi = tr.baselineX + float(hi / clip) * halfLane;
        float gotA = tr.linePoints[size_t(y) * 2].x;
        float gotB = tr.linePoints[size_t(y) * 2 + 1].x;
        CHECK_NEAR(std::min(gotA, gotB), std::min(expLo, expHi), 0.01);
        CHECK_NEAR(std::max(gotA, gotB), std::max(expLo, expHi), 0.01);
    }
}

// ---------------------------------------------------------------------
// Amplitude histogram / spectrum (viewer/analysis.cpp), backing the
// toolbar's Histogram/Spectrum dialogs. Reuses WiggleTestFixture since it
// already builds a real mmap'd synthetic file + RenderContext, which
// computeHistogram()/computeSpectrum() need for the same reason
// computeWiggleLayout() does (both exact-decode, never use the pyramid).
void testHistogramBinsMatchKnownDistribution() {
    section("Analysis: histogram bins/cumulative match a known distribution exactly");
    segy::ThreadPool pool(2);
    int samplesPerTrace = 100;
    WiggleTestFixture fx;
    // Sample value == its own index (0..99) -- with range [0,100) split into
    // 10 bins, each bin should get exactly 10 of the 100 samples (10%).
    makeWiggleTestContext(fx, 1, samplesPerTrace, pool, [](int64_t, int s) { return double(s); });
    segy::HistogramResult result = segy::computeHistogram(fx.ctx, 0, 1, 0, samplesPerTrace, 10, 0.0f, 100.0f);
    CHECK(result.sampleCount == 100);
    CHECK(int(result.percentOfTotal.size()) == 10);
    for (float percent : result.percentOfTotal) CHECK_NEAR(percent, 10.0f, 0.01f);
    float expectedCumulative = 0.0f;
    for (float cumulative : result.cumulativePercent) {
        expectedCumulative += 10.0f;
        CHECK_NEAR(cumulative, expectedCumulative, 0.01f);
    }
    CHECK_NEAR(result.cumulativePercent.back(), 100.0f, 0.01f);
}

void testHistogramClampsOutOfRangeValues() {
    section("Analysis: histogram clamps out-of-range values into the edge bins (saturating, not dropped)");
    segy::ThreadPool pool(2);
    WiggleTestFixture fx;
    // All 50 samples are far above the [0,10) range under test -- every one
    // should land in the last bin rather than being silently discarded.
    makeWiggleTestContext(fx, 1, 50, pool, [](int64_t, int) { return 9999.0; });
    segy::HistogramResult result = segy::computeHistogram(fx.ctx, 0, 1, 0, 50, 5, 0.0f, 10.0f);
    CHECK(result.sampleCount == 50);
    for (size_t i = 0; i + 1 < result.percentOfTotal.size(); ++i) CHECK(result.percentOfTotal[i] == 0.0f);
    CHECK_NEAR(result.percentOfTotal.back(), 100.0f, 0.01f);
}

void testSpectrumPeaksAtKnownFrequency() {
    section("Analysis: spectrum peaks at a pure sine wave's own frequency");
    segy::ThreadPool pool(2);
    int samplesPerTrace = 256;
    double sampleIntervalUs = 1000.0; // 1 kHz sample rate
    double dt = sampleIntervalUs / 1'000'000.0;
    int targetBin = 30; // an exact FFT bin, so the peak isn't split by leakage
    double freqHz = double(targetBin) / (double(samplesPerTrace) * dt);
    WiggleTestFixture fx;
    makeWiggleTestContext(fx, 1, samplesPerTrace, pool, [&](int64_t, int s) {
        return 1000.0 * std::sin(2.0 * std::acos(-1.0) * freqHz * double(s) * dt);
    });
    segy::SpectrumResult result =
        segy::computeSpectrum(fx.ctx, 0, 1, 0, samplesPerTrace, sampleIntervalUs, /*smoothPoints=*/1);
    CHECK(!result.amplitudeDb.empty());
    size_t peakBin = 0;
    for (size_t i = 1; i < result.amplitudeDb.size(); ++i) {
        if (result.amplitudeDb[i] > result.amplitudeDb[peakBin]) peakBin = i;
    }
    // Hann windowing spreads a pure tone's energy across a couple of
    // adjacent bins, so the peak lands within +-1 bin of the true one, not
    // necessarily exactly on it.
    CHECK(std::abs(int(peakBin) - targetBin) <= 1);
    CHECK_NEAR(result.amplitudeDb[peakBin], 0.0f, 0.5f); // it's the reference peak, ~0 dB by construction
    CHECK(result.amplitudeDb[peakBin] - result.amplitudeDb[peakBin + 20] > 10.0f); // clearly a peak, not flat noise
    CHECK_NEAR(result.frequencyHz[targetBin], float(freqHz), 0.01f);
}

// ---------------------------------------------------------------------
// Bandpass filter (viewer/bandpass.cpp), backing Edit > Processing >
// Bandpass and the Octave Band Display window.
void testBandpassFilterIsolatesPassband() {
    section("Bandpass: isolates the passband, rejects a strong out-of-band component");
    int n = 512;
    double sampleIntervalUs = 1000.0; // 1 kHz sample rate
    double dt = sampleIntervalUs / 1'000'000.0;
    int passBin = 10; // ~19.53 Hz -- inside the passband below
    int stopBin = 80; // ~156.25 Hz -- well above highCut
    double freqPass = double(passBin) / (double(n) * dt);
    double freqStop = double(stopBin) / (double(n) * dt);

    std::vector<float> inputTrace(static_cast<size_t>(n));
    std::vector<float> expectedPassOnly(static_cast<size_t>(n));
    for (int s = 0; s < n; ++s) {
        double t = double(s) * dt;
        float passComponent = float(std::sin(2.0 * std::acos(-1.0) * freqPass * t));
        float stopComponent = float(std::sin(2.0 * std::acos(-1.0) * freqStop * t));
        inputTrace[size_t(s)] = passComponent + stopComponent;
        expectedPassOnly[size_t(s)] = passComponent;
    }

    segy::BandpassParams params;
    params.lowCut = 10.0;
    params.lowPass = 15.0;
    params.highPass = 30.0;
    params.highCut = 40.0;
    std::vector<float> filtered = inputTrace;
    segy::applyBandpassFilter(filtered, sampleIntervalUs, params);

    CHECK(filtered.size() == inputTrace.size());
    // Both components sit exactly on FFT bins (no spectral leakage), and the
    // strong component is entirely outside [lowCut, highCut] -- the filtered
    // trace should reconstruct essentially exactly the passband-only signal.
    float maxAbsDiff = 0.0f;
    for (int s = 0; s < n; ++s) {
        maxAbsDiff = std::max(maxAbsDiff, std::abs(filtered[size_t(s)] - expectedPassOnly[size_t(s)]));
    }
    CHECK(maxAbsDiff < 0.01f);

    // A passband well above both components should suppress the trace to
    // near-silence, not leave it mostly unfiltered.
    segy::BandpassParams stopAll;
    stopAll.lowCut = 200.0;
    stopAll.lowPass = 210.0;
    stopAll.highPass = 220.0;
    stopAll.highCut = 230.0;
    std::vector<float> silenced = inputTrace;
    segy::applyBandpassFilter(silenced, sampleIntervalUs, stopAll);
    float maxAbs = 0.0f;
    for (float v : silenced) maxAbs = std::max(maxAbs, std::abs(v));
    CHECK(maxAbs < 0.01f);
}

// ---------------------------------------------------------------------
// SEG-Y writer (src/segy_writer.cpp), backing Calculator/Bandpass/Save
// SEG-Y's generated-dataset output. Round-trips a small synthetic file
// through the app's own normal read path (MappedFile + parse* + decode*)
// rather than re-deriving the on-disk layout by hand a second time here.
void testWriteSegyFileRoundTrips() {
    section("Writer: writeSegyFile round-trips through the normal read path exactly");
    std::filesystem::path path = std::filesystem::temp_directory_path() / "segyread_test_writer_roundtrip.sgy";

    segy::BinaryHeader binHeader;
    binHeader.samplesPerTrace = 8;
    binHeader.sampleIntervalUs = 2000;
    binHeader.formatCode = segy::kIbmFloat32; // deliberately "wrong" -- writeSegyFile must force IEEE float32
    binHeader.tracesPerEnsemble = 1;
    binHeader.segyRevision = 1;

    std::vector<segy::WriteTrace> traces(3);
    for (int t = 0; t < 3; ++t) {
        traces[size_t(t)].headerBytes.assign(segy::kTraceHeaderSize, 0);
        segy::writeI32BE(traces[size_t(t)].headerBytes.data(), t + 1); // traceSequenceLine, bytes 1-4
        traces[size_t(t)].samples.resize(8);
        for (int s = 0; s < 8; ++s) traces[size_t(t)].samples[size_t(s)] = float(t * 10 + s) * 0.5f;
    }

    std::string error;
    std::string textHeader = "C 1 TEST HEADER LINE ONE\nC 2 SECOND LINE";
    bool ok = segy::writeSegyFile(path, textHeader, binHeader, traces, &error);
    CHECK(ok);

    segy::MappedFile file;
    CHECK(file.open(path));
    if (file.isOpen()) {
        segy::BinaryHeader readBack = segy::parseBinaryHeader(file.data() + segy::kTextHeaderSize);
        CHECK(readBack.formatCode == segy::kIeeeFloat32);
        CHECK(readBack.samplesPerTrace == 8);
        CHECK(readBack.sampleIntervalUs == 2000);
        CHECK(readBack.segyRevision == 1);

        std::string decodedText = segy::decodeEbcdicText(file.data());
        CHECK(decodedText.compare(0, 24, "C 1 TEST HEADER LINE ONE") == 0);

        size_t traceStride = segy::kTraceHeaderSize + 8 * sizeof(float);
        for (int t = 0; t < 3; ++t) {
            const uint8_t* traceBase = file.data() + segy::kHeaderTotalSize + size_t(t) * traceStride;
            segy::TraceHeader th = segy::parseTraceHeader(traceBase);
            CHECK(th.traceSequenceLine == t + 1);
            std::vector<float> decoded(8);
            segy::decodeSamples(traceBase + segy::kTraceHeaderSize, decoded.data(), 8, segy::kIeeeFloat32);
            for (int s = 0; s < 8; ++s) CHECK_NEAR(decoded[size_t(s)], float(t * 10 + s) * 0.5f, 1e-5f);
        }
    }
    file.close();
    std::filesystem::remove(path);
}

} // namespace

int main() {
    std::printf("SEG-Y native rewrite -- test harness\n");
    std::printf("AVX2 available: %s\n\n", segy::cpuSupportsAvx2() ? "yes" : "no");

    testIbmFloatKnownValues();
    testIbmFloatRoundTripViaEncoder();
    testAvx2MatchesScalar();
    testHeaderParsing();
    testEbcdicDecode();
    testEbcdicDecodeDetectsAlreadyAsciiHeader();
    testDecodeSamplesAllFormats();
    testPyramidFinestLevel(false);
    testPyramidFinestLevel(true);
    testPyramidCoarseLevelsExact();
    testEmptyAndDegenerateInputs();
    testSelectScanTraceHeadersMatchesParseTraceHeader();
    testSelectTracesFiltersByRangeAndIncrement();
    testSortTraceIndicesMultiKeyStable();
    testPyramidTraceIndexMapMatchesDirectSubsetBuild();
    testAxisTicksBasicContract();
    testColorScalesEndpointsAndZero();
    testChromeTickCountNeverExceedsMax();
    testChromeOverlapAvoidance();
    testChromeTimeUnitSelection();
    testChromeColorBarPolarity();
    testChromeSettingsToggleOffMargins();
    testWiggleTraceCountBounded();
    testWiggleEnvelopePointCountBounded();
    testWiggleNoEnvelopeGivesOnePointPerSample();
    testWigglePointsWithinPlotBounds();
    testWiggleEnvelopeMatchesBruteForceMinMax();
    testHistogramBinsMatchKnownDistribution();
    testHistogramClampsOutOfRangeValues();
    testSpectrumPeaksAtKnownFrequency();
    testBandpassFilterIsolatesPassband();
    testWriteSegyFileRoundTrips();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
