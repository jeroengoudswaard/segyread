// Amplitude histogram and single-trace-average amplitude spectrum, backing
// the toolbar's Histogram/Spectrum parameters dialogs (see native/README.md).
// Platform/GUI-free like renderer.h/chrome.h/wiggle.h -- viewer_qt.cpp calls
// into this and draws the result, nothing here touches Qt.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
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

// Normalized autocorrelation of one trace for lags 0..maxLag (in samples):
//   r[k] = sum_i x[i] * x[i+k] / sum_i x[i]^2
// so r[0] == 1 and |r[k]| <= 1 -- the "biased" estimate (no 1/(n-k)
// rescaling), the usual choice for seismic autocorrelograms since it can't
// blow up at long lags. Negative lags mirror positive ones (r[-k] == r[k]),
// so callers build the -L..+L display from this. An all-zero trace gives
// all zeros. maxLag is clamped to n - 1. Direct O(n * maxLag) sum: exact,
// and cheap at autocorrelogram sizes (a few hundred traces/lags).
std::vector<float> normalizedAutocorrelation(const float* samples, int count, int maxLag);

// Normalized cross-correlation of a with b for lags minLag..maxLag (in
// samples, either may be negative; the result has maxLag - minLag + 1
// values):  c[k] = sum_i a[i] * b[i + k] / sqrt(sum a^2 * sum b^2).
// Positive k: b lags a (an event in b comes k samples later). With a == b
// this is the normalized autocorrelation over an asymmetric lag window.
// Zero energy in either gives zeros.
std::vector<float> normalizedCrossCorrelation(const float* a, const float* b, int count, int minLag, int maxLag);

// Peak-to-peak distance from a correlation over lags minLag..maxLag: the
// side peak nearest to lag 0 -- the first local maximum past the central
// lobe, on either side -- whose value is at least `threshold`. Returns its
// lag in samples (parabolic refinement, sign kept), or NaN when there is
// none.
double nearestSidePeakLag(const std::vector<float>& correlation, int minLag, double threshold);

// ---------------------------------------------------------------------------
// Signal/noise estimation, SIGCOR SPECTR/STNDRD (see native/README.md,
// "Signal/Noise"). A C++ port of the user's gold-standard MATLAB reference
// sigcor_spectr_stndrd.m; every step below names its MATLAB counterpart.
//
// Pairs (streaming model): every current trace c draws actualSkip uniformly
// from NSKIP-IRANSK .. NSKIP+IRANSK and is paired with the earlier trace
// c - (actualSkip + 1). NSKIP is the number of traces BETWEEN the pair.
// Per pair, on the raw gates: normalised correlation at integer lags
// -SHMAX..+SHMAX over the overlapping parts; a maximum at either end of
// the range rejects the pair, a coefficient below CCMIN too. A parabola
// through the peak refines the lag, which is then rounded to a whole
// sample (NIPOL=1). On the overlap at that lag: remove DC, unit RMS, put
// back into a zero-filled gate, Kaiser taper, FFT. Per pair
//     T = (|X|^2 + |Y|^2) / 2,  S = Re(X conj(Y)),  N = |X - Y|^2 / 2,
// averaged over the accepted pairs.

enum class SigcorOption {
    Spectr, // periodograms of the tapered gates, averaged in the frequency domain (the reference)
    Autcor, // auto/crosscorrelations averaged in the time domain, transformed once at the end
};

struct SigcorParams {
    SigcorOption option = SigcorOption::Spectr;
    int nskip = 3;          // NSKIP: traces BETWEEN the two traces of a pair (pair distance = skip + 1), >= 0
    int iransk = 2;         // IRANSK: symmetric random variation, skip in nskip-iransk..nskip+iransk; <= nskip
    double shmaxMs = 20.0;  // SHMAX: largest lag searched; rounded to whole samples, >= 1 sample
    double ccmin = 0.65;    // CCMIN: pairs with a lower normalised correlation are rejected
    double kaiserBeta = 5.0; // Kaiser-Bessel taper parameter
    bool removeDc = true;   // remove the mean of the aligned overlap
    bool normalise = true;  // scale the aligned overlap to unit RMS
    uint32_t seed = 12345;  // MT19937 seed of the IRANSK draws
    int maxPairs = 20000;   // more candidate pairs than this: an even subset of them is computed
};

