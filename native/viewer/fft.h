// Shared radix-2 FFT/IFFT, used by both analysis.cpp (Spectrum's magnitude
// display) and bandpass.cpp (real filtering) -- one implementation instead
// of two, so a bug fix or precision change can't apply to only one caller.
#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace segy {

// Rounds `n` up to the next power of two (n <= 1 -> 1).
size_t nextPowerOfTwo(size_t n);

// Iterative in-place radix-2 Cooley-Tukey FFT. `data.size()` must already be
// a power of 2 (callers zero-pad to the next one via nextPowerOfTwo). A full
// FFT library would be overkill for this app's window sizes (one trace at a
// time, at most a few tens of thousands of samples).
void fft(std::vector<std::complex<float>>& data);

// Inverse FFT (includes the 1/N scaling), via the standard conjugate trick
// (conjugate the input, forward-FFT, conjugate and scale the output) rather
// than a second, separately-tested transform implementation.
void ifft(std::vector<std::complex<float>>& data);

} // namespace segy
