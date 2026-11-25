// Trace processing and attributes (see native/README.md, "Filters, deconvolution
// and attributes"): f-k velocity filter, spectral whitening, predictive
// deconvolution, and the complex-trace attributes (envelope, instantaneous
// phase and frequency, phase rotation). Qt-free, covered by segytest.
#pragma once
#include <complex>
#include <vector>

namespace segy {

// ---- Complex trace (analytic signal) ---------------------------------------

// x + i H[x], via the FFT (zero-padded to twice the next power of two, so
// the circular Hilbert transform does not wrap around the trace ends).
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

struct FkFilterParams {
    double velocity = 1500.0;    // m/s: apparent velocities below this are removed
    double traceSpacing = 12.5;  // m between traces
    double taperPercent = 20.0;  // cosine ramp from (1 - taper) x velocity up to velocity
};

// Velocity (fan) filter on one panel of consecutive traces, in place
// (traces[i] = one trace, all the same length): 2D FFT (zero-padded to
// powers of two, the traces to twice their count), weight 0 where the
// apparent velocity |f| / |k| is below (1 - taper) x velocity, 1 from
// velocity up (flat events, k = 0, always pass), cosine in between, inverse.
void fkFilterPanel(std::vector<std::vector<float>>& traces, double sampleIntervalUs, const FkFilterParams& params);

// The weight fkFilterPanel applies at frequency f (Hz) and wavenumber k
// (cycles/m).
double fkFilterWeight(double frequencyHz, double wavenumberPerM, const FkFilterParams& params);

} // namespace segy