enum class SigcorPairStatus : uint8_t {
    Accepted,
    AtShiftLimit,   // correlation maximum at -SHMAX or +SHMAX
    LowCorrelation, // below CCMIN
    Dead,           // no finite correlation at any lag (an all-zero gate)
};

struct SigcorPair {
    int64_t earlierTrace = 0;
    int64_t currentTrace = 0;
    int actualSkip = 0;          // pair distance = actualSkip + 1
    int discreteLag = 0;         // samples; positive = the event is later on the current trace
    double parabolicLag = 0.0;   // samples, refined by the parabola (= discreteLag at the limit)
    int appliedLag = 0;          // round(parabolicLag), accepted pairs only
    double correlation = 0.0;    // normalised correlation at discreteLag (NaN for Dead)
    SigcorPairStatus status = SigcorPairStatus::Dead;
};

struct SigcorResult {
    std::string error; // set when the parameters or the gate are invalid (nothing computed)
    std::vector<float> frequencyHz;
    // Mean power spectra (linear, not normalised) over the accepted pairs.
    std::vector<double> tracePower;
    std::vector<double> signalPower; // the cospectrum: can be negative at single frequencies
    std::vector<double> noisePower;  // mean of |X - Y|^2 / 2, never negative
    std::vector<SigcorPair> pairs;   // every pair considered, in trace order
    int pairsCandidate = 0;          // pairs the streaming selection produced (before maxPairs)
    int pairsTotal = 0;              // pairs considered (= pairs.size())
    int pairsUsed = 0;
    int pairsLowCorrelation = 0;
    int pairsAtShiftLimit = 0;
    int pairsDead = 0;
    int shmaxSamples = 0;
    int fftLength = 0;
    // Mean and 1 sigma of the accepted pairs' parabolic lags, in ms.
    double shiftMeanMs = 0.0;
    double shiftSigmaMs = 0.0;
    int gateSamples = 0;
};

// Fills `out` with all `samplesPerTrace` samples of trace `traceIndex`.
// Must be safe to call from several threads at once.
using SigcorTraceFetch = std::function<void(int64_t traceIndex, float* out)>;

// Gate = samples [gateStart, gateEnd) of every trace in [traceStart,
// traceEnd). `progress(done, total)` is optional and may be called from
// worker threads.
SigcorResult computeSigcor(const SigcorTraceFetch& fetch, int samplesPerTrace, int64_t traceStart, int64_t traceEnd,
                           int gateStart, int gateEnd, double sampleIntervalUs, const SigcorParams& params,
                           const std::function<void(int, int)>& progress = nullptr);

// The IRANSK draws: actualSkip for current traces 2..count (MATLAB's
// currentTrace = 2:nTraces), one draw each, uniform over
// nskip-iransk..nskip+iransk from an MT19937 seeded with `seed` (53-bit
// uniform double u, skip = lo + floor(u * (hi - lo + 1))). Without
// variation every entry is nskip. Entry k belongs to current trace k + 1
// (0-based), so the vector has count - 1 entries.
std::vector<int> sigcorSkipDraws(int64_t count, int nskip, int iransk, uint32_t seed);

// Kaiser-Bessel window, w[k] = I0(beta sqrt(1 - q^2)) / I0(beta), q = 2k/(n-1) - 1.
std::vector<double> kaiserWindow(int n, double beta);

// Cosine-squared taper: 1 over the central `flatPercent` of n samples,
// sin^2 ramps to 0 over the rest on either side.
std::vector<float> cosineSquaredWindow(int n, double flatPercent);

// ---------------------------------------------------------------------------
// x-F display (see native/README.md, "x-F display"): per trace, the
// amplitude spectrum of a time gate, 0..Nyquist, on a frequency grid whose
// spacing is a whole number of microseconds-as-millihertz -- binIntervalUs
// -- so a dataset with sampleIntervalUs = binIntervalUs reads in Hz where
// the seismic reads in ms. SEG-Y caps both at int16, hence the limits.
struct XfGrid {
    int fftLength = 0;      // power of two >= the gate, at least 64
    double fftBinHz = 0.0;  // native FFT bin spacing
    int binIntervalUs = 0;  // output spacing in mHz (= "us" in the dataset), 1..32767
    int bins = 0;           // output bins, 0..Nyquist, <= 32767
};
XfGrid makeXfGrid(int gateSamples, double sampleIntervalUs);

