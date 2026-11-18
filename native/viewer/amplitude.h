// Display-time amplitude processing: computed (time-variant) gain and
// Automatic Gain Control -- COPILOT.md items 9 and 10. Pure computation, no
// GUI-toolkit dependency, shared by both display paths (renderer.cpp's
// variable density, wiggle.cpp's wiggle) the same way gainDb and
// reversePolarity are: applied to each trace's freshly decoded samples
// every frame, never written back to a dataset or file. Spectrum/Histogram/
// Octave Bands/Bandpass/Save SEG-Y keep working on the raw data.
//
// Order: computed gain first, then AGC (the conventional processing order --
// gain recovery, then AGC). With both on, AGC largely normalizes the curve
// away except within one window, which is expected.
//
// Performance: AmplitudeProcessor is built once per frame (the gain curve
// becomes a per-sample multiplier table there, so no pow() per sample), and
// apply() is O(samples) per trace regardless of AGC window length (prefix
// sums of squares). AGC needs data beyond the visible sample range to be
// view-independent (panning must not change a sample's AGC value), so
// callers decode paddingSamples() extra samples on each side, clamped to
// the trace, and discard them after apply().
#pragma once
#include <vector>

namespace segy {

struct GainPoint {
    double timeMs = 0.0;
    double gainDb = 0.0;
};

struct AmplitudeProcessing {
    // Computed gain: gain in dB at each time, linearly interpolated in dB
    // between points, held constant before the first and after the last.
    // Points need not be sorted -- AmplitudeProcessor sorts its own copy.
    bool gainCurveEnabled = false;
    std::vector<GainPoint> gainCurve;

    // AGC: each sample is divided by the RMS of a window of agcWindowMs
    // centered on it (shrinking at the trace ends), making every window's
    // RMS 1. The display clip then becomes 100 / agcLevelPercent (see
    // processedBaseClip), so a window's RMS sits at agcLevelPercent of the
    // color scale / wiggle excursion -- and gainDb still applies on top.
    bool agcEnabled = false;
    double agcWindowMs = 500.0;
    double agcLevelPercent = 25.0;

    bool active() const { return agcEnabled || (gainCurveEnabled && !gainCurve.empty()); }
};

// Gain in dB at `timeMs` for `curve` (sorted or not). 0 dB for an empty curve.
double gainCurveDbAt(const std::vector<GainPoint>& curve, double timeMs);

// The clip a display should use as its base (before gainDb) given the data's
// own raw clip: unchanged without AGC, 100 / agcLevelPercent with it (AGC
// output is in "window-RMS = 1" units, so the raw clip no longer applies).
float processedBaseClip(float rawClip, const AmplitudeProcessing& processing);

// Built once per frame for one sample range [firstSample, firstSample+count)
// shared by every trace in that frame; apply() is const and thread-safe
// (per-thread scratch), so render workers can call it concurrently.
class AmplitudeProcessor {
public:
    AmplitudeProcessor(const AmplitudeProcessing& processing, int firstSample, int count, double sampleIntervalMs);

    bool active() const { return active_; }
    // In place, on exactly `count` samples starting at `firstSample`.
    void apply(float* samples) const;

    // Extra samples to decode on each side of the visible range so AGC
    // results don't depend on where the view starts/ends. 0 without AGC.
    static int paddingSamples(const AmplitudeProcessing& processing, double sampleIntervalMs);

private:
    bool active_ = false;
    int count_ = 0;
    std::vector<float> gainTable_; // empty when computed gain is off
    bool agc_ = false;
    int agcHalfWindow_ = 1;
};

} // namespace segy
