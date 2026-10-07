#include "segy_pyramid.h"
#include "segy_decode.h"

#include <limits>
#include <atomic>
#include <algorithm>

namespace segy {

namespace {

int64_t ceilDiv(int64_t a, int64_t b) { return (a + b - 1) / b; }

// Builds levels[0], the finest level, directly from the memory-mapped file:
// one pass over every trace, decoding it once and reducing it into all of
// that trace's sample-blocks. Parallelized across trace-block ranges so
// each worker owns disjoint output rows (no synchronization needed on the
// output arrays).
PyramidLevel buildFinestLevel(const BuildParams& p, ThreadPool& pool, const ProgressFn& onProgress) {
    PyramidLevel level;
    level.traceFactor = p.blockFactor;
    level.sampleFactor = p.blockFactor;
    level.traceBlocks = int(ceilDiv(p.traceCount, p.blockFactor));
    level.sampleBlocks = int(ceilDiv(p.samplesPerTrace, p.blockFactor));
    size_t total = size_t(level.traceBlocks) * size_t(level.sampleBlocks);
    level.minv.assign(total, std::numeric_limits<float>::infinity());
    level.maxv.assign(total, -std::numeric_limits<float>::infinity());
    level.mean.assign(total, 0.0f);
    level.count.assign(total, 0);

    std::atomic<int64_t> doneBlocks{0};

    pool.parallelFor(size_t(level.traceBlocks), 1, [&](size_t tbBegin, size_t tbEnd) {
        std::vector<float> traceBuf(size_t(p.samplesPerTrace));
        std::vector<double> sumAcc(size_t(level.sampleBlocks));
        std::vector<float> minAcc(size_t(level.sampleBlocks));
        std::vector<float> maxAcc(size_t(level.sampleBlocks));
        std::vector<int32_t> cntAcc(size_t(level.sampleBlocks));

        for (size_t tb = tbBegin; tb < tbEnd; ++tb) {
            int64_t traceStart = int64_t(tb) * p.blockFactor;
            int64_t traceEnd = std::min<int64_t>(p.traceCount, traceStart + p.blockFactor);

            std::fill(sumAcc.begin(), sumAcc.end(), 0.0);
            std::fill(minAcc.begin(), minAcc.end(), std::numeric_limits<float>::infinity());
            std::fill(maxAcc.begin(), maxAcc.end(), -std::numeric_limits<float>::infinity());
            std::fill(cntAcc.begin(), cntAcc.end(), 0);

            for (int64_t t = traceStart; t < traceEnd; ++t) {
                int64_t physicalTrace = p.traceIndexMap ? p.traceIndexMap[t] : t;
                const uint8_t* traceBase = p.fileBase + kHeaderTotalSize + size_t(physicalTrace) * p.traceStrideBytes;
                const uint8_t* sampleBase = traceBase + kTraceHeaderSize;
                decodeSamples(sampleBase, traceBuf.data(), size_t(p.samplesPerTrace), p.formatCode);

                for (int sb = 0; sb < level.sampleBlocks; ++sb) {
                    int64_t sampleStart = int64_t(sb) * p.blockFactor;
                    int64_t sampleEnd = std::min<int64_t>(p.samplesPerTrace, sampleStart + p.blockFactor);
                    float mn = minAcc[sb], mx = maxAcc[sb];
                    double sum = sumAcc[sb];
                    int32_t cnt = cntAcc[sb];
                    for (int64_t s = sampleStart; s < sampleEnd; ++s) {
                        float v = traceBuf[size_t(s)];
                        mn = std::min(mn, v);
                        mx = std::max(mx, v);
                        sum += v;
                        ++cnt;
                    }
                    minAcc[sb] = mn;
                    maxAcc[sb] = mx;
                    sumAcc[sb] = sum;
                    cntAcc[sb] = cnt;
                }
            }

            for (int sb = 0; sb < level.sampleBlocks; ++sb) {
                size_t idx = level.index(int(tb), sb);
                level.minv[idx] = minAcc[sb];
                level.maxv[idx] = maxAcc[sb];
                level.count[idx] = cntAcc[sb];
                level.mean[idx] = cntAcc[sb] > 0 ? float(sumAcc[sb] / cntAcc[sb]) : 0.0f;
            }

            if (onProgress) {
                onProgress(doneBlocks.fetch_add(1) + 1, int64_t(level.traceBlocks));
            }
        }
    });

    return level;
}

// Coarsens `prev` by `blockFactor` in both axes. Cheap relative to the
// finest level (data volume already shrunk by blockFactor^2), so this runs
// with a simple parallelFor too but doesn't need per-call tuning.
PyramidLevel coarsenLevel(const PyramidLevel& prev, int blockFactor, ThreadPool& pool) {
    PyramidLevel level;
    level.traceFactor = prev.traceFactor * blockFactor;
    level.sampleFactor = prev.sampleFactor * blockFactor;
    level.traceBlocks = int(ceilDiv(prev.traceBlocks, blockFactor));
    level.sampleBlocks = int(ceilDiv(prev.sampleBlocks, blockFactor));
    size_t total = size_t(level.traceBlocks) * size_t(level.sampleBlocks);
    level.minv.resize(total);
    level.maxv.resize(total);
    level.mean.resize(total);
    level.count.resize(total);

    pool.parallelFor(size_t(level.traceBlocks), 64, [&](size_t tbBegin, size_t tbEnd) {
        for (size_t tb = tbBegin; tb < tbEnd; ++tb) {
            int prevTraceStart = int(tb) * blockFactor;
            int prevTraceEnd = std::min(prev.traceBlocks, prevTraceStart + blockFactor);
            for (int sb = 0; sb < level.sampleBlocks; ++sb) {
                int prevSampleStart = sb * blockFactor;
                int prevSampleEnd = std::min(prev.sampleBlocks, prevSampleStart + blockFactor);

                float mn = std::numeric_limits<float>::infinity();
                float mx = -std::numeric_limits<float>::infinity();
                double sum = 0.0;
                int64_t cnt = 0;
                for (int pt = prevTraceStart; pt < prevTraceEnd; ++pt) {
                    for (int ps = prevSampleStart; ps < prevSampleEnd; ++ps) {
                        size_t pidx = prev.index(pt, ps);
                        int32_t pc = prev.count[pidx];
                        if (pc == 0) continue;
                        mn = std::min(mn, prev.minv[pidx]);
                        mx = std::max(mx, prev.maxv[pidx]);
                        sum += double(prev.mean[pidx]) * pc; // exact area-weighted mean, not a mean-of-means
                        cnt += pc;
                    }
                }
                size_t idx = level.index(int(tb), sb);
                level.minv[idx] = cnt > 0 ? mn : 0.0f;
                level.maxv[idx] = cnt > 0 ? mx : 0.0f;
                level.mean[idx] = cnt > 0 ? float(sum / double(cnt)) : 0.0f;
                level.count[idx] = int32_t(cnt);
            }
        }
    });

    return level;
}

} // namespace

Pyramid buildPyramid(const BuildParams& p, ThreadPool& pool, const ProgressFn& onProgress) {
    Pyramid pyramid;
    pyramid.traceCount = p.traceCount;
    pyramid.samplesPerTrace = p.samplesPerTrace;

    if (p.traceCount <= 0 || p.samplesPerTrace <= 0) return pyramid;

    pyramid.levels.push_back(buildFinestLevel(p, pool, onProgress));

    for (int i = 0; i < p.maxLevels; ++i) {
        const PyramidLevel& prev = pyramid.levels.back();
        if (prev.traceBlocks <= 1 && prev.sampleBlocks <= 1) break;
        pyramid.levels.push_back(coarsenLevel(prev, p.blockFactor, pool));
    }

    float gmin = std::numeric_limits<float>::infinity();
    float gmax = -std::numeric_limits<float>::infinity();
    const PyramidLevel& coarsest = pyramid.levels.back();
    for (size_t i = 0; i < coarsest.minv.size(); ++i) {
        if (coarsest.count[i] == 0) continue;
        gmin = std::min(gmin, coarsest.minv[i]);
        gmax = std::max(gmax, coarsest.maxv[i]);
    }
    if (!(gmin <= gmax)) { gmin = -1.0f; gmax = 1.0f; } // degenerate (empty/NaN) fallback
    pyramid.globalMin = gmin;
    pyramid.globalMax = gmax;
    return pyramid;
}

} // namespace segy
