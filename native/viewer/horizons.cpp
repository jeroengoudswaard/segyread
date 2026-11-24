#include "horizons.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace segy {

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// Normalized correlation of a[ca - half .. ca + half] with b[cb - half .. cb + half]
// (integer centres; out-of-range samples count as zero).
double windowCorrelation(const std::vector<float>& a, int ca, const std::vector<float>& b, int cb, int half) {
    double sum = 0.0, ea = 0.0, eb = 0.0;
    const int n = int(a.size());
    for (int k = -half; k <= half; ++k) {
        const int ia = ca + k, ib = cb + k;
        const double va = ia >= 0 && ia < n ? a[size_t(ia)] : 0.0;
        const double vb = ib >= 0 && ib < n ? b[size_t(ib)] : 0.0;
        sum += va * vb;
        ea += va * va;
        eb += vb * vb;
    }
    return ea > 0.0 && eb > 0.0 ? sum / std::sqrt(ea * eb) : 0.0;
}

} // namespace

double snapToEvent(const float* trace, int count, double sample, int radius, PickMode mode) {
    if (count < 3 || !std::isfinite(sample)) return sample;
    const int centre = std::clamp(int(std::lround(sample)), 0, count - 1);
    const int lo = std::max(1, centre - radius), hi = std::min(count - 2, centre + radius);
    if (lo > hi) return sample;
    if (mode == PickMode::ZeroCrossing) {
        double best = kNaN, bestDistance = std::numeric_limits<double>::infinity();
        for (int i = std::max(0, centre - radius); i < std::min(count - 1, centre + radius); ++i) {
            const double a = trace[i], b = trace[i + 1];
            if ((a <= 0.0 && b > 0.0) || (a >= 0.0 && b < 0.0)) {
                const double at = a == b ? double(i) : double(i) + a / (a - b);
                if (std::fabs(at - sample) < bestDistance) {
                    bestDistance = std::fabs(at - sample);
                    best = at;
                }
            }
        }
        return std::isnan(best) ? sample : best;
    }
    const double sign = mode == PickMode::Peak ? 1.0 : -1.0;
    int bestIndex = lo;
    for (int i = lo; i <= hi; ++i) {
        if (sign * trace[i] > sign * trace[bestIndex]) bestIndex = i;
    }
    // Parabolic refinement through the extremum and its neighbours.
    const double ym = trace[bestIndex - 1], y0 = trace[bestIndex], yp = trace[bestIndex + 1];
    const double curvature = ym - 2.0 * y0 + yp;
    const double delta = curvature != 0.0 ? std::clamp(0.5 * (ym - yp) / curvature, -0.5, 0.5) : 0.0;
    return double(bestIndex) + delta;
}

std::vector<double> autoPickSeabed(const HorizonTraceFetch& fetch, int64_t traceStart, int64_t traceEnd,
                                   int samplesPerTrace, double thresholdFraction, PickMode mode, int firstSample,
                                   int outlierSamples) {
    const int64_t n = std::max<int64_t>(0, traceEnd - traceStart);
    std::vector<double> picks(static_cast<size_t>(n), kNaN);
    if (n == 0 || samplesPerTrace < 3 || !fetch) return picks;
    std::vector<float> trace(static_cast<size_t>(samplesPerTrace));
    auto firstBreak = [&](const std::vector<float>& tr, int from, int to) -> double {
        float peak = 0.0f;
        for (int i = std::max(0, firstSample); i < samplesPerTrace; ++i) peak = std::max(peak, std::fabs(tr[size_t(i)]));
        if (peak <= 0.0f) return kNaN;
        const float threshold = float(thresholdFraction) * peak;
        for (int i = std::max(from, firstSample); i < std::min(to, samplesPerTrace); ++i) {
            if (std::fabs(tr[size_t(i)]) >= threshold) {
                // Snap forward onto the event: search just past the crossing.
                return snapToEvent(tr.data(), samplesPerTrace, double(i) + 3.0, 4, mode);
            }
        }
        return kNaN;
    };
    for (int64_t t = 0; t < n; ++t) {
        fetch(traceStart + t, trace.data());
        picks[size_t(t)] = firstBreak(trace, 0, samplesPerTrace);
    }
    // Neighbour consistency: re-pick outliers near the local median.
    std::vector<double> cleaned = picks;
    for (int64_t t = 0; t < n; ++t) {
        std::vector<double> near;
        for (int64_t k = std::max<int64_t>(0, t - 10); k <= std::min<int64_t>(n - 1, t + 10); ++k) {
            if (k != t && std::isfinite(picks[size_t(k)])) near.push_back(picks[size_t(k)]);
        }
        if (near.size() < 3) continue;
        std::nth_element(near.begin(), near.begin() + std::ptrdiff_t(near.size() / 2), near.end());
        const double median = near[near.size() / 2];
        if (std::isfinite(picks[size_t(t)]) && std::fabs(picks[size_t(t)] - median) <= outlierSamples) continue;
        fetch(traceStart + t, trace.data());
        const int from = std::max(0, int(median) - outlierSamples);
        const double repick = firstBreak(trace, from, int(median) + outlierSamples + 1);
        cleaned[size_t(t)] = std::isfinite(repick) ? repick : snapToEvent(trace.data(), samplesPerTrace, median, 4, mode);
    }
    return cleaned;
}

