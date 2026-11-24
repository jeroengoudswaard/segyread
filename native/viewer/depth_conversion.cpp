#include "depth_conversion.h"

#include <algorithm>
#include <cmath>

namespace segy {

namespace {

// Linear interpolation of y over increasing x; `tail` extends past the end
// with slope `tailSlope` (y per x). Before the start: y[0] (x[0] is 0).
double lookup(const std::vector<double>& x, const std::vector<double>& y, double at, double tailSlope) {
    if (x.empty()) return 0.0;
    if (at <= x.front()) return y.front();
    if (at >= x.back()) return y.back() + (at - x.back()) * tailSlope;
    const size_t hi = size_t(std::upper_bound(x.begin(), x.end(), at) - x.begin());
    const size_t lo = hi - 1;
    const double f = (at - x[lo]) / (x[hi] - x[lo]);
    return y[lo] + (y[hi] - y[lo]) * f;
}

double sampleAt(const float* trace, int count, double x) {
    if (x < 0.0 || x > double(count - 1)) return 0.0;
    const int i0 = int(std::floor(x));
    if (i0 >= count - 1) return trace[count - 1];
    const double f = x - i0;
    return trace[i0] * (1.0 - f) + trace[i0 + 1] * f;
}

} // namespace

double TimeDepthMap::depthAt(double t) const { return lookup(time, depth, t, tailVelocity / 2.0); }

double TimeDepthMap::timeAt(double z) const { return lookup(depth, time, z, 2.0 / tailVelocity); }

TimeDepthMap buildTimeDepthMap(const float* velocity, int count, double interval, bool velocityInDepth,
                               VelocityType type, std::string* error) {
    TimeDepthMap map;
    if (!velocity || count < 1 || !(interval > 0.0)) {
        if (error) *error = "Empty velocity trace.";
        return map;
    }
    if (velocityInDepth && type == VelocityType::Rms) {
        if (error) *error = "RMS velocities are only supported in time.";
        return map;
    }
    std::vector<double> v(static_cast<size_t>(count));
    double previous = 1500.0;
    for (int i = 0; i < count; ++i) {
        const double value = velocity[i];
        v[size_t(i)] = std::isfinite(value) && value > 0.0 ? value : previous;
        previous = v[size_t(i)];
    }
    map.time.assign(static_cast<size_t>(count), 0.0);
    map.depth.assign(static_cast<size_t>(count), 0.0);
    // The interval velocity of layer i (between samples i-1 and i).
    std::vector<double> vint(static_cast<size_t>(count), v[0]);
    if (!velocityInDepth) {
        for (int i = 0; i < count; ++i) map.time[size_t(i)] = i * interval;
        if (type == VelocityType::Rms) {
            // Dix: vint_i^2 = (vrms_i^2 t_i - vrms_{i-1}^2 t_{i-1}) / (t_i - t_{i-1}).
            for (int i = 1; i < count; ++i) {
                const double t1 = map.time[size_t(i)], t0 = map.time[size_t(i - 1)];
                const double num = v[size_t(i)] * v[size_t(i)] * t1 - v[size_t(i - 1)] * v[size_t(i - 1)] * t0;
                vint[size_t(i)] = num > 0.0 ? std::sqrt(num / (t1 - t0)) : vint[size_t(i - 1)];
            }
        } else if (type == VelocityType::Interval) {
            vint = v;
        }
        for (int i = 1; i < count; ++i) {
            if (type == VelocityType::Average) {
                const double z = v[size_t(i)] * map.time[size_t(i)] / 2.0;
                map.depth[size_t(i)] = std::max(z, map.depth[size_t(i - 1)] + 1e-6);
                vint[size_t(i)] = 2.0 * (map.depth[size_t(i)] - map.depth[size_t(i - 1)]) / interval;
            } else {
                map.depth[size_t(i)] = map.depth[size_t(i - 1)] + vint[size_t(i)] * interval / 2.0;
            }
        }
    } else {
        for (int i = 0; i < count; ++i) map.depth[size_t(i)] = i * interval;
        for (int i = 1; i < count; ++i) {
            if (type == VelocityType::Average) {
                const double t = 2.0 * map.depth[size_t(i)] / v[size_t(i)];
                map.time[size_t(i)] = std::max(t, map.time[size_t(i - 1)] + 1e-9);
                vint[size_t(i)] = 2.0 * interval / (map.time[size_t(i)] - map.time[size_t(i - 1)]);
            } else {
                vint[size_t(i)] = v[size_t(i)];
                map.time[size_t(i)] = map.time[size_t(i - 1)] + 2.0 * interval / v[size_t(i)];
            }
        }
    }
    map.tailVelocity = vint.back() > 0.0 ? vint.back() : 1500.0;
    return map;
}

void convertTrace(const float* input, int inputCount, double inputInterval, const TimeDepthMap& map, bool toDepth,
                  float* output, int outputCount, double outputInterval) {
    for (int k = 0; k < outputCount; ++k) {
        const double out = k * outputInterval;
        const double in = toDepth ? map.timeAt(out) : map.depthAt(out);
        output[k] = inputCount > 0 && inputInterval > 0.0 ? float(sampleAt(input, inputCount, in / inputInterval)) : 0.0f;
    }
}

void interpolateVelocity(const std::vector<int64_t>& velocityCdps, const std::vector<std::vector<float>>& traces,
                         int64_t cdp, int count, float* out) {
    if (traces.empty()) {
        std::fill(out, out + count, 0.0f);
        return;
    }
    auto copy = [&](size_t i) { std::copy_n(traces[i].begin(), count, out); };
    if (traces.size() == 1 || cdp <= velocityCdps.front()) return copy(0);
    if (cdp >= velocityCdps.back()) return copy(traces.size() - 1);
    const size_t hi = size_t(std::upper_bound(velocityCdps.begin(), velocityCdps.end(), cdp) - velocityCdps.begin());
    const size_t lo = hi - 1;
    const double span = double(velocityCdps[hi] - velocityCdps[lo]);
    const double f = span > 0.0 ? double(cdp - velocityCdps[lo]) / span : 0.0;
    for (int i = 0; i < count; ++i) {
        out[i] = float(traces[lo][size_t(i)] * (1.0 - f) + traces[hi][size_t(i)] * f);
    }
}

} // namespace segy
