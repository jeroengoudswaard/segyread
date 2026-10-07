#include "segy_decode.h"
#include "segy_format.h"

#include <cstring>
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace segy {

namespace {

// Index (0..31) of the highest set bit of a nonzero 32-bit value. MSVC and
// GCC/Clang each expose this as a different intrinsic; wrapped here so the
// rest of the file (and the derivation comment below) doesn't care which.
inline unsigned bitScanReverse32(uint32_t v) {
#if defined(_MSC_VER)
    unsigned long msb = 0;
    _BitScanReverse(&msb, v);
    return unsigned(msb);
#else
    return 31u - unsigned(__builtin_clz(v));
#endif
}

} // namespace

bool cpuSupportsAvx2() {
    static const bool kSupported = [] {
#if defined(_MSC_VER)
        int info[4] = {0, 0, 0, 0};
        __cpuid(info, 0);
        int maxLeaf = info[0];
        if (maxLeaf < 7) return false;
        __cpuidex(info, 7, 0);
        return (info[1] & (1 << 5)) != 0; // EBX bit 5 = AVX2
#else
        unsigned eax, ebx, ecx, edx;
        if (__get_cpuid_max(0, nullptr) < 7) return false;
        __cpuid_count(7, 0, eax, ebx, ecx, edx);
        return (ebx & (1u << 5)) != 0; // EBX bit 5 = AVX2
#endif
    }();
    return kSupported;
}

// -----------------------------------------------------------------------
// IBM System/370 hex-float -> IEEE 754 single precision.
//
// IBM float: value = (-1)^sign * 0.mantissa(hex) * 16^(exponent-64), with a
// 7-bit excess-64 exponent and 24-bit mantissa. Normalized IBM floats have
// their leading hex digit (mantissa bits 23-20) nonzero, but this function
// handles the fully general case via a bit-scan, so it is correct even for
// non-normalized encodings some producers emit.
//
// Derivation: let `shift` = number of leading zero bits in the 24-bit
// mantissa before its first set bit. Shifting the mantissa left by `shift`
// makes bit 23 the implicit leading one of an IEEE mantissa. Matching up
// the two definitions of the value gives:
//     ieee_biased_exponent = 4*exponent - shift - 130
// (see native/README.md for the full derivation). This was validated
// against known IBM/IEEE conversion pairs in the test suite.
// -----------------------------------------------------------------------
float ibmToIeeeScalar(uint32_t ibm) {
    const uint32_t sign = ibm & 0x80000000u;
    const int32_t exponent = int32_t((ibm >> 24) & 0x7Fu);
    uint32_t mantissa = ibm & 0x00FFFFFFu;

    if (mantissa == 0) {
        uint32_t bits = sign;
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    const unsigned msb = bitScanReverse32(mantissa); // index of highest set bit, 0..23
    const int shift = 23 - int(msb);
    mantissa <<= shift;

    const int32_t ieeeExp = 4 * exponent - shift - 130;

    uint32_t bits;
    if (ieeeExp <= 0) {
        bits = sign; // underflow -> flush to signed zero
    } else if (ieeeExp >= 255) {
        bits = sign | 0x7F800000u; // overflow -> signed infinity
    } else {
        bits = sign | (uint32_t(ieeeExp) << 23) | (mantissa & 0x007FFFFFu);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void ibmToIeeeScalarBlock(const uint8_t* srcBE, float* dst, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = ibmToIeeeScalar(readU32BE(srcBE + i * 4));
    }
}

void ibmToIeeeAvx2(const uint8_t* srcBE, float* dst, size_t count) {
    const size_t lanes = 8;
    size_t i = 0;

    // Byte-swap mask: reverses bytes within each 32-bit lane (big-endian ->
    // native little-endian), independently in each 128-bit half.
    const __m256i swapMask = _mm256_setr_epi8(
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

    const __m256i signMask = _mm256_set1_epi32(int(0x80000000u));
    const __m256i mantissaMask = _mm256_set1_epi32(0x00FFFFFF);
    const __m256i bit23 = _mm256_set1_epi32(0x00800000);
    const __m256i bit22 = _mm256_set1_epi32(0x00400000);
    const __m256i bit21 = _mm256_set1_epi32(0x00200000);
    const __m256i one = _mm256_set1_epi32(1);
    const __m256i two = _mm256_set1_epi32(2);
    const __m256i three = _mm256_set1_epi32(3);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i lowMantissa = _mm256_set1_epi32(0x007FFFFF);
    const __m256i infBits = _mm256_set1_epi32(0x7F800000);

    for (; i + lanes <= count; i += lanes) {
        __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcBE + i * 4));
        __m256i ibm = _mm256_shuffle_epi8(raw, swapMask);

        __m256i sign = _mm256_and_si256(ibm, signMask);
        __m256i exponent = _mm256_and_si256(_mm256_srli_epi32(ibm, 24), _mm256_set1_epi32(0x7F));
        __m256i mantissa = _mm256_and_si256(ibm, mantissaMask);
        __m256i isZero = _mm256_cmpeq_epi32(mantissa, zero);

        // shift = 0 if bit23 set, else 1 if bit22 set, else 2 if bit21 set, else 3 (assumed).
        __m256i has23 = _mm256_cmpgt_epi32(_mm256_and_si256(mantissa, bit23), zero);
        __m256i has22 = _mm256_cmpgt_epi32(_mm256_and_si256(mantissa, bit22), zero);
        __m256i has21 = _mm256_cmpgt_epi32(_mm256_and_si256(mantissa, bit21), zero);
        __m256i shift = three;
        shift = _mm256_blendv_epi8(shift, two, has21);
        shift = _mm256_blendv_epi8(shift, one, has22);
        shift = _mm256_blendv_epi8(shift, zero, has23);

        __m256i mantissaShifted = _mm256_sllv_epi32(mantissa, shift);

        // ieeeExp = 4*exponent - shift - 130
        __m256i ieeeExp = _mm256_sub_epi32(_mm256_sub_epi32(_mm256_slli_epi32(exponent, 2), shift),
                                            _mm256_set1_epi32(130));

        __m256i underflow = _mm256_cmpgt_epi32(one, ieeeExp);          // ieeeExp <= 0
        __m256i overflow = _mm256_cmpgt_epi32(ieeeExp, _mm256_set1_epi32(254)); // ieeeExp >= 255

        __m256i normalBits = _mm256_or_si256(
            sign, _mm256_or_si256(_mm256_slli_epi32(ieeeExp, 23),
                                   _mm256_and_si256(mantissaShifted, lowMantissa)));
        __m256i bits = _mm256_blendv_epi8(normalBits, _mm256_or_si256(sign, infBits), overflow);
        bits = _mm256_blendv_epi8(bits, sign, underflow);
        bits = _mm256_blendv_epi8(bits, sign, isZero);

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), bits);

        // Detect lanes where the "assumed shift <= 3" fast path was wrong:
        // after shifting by our assumed amount, bit23 should now be set
        // (mantissa nonzero and not already flagged zero/under/overflow).
        __m256i stillMissingBit23 = _mm256_cmpeq_epi32(_mm256_and_si256(mantissaShifted, bit23), zero);
        __m256i needsFixup = _mm256_andnot_si256(
            isZero, _mm256_andnot_si256(underflow, _mm256_andnot_si256(overflow, stillMissingBit23)));
        int lane_mask = _mm256_movemask_ps(_mm256_castsi256_ps(needsFixup));
        if (lane_mask != 0) {
            for (int lane = 0; lane < 8; ++lane) {
                if (lane_mask & (1 << lane)) {
                    dst[i + lane] = ibmToIeeeScalar(readU32BE(srcBE + (i + lane) * 4));
                }
            }
        }
    }

    // Scalar tail (< 8 remaining samples).
    for (; i < count; ++i) {
        dst[i] = ibmToIeeeScalar(readU32BE(srcBE + i * 4));
    }
}