std::vector<double> pickBetween(const HorizonTraceFetch& fetch, int samplesPerTrace, int64_t traceA, double sampleA,
                                int64_t traceB, double sampleB, int searchSamples, int windowSamples, double trust,
                                PickMode mode) {
    const int64_t steps = std::llabs(traceB - traceA);
    std::vector<double> picks(static_cast<size_t>(steps + 1), kNaN);
    if (!fetch || samplesPerTrace < 3) return picks;
    const int64_t dir = traceB >= traceA ? 1 : -1;
    std::vector<float> previous(static_cast<size_t>(samplesPerTrace)), current(static_cast<size_t>(samplesPerTrace));
    fetch(traceA, previous.data());
    picks[0] = snapToEvent(previous.data(), samplesPerTrace, sampleA, 2, mode);
    if (steps == 0) return picks;
    const int half = std::max(2, windowSamples / 2);
    double previousPick = picks[0];
    for (int64_t i = 1; i < steps; ++i) {
        fetch(traceA + dir * i, current.data());
        const double line = sampleA + (sampleB - sampleA) * double(i) / double(steps);
        double best = -2.0;
        int bestCentre = int(std::lround(line));
        for (int s = -searchSamples; s <= searchSamples; ++s) {
            const int centre = int(std::lround(line)) + s;
            if (centre < 0 || centre >= samplesPerTrace) continue;
            const double c = windowCorrelation(previous, int(std::lround(previousPick)), current, centre, half);
            if (c > best) {
                best = c;
                bestCentre = centre;
            }
        }
        // Trust: only a correlation at least this good moves the pick off the line.
        const double pick = best >= trust ? snapToEvent(current.data(), samplesPerTrace, bestCentre, 2, mode) : line;
        picks[size_t(i)] = pick;
        previousPick = pick;
        std::swap(previous, current);
    }
    fetch(traceB, current.data());
    picks[size_t(steps)] = snapToEvent(current.data(), samplesPerTrace, sampleB, 2, mode);
    return picks;
}

void flattenTrace(std::vector<float>& trace, double horizonSample, double referenceSample) {
    if (trace.empty() || !std::isfinite(horizonSample) || !std::isfinite(referenceSample)) return;
    const double shift = horizonSample - referenceSample; // out[i] = in[i + shift]
    std::vector<float> out(trace.size(), 0.0f);
    const int n = int(trace.size());
    for (int i = 0; i < n; ++i) {
        const double x = double(i) + shift;
        const int i0 = int(std::floor(x));
        if (i0 < 0 || i0 + 1 >= n) {
            if (i0 == n - 1 && x == double(i0)) out[size_t(i)] = trace[size_t(i0)];
            continue;
        }
        const double f = x - i0;
        out[size_t(i)] = float(trace[size_t(i0)] * (1.0 - f) + trace[size_t(i0 + 1)] * f);
    }
    trace.swap(out);
}

} // namespace segy
