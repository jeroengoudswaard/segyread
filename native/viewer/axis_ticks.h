// Generic "nice" (1, 2, 5 x 10^k) axis tick computation with overlap
// avoidance -- extracted from chrome.cpp's Time-axis tick logic so every
// numeric axis in the app (the seismic canvas's Time axis, and the
// Histogram/Spectrum popup plots' amplitude/frequency/dB axes) picks tick
// intervals the same way, rather than each having its own copy that could
// silently drift apart. Qt-free, part of segyviewercore, covered by
// segytest.
#pragma once
#include <string>
#include <vector>
#include <functional>

namespace segy {

struct AxisTick {
    double value = 0.0; // the tick's data-space value
    int pixelPos = 0;    // position along the axis, pixels from its start (0..pixelSpan)
    std::string label;
};

// Measures the rendered width of a label in the caller's own axis font --
// same shape as chrome.h's TextMetrics::measureWidth, kept decoupled from
// Qt here so this stays linkable into segytest without pulling in Qt.
using MeasureTextWidthFn = std::function<int(const std::string&)>;
using FormatAxisValueFn = std::function<std::string(double)>;

// Picks the finest "nice" tick interval covering [rangeMin, rangeMax] with
// at most maxLines ticks, then coarsens (walking up the 1-2-5 sequence)
// just enough that adjacent labels -- as measured by measureWidth, compared
// against the pixel gap between their tick positions -- stop overlapping.
// Ticks are mapped linearly onto pixel positions [0, pixelSpan).
// `formatValue` defaults to fixed-point with trailing zeros trimmed (and
// "-0" suppressed) when not given. Returns an empty vector for a
// degenerate range/span/maxLines.
std::vector<AxisTick> computeNiceAxisTicks(double rangeMin, double rangeMax, int pixelSpan, int maxLines,
                                            const MeasureTextWidthFn& measureWidth,
                                            const FormatAxisValueFn& formatValue = nullptr);

} // namespace segy
