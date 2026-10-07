// Sample decoding: converts on-disk SEG-Y sample formats to IEEE float32.
//
// The historically dominant format in real seismic SEG-Y files is format 1
// (IBM System/370 hexadecimal float), which has no native hardware support
// on x86 and is the single hottest loop in a SEG-Y reader. It gets a
// hand-written scalar reference implementation (used as the correctness
// oracle) plus an AVX2 implementation that processes 8 samples/instruction.
#pragma once
#include <cstdint>
#include <cstddef>

namespace segy {

// True if the running CPU supports AVX2 (checked once, cached).
bool cpuSupportsAvx2();

// Canonical, fully-general IBM-float -> IEEE-float32 conversion for a single
// big-endian 32-bit word. Handles arbitrary mantissa normalization (not just
// the common case), so this is the ground truth used by tests and by the
// scalar fallback path. Not the fast path.
float ibmToIeeeScalar(uint32_t ibmBitsBE);

// Converts `count` big-endian IBM-float32 samples starting at `srcBE` into
// `dst`. Pure scalar; always correct; used when AVX2 is unavailable.
void ibmToIeeeScalarBlock(const uint8_t* srcBE, float* dst, size_t count);

// AVX2 implementation of the same conversion. Fast-paths the common case
// (mantissa already normalized per the IBM float convention: at most 3
// leading zero bits before the first set bit), which covers effectively all
// real-world seismic data. Any lane that turns out to need a larger
// normalization shift is corrected afterwards via ibmToIeeeScalar, so the
// result is bit-for-bit identical to the scalar path regardless of input.
// Caller must ensure cpuSupportsAvx2() is true before calling this.
void ibmToIeeeAvx2(const uint8_t* srcBE, float* dst, size_t count);

// Decodes `count` samples of `formatCode` from big-endian `src` into `dst`
// (IEEE float32). Dispatches to the AVX2 path for IBM float when available
// and `useAvx2` is true. Unsupported format codes zero-fill `dst`; callers
// should validate formatCode with isSampleFormatSupported() up front and
// fail the file load instead of silently rendering zeros.
void decodeSamples(const uint8_t* src, float* dst, size_t count,
                    int16_t formatCode, bool useAvx2 = true);

} // namespace segy
