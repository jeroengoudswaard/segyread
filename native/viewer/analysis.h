// Amplitude histogram and single-trace-average amplitude spectrum, backing
// the toolbar's Histogram/Spectrum parameters dialogs (see native/README.md).
// Platform/GUI-free like renderer.h/chrome.h/wiggle.h -- viewer_qt.cpp calls
// into this and draws the result, nothing here touches Qt.
#pragma once
#include <cstdint>
#include <vector>

#include "renderer.h"

namespace segy {

struct HistogramResult {
    std::vector<float> binCenters;
    std::vector<float> percentOfTotal;    // per bin, sums to ~100 over all bins
    std::vector<float> cumulativePercent; // running total, ends at ~100
    float rangeMin = 0.0f;
    float rangeMax = 0.0f;
    int64_t sampleCount = 0; // how many decoded samples actually went into it
};

// Bins decoded sample amplitudes from traces [traceStart, traceEnd) x samples
// [sampleStart, sampleEnd) into `numBins` equal-width bins spanning
// [rangeMin, rangeMax] (values outside are clamped into the first/last bin,
// same as a saturating histogram). Traces are strided (not read exhaustively)
// once the range covers more than kHistogramMaxTraces, to keep this fast
// enough to run synchronously from a dialog's Apply button on a huge file.
HistogramResult computeHistogram(const RenderContext& ctx, int64_t traceStart, int64_t traceEnd, int32_t sampleStart,
                                  int32_t sampleEnd, int numBins, float rangeMin, float rangeMax);

struct SpectrumResult {
    std::vector<float> frequencyHz;
    std::vector<float> amplitudeDb; // 0 dB at the reference peak; most values negative
};

// Averages the amplitude spectrum (FFT magnitude) across up to
// kSpectrumMaxTraces traces in [traceStart, traceEnd) x [sampleStart,
// sampleEnd), expressed in dB relative to the averaged spectrum's own peak,
// then smooths the dB curve with a `smoothPoints`-wide centered
// moving average (even values are rounded up to the next odd one; <=1 means
// no smoothing). `sampleIntervalUs` is the file's own sample interval
// (BinaryHeader::sampleIntervalUs), used to convert FFT bins to Hz.
SpectrumResult computeSpectrum(const RenderContext& ctx, int64_t traceStart, int64_t traceEnd, int32_t sampleStart,
                                int32_t sampleEnd, double sampleIntervalUs, int smoothPoints);

} // namespace segy
