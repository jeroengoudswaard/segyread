#include "fft.h"

#include <cmath>
#include <memory>

namespace segy {

size_t nextPowerOfTwo(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

namespace {

// Per-length tables, built once per thread and size: the bit-reversal
// permutation and the twiddles exp(-2 pi i k / n), k < n/2, computed in
// double precision. The previous version re-derived each stage's twiddles
// by repeated multiplication (w *= wlen), which was both slower and lost
// float precision over long transforms.
struct FftPlan {
    std::vector<uint32_t> reversed;
    std::vector<float> twiddleRe;
    std::vector<float> twiddleIm;
};

const FftPlan& planFor(size_t n) {
    thread_local std::vector<std::unique_ptr<FftPlan>> plans(64);
    size_t log2n = 0;
    while ((size_t(1) << log2n) < n) ++log2n;
    std::unique_ptr<FftPlan>& slot = plans[log2n];
    if (!slot) {
        slot = std::make_unique<FftPlan>();
        slot->reversed.resize(n);
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < log2n; ++b) r |= ((i >> b) & 1u) << (log2n - 1 - b);
            slot->reversed[i] = uint32_t(r);
        }
        // Stored per stage, contiguously: stage `len` (4, 8, ..., n) uses
        // exp(-2 pi i k / len), k < len/2, starting at offset len/2 - 2.
        const double twoPi = 2.0 * std::acos(-1.0);
        slot->twiddleRe.reserve(n);
        slot->twiddleIm.reserve(n);
        for (size_t len = 4; len <= n; len <<= 1) {
            for (size_t k = 0; k < len / 2; ++k) {
                slot->twiddleRe.push_back(float(std::cos(twoPi * double(k) / double(len))));
                slot->twiddleIm.push_back(float(-std::sin(twoPi * double(k) / double(len))));
            }
        }
    }
    return *slot;
}

} // namespace

void fft(std::vector<std::complex<float>>& data) {
    const size_t n = data.size();
    if (n <= 1) return;
    const FftPlan& plan = planFor(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t j = plan.reversed[i];
        if (i < j) std::swap(data[i], data[j]);
    }
    // Plain float arithmetic on the interleaved re/im pairs (std::complex
    // multiplication is noticeably slower here with MSVC).
    float* d = reinterpret_cast<float*>(data.data());
    // First stage (len 2): twiddle 1, no multiplications.
    for (size_t i = 0; i < n; i += 2) {
        const float ar = d[2 * i], ai = d[2 * i + 1], br = d[2 * i + 2], bi = d[2 * i + 3];
        d[2 * i] = ar + br;
        d[2 * i + 1] = ai + bi;
        d[2 * i + 2] = ar - br;
        d[2 * i + 3] = ai - bi;
    }
    for (size_t len = 4; len <= n; len <<= 1) {
        const size_t half = len / 2;
        const float* twRe = plan.twiddleRe.data() + (half - 2);
        const float* twIm = plan.twiddleIm.data() + (half - 2);
        for (size_t i = 0; i < n; i += len) {
            float* a = d + 2 * i;
            float* b = d + 2 * (i + half);
            for (size_t k = 0; k < half; ++k) {
                const float wr = twRe[k], wi = twIm[k];
                const float br = b[2 * k], bi = b[2 * k + 1];
                const float vr = br * wr - bi * wi;
                const float vi = br * wi + bi * wr;
                const float ur = a[2 * k], ui = a[2 * k + 1];
                a[2 * k] = ur + vr;
                a[2 * k + 1] = ui + vi;
                b[2 * k] = ur - vr;
                b[2 * k + 1] = ui - vi;
            }
        }
    }
}

void ifft(std::vector<std::complex<float>>& data) {
    for (auto& c : data) c = std::conj(c);
    fft(data);
    float invN = data.empty() ? 1.0f : 1.0f / float(data.size());
    for (auto& c : data) c = std::conj(c) * invN;
}

} // namespace segy
