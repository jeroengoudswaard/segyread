#include "analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "fft.h"
#include "segy_decode.h"
#include "segy_format.h"

namespace segy {

namespace {

// Bounds how much a "Full View" histogram/spectrum over a huge file has to
// decode -- both are meant as a quick-look diagnostic, not an exhaustive
// scan, so striding down to a fixed trace budget keeps them fast without
// materially changing the shape of either result for real seismic data
// (adjacent traces are highly correlated).
constexpr int64_t kHistogramMaxTraces = 500;
constexpr int64_t kSpectrumMaxTraces = 50;

std::vector<int64_t> strideTraceIndices(int64_t traceStart, int64_t traceEnd, int64_t traceCount, int64_t maxTraces) {
    std::vector<int64_t> indices;
    traceStart = std::clamp<int64_t>(traceStart, 0, std::max<int64_t>(0, traceCount - 1));
    traceEnd = std::clamp<int64_t>(traceEnd, traceStart + 1, traceCount);
    int64_t span = traceEnd - traceStart;
    if (span <= 0) return indices;
    int64_t step = std::max<int64_t>(1, span / maxTraces);
    for (int64_t t = traceStart; t < traceEnd; t += step) indices.push_back(t);
    return indices;
}

// One trace's decoded samples over [sampleStart, sampleEnd), clamped to the
// file's actual sample count.
std::vector<float> decodeTraceRange(const RenderContext& ctx, int64_t traceIdx, int32_t sampleStart,
                                     int32_t sampleEnd) {
    int samplesPerTrace = ctx.binHeader.samplesPerTrace;
    int s0 = std::clamp<int32_t>(sampleStart, 0, samplesPerTrace);
    int s1 = std::clamp<int32_t>(sampleEnd, s0, samplesPerTrace);
    std::vector<float> out(size_t(s1 - s0));
    if (out.empty()) return out;
    const uint8_t* fileBase = ctx.file->data();
    const uint8_t* traceBase = fileBase + kHeaderTotalSize + size_t(traceIdx) * ctx.traceStrideBytes;
    int sampleSize = sampleFormatSizeBytes(ctx.binHeader.formatCode);
    const uint8_t* sampleBase = traceBase + kTraceHeaderSize + size_t(s0) * size_t(sampleSize);
    decodeSamples(sampleBase, out.data(), out.size(), ctx.binHeader.formatCode);
    return out;
}

} // namespace

HistogramResult computeHistogram(const RenderContext& ctx, int64_t traceStart, int64_t traceEnd, int32_t sampleStart,
                                  int32_t sampleEnd, int numBins, float rangeMin, float rangeMax) {
    HistogramResult result;
    numBins = std::max(1, numBins);
    if (rangeMax <= rangeMin) rangeMax = rangeMin + 1.0f;
    result.rangeMin = rangeMin;
    result.rangeMax = rangeMax;
    result.binCenters.resize(size_t(numBins));
    float binWidth = (rangeMax - rangeMin) / float(numBins);
    for (int i = 0; i < numBins; ++i) result.binCenters[size_t(i)] = rangeMin + (float(i) + 0.5f) * binWidth;

    std::vector<int64_t> counts(size_t(numBins), 0);
    if (!ctx.file || !ctx.file->isOpen() || ctx.traceCount <= 0) return result;

    for (int64_t traceIdx : strideTraceIndices(traceStart, traceEnd, ctx.traceCount, kHistogramMaxTraces)) {
        std::vector<float> samples = decodeTraceRange(ctx, traceIdx, sampleStart, sampleEnd);
        for (float value : samples) {
            int bin = int((value - rangeMin) / binWidth);
            bin = std::clamp(bin, 0, numBins - 1);
            ++counts[size_t(bin)];
            ++result.sampleCount;
        }
    }

    result.percentOfTotal.resize(size_t(numBins), 0.0f);
    result.cumulativePercent.resize(size_t(numBins), 0.0f);
    if (result.sampleCount > 0) {
        double cumulative = 0.0;
        for (int i = 0; i < numBins; ++i) {
            double percent = 100.0 * double(counts[size_t(i)]) / double(result.sampleCount);
            result.percentOfTotal[size_t(i)] = float(percent);
            cumulative += percent;
            result.cumulativePercent[size_t(i)] = float(cumulative);
        }
    }
    return result;
}

SpectrumResult computeSpectrum(const RenderContext& ctx, int64_t traceStart, int64_t traceEnd, int32_t sampleStart,
                                int32_t sampleEnd, double sampleIntervalUs, int smoothPoints) {
    SpectrumResult result;
    if (!ctx.file || !ctx.file->isOpen() || ctx.traceCount <= 0 || sampleIntervalUs <= 0) return result;

    std::vector<int64_t> traceIndices = strideTraceIndices(traceStart, traceEnd, ctx.traceCount, kSpectrumMaxTraces);
    if (traceIndices.empty()) return result;

    int rawLength = std::max(0, sampleEnd - sampleStart);
    if (rawLength < 2) return result;
    size_t fftLength = nextPowerOfTwo(size_t(rawLength));
    size_t halfLength = fftLength / 2;

    std::vector<double> magnitudeSum(halfLength + 1, 0.0);
    int accumulated = 0;
    for (int64_t traceIdx : traceIndices) {
        std::vector<float> samples = decodeTraceRange(ctx, traceIdx, sampleStart, sampleEnd);
        if (samples.size() < 2) continue;

        std::vector<std::complex<float>> spectrum(fftLength, std::complex<float>(0.0f, 0.0f));
        // Hann window: reduces spectral leakage from the window's hard edges
        // (the FFT otherwise implicitly assumes the windowed segment
        // repeats periodically, which a raw rectangular cut does not).
        for (size_t i = 0; i < samples.size(); ++i) {
            double w = 0.5 - 0.5 * std::cos(2.0 * std::acos(-1.0) * double(i) / double(samples.size() - 1));
            spectrum[i] = std::complex<float>(samples[i] * float(w), 0.0f);
        }
        fft(spectrum);
        for (size_t k = 0; k <= halfLength; ++k) magnitudeSum[k] += std::abs(spectrum[k]);
        ++accumulated;
    }
    if (accumulated == 0) return result;

    double dt = sampleIntervalUs / 1'000'000.0; // seconds
    std::vector<double> magnitudeAvg(halfLength + 1);
    double referencePeak = 0.0;
    for (size_t k = 0; k <= halfLength; ++k) {
        magnitudeAvg[k] = magnitudeSum[k] / double(accumulated);
        referencePeak = std::max(referencePeak, magnitudeAvg[k]);
    }
    if (referencePeak <= 0.0) referencePeak = 1.0;

    result.frequencyHz.resize(halfLength + 1);
    result.amplitudeDb.resize(halfLength + 1);
    for (size_t k = 0; k <= halfLength; ++k) {
        result.frequencyHz[k] = float(double(k) / (double(fftLength) * dt));
        double ratio = magnitudeAvg[k] / referencePeak;
        result.amplitudeDb[k] = float(20.0 * std::log10(std::max(ratio, 1e-6)));
    }

    // Centered moving-average smoothing over the dB curve -- even widths
    // round up to the next odd one so the window has a well-defined center.
    if (smoothPoints > 1) {
        int window = smoothPoints % 2 == 0 ? smoothPoints + 1 : smoothPoints;
        int half = window / 2;
        std::vector<float> smoothed(result.amplitudeDb.size());
        for (size_t i = 0; i < result.amplitudeDb.size(); ++i) {
            int lo = std::max(0, int(i) - half);
            int hi = std::min(int(result.amplitudeDb.size()) - 1, int(i) + half);
            double sum = 0.0;
            for (int j = lo; j <= hi; ++j) sum += result.amplitudeDb[size_t(j)];
            smoothed[i] = float(sum / double(hi - lo + 1));
        }
        result.amplitudeDb = std::move(smoothed);
    }
    return result;
}

} // namespace segy