// Linear amplitude |A(f)| of `count` samples on `grid`: mean removed,
// multiplied by `window` (count values), zero-padded FFT, linearly
// interpolated onto the output bins. `out` gets grid.bins values.
void xfAmplitudeSpectrum(const float* samples, int count, const std::vector<float>& window, const XfGrid& grid,
                         float* out);

// The x-f display's spectra: |A(f)| of gate [gateStart, gateEnd) for every
// trace in [traceStart, traceEnd), cosine-squared taper (80% flat), one
// vector of grid.bins per trace. Runs in parallel over traces, so `fetch`
// must be safe to call from several threads (decoding from the mmap is);
// `progress(done)` may also be called from worker threads.
std::vector<std::vector<float>> computeXfAmplitudes(const std::function<void(int64_t traceIndex, float* out)>& fetch,
                                                    int samplesPerTrace, int64_t traceStart, int64_t traceEnd,
                                                    int gateStart, int gateEnd, const XfGrid& grid,
                                                    const std::function<void(int64_t done)>& progress = nullptr);

// ---------------------------------------------------------------------------
// F-K (see native/README.md, "F-K window"): the 2D amplitude spectrum of
// panels of traces, power-averaged over the panels. A panel is a gather
// (a run of consecutive traces with the same key, e.g. FFID or CDP), or a
// sliding window of traces when the data has no gathers.
struct FkPanel {
    int64_t start = 0; // first trace (absolute index)
    int64_t end = 0;   // one past the last trace
    // Optional explicit trace order (absolute indices), e.g. a gather
    // sorted by offset; empty = start..end-1 in file order.
    std::vector<int64_t> traces;
    int64_t traceCount() const { return traces.empty() ? end - start : int64_t(traces.size()); }
    int64_t traceAt(int64_t i) const { return traces.empty() ? start + i : traces[size_t(i)]; }
};

// Runs of equal consecutive `keys`, as panels offset by `firstTrace`;
// runs shorter than `minTraces` are dropped.
std::vector<FkPanel> splitIntoGathers(const std::vector<int32_t>& keys, int64_t firstTrace, int minTraces);

// Windows of `width` traces with 50% overlap covering [start, end) (one
// shorter window when the range is narrower than `width`).
std::vector<FkPanel> slidingPanels(int64_t start, int64_t end, int width);

struct FkResult {
    int frequencies = 0;  // rows: f = row * frequencyStepHz, 0..Nyquist
    int wavenumbers = 0;  // columns: k = (col - wavenumbers / 2) / wavenumbers cycles/trace, -0.5..<0.5
    double frequencyStepHz = 0.0;
    std::vector<float> db; // frequencies x wavenumbers, row-major; 0 dB = max, floor -120
    int panelsUsed = 0;
};

// Runs in parallel (time FFTs over traces, space FFTs over frequencies),
// so `fetch` must be safe to call from several threads; `progress` is
// called from the calling thread, once per panel.
// Per panel: mean removed per trace, cosine-squared taper (80% flat) in
// time and across the traces, 2D FFT on a common power-of-two grid (the
// widest panel sets the wavenumber count). Sign convention: an event that
// gets later on later traces (t = t0 + p * x, p > 0) lands at k = f * p > 0.
FkResult computeFk(const SigcorTraceFetch& fetch, int samplesPerTrace, const std::vector<FkPanel>& panels,
                   int gateStart, int gateEnd, double sampleIntervalUs,
                   const std::function<void(int, int)>& progress = nullptr);

// Trace differentiation (slopeDbPerOctave > 0) or integration (< 0), full
// or fractional: the spectrum is multiplied by (i 2 pi f)^a with
// a = slope / 6.0206, so 6 dB/octave is exactly d/dt (or the integral dt),
// including its +90 (-90) degree phase, 3 dB/octave a half derivative
// (amplitude ~ sqrt(f), 45 degrees), 0 leaves the trace alone. The mean
// (DC) is handled outside the FFT: differentiation drops it, integration
// adds its exact integral (m t^alpha / Gamma(alpha + 1)) and then removes
// the arbitrary integration constant (the result's mean). Zero-padded to twice the length (no wrap-around). Time
// in seconds: full differentiation scales by 2 pi f.
void traceDerivative(std::vector<float>& trace, double sampleIntervalUs, double slopeDbPerOctave);

} // namespace segy
