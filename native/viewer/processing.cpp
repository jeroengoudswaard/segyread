#include "processing.h"
#include "fft.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace segy {

namespace {

constexpr double kPi = 3.14159265358979323846;

double rms(const std::vector<float>& trace) {
    double sum = 0.0;
    for (float v : trace) sum += double(v) * v;
    return trace.empty() ? 0.0 : std::sqrt(sum / double(trace.size()));
}

} // namespace

LiveRange liveRange(const float* samples, size_t count) {
    LiveRange r;
    size_t first = 0;
    while (first < count && samples[first] == 0.0f) ++first;
    if (first == count) return r; // dead
    size_t last = count - 1;
    while (last > first && samples[last] == 0.0f) --last;
    r.first = first;
    r.last = last;
    return r;
}

void applyMute(float* samples, size_t count, const LiveRange& range) {
    if (range.first > range.last) {
        std::fill(samples, samples + count, 0.0f);
        return;
    }
    std::fill(samples, samples + std::min(range.first, count), 0.0f);
    if (range.last + 1 < count) std::fill(samples + range.last + 1, samples + count, 0.0f);
}

std::vector<std::complex<float>> analyticSignal(const std::vector<float>& trace) {
    const size_t n = trace.size();
    std::vector<std::complex<float>> spectrum(nextPowerOfTwo(std::max<size_t>(2, n + std::max<size_t>(n / 2, 64))));
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

namespace {

double phaseScanAngle(int i) { return -90.0 + double(i) * 180.0 / double(kPhaseScanSteps); }

// Best index of a 180-degree-periodic curve, refined by a parabola through
// its neighbours; returned as an angle in -90..90.
double refinedPeak(const std::vector<double>& score) {
    const int n = int(score.size());
    int best = 0;
    for (int i = 1; i < n; ++i) if (score[size_t(i)] > score[size_t(best)]) best = i;
    const double l = score[size_t((best + n - 1) % n)], c = score[size_t(best)], r = score[size_t((best + 1) % n)];
    const double denom = l - 2.0 * c + r;
    const double shift = denom < 0.0 ? std::clamp(0.5 * (l - r) / denom, -0.5, 0.5) : 0.0;
    double angle = phaseScanAngle(best) + shift * 180.0 / double(n);
    if (angle < -90.0) angle += 180.0;
    if (angle >= 90.0) angle -= 180.0;
    return angle;
}

std::vector<double> normalised(const std::vector<double>& v, bool higherIsBetter) {
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    const double span = *hi - *lo;
    std::vector<double> out(v.size(), 1.0);
    if (span <= 0.0) return out;
    for (size_t i = 0; i < v.size(); ++i) out[i] = higherIsBetter ? (v[i] - *lo) / span : (*hi - v[i]) / span;
    return out;
}

} // namespace

namespace {

void addMoments(PhaseScanMoments& m, const std::vector<std::complex<float>>& analytic) {
    for (const auto& z : analytic) {
        const double a = z.real(), b = z.imag();
        const double aa = a * a, bb = b * b;
        m.a2 += aa;
        m.ab += a * b;
        m.b2 += bb;
        m.a4 += aa * aa;
        m.a3b += aa * a * b;
        m.a2b2 += aa * bb;
        m.ab3 += a * b * bb;
        m.b4 += bb * bb;
    }
    m.count += double(analytic.size());
}

// count x sum x^4 / (sum x^2)^2 of x = a cos - b sin, from the moments.
double kurtosisAt(const PhaseScanMoments& m, double c, double sn) {
    const double e = c * c * m.a2 - 2.0 * c * sn * m.ab + sn * sn * m.b2;
    const double q = c * c * c * c * m.a4 - 4.0 * c * c * c * sn * m.a3b + 6.0 * c * c * sn * sn * m.a2b2 -
                     4.0 * c * sn * sn * sn * m.ab3 + sn * sn * sn * sn * m.b4;
    return e > 0.0 ? m.count * q / (e * e) : 0.0;
}

} // namespace

void addPhaseScanTrace(PhaseScanSums& sums, const std::vector<float>& trace) {
    if (!(rms(trace) > 0.0)) return;
    const auto analytic = analyticSignal(trace);
    PhaseScanMoments m;
    addMoments(m, analytic);
    for (int k = 0; k < kPhaseScanSteps; ++k) {
        const double angle = phaseScanAngle(k) * kPi / 180.0;
        sums.varimaxSum[size_t(k)] += kurtosisAt(m, std::cos(angle), std::sin(angle));
    }
    PhaseScanMoments& p = sums.pooled;
    p.a2 += m.a2;
    p.ab += m.ab;
    p.b2 += m.b2;
    p.a4 += m.a4;
    p.a3b += m.a3b;
    p.a2b2 += m.a2b2;
    p.ab3 += m.ab3;
    p.b4 += m.b4;
    p.count += m.count;
    sums.traces += 1.0;
}

void mergePhaseScan(PhaseScanSums& into, const PhaseScanSums& from) {
    PhaseScanMoments& p = into.pooled;
    const PhaseScanMoments& q = from.pooled;
    p.a2 += q.a2;
    p.ab += q.ab;
    p.b2 += q.b2;
    p.a4 += q.a4;
    p.a3b += q.a3b;
    p.a2b2 += q.a2b2;
    p.ab3 += q.ab3;
    p.b4 += q.b4;
    p.count += q.count;
    into.traces += from.traces;
    for (size_t k = 0; k < into.varimaxSum.size() && k < from.varimaxSum.size(); ++k) into.varimaxSum[k] += from.varimaxSum[k];
}

PhaseScanResult finishPhaseScan(const PhaseScanSums& s) {
    PhaseScanResult r;
    if (s.pooled.count < 8 || !(s.pooled.a2 > 0.0) || s.traces < 1.0) return r;
    r.anglesDeg.resize(kPhaseScanSteps);
    r.kurtosis.resize(kPhaseScanSteps);
    r.varimax.resize(kPhaseScanSteps);
    for (int k = 0; k < kPhaseScanSteps; ++k) {
        const double angle = phaseScanAngle(k);
        r.anglesDeg[size_t(k)] = angle;
        r.kurtosis[size_t(k)] = kurtosisAt(s.pooled, std::cos(angle * kPi / 180.0), std::sin(angle * kPi / 180.0));
        r.varimax[size_t(k)] = s.varimaxSum[size_t(k)] / s.traces;
    }
    r.kurtosisScore = normalised(r.kurtosis, true);
    r.entropyScore = normalised(r.varimax, true);
    std::vector<double> product(kPhaseScanSteps);
    for (int k = 0; k < kPhaseScanSteps; ++k) product[size_t(k)] = r.kurtosisScore[size_t(k)] * r.entropyScore[size_t(k)];
    r.agreementScore = normalised(product, true);
    r.kurtosisAngle = refinedPeak(r.kurtosisScore);
    r.entropyAngle = refinedPeak(r.entropyScore);
    r.agreementAngle = refinedPeak(r.agreementScore);
    const double spread = std::fabs(r.kurtosisAngle - r.entropyAngle);
    r.spreadDeg = std::min(spread, 180.0 - spread);
    r.valid = true;
    return r;
}

double ormsbyWeight(double frequencyHz, const OrmsbyParams& p) {
    const double f = std::fabs(frequencyHz);
    if (f < p.lowCutHz || f > p.highCutHz) return 0.0;
    if (f < p.lowPassHz) return p.lowPassHz > p.lowCutHz ? (f - p.lowCutHz) / (p.lowPassHz - p.lowCutHz) : 1.0;
    if (f > p.highPassHz) return p.highCutHz > p.highPassHz ? (p.highCutHz - f) / (p.highCutHz - p.highPassHz) : 1.0;
    return 1.0;
}

void ormsbyFilter(std::vector<float>& trace, double sampleIntervalUs, const OrmsbyParams& params) {
    const size_t n = trace.size();
    if (n < 2 || sampleIntervalUs <= 0.0) return;
    const size_t pad = std::min(n - 1, std::max<size_t>(64, n / 4));
    // [mirror of x[pad..1] | x | mirror of x[n-2..n-1-pad]], then zeros.
    std::vector<std::complex<float>> spectrum(nextPowerOfTwo(n + 2 * pad));
    for (size_t i = 0; i < pad; ++i) spectrum[i] = trace[pad - i];
    for (size_t i = 0; i < n; ++i) spectrum[pad + i] = trace[i];
    for (size_t i = 0; i < pad; ++i) spectrum[pad + n + i] = trace[n - 2 - i];
    fft(spectrum);
    const size_t m = spectrum.size();
    const double df = 1.0 / (double(m) * sampleIntervalUs * 1e-6);
    for (size_t i = 0; i <= m / 2; ++i) {
        const float w = float(ormsbyWeight(double(i) * df, params));
        spectrum[i] *= w;
        if (i > 0 && i < m / 2) spectrum[m - i] *= w;
    }
    ifft(spectrum);
    for (size_t i = 0; i < n; ++i) trace[i] = spectrum[pad + i].real();
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
    const double v = k > 0.0 ? f / k : std::numeric_limits<double>::infinity(); // k = 0: flat events
    const double lo = std::max(0.0, params.minVelocity);
    const double hi = params.maxVelocity > 0.0 ? params.maxVelocity : std::numeric_limits<double>::infinity();
    const double t = std::clamp(params.taperPercent, 0.0, 99.0) / 100.0;
    // 1 inside [lo, hi], cosine down to 0 outside it.
    double inside = 1.0;
    if (v < lo) {
        const double edge = lo * (1.0 - t);
        inside = v <= edge ? 0.0 : 0.5 - 0.5 * std::cos(kPi * (v - edge) / (lo - edge));
    } else if (v > hi) {
        const double edge = hi * (1.0 + t);
        inside = v >= edge ? 0.0 : 0.5 + 0.5 * std::cos(kPi * (v - hi) / (edge - hi));
    }
    return params.pass ? inside : 1.0 - inside;
}

FkFilterPlan makeFkFilterPlan(size_t traces, size_t samples, double sampleIntervalUs, const FkFilterParams& params) {
    FkFilterPlan plan;
    plan.traces = traces;
    plan.samples = samples;
    if (traces < 2 || samples == 0 || sampleIntervalUs <= 0.0 || params.traceSpacing <= 0.0) return plan;
    plan.nt = nextPowerOfTwo(samples);
    plan.nk = nextPowerOfTwo(2 * traces);
    const size_t nf = plan.nt / 2 + 1, nk = plan.nk;
    const double dt = sampleIntervalUs * 1e-6;
    plan.weights.resize(nf * nk);
    plan.columnKind.resize(nf);
    for (size_t j = 0; j < nf; ++j) {
        const double f = double(j) / (double(plan.nt) * dt);
        bool ones = true, zeros = true;
        for (size_t m = 0; m < nk; ++m) {
            const double k = (m <= nk / 2 ? double(m) : double(m) - double(nk)) / (double(nk) * params.traceSpacing);
            const float w = float(fkFilterWeight(f, k, params));
            plan.weights[j * nk + m] = w;
            ones = ones && w == 1.0f;
            zeros = zeros && w == 0.0f;
        }
        plan.columnKind[j] = ones ? 1 : zeros ? 2 : 0;
    }
    return plan;
}

void fkFilterPanel(std::vector<std::vector<float>>& traces, const FkFilterPlan& plan) {
    const size_t nx = traces.size();
    if (plan.nt == 0 || nx != plan.traces || traces[0].size() != plan.samples) return;
    const size_t ns = plan.samples, nt = plan.nt, nk = plan.nk, nf = nt / 2 + 1;
    // Half spectra (0..Nyquist) per trace, two traces per complex FFT.
    std::vector<std::complex<float>> half(nx * nf);
    std::vector<std::complex<float>> buffer(nt);
    for (size_t x = 0; x < nx; x += 2) {
        const bool pair = x + 1 < nx;
        std::fill(buffer.begin(), buffer.end(), std::complex<float>(0.0f));
        for (size_t i = 0; i < ns; ++i) buffer[i] = {traces[x][i], pair ? traces[x + 1][i] : 0.0f};
        fft(buffer);
        for (size_t j = 0; j < nf; ++j) {
            const std::complex<float> z = buffer[j], zc = std::conj(buffer[(nt - j) % nt]);
            half[x * nf + j] = 0.5f * (z + zc);
            if (pair) half[(x + 1) * nf + j] = std::complex<float>(0.0f, -0.5f) * (z - zc);
        }
    }
    // Per frequency: FFT across traces, weight, inverse. Eight neighbouring
    // frequencies are gathered per pass: one cache line per trace instead
    // of one miss per trace and frequency (rows are nf values apart).
    constexpr size_t kBlock = 8;
    std::vector<std::vector<std::complex<float>>> columns(kBlock, std::vector<std::complex<float>>(nk));
    for (size_t j0 = 0; j0 < nf; j0 += kBlock) {
        const size_t count = std::min(kBlock, nf - j0);
        for (size_t b = 0; b < count; ++b) std::fill(columns[b].begin(), columns[b].end(), std::complex<float>(0.0f));
        for (size_t x = 0; x < nx; ++x) {
            const std::complex<float>* row = half.data() + x * nf + j0;
            for (size_t b = 0; b < count; ++b) columns[b][x] = row[b];
        }
        for (size_t b = 0; b < count; ++b) {
            const size_t j = j0 + b;
            std::vector<std::complex<float>>& column = columns[b];
            if (plan.columnKind[j] == 1) continue;
            if (plan.columnKind[j] == 2) {
                std::fill(column.begin(), column.end(), std::complex<float>(0.0f));
                continue;
            }
            fft(column);
            const float* w = plan.weights.data() + j * nk;
            for (size_t m = 0; m < nk; ++m) column[m] *= w[m];
            ifft(column);
        }
        for (size_t x = 0; x < nx; ++x) {
            std::complex<float>* row = half.data() + x * nf + j0;
            for (size_t b = 0; b < count; ++b) row[b] = columns[b][x];
        }
    }
    // Back to time, two traces per inverse FFT: Y = Ya + i Yb with each
    // extended Hermitian, so the real part is trace a and the imaginary b.
    for (size_t x = 0; x < nx; x += 2) {
        const bool pair = x + 1 < nx;
        for (size_t j = 0; j < nf; ++j) {
            const std::complex<float> a = half[x * nf + j];
            const std::complex<float> b = pair ? half[(x + 1) * nf + j] : std::complex<float>(0.0f);
            buffer[j] = a + std::complex<float>(0.0f, 1.0f) * b;
            if (j > 0 && j < nt / 2) buffer[nt - j] = std::conj(a) + std::complex<float>(0.0f, 1.0f) * std::conj(b);
        }
        // Nyquist and DC of a real signal are real: drop any imaginary residue
        // so it does not leak into the other trace.
        ifft(buffer);
        for (size_t i = 0; i < ns; ++i) {
            traces[x][i] = buffer[i].real();
            if (pair) traces[x + 1][i] = buffer[i].imag();
        }
    }
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
