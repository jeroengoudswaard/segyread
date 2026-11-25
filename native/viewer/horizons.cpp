#include "horizons.h"
#include "threadpool.h"

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
    // (Nearly) empty traces get no pick: their peak is noise, not a seabed.
    // The first pass is per trace and independent: in parallel (`fetch`
    // must be thread-safe; decoding from an mmap is).
    std::vector<float> peaks(static_cast<size_t>(n), 0.0f);
    {
        ThreadPool pool;
        pool.parallelFor(size_t(n), 256, [&](size_t begin, size_t end) {
            std::vector<float> local(static_cast<size_t>(samplesPerTrace));
            for (size_t t = begin; t < end; ++t) {
                fetch(traceStart + int64_t(t), local.data());
                for (int i = std::max(0, firstSample); i < samplesPerTrace; ++i) {
                    peaks[t] = std::max(peaks[t], std::fabs(local[size_t(i)]));
                }
                picks[t] = firstBreak(local, 0, samplesPerTrace);
            }
        });
    }
    std::vector<float> sortedPeaks = peaks;
    std::nth_element(sortedPeaks.begin(), sortedPeaks.begin() + std::ptrdiff_t(sortedPeaks.size() / 2), sortedPeaks.end());
    const float deadBelow = 0.05f * sortedPeaks[sortedPeaks.size() / 2];
    std::vector<bool> dead(static_cast<size_t>(n), false);
    for (int64_t t = 0; t < n; ++t) {
        if (peaks[size_t(t)] <= deadBelow) {
            dead[size_t(t)] = true;
            picks[size_t(t)] = kNaN;
        }
    }
    // Neighbour consistency: re-pick outliers near the local median. The
    // median reads the already cleaned picks (in place, left to right), so a
    // run of bad picks at an edge is repaired from the good side inwards.
    std::vector<double>& cleaned = picks;
    for (int64_t t = 0; t < n; ++t) {
        if (dead[size_t(t)]) continue;
        std::vector<double> near;
        for (int64_t k = std::max<int64_t>(0, t - 10); k <= std::min<int64_t>(n - 1, t + 10); ++k) {
            if (k != t && std::isfinite(cleaned[size_t(k)])) near.push_back(cleaned[size_t(k)]);
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
    // Runs of bad picks too long for the median (a fading seabed at the edge
    // of the data picks a deeper reflector for dozens of traces): split the
    // picks into continuous segments (a jump of more than outlierSamples or
    // a dead trace ends one) and re-track every short segment from an
    // adjacent long one, trace by trace.
    const int64_t kShortSegment = 50;
    struct Segment { int64_t begin, end; };
    std::vector<Segment> segments;
    for (int64_t t = 0; t < n;) {
        if (!std::isfinite(cleaned[size_t(t)])) { ++t; continue; }
        int64_t e = t + 1;
        while (e < n && std::isfinite(cleaned[size_t(e)]) &&
               std::fabs(cleaned[size_t(e)] - cleaned[size_t(e - 1)]) <= outlierSamples) ++e;
        segments.push_back({t, e});
        t = e;
    }
    auto isLong = [&](const Segment& sg) { return sg.end - sg.begin >= kShortSegment; };
    // Tracking ends (NaN) where the seabed itself is gone -- muted or
    // tapered away while deeper data remain: nothing within the window
    // reaches 10% of the median trace peak.
    const float goneBelow = 2.0f * deadBelow;
    auto track = [&](int64_t t, double previous) {
        if (!std::isfinite(previous)) {
            cleaned[size_t(t)] = kNaN;
            return;
        }
        fetch(traceStart + t, trace.data());
        const int from = std::max(0, int(previous) - outlierSamples);
        float local = 0.0f;
        for (int i = from; i <= std::min(samplesPerTrace - 1, int(previous) + outlierSamples); ++i) {
            local = std::max(local, std::fabs(trace[size_t(i)]));
        }
        if (local < goneBelow) {
            cleaned[size_t(t)] = kNaN;
            return;
        }
        const double repick = firstBreak(trace, from, int(previous) + outlierSamples + 1);
        cleaned[size_t(t)] = std::isfinite(repick) && std::fabs(repick - previous) <= outlierSamples
                                 ? repick
                                 : snapToEvent(trace.data(), samplesPerTrace, previous, 4, mode);
    };
    if (std::any_of(segments.begin(), segments.end(), isLong)) {
        for (size_t i = 0; i < segments.size(); ++i) { // left to right: from a long left neighbour
            if (isLong(segments[i]) || i == 0 || segments[i - 1].end != segments[i].begin) continue;
            if (!isLong(segments[i - 1])) continue;
            for (int64_t t = segments[i].begin; t < segments[i].end; ++t) track(t, cleaned[size_t(t - 1)]);
            segments[i - 1].end = segments[i].end; // merged: now part of the long segment
            segments[i] = segments[i - 1];
        }
        for (size_t i = segments.size(); i-- > 0;) { // right to left: what is left over
            if (isLong(segments[i]) || i + 1 >= segments.size() || segments[i].end != segments[i + 1].begin) continue;
            if (!isLong(segments[i + 1])) continue;
            for (int64_t t = segments[i].end - 1; t >= segments[i].begin; --t) track(t, cleaned[size_t(t + 1)]);
            segments[i + 1].begin = segments[i].begin;
            segments[i] = segments[i + 1];
        }
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

std::vector<double> fillHorizonGaps(std::vector<double> picks) {
    const int64_t n = int64_t(picks.size());
    int64_t previous = -1;
    for (int64_t t = 0; t < n; ++t) {
        if (!std::isfinite(picks[size_t(t)])) continue;
        if (previous < 0) {
            for (int64_t k = 0; k < t; ++k) picks[size_t(k)] = picks[size_t(t)];
        } else {
            for (int64_t k = previous + 1; k < t; ++k) {
                const double f = double(k - previous) / double(t - previous);
                picks[size_t(k)] = picks[size_t(previous)] * (1.0 - f) + picks[size_t(t)] * f;
            }
        }
        previous = t;
    }
    if (previous >= 0) {
        for (int64_t k = previous + 1; k < n; ++k) picks[size_t(k)] = picks[size_t(previous)];
    }
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
