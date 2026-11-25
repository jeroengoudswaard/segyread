#include "renderer.h"
#include "segy_decode.h"
#include "colormap.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <chrono>

namespace segy {

void resetView(ViewRange& view, int64_t traceCount, int samplesPerTrace) {
    view.traceStart = 0;
    view.traceEnd = double(traceCount);
    view.sampleStart = 0;
    view.sampleEnd = double(samplesPerTrace);
}

void clampView(ViewRange& view, int64_t traceCount, int samplesPerTrace) {
    double maxTrace = double(traceCount);
    double maxSample = double(samplesPerTrace);
    // Never let a view range collapse to zero or invert.
    view.traceEnd = std::max(view.traceEnd, view.traceStart + 1e-6);
    view.sampleEnd = std::max(view.sampleEnd, view.sampleStart + 1e-6);
    double tSpan = std::min(view.traceEnd - view.traceStart, maxTrace);
    double sSpan = std::min(view.sampleEnd - view.sampleStart, maxSample);
    if (view.traceStart < 0) { view.traceEnd = tSpan; view.traceStart = 0; }
    if (view.traceEnd > maxTrace) { view.traceStart = maxTrace - tSpan; view.traceEnd = maxTrace; }
    if (view.sampleStart < 0) { view.sampleEnd = sSpan; view.sampleStart = 0; }
    if (view.sampleEnd > maxSample) { view.sampleStart = maxSample - sSpan; view.sampleEnd = maxSample; }
}

void zoomAt(ViewRange& view, int64_t traceCount, int samplesPerTrace, int width, int height,
            int px, int py, double factor) {
    width = std::max(1, width);
    height = std::max(1, height);
    double traceSpan = view.traceEnd - view.traceStart;
    double sampleSpan = view.sampleEnd - view.sampleStart;
    double traceCoord = view.traceStart + double(px) / width * traceSpan;
    double sampleCoord = view.sampleStart + double(py) / height * sampleSpan;

    double newTraceSpan = traceSpan * factor;
    double newSampleSpan = sampleSpan * factor;
    view.traceStart = traceCoord - double(px) / width * newTraceSpan;
    view.traceEnd = view.traceStart + newTraceSpan;
    view.sampleStart = sampleCoord - double(py) / height * newSampleSpan;
    view.sampleEnd = view.sampleStart + newSampleSpan;
    clampView(view, traceCount, samplesPerTrace);
}

void panFromAnchor(ViewRange& view, const ViewRange& anchor, int64_t traceCount, int samplesPerTrace,
                    int width, int height, int dxPixels, int dyPixels) {
    width = std::max(1, width);
    height = std::max(1, height);
    double traceSpan = anchor.traceEnd - anchor.traceStart;
    double sampleSpan = anchor.sampleEnd - anchor.sampleStart;
    double dTrace = -double(dxPixels) / width * traceSpan;
    double dSample = -double(dyPixels) / height * sampleSpan;
    view.traceStart = anchor.traceStart + dTrace;
    view.traceEnd = anchor.traceEnd + dTrace;
    view.sampleStart = anchor.sampleStart + dSample;
    view.sampleEnd = anchor.sampleEnd + dSample;
    clampView(view, traceCount, samplesPerTrace);
}

namespace {

// Given a continuous sample coordinate, returns the two neighboring decoded
// sample indices to blend between and the blend fraction, clamping at the
// array edges so an out-of-range coordinate repeats the nearest edge value
// rather than reading out of bounds. A decoded sample is a single point
// value that sits exactly at its own integer position, no half-index shift
// needed (unlike a block average, which would sit at its block's center).
void bilinearPointIndex(double coord, int count, int& i0, int& i1, float& frac) {
    int base = int(std::floor(coord));
    frac = float(coord - base);
    i0 = std::clamp(base, 0, count - 1);
    i1 = std::clamp(base + 1, 0, count - 1);
}

// A gain in dB is a multiplier on the displayed amplitude
// (linear = 10^(dB/20)); applying it as a *divisor* on the clip threshold
// achieves the same visual effect (higher gain -> smaller effective clip ->
// more of the colormap's range used by weaker signal) without touching the
// per-pixel hot loop at all -- computed once outside it.
float clipForGain(float rawClip, double gainDb) {
    double linearGain = std::pow(10.0, gainDb / 20.0);
    float clip = float(double(rawClip) / linearGain);
    return clip > 0.0f ? clip : 1.0f;
}

void renderExactPath(const RenderContext& ctx, const ViewRange& v, uint32_t* pixels, int width, int height,
                      double gainDb, ColorScale colorScale, const ManualClip& manualClip, bool reversePolarity,
                      bool colorScaleFlipped, const AmplitudeProcessing* processing) {
    int64_t t0 = std::max<int64_t>(0, int64_t(std::floor(v.traceStart)));
    int64_t t1 = std::min<int64_t>(ctx.traceCount, int64_t(std::ceil(v.traceEnd)) + 1);
    int s0 = std::max(0, int(std::floor(v.sampleStart)));
    int s1 = std::min(int(ctx.binHeader.samplesPerTrace), int(std::ceil(v.sampleEnd)) + 1);
    int64_t visibleTraceCount = t1 - t0;
    int decodedHeight = s1 - s0;
    if (visibleTraceCount <= 0 || decodedHeight <= 0) return;

    // Once decoding every visible trace at full vertical resolution would
    // exceed the sample budget, decode only every `stride`-th trace instead
    // of substituting coarser pyramid-block min/max/mean statistics -- each
    // *decoded* trace is still its true, full-resolution samples, so a
    // heavily zoomed-out view reads as "fewer real traces shown" rather than
    // "same traces, blurred/averaged together" (the latter washed out real
    // amplitude contrast and made the block grid itself visible as a
    // staircase). Also capped at ~2x the pixel width regardless of budget --
    // decoding many more traces than screen columns can distinguish would
    // only waste time, never add visible detail.
    int64_t budgetTraces = std::max<int64_t>(1, int64_t(kExactDecodeSampleBudget) / decodedHeight);
    int64_t maxTraces = std::min<int64_t>(budgetTraces, int64_t(width) * 2);
    int stride = int(std::max<int64_t>(1, (visibleTraceCount + maxTraces - 1) / maxTraces));
    int decodedWidth = int((visibleTraceCount + stride - 1) / stride);

    std::vector<float> decoded(size_t(decodedWidth) * size_t(decodedHeight));
    const uint8_t* fileBase = ctx.file->data();
    int sampleSize = sampleFormatSizeBytes(ctx.binHeader.formatCode);

    // Computed gain/AGC (amplitude.h): decode a padded range so AGC sees
    // the same neighborhood no matter where the view starts, process it,
    // then keep only the visible part. Built once per frame, shared by all
    // workers.
    const bool processingOn = processing && processing->active();
    const double sampleIntervalMs = ctx.binHeader.sampleIntervalUs / 1000.0;
    const int pad = processingOn ? AmplitudeProcessor::paddingSamples(*processing, sampleIntervalMs) : 0;
    const int ps0 = std::max(0, s0 - pad);
    const int ps1 = std::min(int(ctx.binHeader.samplesPerTrace), s1 + pad);
    const int paddedHeight = ps1 - ps0;
    const AmplitudeProcessor processor =
        processingOn ? AmplitudeProcessor(*processing, ps0, paddedHeight, sampleIntervalMs)
                     : AmplitudeProcessor(AmplitudeProcessing{}, 0, 0, sampleIntervalMs);

    ctx.pool->parallelFor(size_t(decodedWidth), 4, [&](size_t ltBegin, size_t ltEnd) {
        std::vector<float> padded(processingOn ? size_t(paddedHeight) : 0);
        for (size_t lt = ltBegin; lt < ltEnd; ++lt) {
            int64_t absTrace = std::min<int64_t>(t0 + int64_t(lt) * stride, t1 - 1);
            const uint8_t* traceBase = fileBase + kHeaderTotalSize + size_t(absTrace) * ctx.traceStrideBytes;
            float* out = decoded.data() + lt * size_t(decodedHeight);
            if (!processingOn) {
                const uint8_t* sampleBase = traceBase + kTraceHeaderSize + size_t(s0) * size_t(sampleSize);
                decodeSamples(sampleBase, out, size_t(decodedHeight), ctx.binHeader.formatCode);
                continue;
            }
            const uint8_t* sampleBase = traceBase + kTraceHeaderSize + size_t(ps0) * size_t(sampleSize);
            decodeSamples(sampleBase, padded.data(), size_t(paddedHeight), ctx.binHeader.formatCode);
            processor.apply(padded.data());
            std::copy_n(padded.data() + (s0 - ps0), size_t(decodedHeight), out);
        }
    });

    float rawClip = autoClip(*ctx.pyramid);
    float clipPos, clipNeg;
    displayClipPosNeg(rawClip, gainDb, manualClip, processing, &clipPos, &clipNeg);
    const float colorScaleDirection = colorScaleFlipped ? -1.0f : 1.0f;
    double traceSpan = v.traceEnd - v.traceStart;
    double sampleSpan = v.sampleEnd - v.sampleStart;

    ctx.pool->parallelFor(size_t(height), 8, [&](size_t yBegin, size_t yEnd) {
        for (size_t y = yBegin; y < yEnd; ++y) {
            double sampleCoord = v.sampleStart + (double(y) + 0.5) / height * sampleSpan;
            int ls0, ls1;
            float sf;
            bilinearPointIndex(sampleCoord - double(s0), decodedHeight, ls0, ls1, sf);
            uint32_t* row = pixels + y * width;
            for (int x = 0; x < width; ++x) {
                double traceCoord = v.traceStart + (double(x) + 0.5) / width * traceSpan;

                // Nearest decoded (decimated) trace, not blended across it
                // and its neighbor: blending real samples from two traces
                // several skipped traces apart would smear them together,
                // recreating the same washed-out look decimation is meant
                // to avoid. Each screen column instead shows one real
                // trace's true samples, stepping to the next as the view
                // crosses the midpoint between two decoded traces.
                int64_t nearestAbs = std::clamp<int64_t>(int64_t(std::lround(traceCoord)), t0, t1 - 1);
                int lt = int(std::lround(double(nearestAbs - t0) / stride));
                lt = std::clamp(lt, 0, decodedWidth - 1);

                float vTop = decoded[size_t(lt) * size_t(decodedHeight) + size_t(ls0)];
                float vBot = decoded[size_t(lt) * size_t(decodedHeight) + size_t(ls1)];
                float value = vTop + (vBot - vTop) * sf - manualClip.center;
                if (reversePolarity) value = -value;

                uint8_t r, g, b;
                float clip = value >= 0.0f ? clipPos : clipNeg;
                applyColorScale(colorScale, (value / clip) * colorScaleDirection, &b, &g, &r);
                row[x] = (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
            }
        }
    });
}

} // namespace

void displayClipPosNeg(float rawClip, double gainDb, const ManualClip& manualClip,
                       const AmplitudeProcessing* processing, float* clipPos, float* clipNeg) {
    if (processing && processing->agcEnabled) {
        float base = processedBaseClip(rawClip, *processing);
        *clipPos = clipForGain(base, gainDb);
        *clipNeg = *clipPos;
        return;
    }
    float basePos = manualClip.enabled ? manualClip.posMagnitude : rawClip;
    float baseNeg = manualClip.enabled ? manualClip.negMagnitude : rawClip;
    *clipPos = clipForGain(basePos, gainDb);
    *clipNeg = clipForGain(baseNeg, gainDb);
}

ManualClip effectiveClip(const ManualClip& manualClip, float dataMin, float dataMax,
                         const AmplitudeProcessing* processing) {
    if (manualClip.enabled || (processing && processing->agcEnabled)) return manualClip;
    if (!(dataMax > dataMin) || (dataMin < 0.0f && dataMax > 0.0f)) return manualClip;
    return clipFromRange(dataMin, dataMax);
}

void clipToRange(const ManualClip& clip, float* low, float* high) {
    *low = clip.center - clip.negMagnitude;
    *high = clip.center + clip.posMagnitude;
}

ManualClip clipFromRange(float low, float high) {
    if (!(high > low)) high = low + 1.0f;
    ManualClip clip;
    clip.enabled = true;
    if (low < 0.0f && high > 0.0f) {
        clip.center = 0.0f;
        clip.posMagnitude = high;
        clip.negMagnitude = -low;
    } else {
        clip.center = 0.5f * (low + high);
        clip.posMagnitude = clip.negMagnitude = 0.5f * (high - low);
    }
    return clip;
}

double renderFrame(const RenderContext& ctx, const ViewRange& view, uint32_t* pixels, int width, int height,
                    double gainDb, ColorScale colorScale, ManualClip manualClip, bool reversePolarity,
                    bool colorScaleFlipped, const AmplitudeProcessing* processing) {
    if (width <= 0 || height <= 0 || !ctx.pyramid || !ctx.pool) return 0.0;
    manualClip = effectiveClip(manualClip, ctx.pyramid->globalMin, ctx.pyramid->globalMax, processing);
    auto start = std::chrono::high_resolution_clock::now();

    if (ctx.file && ctx.file->isOpen()) {
        renderExactPath(ctx, view, pixels, width, height, gainDb, colorScale, manualClip, reversePolarity,
                        colorScaleFlipped, processing);
    }

    return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start).count();
}

} // namespace segy
