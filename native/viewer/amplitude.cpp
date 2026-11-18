#include "amplitude.h"

#include <algorithm>
#include <cmath>

namespace segy {

namespace {

// A non-positive sample interval (a broken binary header) would make every
// ms<->sample conversion meaningless; treat it as 1 ms so the controls still
// do something predictable instead of dividing by zero.
double safeIntervalMs(double sampleIntervalMs) { return sampleIntervalMs > 0.0 ? sampleIntervalMs : 1.0; }

} // namespace

double gainCurveDbAt(const std::vector<GainPoint>& curve, double timeMs) {
    if (curve.empty()) return 0.0;
    std::vector<GainPoint> sorted = curve;
    std::sort(sorted.begin(), sorted.end(), [](const GainPoint& a, const GainPoint& b) { return a.timeMs < b.timeMs; });
    if (timeMs <= sorted.front().timeMs) return sorted.front().gainDb;
    if (timeMs >= sorted.back().timeMs) return sorted.back().gainDb;
    for (size_t i = 1; i < sorted.size(); ++i) {
        if (timeMs <= sorted[i].timeMs) {
            const GainPoint& a = sorted[i - 1];
            const GainPoint& b = sorted[i];
            double span = b.timeMs - a.timeMs;
            if (span <= 0.0) return b.gainDb; // duplicate times: the later entry wins
            return a.gainDb + (b.gainDb - a.gainDb) * (timeMs - a.timeMs) / span;
        }
    }
    return sorted.back().gainDb;
}

float processedBaseClip(float rawClip, const AmplitudeProcessing& processing) {
    if (!processing.agcEnabled) return rawClip;
    double level = std::clamp(processing.agcLevelPercent, 0.1, 100.0);
    return float(100.0 / level);
}

int AmplitudeProcessor::paddingSamples(const AmplitudeProcessing& processing, double sampleIntervalMs) {
    if (!processing.agcEnabled) return 0;
    double halfMs = std::max(0.0, processing.agcWindowMs) * 0.5;
    return std::max(1, int(std::lround(halfMs / safeIntervalMs(sampleIntervalMs))));
}

AmplitudeProcessor::AmplitudeProcessor(const AmplitudeProcessing& processing, int firstSample, int count,
                                       double sampleIntervalMs)
    : count_(std::max(0, count)) {
    double dt = safeIntervalMs(sampleIntervalMs);
    if (processing.gainCurveEnabled && !processing.gainCurve.empty() && count_ > 0) {
        // Sorted once here rather than per sample (gainCurveDbAt sorts its
        // own copy -- fine for a single lookup, not for a table).
        std::vector<GainPoint> sorted = processing.gainCurve;
        std::sort(sorted.begin(), sorted.end(),
                  [](const GainPoint& a, const GainPoint& b) { return a.timeMs < b.timeMs; });
        gainTable_.resize(size_t(count_));
        size_t seg = 0;
        for (int i = 0; i < count_; ++i) {
            double t = double(firstSample + i) * dt;
            double db;
            if (t <= sorted.front().timeMs) {
                db = sorted.front().gainDb;
            } else if (t >= sorted.back().timeMs) {
                db = sorted.back().gainDb;
            } else {
                // Times increase with i, so the segment index only moves forward.
                while (seg + 1 < sorted.size() && sorted[seg + 1].timeMs < t) ++seg;
                const GainPoint& a = sorted[seg];
                const GainPoint& b = sorted[seg + 1];
                double span = b.timeMs - a.timeMs;
                db = span > 0.0 ? a.gainDb + (b.gainDb - a.gainDb) * (t - a.timeMs) / span : b.gainDb;
            }
            gainTable_[size_t(i)] = float(std::pow(10.0, db / 20.0));
        }
    }
    agc_ = processing.agcEnabled;
    agcHalfWindow_ = paddingSamples(processing, sampleIntervalMs);
    active_ = count_ > 0 && (!gainTable_.empty() || agc_);
}

void AmplitudeProcessor::apply(float* samples) const {
    if (!active_) return;
    if (!gainTable_.empty()) {
        for (int i = 0; i < count_; ++i) samples[i] *= gainTable_[size_t(i)];
    }
    if (!agc_) return;

    // Prefix sums of squares (double: a float running sum over thousands of
    // samples loses the small late-time energy AGC exists to bring up).
    thread_local std::vector<double> prefix;
    prefix.resize(size_t(count_) + 1);
    prefix[0] = 0.0;
    for (int i = 0; i < count_; ++i) {
        double v = samples[i];
        prefix[size_t(i) + 1] = prefix[size_t(i)] + v * v;
    }
    for (int i = 0; i < count_; ++i) {
        int lo = std::max(0, i - agcHalfWindow_);
        int hi = std::min(count_ - 1, i + agcHalfWindow_);
        double meanSquare = (prefix[size_t(hi) + 1] - prefix[size_t(lo)]) / double(hi - lo + 1);
        // A dead (all-zero) stretch stays zero rather than exploding into
        // amplified rounding noise.
        samples[i] = meanSquare > 1e-30 ? float(double(samples[i]) / std::sqrt(meanSquare)) : 0.0f;
    }
}

} // namespace segy
