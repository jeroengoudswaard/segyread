// Horizon picking and flattening (see native/README.md, "Horizons"):
// automatic seabed picks, point-to-point picks between user anchors guided
// by trace-to-trace correlation, snapping to peak/trough/zero crossing, and
// flattening a trace on a horizon. Qt-free, covered by segytest. Times are
// in samples (fractional); the Qt shell converts to and from ms.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace segy {

enum class PickMode { Peak, Trough, ZeroCrossing };

// Reads all samples of one trace.
using HorizonTraceFetch = std::function<void(int64_t traceIndex, float* out)>;

// The event nearest to `sample` within +/-`radius` samples: the largest
// (Peak) or smallest (Trough) value, refined with a parabola, or the
// nearest sign change (ZeroCrossing, linearly interpolated). Returns
// `sample` itself when there is no such event in range.
double snapToEvent(const float* trace, int count, double sample, int radius, PickMode mode);

// Seabed per trace in [traceStart, traceEnd): the first sample from
// `firstSample` on whose |amplitude| reaches thresholdFraction x the
// trace's maximum, snapped forward to the event (`mode`). Picks that sit
// more than `outlierSamples` from the median of their +/-10 neighbours are
// re-picked near that median (neighbour consistency); runs of bad picks
// shorter than 50 traces next to a long continuous stretch are re-tracked
// from that stretch, trace by trace; tracking stops (NaN) where the seabed
// is muted away (nothing near the previous pick above 10% of the median
// trace peak). Dead or nearly empty
// traces (peak below 5% of the median trace peak) get NaN.
std::vector<double> autoPickSeabed(const HorizonTraceFetch& fetch, int64_t traceStart, int64_t traceEnd,
                                   int samplesPerTrace, double thresholdFraction, PickMode mode, int firstSample = 0,
                                   int outlierSamples = 10);

// Point-to-point: picks for every trace from traceA to traceB (inclusive,
// either direction) between the anchors sampleA and sampleB. Each trace in
// between is correlated (window of `windowSamples` around the previous
// pick) with the previous trace at shifts within +/-`searchSamples` of the
// straight line; a correlation of at least `trust` takes that position
// (snapped to the event), below it the pick stays on the line. Anchors are
// snapped too.
std::vector<double> pickBetween(const HorizonTraceFetch& fetch, int samplesPerTrace, int64_t traceA, double sampleA,
                                int64_t traceB, double sampleB, int searchSamples, int windowSamples, double trust,
                                PickMode mode);

// A value for every trace: NaN gaps between picks are filled by linear
// interpolation, before the first / after the last pick with that pick.
// All NaN stays all NaN.
std::vector<double> fillHorizonGaps(std::vector<double> picks);

// Shifts a trace so the horizon at `horizonSample` lands on
// `referenceSample` (linear interpolation; zero where it runs out).
void flattenTrace(std::vector<float>& trace, double horizonSample, double referenceSample);

} // namespace segy
