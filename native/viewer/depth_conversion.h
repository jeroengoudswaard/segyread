// Time-depth conversion (see native/README.md, "Time-depth conversion"):
// a velocity trace becomes a monotonic two-way-time <-> depth mapping, and
// a seismic trace is resampled through it, time to depth or depth to time.
// Qt-free, covered by segytest. Units: seconds (two-way time), metres, m/s.
// Depth is below the zero of the data (sea level); no datum correction.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace segy {

enum class VelocityType { Interval, Average, Rms };

// Pairs of (two-way time, depth), both increasing, starting at (0, 0).
struct TimeDepthMap {
    std::vector<double> time;  // s
    std::vector<double> depth; // m
    // Beyond the last pair: the last interval velocity (m/s).
    double tailVelocity = 1500.0;

    double depthAt(double t) const;
    double timeAt(double z) const;
};

// Builds the mapping from one velocity trace of `count` values, sampled
// every `interval` (s when `velocityInDepth` is false, m when true).
// RMS velocities are turned into interval velocities with Dix's equation
// (time-domain only: RMS in depth is refused with an empty map and
// `error`). Non-positive velocities are replaced by the previous value
// (or 1500 m/s at the start).
TimeDepthMap buildTimeDepthMap(const float* velocity, int count, double interval, bool velocityInDepth,
                               VelocityType type, std::string* error = nullptr);

// Resamples a trace through the map: time to depth (`toDepth`, input
// every `inputInterval` s, output every `outputInterval` m) or depth to
// time (input every `inputInterval` m, output every `outputInterval` s).
// Linear interpolation; zero beyond the input.
void convertTrace(const float* input, int inputCount, double inputInterval, const TimeDepthMap& map, bool toDepth,
                  float* output, int outputCount, double outputInterval);

// Velocity for a trace at `cdp` from velocity traces at `velocityCdps`
// (sorted ascending, one entry per velocity trace in `traces`): linear
// interpolation between the two neighbouring CDPs, the nearest one
// outside their range. All traces have `count` samples.
void interpolateVelocity(const std::vector<int64_t>& velocityCdps, const std::vector<std::vector<float>>& traces,
                         int64_t cdp, int count, float* out);

} // namespace segy
