#include "processing.h"
#include "fft.h"

#include <algorithm>
#include <cmath>

namespace segy {

namespace {

constexpr double kPi = 3.14159265358979323846;

double rms(const std::vector<float>& trace) {
    double sum = 0.0;
    for (float v : trace) sum += double(v) * v;
    return trace.empty() ? 0.0 : std::sqrt(sum / double(trace.size()));
}

} // namespace

std::vector<std::complex<float>> analyticSignal(const std::vector<float>& trace) {
    const size_t n = trace.size();
    std::vector<std::complex<float>> spectrum(nextPowerOfTwo(std::max<size_t>(2, 2 * n)));
    for (size_t i = 0; i < n; ++i) spectrum[i] = trace[i];
    fft(spectrum);
    // Keep DC and Nyquist, double the positive frequencies, zero the negative ones.
    const size_t m = spectrum.size();
    for (size_t i = 1; i < m / 2; ++i) spectrum[i] *= 2.0f;
    for (size_t i = m / 2 + 1; i < m; ++i) spectrum[i] = 0.0f;
    ifft(spectrum);
    spectrum.resize(n);
    return spectrum;
}

void envelope(std::vector<float>& trace) {
    const auto a = analyticSignal(trace);
    for (size_t i = 0; i < trace.size(); ++i) trace[i] = std::abs(a[i]);
}

void instantaneousPhase(std::vector<float>& trace) {
    const auto a = analyticSignal(trace);
    for (size_t i = 0; i < trace.size(); ++i) {
        trace[i] = std::abs(a[i]) > 0.0f ? float(std::atan2(a[i].imag(), a[i].real()) * 180.0 / kPi) : 0.0f;
    }
}

void instantaneousFrequency(std::vector<float>& trace, double sampleIntervalUs) {
    const size_t n = trace.size();
    if (n < 3 || sampleIntervalUs <= 0.0) {
        std::fill(trace.begin(), trace.end(), 0.0f);
        return;
    }
    const double dt = sampleIntervalUs * 1e-6;
    const double nyquist = 0.5 / dt;
    const auto a = analyticSignal(trace);
    // Phase advance over two samples, a[i+1] conj(a[i-1]) (exact for a pure
    // tone, unlike a finite-difference derivative), summed over a 5-sample
    // window: the sum weights by the envelope squared.
    std::vector<std::complex<double>> step(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t lo = i == 0 ? 0 : i - 1, hi = i + 1 < n ? i + 1 : n - 1;
        step[i] = std::complex<double>(a[hi]) * std::conj(std::complex<double>(a[lo]));
        if (hi - lo == 1) step[i] *= step[i]; // one-sample step at the trace ends: square it to two
    }
    for (size_t i = 0; i < n; ++i) {
        std::complex<double> sum = 0.0;
        for (size_t k = i >= 2 ? i - 2 : 0; k <= std::min(n - 1, i + 2); ++k) sum += step[k];
        const double f = std::abs(sum) > 0.0 ? std::arg(sum) / (2.0 * kPi * 2.0 * dt) : 0.0;
        trace[i] = float(std::clamp(f, 0.0, nyquist));
    }
}

void phaseRotate(std::vector<float>& trace, double degrees) {
    const auto a = analyticSignal(trace);
    const double c = std::cos(degrees * kPi / 180.0), s = std::sin(degrees * kPi / 180.0);
    for (size_t i = 0; i < trace.size(); ++i) trace[i] = float(a[i].real() * c - a[i].imag() * s);
}

void spectralWhitening(std::vector<float>& trace, double sampleIntervalUs, const WhiteningParams& params) {
    const size_t n = trace.size();
    const double before = rms(trace);
    if (n < 4 || sampleIntervalUs <= 0.0 || before <= 0.0) return;
    std::vector<std::complex<float>> spectrum(nextPowerOfTwo(n));
    const size_t m = spectrum.size();
    // Remove the mean first so DC does not dominate the smoothing.
    double mean = 0.0;
    for (float v : trace) mean += v;
    mean /= double(n);
    for (size_t i = 0; i < n; ++i) spectrum[i] = float(trace[i] - mean);
    fft(spectrum);
    const double df = 1.0 / (double(m) * sampleIntervalUs * 1e-6);
    const size_t half = m / 2;
    std::vector<double> amplitude(half + 1), prefix(half + 2, 0.0);
    for (size_t i = 0; i <= half; ++i) amplitude[i] = std::abs(spectrum[i]);
    for (size_t i = 0; i <= half; ++i) prefix[i + 1] = prefix[i] + amplitude[i];
    const long width = std::max(1L, long(std::lround(params.smoothingHz / df / 2.0)));
    double maxSmooth = 0.0;
    std::vector<double> smooth(half + 1);
    for (size_t i = 0; i <= half; ++i) {
        const long lo = std::max(0L, long(i) - width), hi = std::min(long(half), long(i) + width);
        smooth[i] = (prefix[size_t(hi) + 1] - prefix[size_t(lo)]) / double(hi - lo + 1);
        maxSmooth = std::max(maxSmooth, smooth[i]);
    }
    const double ramp = std::max(params.smoothingHz, df);
    auto taper = [&](double f) {
        if (f < params.lowHz - ramp || f > params.highHz + ramp) return 0.0;
        if (f < params.lowHz) return 0.5 - 0.5 * std::cos(kPi * (f - (params.lowHz - ramp)) / ramp);
        if (f > params.highHz) return 0.5 + 0.5 * std::cos(kPi * (f - params.highHz) / ramp);
        return 1.0;
    };
    for (size_t i = 0; i <= half; ++i) {
        const double gain = taper(double(i) * df) / std::max(smooth[i], 1e-6 * maxSmooth + 1e-30);
        spectrum[i] *= float(gain);
        if (i > 0 && i < half) spectrum[m - i] *= float(gain);
    }
    ifft(spectrum);
    for (size_t i = 0; i < n; ++i) trace[i] = spectrum[i].real();
    const double after = rms(trace);
    if (after > 0.0) {
        const float scale = float(before / after);
        for (float& v : trace) v *= scale;
    }
}

std::vector<double> levinsonSolve(const std::vector<double>& r, const std::vector<double>& g) {
    const size_t n = r.size();
    std::vector<double> a(n, 0.0);
    if (n == 0 || g.size() != n || !(r[0] > 0.0)) return a;
    // Levinson: f = forward vector of the symmetric Toeplitz matrix, grown
    // one order at a time, and the solution a updated alongside.
    std::vector<double> f(n, 0.0), next(n, 0.0);
    f[0] = 1.0 / r[0];
    a[0] = g[0] / r[0];
    for (size_t k = 1; k < n; ++k) {
        double ef = 0.0;
        for (size_t i = 0; i < k; ++i) ef += r[k - i] * f[i];
        const double denom = 1.0 - ef * ef;
        if (std::fabs(denom) < 1e-12) return std::vector<double>(n, 0.0);
        // New forward vector from the old forward and (mirrored) backward ones.
        for (size_t i = 0; i <= k; ++i) {
            const double fwd = i < k ? f[i] : 0.0;
            const double bwd = i > 0 ? f[k - i] : 0.0;
            next[i] = (fwd - ef * bwd) / denom;
        }
        std::copy(next.begin(), next.begin() + std::ptrdiff_t(k + 1), f.begin());
        double ea = 0.0;
        for (size_t i = 0; i < k; ++i) ea += r[k - i] * a[i];
        const double c = g[k] - ea;
        for (size_t i = 0; i <= k; ++i) a[i] += c * f[k - i];
    }
    return a;
}

void predictiveDeconvolution(std::vector<float>& trace, double sampleIntervalUs, const DeconParams& params) {
    const size_t n = trace.size();
    if (n < 8 || sampleIntervalUs <= 0.0) return;
    const double dtMs = sampleIntervalUs / 1000.0;
    const size_t gap = std::max<size_t>(1, size_t(std::lround(params.gapMs / dtMs)));
    const size_t length = std::clamp<size_t>(size_t(std::lround(params.operatorMs / dtMs)), 1, n / 2);
    if (gap + length >= n) return;
    std::vector<double> r(gap + length, 0.0);
    for (size_t lag = 0; lag < r.size(); ++lag) {
        double sum = 0.0;
        for (size_t i = lag; i < n; ++i) sum += double(trace[i]) * trace[i - lag];
        r[lag] = sum;
    }
    if (!(r[0] > 0.0)) return; // dead trace
    std::vector<double> toeplitz(r.begin(), r.begin() + std::ptrdiff_t(length));
    toeplitz[0] *= 1.0 + params.whiteNoisePercent / 100.0;
    const std::vector<double> rhs(r.begin() + std::ptrdiff_t(gap), r.begin() + std::ptrdiff_t(gap + length));
    const std::vector<double> a = levinsonSolve(toeplitz, rhs);
    std::vector<float> out(n);
    for (size_t t = 0; t < n; ++t) {
        double predicted = 0.0;
        for (size_t k = 0; k < length && t >= gap + k; ++k) predicted += a[k] * trace[t - gap - k];
        out[t] = float(trace[t] - predicted);
    }
    trace.swap(out);
}

double fkFilterWeight(double frequencyHz, double wavenumberPerM, const FkFilterParams& params) {
    const double f = std::fabs(frequencyHz), k = std::fabs(wavenumberPerM);
    if (k <= 0.0) return 1.0; // flat events
    const double v = f / k;
    const double high = params.velocity;
    const double low = params.velocity * (1.0 - std::clamp(params.taperPercent, 0.0, 99.0) / 100.0);
    if (v >= high) return 1.0;
    if (v <= low) return 0.0;
    return 0.5 - 0.5 * std::cos(kPi * (v - low) / (high - low));
}

void fkFilterPanel(std::vector<std::vector<float>>& traces, double sampleIntervalUs, const FkFilterParams& params) {
    const size_t nx = traces.size();
    if (nx < 2 || traces[0].empty() || sampleIntervalUs <= 0.0 || params.traceSpacing <= 0.0) return;
    const size_t ns = traces[0].size();
    const size_t nt = nextPowerOfTwo(ns), nk = nextPowerOfTwo(2 * nx);
    const double dt = sampleIntervalUs * 1e-6;
    // Time FFT per trace (the zero-padded traces beyond nx stay zero).
    std::vector<std::vector<std::complex<float>>> spectra(nx, std::vector<std::complex<float>>(nt));
    for (size_t x = 0; x < nx; ++x) {
        for (size_t i = 0; i < ns; ++i) spectra[x][i] = traces[x][i];
        fft(spectra[x]);
    }
    // Per frequency: FFT across traces, weight, inverse. Only the
    // non-negative frequencies; the negative ones are their conjugates.
    std::vector<std::complex<float>> column(nk);
    for (size_t j = 0; j <= nt / 2; ++j) {
        std::fill(column.begin(), column.end(), std::complex<float>(0.0f));
        for (size_t x = 0; x < nx; ++x) column[x] = spectra[x][j];
        fft(column);
        const double f = double(j) / (double(nt) * dt);
        for (size_t m = 0; m < nk; ++m) {
            const double k = (m <= nk / 2 ? double(m) : double(m) - double(nk)) / (double(nk) * params.traceSpacing);
            column[m] *= float(fkFilterWeight(f, k, params));
        }
        ifft(column);
        for (size_t x = 0; x < nx; ++x) {
            spectra[x][j] = column[x];
            if (j > 0 && j < nt / 2) spectra[x][nt - j] = std::conj(column[x]);
        }
    }
    for (size_t x = 0; x < nx; ++x) {
        ifft(spectra[x]);
        for (size_t i = 0; i < ns; ++i) traces[x][i] = spectra[x][i].real();
    }
}

} // namespace segy
