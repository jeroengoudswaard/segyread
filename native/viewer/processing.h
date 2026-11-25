// Trace processing and attributes (see native/README.md, "Filters, deconvolution
// and attributes"): f-k velocity filter, spectral whitening, predictive
// deconvolution, and the complex-trace attributes (envelope, instantaneous
// phase and frequency, phase rotation). Qt-free, covered by segytest.
#pragma once
#include <complex>
#include <cstdint>
#include <vector>

namespace segy {

// ---- Mutes -------------------------------------------------------------------

// The live part of a trace: [first, last] non-zero sample, or first > last
// for a dead trace. Leading and trailing exact zeros are a mute (a water
// column, a top mute) that filters must not fill in.
struct LiveRange {
    size_t first = 1, last = 0;
};
LiveRange liveRange(const float* samples, size_t count);
// Zeros `samples` outside `range` (everything for a dead trace).
void applyMute(float* samples, size_t count, const LiveRange& range);

// ---- Complex trace (analytic signal) ---------------------------------------

// x + i H[x], via the FFT, zero-padded by at least half the trace length
// (to a power of two) so the circular Hilbert transform's wrap-around is
// negligible: the kernel 1/(pi t) is below 1e-3 that far out for any
// trace over a few hundred samples. Half the FFT of padding to twice.
std::vector<std::complex<float>> analyticSignal(const std::vector<float>& trace);

// |x + iH[x]|.
void envelope(std::vector<float>& trace);
// atan2(H[x], x) in degrees, -180..180.
void instantaneousPhase(std::vector<float>& trace);
// The phase's rate of change in Hz: the angle of a[t+1] conj(a[t-1])
// summed over a 5-sample window (weighted by the envelope squared, so it
// does not spike where the envelope is near zero) over 2 pi 2 dt; exact for
// a pure tone. Clamped to 0..Nyquist.
void instantaneousFrequency(std::vector<float>& trace, double sampleIntervalUs);
// Constant phase rotation: x cos(theta) - H[x] sin(theta). cos(wt) becomes
// cos(wt + theta); 180 degrees flips the polarity, 90 degrees turns a
// zero-phase wavelet into an antisymmetric one.
void phaseRotate(std::vector<float>& trace, double degrees);

// ---- Trapezoidal (Ormsby) bandpass ------------------------------------------

struct OrmsbyParams {
    double lowCutHz = 5.0;   // f1: zero below
    double lowPassHz = 10.0; // f2: full from here ...
    double highPassHz = 50.0;// f3: ... to here
    double highCutHz = 70.0; // f4: zero above
};

// The trapezoid's amplitude at f: 0 below f1 and above f4, 1 from f2 to f3,
// linear ramps in between (f1 == f2 or f3 == f4: a hard edge).
double ormsbyWeight(double frequencyHz, const OrmsbyParams& params);

// Zero-phase trapezoidal bandpass in the frequency domain. The trace is
// mirrored past both ends (a quarter of its length, at least 64 samples,
// like the Butterworth filter's even padding) so the ends do not ring,
// FFT'd, weighted, inverted.
void ormsbyFilter(std::vector<float>& trace, double sampleIntervalUs, const OrmsbyParams& params);

// ---- Spectral whitening -----------------------------------------------------

struct WhiteningParams {
    double lowHz = 8.0;        // whitened band, flat between low and high
    double highHz = 60.0;
    double smoothingHz = 10.0; // running-average width of the amplitude spectrum
};

// Divides the amplitude spectrum by its own running average (smoothingHz
// wide), keeps the phase, tapers outside [low, high] with cosine ramps one
// smoothing width wide, and restores the trace's RMS. Dead traces stay dead.
void spectralWhitening(std::vector<float>& trace, double sampleIntervalUs, const WhiteningParams& params);

// ---- Predictive deconvolution ----------------------------------------------

struct DeconParams {
    double operatorMs = 160.0;     // prediction filter length
    double gapMs = 24.0;           // prediction distance (one sample = spiking decon)
    double whiteNoisePercent = 0.1;
};

// Solves the Toeplitz system R a = g (R symmetric, first column r) with
// Levinson's recursion. r.size() == g.size() == n; returns a (n values), or
// all zeros when the system is singular.
std::vector<double> levinsonSolve(const std::vector<double>& r, const std::vector<double>& g);

// Wiener prediction-error filter designed on the whole trace (its
// autocorrelation, prewhitened): y(t) = x(t) - sum a_k x(t - gap - k).
// Removes what is predictable from `gap` on (reverberations, short-period
// multiples); a gap of one sample is spiking deconvolution.
void predictiveDeconvolution(std::vector<float>& trace, double sampleIntervalUs, const DeconParams& params);

// ---- f-k filter -------------------------------------------------------------

// A range of apparent velocities |f| / |k| that is passed (everything
// else removed) or rejected (everything else kept). The range is passed or
// removed in full; the cosine taper lies outside it, from (1 - taper) x
// min up to min and from max up to (1 + taper) x max. maxVelocity 0 = no
// upper limit (flat events, k = 0, have infinite apparent velocity).
// The default rejects 0..1500 m/s: steep dips, ground roll.
struct FkFilterParams {
    double minVelocity = 0.0;    // m/s
    double maxVelocity = 1500.0; // m/s, 0 = no upper limit
    double traceSpacing = 12.5;  // m between traces
    double taperPercent = 20.0;
    bool pass = false;           // true: keep the range; false: remove it
};

// Velocity (fan) filter on one panel of consecutive traces, in place
// (traces[i] = one trace, all the same length): 2D FFT (zero-padded to
// powers of two, the traces to twice their count), weighted by
// fkFilterWeight, inverse.
void fkFilterPanel(std::vector<std::vector<float>>& traces, double sampleIntervalUs, const FkFilterParams& params);

// The weights for every (frequency, wavenumber) cell of a panel of `traces`
// x `samples`, computed once and reused for every panel of a file (the
// weight costs a division and a cosine; a 6 GB file has ~10k panels).
// Columns whose weights are all 1 are skipped, all 0 are zeroed.
struct FkFilterPlan {
    size_t traces = 0, samples = 0, nt = 0, nk = 0;
    std::vector<float> weights;     // (nt / 2 + 1) x nk, row = frequency
    std::vector<uint8_t> columnKind; // per frequency: 0 mixed, 1 all ones, 2 all zeros
};
FkFilterPlan makeFkFilterPlan(size_t traces, size_t samples, double sampleIntervalUs, const FkFilterParams& params);
// Same result as fkFilterPanel for a panel of the plan's size. Two traces
// share each time FFT (real inputs packed as real and imaginary parts).
void fkFilterPanel(std::vector<std::vector<float>>& traces, const FkFilterPlan& plan);

// The weight fkFilterPanel applies at frequency f (Hz) and wavenumber k
// (cycles/m).
double fkFilterWeight(double frequencyHz, double wavenumberPerM, const FkFilterParams& params);

// ---- Statistical phase estimate ---------------------------------------------
//
// Ockham's razor: the constant phase rotation that makes the data simplest
// (spikiest) is the one that makes the wavelet zero-phase. Scanned over
// -90..+89 degrees in 1-degree steps (both measures repeat every 180
// degrees: they cannot tell polarity). Each rotation is x cos(a) - H[x]
// sin(a), exactly phaseRotate's, so the best angle is the Phase Rotation to
// apply; the wavelet's phase is about minus that angle (or 180 off).
//  - Maximum kurtosis (van der Baan 2008): N sum x^4 / (sum x^2)^2 over all
//    samples in the window together.
//  - Minimum entropy (Wiggins' varimax norm, as Levy & Oldenburg 1987 used
//    it for phase correction of stacks): n sum x^4 / (sum x^2)^2 per trace,
//    averaged, so every trace counts the same however loud it is. (Low-order
//    measures -- Shannon entropy of x^2 / sum x^2, L1/L2, negentropy with
//    log cosh -- were tried and prefer a two-lobed 90-degree wavelet on
//    synthetics; the varimax norm is the Renyi order-2 entropy.)
//  - Agreement: the product of the two normalised scores.
// Both are trigonometric polynomials in the angle: eight sums of the
// analytic signal (per trace for the varimax) give the whole scan.

constexpr int kPhaseScanSteps = 180; // -90 .. +89 degrees

struct PhaseScanMoments {
    double a2 = 0, ab = 0, b2 = 0;                    // sums of a^2, ab, b^2 (a = x, b = H[x])
    double a4 = 0, a3b = 0, a2b2 = 0, ab3 = 0, b4 = 0; // fourth moments
    double count = 0;                                 // samples
};

struct PhaseScanSums {
    PhaseScanMoments pooled;
    std::vector<double> varimaxSum = std::vector<double>(kPhaseScanSteps, 0.0); // per angle, over traces
    double traces = 0;
};

// Adds one trace (already cut to the analysis window). Dead traces add nothing.
void addPhaseScanTrace(PhaseScanSums& sums, const std::vector<float>& trace);
void mergePhaseScan(PhaseScanSums& into, const PhaseScanSums& from);

struct PhaseScanResult {
    std::vector<double> anglesDeg;      // -90 .. 89
    std::vector<double> kurtosis;       // pooled kurtosis per angle
    std::vector<double> varimax;        // mean per-trace varimax per angle
    std::vector<double> kurtosisScore;  // 0..1, 1 = best
    std::vector<double> entropyScore;   // 0..1, 1 = best (highest varimax = lowest entropy)
    std::vector<double> agreementScore; // kurtosisScore x entropyScore, rescaled to 0..1
    double kurtosisAngle = 0.0;         // best angles, refined between the 1-degree steps
    double entropyAngle = 0.0;
    double agreementAngle = 0.0;
    double spreadDeg = 0.0;             // kurtosis vs. entropy angle, modulo 180
    bool valid = false;
};
PhaseScanResult finishPhaseScan(const PhaseScanSums& sums);

} // namespace segy
