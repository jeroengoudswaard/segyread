// Amplitude histogram and single-trace-average amplitude spectrum, backing
// the toolbar's Histogram/Spectrum parameters dialogs (see native/README.md).
// Platform/GUI-free like renderer.h/chrome.h/wiggle.h -- viewer_qt.cpp calls
// into this and draws the result, nothing here touches Qt.
#pragma once
#include <cstdint>
#include <functional>
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

// ---------------------------------------------------------------------------
// Signal/noise estimation after Shell SIPMAP's SIGCOR (see native/README.md,
// "Signal/Noise"). Pairs of traces a lateral distance NSKIP (+ a random
// 0..IRANSK extra) apart are time-aligned by crosscorrelation within +/-SHMAX;
// the signal power spectrum is the mean cospectrum (real part of the cross
// spectrum) of the aligned gates, the trace spectrum the mean of their power
// spectra, and noise = trace - signal.

enum class SigcorOption {
    Spectr, // periodograms of the tapered gates, averaged in the frequency domain
    Autcor, // auto/crosscorrelations averaged in the time domain, transformed once at the end
};

enum class SigcorCrossCorrelator {
    Basic,      // unnormalized crosscorrelation picks the shift
    Normalized, // each lag weighted by the RMS of the overlapping gate parts
};

struct SigcorParams {
    SigcorOption option = SigcorOption::Spectr;
    int nskip = 3;  // lateral distance (traces) between the two traces of a pair, >= 1
    int iransk = 2; // random extra distance, uniform 0..iransk
    double shmaxMs = 30.0;
    double ccmin = 0.3; // pairs with a lower correlation coefficient add no signal estimate
    SigcorCrossCorrelator tcross = SigcorCrossCorrelator::Basic;
    double flatPercent = 5.0; // cosine-squared taper: flat part, % of the gate
    bool normalise = true;    // equalize the energy of the two gates
    bool removeDc = true;     // remove each gate's mean before tapering
    uint32_t seed = 1;        // IRANSK's random distances are reproducible
    int maxPairs = 2000;      // start traces are strided down to this many
};

struct SigcorResult {
    std::vector<float> frequencyHz;
    // Mean power spectra (linear, not normalized), over the accepted pairs.
    std::vector<double> tracePower;
    std::vector<double> signalPower;
    std::vector<double> noisePower; // trace - signal, may dip below zero at single bins
    // Per accepted pair: first trace index, shift (ms, positive = later on the
    // later trace) and correlation coefficient of the aligned gates.
    std::vector<int64_t> pairTrace;
    std::vector<float> pairShiftMs;
    std::vector<float> pairCorrelation;
    int pairsTotal = 0;
    int pairsUsed = 0;
    int pairsLowCorrelation = 0; // below CCMIN
    int pairsAtShiftLimit = 0;   // crosscorrelation maximum at +/-SHMAX
    int pairsDead = 0;           // an all-zero gate
    double shiftMeanMs = 0.0;
    double shiftSigmaMs = 0.0; // 1 sigma of pairShiftMs
    int gateSamples = 0;
};

// Fills `out` with all `samplesPerTrace` samples of trace `traceIndex`.
using SigcorTraceFetch = std::function<void(int64_t traceIndex, float* out)>;

// Gate = samples [gateStart, gateEnd) of every trace in [traceStart,
// traceEnd). `progress(done, total)` is optional, called once per pair.
SigcorResult computeSigcor(const SigcorTraceFetch& fetch, int samplesPerTrace, int64_t traceStart, int64_t traceEnd,
                           int gateStart, int gateEnd, double sampleIntervalUs, const SigcorParams& params,
                           const std::function<void(int, int)>& progress = nullptr);

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
// (amplitude ~ sqrt(f), 45 degrees), 0 leaves the trace alone. The mean is
// removed first (and after integration: the integration constant); DC
// gets zero gain. Zero-padded to twice the length (no wrap-around). Time
// in seconds: full differentiation scales by 2 pi f.
void traceDerivative(std::vector<float>& trace, double sampleIntervalUs, double slopeDbPerOctave);

// Shifts x by a fraction of a sample: out[i] = x(i + shift), band-limited
// (phase ramp in the frequency domain). |shift| <= 1 is the intended use.
std::vector<float> fractionalShift(const std::vector<float>& x, double shift);

} // namespace segy
