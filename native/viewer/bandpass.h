// Hanning-tapered trapezoidal bandpass filtering, shared by Edit >
// Processing > Bandpass and the Octave Band Display window (see
// native/README.md). Platform/GUI-free like renderer.h/analysis.h --
// viewer_qt.cpp decodes samples and calls into this.
#pragma once
#include <vector>

namespace segy {

// Corner frequencies in Hz: stop below lowCut, a raised-cosine (Hanning-
// shaped) taper up to full pass at lowPass, flat pass through highPass, a
// symmetric taper back down to stop at highCut. Callers are expected to
// keep 0 <= lowCut <= lowPass <= highPass <= highCut -- not enforced here,
// since a caller-side spin box can already guarantee the ordering more
// cheaply than re-deriving/clamping it per trace.
struct BandpassParams {
    double lowCut = 5.0;
    double lowPass = 10.0;
    double highPass = 50.0;
    double highCut = 60.0;
};

// Filters one trace's samples in place via FFT: zero-pads to the next power
// of two (segy::nextPowerOfTwo), multiplies the spectrum by a gain mask
// built from `params` -- mirrored around the Nyquist bin so a real-valued
// signal in stays real-valued out -- inverse-FFTs, then truncates back to
// `samples`' original length. A boxcar (hard-edged) passband would ring
// (Gibbs phenomenon) at the cutoffs; the raised-cosine taper is what the
// "Hanning" in the feature name refers to. `sampleIntervalUs` is the file's
// own sample interval (BinaryHeader::sampleIntervalUs), used to map FFT
// bins to Hz.
void applyBandpassFilter(std::vector<float>& samples, double sampleIntervalUs, const BandpassParams& params);

} // namespace segy
