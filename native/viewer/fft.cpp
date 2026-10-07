#include "fft.h"

#include <cmath>

namespace segy {

size_t nextPowerOfTwo(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

void fft(std::vector<std::complex<float>>& data) {
    size_t n = data.size();
    if (n <= 1) return;
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double angle = -2.0 * std::acos(-1.0) / double(len);
        std::complex<float> wlen(float(std::cos(angle)), float(std::sin(angle)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                std::complex<float> u = data[i + k];
                std::complex<float> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= wlen;
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
