#include "axis_ticks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace segy {

namespace {

// The classic "1, 2, 5 x 10^k" sequence used for chart tick intervals,
// indexed by a single integer so callers can walk it in either direction:
// ..., 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, ...
double intervalForIndex(int n) {
    static const double bases[3] = {1.0, 2.0, 5.0};
    int cycle = ((n % 3) + 3) % 3;
    int power = (n - cycle) / 3;
    return bases[cycle] * std::pow(10.0, power);
}

// Smallest sequence value >= minInterval (the sequence is monotonic in n,
// so a log10 estimate plus a short linear correction is exact and cheap).
int indexAtOrAbove(double minInterval) {
    if (!(minInterval > 0.0)) return -60;
    int n = int(std::floor(std::log10(minInterval) * 3.0)) - 2;
    while (intervalForIndex(n) < minInterval) ++n;
    return n;
}

std::string defaultFormatValue(double value) {
    if (std::fabs(value) < 1e-9) value = 0.0; // avoid "-0"
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

} // namespace

std::vector<AxisTick> computeNiceAxisTicks(double rangeMin, double rangeMax, int pixelSpan, int maxLines,
                                            const MeasureTextWidthFn& measureWidth,
                                            const FormatAxisValueFn& formatValue) {
    std::vector<AxisTick> result;
    double span = rangeMax - rangeMin;
    if (pixelSpan <= 0 || !(span > 0.0) || maxLines <= 0) return result;

    FormatAxisValueFn format = formatValue ? formatValue : FormatAxisValueFn(defaultFormatValue);

    int idx = indexAtOrAbove(span / maxLines);
    for (int guard = 0; guard < 60; ++guard) {
        double interval = intervalForIndex(idx);
        double firstTick = std::ceil(rangeMin / interval) * interval;

        std::vector<AxisTick> candidate;
        for (double v = firstTick; v <= rangeMax + interval * 1e-6; v += interval) {
            double frac = (v - rangeMin) / span;
            candidate.push_back({v, int(std::lround(frac * pixelSpan)), format(v)});
        }

        bool overlap = false;
        if (measureWidth && candidate.size() >= 2) {
            int minGapPx = std::numeric_limits<int>::max();
            for (size_t i = 0; i + 1 < candidate.size(); ++i) {
                minGapPx = std::min(minGapPx, std::abs(candidate[i + 1].pixelPos - candidate[i].pixelPos));
            }
            for (const AxisTick& tk : candidate) {
                if (measureWidth(tk.label) + 4 > minGapPx) {
                    overlap = true;
                    break;
                }
            }
        }
        if (!overlap || guard == 59) {
            result = std::move(candidate);
            break;
        }
        ++idx;
    }
    return result;
}

} // namespace segy
