#include "bandpass.h"
#include "fft.h"

#include <cmath>
#include <complex>

namespace segy {

namespace {

// Raised-half-cosine ramp: 0 at/before `lo`, rises smoothly to 1 at `hi` --
// a Hanning-shaped taper rather than a hard (boxcar) step, which is what
// keeps the filter from ringing at its cutoffs.
double taperGain(double freq, double lo, double hi) {
    if (hi <= lo) return freq < lo ? 0.0 : 1.0;
    if (freq <= lo) return 0.0;
    if (freq >= hi) return 1.0;
    double t = (freq - lo) / (hi - lo);
    return 0.5 - 0.5 * std::cos(t * std::acos(-1.0));
}

double bandGain(double freq, const BandpassParams& p) {
    if (freq <= p.lowCut || freq >= p.highCut) return 0.0;
    if (freq < p.lowPass) return taperGain(freq, p.lowCut, p.lowPass);
    if (freq > p.highPass) return 1.0 - taperGain(freq, p.highPass, p.highCut);
    return 1.0;
}

} // namespace

void applyBandpassFilter(std::vector<float>& samples, double sampleIntervalUs, const BandpassParams& params) {
    size_t n = samples.size();
    if (n < 2 || sampleIntervalUs <= 0.0) return;

    size_t fftLength = nextPowerOfTwo(n);
    std::vector<std::complex<float>> spectrum(fftLength, std::complex<float>(0.0f, 0.0f));
    for (size_t i = 0; i < n; ++i) spectrum[i] = std::complex<float>(samples[i], 0.0f);
    fft(spectrum);

    double dt = sampleIntervalUs / 1'000'000.0; // seconds
    double binHz = 1.0 / (double(fftLength) * dt);
    for (size_t k = 0; k < fftLength; ++k) {
        // Real input has a conjugate-symmetric spectrum: bin k and bin
        // (fftLength - k) are the same physical frequency (positive vs.
        // negative), so they must get the same real-valued gain -- an
        // asymmetric mask would leave the inverse FFT with a nonzero
        // imaginary part, which a real filter can't produce.
        size_t mirrored = k <= fftLength / 2 ? k : fftLength - k;
        double freq = double(mirrored) * binHz;
        spectrum[k] *= float(bandGain(freq, params));
    }

    ifft(spectrum);
    for (size_t i = 0; i < n; ++i) samples[i] = spectrum[i].real();
}

} // namespace segy
