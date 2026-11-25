// Butterworth IIR filters in second-order sections, applied zero-phase
// forward-backward -- a port of scipy.signal's
//   butter(order, ..., output='sos') + sosfiltfilt(sos, x)
// with sosfiltfilt's defaults (padtype='odd', default padlen, steady-state
// initial conditions from sosfilt_zi), so results match scipy to floating-
// point precision (checked against scipy 1.18 reference values in
// test_main.cpp). Used by the Octave Band Display and Edit > Processing >
// Bandpass.
//
// Edge handling, the reason filtfilt is the standard: before filtering,
// the trace is extended at both ends by a reflection of itself, and each
// pass starts from the filter's steady state for its first sample, so
// neither end gets a start-up transient or an artificial step to zero. The
// cascade is computed in double precision; float only at the interface.
//
// The app uses EVEN padding (a plain mirror, scipy padtype='even'), not
// scipy's default ODD padding (point reflection, 2*x[0] - x[k]). Odd
// padding keeps the end sample's value as the extension's level, so a trace
// that happens to end on a large high-frequency excursion gets a step-like
// extension whose low-frequency energy leaks into low passbands. Measured on
// real data (native/README.md), even padding roughly halved the edge error
// in the 4-8 and 8-16 Hz octaves; a longer padlen didn't help either type.
#pragma once
#include <string>
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

// scipy.signal.butter(order, cutoffHz, btype='low', fs=sampleRateHz,
// output='sos'). Requires 0 < cutoffHz < sampleRateHz / 2.
std::vector<Biquad> butterworthLowpass(int order, double cutoffHz, double sampleRateHz);

// scipy.signal.sosfiltfilt's padtype: how the trace is extended past each end.
enum class EdgePadding {
    Even,     // mirror: x[k] reflected about the end sample (padtype='even') -- the app's choice
    Odd,      // point reflection: 2*x[end] - x[k] (padtype='odd', scipy's default)
    Constant, // repeat the end sample (padtype='constant')
};

// Edit > Processing > Bandpass's filter: Butterworth between two corners
// (each at -3 dB per pass, so -6 dB after the forward-backward sosfiltfilt).
// lowHz <= 0 means no low cut (a lowpass at highHz); highHz >= Nyquist
// means no high cut (a highpass at lowHz).
// The low cut and the high cut can have their own order (lowOrder /
// highOrder, 0 = `order`); each order adds 6 dB/octave per pass, so 12 after
// sosfiltfilt. Equal orders use scipy's bandpass design exactly; different
// ones cascade a highpass (low corner) and a lowpass (high corner).
struct ButterworthBandParams {
    double lowHz = 10.0;
    double highHz = 50.0;
    int order = 4;
    EdgePadding padding = EdgePadding::Even; // passed to sosFiltFilt, not part of the design
    int lowOrder = 0;
    int highOrder = 0;
};

// Designs the filter for `params` at `sampleRateHz`. Returns no sections
// (and sets *error) when there is nothing valid to design: both corners
// open, the low corner at/above Nyquist, or low >= high.
std::vector<Biquad> designButterworthBand(const ButterworthBandParams& params, double sampleRateHz,
                                          std::string* error = nullptr);

// scipy.signal.sosfiltfilt's default padlen:
//   3 * (2 * nSections + 1 - min(#sections with b2 == 0, #sections with a2 == 0))
int sosFiltFiltDefaultPadlen(const std::vector<Biquad>& sos);

// scipy.signal.sosfiltfilt(sos, x, padtype=..., padlen=padlen), in place.
// padlen < 0 means the default above. scipy raises when the trace is too
// short for the padding; here the padding is shortened to len(x) - 1
// instead, so a very short trace still filters rather than failing.
void sosFiltFilt(const std::vector<Biquad>& sos, std::vector<float>& x, int padlen = -1,
                 EdgePadding padding = EdgePadding::Even);

} // namespace segy
