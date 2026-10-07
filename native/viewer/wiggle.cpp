#include "wiggle.h"
#include "segy_decode.h"
#include "segy_format.h"
#include "segy_pyramid.h"

#include <algorithm>
#include <cmath>

namespace segy {

namespace {

// Selects up to `maxTraces` trace indices, evenly spaced across
// [view.traceStart, view.traceEnd), clamped to [0, traceCount). Fewer than
// maxTraces are returned if the visible range itself has fewer traces than
// that (no point drawing duplicates).
std::vector<int64_t> selectTraces(const ViewRange& view, int64_t traceCount, int maxTraces) {
    std::vector<int64_t> result;
    if (maxTraces <= 0 || traceCount <= 0) return result;
    double traceSpan = view.traceEnd - view.traceStart;
    if (traceSpan <= 0.0) return result;
    double step = traceSpan / double(maxTraces);
    int64_t lastAdded = -1;
    for (int i = 0; i < maxTraces; ++i) {
        double coord = view.traceStart + (double(i) + 0.5) * step;
        if (coord < 0.0 || coord >= double(traceCount)) continue;
        int64_t idx = int64_t(coord);
        if (idx == lastAdded) continue; // dedupe when step < 1 (more room than traces)
        result.push_back(idx);
        lastAdded = idx;
    }
    return result;
}

} // namespace

WiggleLayout computeWiggleLayout(const RenderContext& ctx, const ViewRange& view, int plotWidth, int plotHeight,
                                  double gainDb, bool reversePolarity) {
    WiggleLayout layout;
    if (plotWidth <= 0 || plotHeight <= 0 || !ctx.file || !ctx.file->isOpen() || ctx.traceCount <= 0) return layout;

    int maxTraces = std::max(1, plotWidth / kMinPixelsPerTrace);
    std::vector<int64_t> traceIndices = selectTraces(view, ctx.traceCount, maxTraces);
    if (traceIndices.empty()) return layout;

    double traceSpan = view.traceEnd - view.traceStart;
    double sampleSpan = view.sampleEnd - view.sampleStart;
    if (traceSpan <= 0.0 || sampleSpan <= 0.0) return layout;
    int samplesPerTrace = ctx.binHeader.samplesPerTrace;

    int s0 = std::max(0, int(std::floor(view.sampleStart)));
    int s1 = std::min(samplesPerTrace, int(std::ceil(view.sampleEnd)) + 1);
    int decodedCount = s1 - s0;
    if (decodedCount <= 0) return layout;

    // Same clip value the raster paths use for their colormap
    // normalization, gain-adjusted the same way (renderer.cpp's
    // clipForGain), so a trace's full-scale excursion spans roughly half
    // the average trace-to-trace spacing at 0 dB, and switching display
    // modes doesn't change how "loud" the data looks.
    float rawClip = 0.0f;
    if (ctx.pyramid) rawClip = std::max(std::fabs(ctx.pyramid->globalMin), std::fabs(ctx.pyramid->globalMax));
    if (rawClip <= 0.0f) rawClip = 1.0f;
    double linearGain = std::pow(10.0, gainDb / 20.0);
    float clip = float(double(rawClip) / linearGain);
    if (clip <= 0.0f) clip = 1.0f;
    float halfLanePixels = float(plotWidth) / float(std::max<size_t>(1, traceIndices.size())) * 0.5f;

    layout.traces.resize(traceIndices.size());

    int sampleFormatSize = sampleFormatSizeBytes(ctx.binHeader.formatCode);
    const uint8_t* fileBase = ctx.file->data();

    // Not much vertical oversampling -> one vertex per raw sample (exact,
    // per spec: "just straight lines between samples"). Zoomed out far
    // enough that many raw samples would map to one pixel row -> reduce to
    // a min/max envelope per row instead, so point count stays O(plotHeight)
    // regardless of how many raw samples are actually in view.
    bool useEnvelope = decodedCount > plotHeight * 2;

    size_t decodedCountSz = size_t(decodedCount);
    auto worker = [&](size_t begin, size_t end) {
        std::vector<float> decoded(decodedCountSz);
        for (size_t i = begin; i < end; ++i) {
            int64_t absTrace = traceIndices[i];
            const uint8_t* traceBase = fileBase + kHeaderTotalSize + size_t(absTrace) * ctx.traceStrideBytes;
            const uint8_t* sampleBase = traceBase + kTraceHeaderSize + size_t(s0) * size_t(sampleFormatSize);
            decodeSamples(sampleBase, decoded.data(), size_t(decodedCount), ctx.binHeader.formatCode);

            float baselineX = float((double(absTrace) + 0.5 - view.traceStart) / traceSpan * plotWidth);

            WiggleTrace& out = layout.traces[i];
            out.baselineX = baselineX;

            auto pixelYForSample = [&](double sampleIdx) -> float {
                return float((sampleIdx - view.sampleStart) / sampleSpan * plotHeight);
            };
            auto pixelXForValue = [&](float value) -> float {
                if (reversePolarity) value = -value;
                return baselineX + (value / clip) * halfLanePixels;
            };

            if (!useEnvelope) {
                out.linePoints.reserve(size_t(decodedCount));
                for (int s = 0; s < decodedCount; ++s) {
                    out.linePoints.push_back({pixelXForValue(decoded[size_t(s)]), pixelYForSample(double(s0 + s))});
                }
            } else {
                // Alternating min/max order per row draws a continuous
                // zigzag envelope (rather than a line that snaps back
                // across the column each row), so genuine spikes narrower
                // than one pixel row still show -- the same reason the
                // pyramid keeps min/max per block, not just mean.
                out.linePoints.reserve(size_t(plotHeight) * 2);
                for (int y = 0; y < plotHeight; ++y) {
                    double rowStart = view.sampleStart + double(y) / plotHeight * sampleSpan;
                    double rowEnd = view.sampleStart + double(y + 1) / plotHeight * sampleSpan;
                    int rs0 = std::clamp(int(std::floor(rowStart)) - s0, 0, decodedCount - 1);
                    int rs1 = std::clamp(int(std::ceil(rowEnd)) - s0, rs0, decodedCount - 1);
                    float lo = decoded[size_t(rs0)];
                    float hi = lo;
                    for (int s = rs0; s <= rs1; ++s) {
                        lo = std::min(lo, decoded[size_t(s)]);
                        hi = std::max(hi, decoded[size_t(s)]);
                    }
                    float rowY = float(y);
                    if (y % 2 == 0) {
                        out.linePoints.push_back({pixelXForValue(lo), rowY});
                        out.linePoints.push_back({pixelXForValue(hi), rowY});
                    } else {
                        out.linePoints.push_back({pixelXForValue(hi), rowY});
                        out.linePoints.push_back({pixelXForValue(lo), rowY});
                    }
                }
            }
        }
    };

    if (ctx.pool) {
        ctx.pool->parallelFor(traceIndices.size(), 4, worker);
    } else {
        worker(0, traceIndices.size());
    }

    return layout;
}

} // namespace segy
