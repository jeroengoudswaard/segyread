// Butterworth IIR filters in second-order sections, applied zero-phase
// forward-backward -- a port of scipy.signal's
//   butter(order, ..., output='sos') + sosfiltfilt(sos, x)
// with sosfiltfilt's defaults (padtype='odd', default padlen, steady-state
// initial conditions from sosfilt_zi), so results match scipy to floating-
// point precision (checked against scipy 1.18 reference values in
// test_main.cpp). Used by the Octave Band Display; Edit > Processing >
// Bandpass keeps its FFT trapezoid (Ormsby-style) filter, see bandpass.h.
//
// Edge handling, the reason filtfilt is the standard: before filtering,
// the trace is extended at both ends by an odd (point-symmetric) reflection
// of itself (2*x[0] - x[k]), and each pass starts from the filter's
// steady-state for its first sample, so neither end gets a start-up
// transient or an artificial step to zero. The cascade is computed in
// double precision; float only at the interface.
#pragma once
#include <vector>

namespace segy {

// One second-order section, a0 normalized to 1:
//   H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
// A first-order section has b2 == a2 == 0.
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
};

// scipy.signal.butter(order, [lowHz, highHz], btype='band', fs=sampleRateHz,
// output='sos'). Requires 0 < lowHz < highHz < sampleRateHz / 2; returns
// `order` sections (the bandpass has order 2*order).
std::vector<Biquad> butterworthBandpass(int order, double lowHz, double highHz, double sampleRateHz);

// scipy.signal.butter(order, cutoffHz, btype='high', fs=sampleRateHz,
// output='sos'). Requires 0 < cutoffHz < sampleRateHz / 2.
std::vector<Biquad> butterworthHighpass(int order, double cutoffHz, double sampleRateHz);

// scipy.signal.sosfiltfilt's default padlen:
//   3 * (2 * nSections + 1 - min(#sections with b2 == 0, #sections with a2 == 0))
int sosFiltFiltDefaultPadlen(const std::vector<Biquad>& sos);

// scipy.signal.sosfiltfilt(sos, x, padtype='odd', padlen=padlen), in place.
// padlen < 0 means the default above. scipy raises when the trace is too
// short for the padding; here the padding is shortened to len(x) - 1
// instead, so a very short trace still filters rather than failing.
void sosFiltFilt(const std::vector<Biquad>& sos, std::vector<float>& x, int padlen = -1);

} // namespace segy
