// Builds small in-memory SEG-Y byte buffers with known contents, for tests
// and benchmarks that need a realistic file layout without shipping a
// fixture file.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <functional>
#include "segy_format.h"

namespace segy::test {

inline void writeI16BE(std::vector<uint8_t>& buf, size_t pos, int16_t v) {
    buf[pos] = uint8_t((uint16_t(v) >> 8) & 0xFF);
    buf[pos + 1] = uint8_t(uint16_t(v) & 0xFF);
}
inline void writeI32BE(std::vector<uint8_t>& buf, size_t pos, int32_t v) {
    buf[pos] = uint8_t((uint32_t(v) >> 24) & 0xFF);
    buf[pos + 1] = uint8_t((uint32_t(v) >> 16) & 0xFF);
    buf[pos + 2] = uint8_t((uint32_t(v) >> 8) & 0xFF);
    buf[pos + 3] = uint8_t(uint32_t(v) & 0xFF);
}

// Converts an IEEE float to the IBM hex-float bit pattern representing the
// (approximately) same value. Used to synthesize IBM-format test files with
// known expected results; deliberately independent of ibmToIeeeScalar's own
// bit-twiddling so the two don't share bugs.
inline uint32_t ieeeToIbmBitsReference(double value) {
    if (value == 0.0) return 0;
    uint32_t sign = value < 0 ? 0x80000000u : 0;
    double a = value < 0 ? -value : value;
    int exp = 64;
    // Normalize so that 1/16 <= a/16^k < 1
    while (a >= 1.0) { a /= 16.0; ++exp; }
    while (a < 1.0 / 16.0) { a *= 16.0; --exp; }
    uint32_t mantissa = uint32_t(a * 16777216.0 + 0.5); // round to nearest
    if (mantissa >= (1u << 24)) { mantissa >>= 4; ++exp; } // carry from rounding
    return sign | (uint32_t(exp & 0x7F) << 24) | (mantissa & 0x00FFFFFFu);
}

struct SyntheticFile {
    std::vector<uint8_t> bytes;
    int64_t traceCount = 0;
    int samplesPerTrace = 0;
    int16_t formatCode = 0;
    size_t traceStrideBytes = 0;
};

// `sampleValue(trace, sample)` supplies the value to encode at that
// position; formatCode selects the on-disk encoding.
inline SyntheticFile makeSyntheticSegy(int64_t traceCount, int samplesPerTrace, int16_t formatCode,
                                        const std::function<double(int64_t, int)>& sampleValue) {
    SyntheticFile out;
    out.traceCount = traceCount;
    out.samplesPerTrace = samplesPerTrace;
    out.formatCode = formatCode;
    int sampleSize = segy::sampleFormatSizeBytes(formatCode);
    out.traceStrideBytes = segy::kTraceHeaderSize + size_t(samplesPerTrace) * size_t(sampleSize);

    size_t total = segy::kHeaderTotalSize + size_t(traceCount) * out.traceStrideBytes;
    out.bytes.assign(total, 0);

    // Binary header.
    writeI16BE(out.bytes, 3220, int16_t(samplesPerTrace)); // bytes 3221-3222
    writeI16BE(out.bytes, 3224, formatCode);                // bytes 3225-3226
    writeI16BE(out.bytes, 3216, 1000);                      // sample interval, bytes 3217-3218

    for (int64_t t = 0; t < traceCount; ++t) {
        size_t traceOff = segy::kHeaderTotalSize + size_t(t) * out.traceStrideBytes;
        writeI32BE(out.bytes, traceOff + 0, int32_t(t + 1));       // TraceSequenceLine
        writeI32BE(out.bytes, traceOff + 12, int32_t(t + 1));      // TraceNumber
        size_t sampleOff = traceOff + segy::kTraceHeaderSize;
        for (int s = 0; s < samplesPerTrace; ++s) {
            double v = sampleValue(t, s);
            size_t pos = sampleOff + size_t(s) * size_t(sampleSize);
            switch (formatCode) {
                case segy::kIbmFloat32:
                    writeI32BE(out.bytes, pos, int32_t(ieeeToIbmBitsReference(v)));
                    break;
                case segy::kIeeeFloat32: {
                    float f = float(v);
                    uint32_t bits;
                    std::memcpy(&bits, &f, 4);
                    writeI32BE(out.bytes, pos, int32_t(bits));
                    break;
                }
                case segy::kInt32:
                    writeI32BE(out.bytes, pos, int32_t(v));
                    break;
                case segy::kInt16:
                    writeI16BE(out.bytes, pos, int16_t(v));
                    break;
                case segy::kInt8:
                    out.bytes[pos] = uint8_t(int8_t(v));
                    break;
                default:
                    break;
            }
        }
    }
    return out;
}

} // namespace segy::test