void decodeSamples(const uint8_t* src, float* dst, size_t count, int16_t formatCode, bool useAvx2) {
    switch (formatCode) {
        case kIbmFloat32:
            if (useAvx2 && cpuSupportsAvx2()) {
                ibmToIeeeAvx2(src, dst, count);
            } else {
                ibmToIeeeScalarBlock(src, dst, count);
            }
            break;
        case kInt32:
            for (size_t i = 0; i < count; ++i) dst[i] = float(readI32BE(src + i * 4));
            break;
        case kInt16:
            for (size_t i = 0; i < count; ++i) dst[i] = float(readI16BE(src + i * 2));
            break;
        case kIeeeFloat32:
            for (size_t i = 0; i < count; ++i) {
                uint32_t bits = readU32BE(src + i * 4);
                float f;
                std::memcpy(&f, &bits, sizeof(f));
                dst[i] = f;
            }
            break;
        case kIeeeFloat64:
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* p = src + i * 8;
                uint64_t hi = readU32BE(p);
                uint64_t lo = readU32BE(p + 4);
                uint64_t bits = (hi << 32) | lo;
                double d;
                std::memcpy(&d, &bits, sizeof(d));
                dst[i] = float(d);
            }
            break;
        case kInt24:
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* p = src + i * 3;
                int32_t v = int32_t((uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]));
                if (v & 0x00800000) v |= int32_t(0xFF000000u); // sign-extend 24 -> 32
                dst[i] = float(v);
            }
            break;
        case kInt8:
            for (size_t i = 0; i < count; ++i) dst[i] = float(int8_t(src[i]));
            break;
        default:
            std::memset(dst, 0, count * sizeof(float));
            break;
    }
}

} // namespace segy
