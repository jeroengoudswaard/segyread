#include "iirfilter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace segy {

namespace {

using cplx = std::complex<double>;
const double kPi = std::acos(-1.0);

// scipy.signal.buttap: analog lowpass prototype poles, unit cutoff, gain 1.
std::vector<cplx> butterworthPrototypePoles(int order) {
    std::vector<cplx> poles;
    for (int m = -order + 1; m < order; m += 2) {
        poles.push_back(-std::exp(cplx(0.0, kPi * double(m) / (2.0 * double(order)))));
    }
    return poles;
}

// The bilinear transform as scipy applies it inside butter(): frequencies
// normalized to Nyquist and prewarped with fs = 2 (so the analog-domain
// frequency is 4 * tan(pi * Wn / 2)), then mapped with z = (4 + s) / (4 - s).
double prewarp(double hz, double sampleRateHz) {
    double wn = hz / (sampleRateHz / 2.0);
    return 4.0 * std::tan(kPi * wn / 2.0);
}
cplx bilinear(cplx s) { return (4.0 + s) / (4.0 - s); }

// Groups digital poles into conjugate pairs (complex) or pairs of real
// poles, and zeros likewise, into sections. Any pairing gives the same
// overall transfer function; this one keeps each section real-valued.
// `zeros` must have the same count as `poles`.
std::vector<Biquad> toSections(std::vector<cplx> poles, std::vector<cplx> zeros, double gain) {
    auto split = [](std::vector<cplx>& roots, std::vector<std::pair<cplx, cplx>>& pairs) {
        std::vector<cplx> reals;
        std::vector<cplx> upper;
        for (const cplx& r : roots) {
            if (std::fabs(r.imag()) < 1e-12 * std::max(1.0, std::abs(r))) {
                reals.push_back(cplx(r.real(), 0.0));
            } else if (r.imag() > 0.0) {
                upper.push_back(r);
            }
        }
        for (const cplx& u : upper) pairs.push_back({u, std::conj(u)});
        std::sort(reals.begin(), reals.end(), [](const cplx& a, const cplx& b) { return a.real() < b.real(); });
        for (size_t i = 0; i + 1 < reals.size(); i += 2) pairs.push_back({reals[i], reals[i + 1]});
        if (reals.size() % 2 == 1) pairs.push_back({reals.back(), cplx(0.0, 0.0)}); // first-order section
    };
    std::vector<std::pair<cplx, cplx>> polePairs, zeroPairs;
    split(poles, polePairs);
    split(zeros, zeroPairs);

    // A first-order pole pair (second root 0) gets a first-order zero pair.
    auto isFirstOrder = [](const std::pair<cplx, cplx>& p) { return p.second == cplx(0.0, 0.0) && p.first.imag() == 0.0; };
    std::stable_partition(polePairs.begin(), polePairs.end(), [&](const auto& p) { return !isFirstOrder(p); });
    std::stable_partition(zeroPairs.begin(), zeroPairs.end(), [&](const auto& p) { return !isFirstOrder(p); });

    std::vector<Biquad> sos;
    for (size_t i = 0; i < polePairs.size(); ++i) {
        const auto& p = polePairs[i];
        const auto& z = zeroPairs[std::min(i, zeroPairs.size() - 1)];
        Biquad s;
        if (isFirstOrder(p)) {
            s.a1 = -p.first.real();
            s.a2 = 0.0;
            s.b0 = 1.0;
            s.b1 = -z.first.real();
            s.b2 = 0.0;
        } else {
            s.a1 = -(p.first + p.second).real();
            s.a2 = (p.first * p.second).real();
            s.b0 = 1.0;
            s.b1 = -(z.first + z.second).real();
            s.b2 = (z.first * z.second).real();
        }
        sos.push_back(s);
    }
    if (!sos.empty()) {
        sos.front().b0 *= gain;
        sos.front().b1 *= gain;
        sos.front().b2 *= gain;
    }
    return sos;
}

// Run one cascade pass (scipy.signal.sosfilt, transposed direct form II)
// over `x` in place, starting from per-section state `zi` scaled by `x0`.
void sosFilterPass(const std::vector<Biquad>& sos, const std::vector<std::array<double, 2>>& zi, double x0,
                   std::vector<double>& x) {
    std::vector<std::array<double, 2>> state(sos.size());
    for (size_t s = 0; s < sos.size(); ++s) state[s] = {zi[s][0] * x0, zi[s][1] * x0};
    for (double& v : x) {
        double in = v;
        for (size_t s = 0; s < sos.size(); ++s) {
            const Biquad& q = sos[s];
            double out = q.b0 * in + state[s][0];
            state[s][0] = q.b1 * in - q.a1 * out + state[s][1];
            state[s][1] = q.b2 * in - q.a2 * out;
            in = out;
        }
        v = in;
    }
}

// scipy.signal.sosfilt_zi: per-section initial state giving the steady-
// state response to a unit step, each section's scaled by the DC gain of
// the sections before it.
std::vector<std::array<double, 2>> sosFiltZi(const std::vector<Biquad>& sos) {
    std::vector<std::array<double, 2>> zi(sos.size());
    double scale = 1.0;
    for (size_t s = 0; s < sos.size(); ++s) {
        const Biquad& q = sos[s];
        // lfilter_zi for a 2nd-order section: solve (I - A^T) zi = b[1:] - a[1:] * b0.
        double B0 = q.b1 - q.a1 * q.b0;
        double B1 = q.b2 - q.a2 * q.b0;
        double denom = 1.0 + q.a1 + q.a2;
        double z0 = denom != 0.0 ? (B0 + B1) / denom : 0.0;
        double z1 = B1 - q.a2 * z0;
        zi[s] = {scale * z0, scale * z1};
        double dcDen = 1.0 + q.a1 + q.a2;
        scale *= dcDen != 0.0 ? (q.b0 + q.b1 + q.b2) / dcDen : 0.0;
    }
    return zi;
}

} // namespace

std::vector<Biquad> butterworthBandpass(int order, double lowHz, double highHz, double sampleRateHz) {
    const double w1 = prewarp(lowHz, sampleRateHz);
    const double w2 = prewarp(highHz, sampleRateHz);
    const double bw = w2 - w1;
    const double wo = std::sqrt(w1 * w2);
    // lp2bp_zpk: each prototype pole p -> p*bw/2 +/- sqrt((p*bw/2)^2 - wo^2);
    // `order` zeros at s = 0; gain bw^order.
    std::vector<cplx> analogPoles;
    for (const cplx& p : butterworthPrototypePoles(order)) {
        cplx lp = p * (bw / 2.0);
        cplx root = std::sqrt(lp * lp - wo * wo);
        analogPoles.push_back(lp + root);
        analogPoles.push_back(lp - root);
    }
    double analogGain = std::pow(bw, order);
    // bilinear_zpk: zeros at s = 0 -> z = 1; the `order` zeros "at infinity"
    // (degree difference) -> z = -1; gain * prod(4 - z) / prod(4 - p).
    std::vector<cplx> poles, zeros;
    cplx num = std::pow(cplx(4.0, 0.0), order); // prod(4 - 0) over the `order` analog zeros
    cplx den(1.0, 0.0);
    for (const cplx& p : analogPoles) {
        poles.push_back(bilinear(p));
        den *= (4.0 - p);
    }
    for (int i = 0; i < order; ++i) zeros.push_back(cplx(1.0, 0.0));
    for (int i = 0; i < order; ++i) zeros.push_back(cplx(-1.0, 0.0));
    double gain = analogGain * (num / den).real();
    return toSections(poles, zeros, gain);
}

std::vector<Biquad> butterworthHighpass(int order, double cutoffHz, double sampleRateHz) {
    const double wo = prewarp(cutoffHz, sampleRateHz);
    // lp2hp_zpk: p -> wo / p; `order` zeros at s = 0; gain 1 / real(prod(-p)).
    std::vector<cplx> analogPoles;
    cplx prodNegP(1.0, 0.0);
    for (const cplx& p : butterworthPrototypePoles(order)) {
        analogPoles.push_back(wo / p);
        prodNegP *= -p;
    }
    double analogGain = 1.0 / prodNegP.real();
    std::vector<cplx> poles, zeros;
    cplx num = std::pow(cplx(4.0, 0.0), order);
    cplx den(1.0, 0.0);
    for (const cplx& p : analogPoles) {
        poles.push_back(bilinear(p));
        den *= (4.0 - p);
    }
    for (int i = 0; i < order; ++i) zeros.push_back(cplx(1.0, 0.0));
    double gain = analogGain * (num / den).real();
    return toSections(poles, zeros, gain);
}

std::vector<Biquad> butterworthLowpass(int order, double cutoffHz, double sampleRateHz) {
    const double wo = prewarp(cutoffHz, sampleRateHz);
    // lp2lp_zpk: p -> p * wo; gain wo^order. bilinear_zpk: all `order`
    // zeros are "at infinity" -> z = -1; gain * prod(4 - z) / prod(4 - p)
    // with no analog zeros, i.e. 1 / prod(4 - p).
    std::vector<cplx> poles, zeros;
    cplx den(1.0, 0.0);
    for (const cplx& p : butterworthPrototypePoles(order)) {
        cplx analog = p * wo;
        poles.push_back(bilinear(analog));
        den *= (4.0 - analog);
    }
    for (int i = 0; i < order; ++i) zeros.push_back(cplx(-1.0, 0.0));
    double gain = std::pow(wo, order) * (cplx(1.0, 0.0) / den).real();
    return toSections(poles, zeros, gain);
}

std::vector<Biquad> designButterworthBand(const ButterworthBandParams& params, double sampleRateHz,
                                          std::string* error) {
    auto fail = [error](const char* message) {
        if (error) *error = message;
        return std::vector<Biquad>{};
    };
    const double nyquist = sampleRateHz / 2.0;
    if (sampleRateHz <= 0.0) return fail("The file has no valid sample interval.");
    const int order = std::clamp(params.order, 1, 20);
    const bool hasLow = params.lowHz > 0.0;
    const bool hasHigh = params.highHz < nyquist * 0.999;
    if (hasLow && params.lowHz >= nyquist * 0.999) return fail("The low corner is at or above the Nyquist frequency.");
    if (!hasLow && !hasHigh) return fail("Both corners are open (low 0 Hz, high at/above Nyquist): nothing to filter.");
    if (hasLow && hasHigh) {
        if (params.lowHz >= params.highHz) return fail("The low corner must be below the high corner.");
        return butterworthBandpass(order, params.lowHz, params.highHz, sampleRateHz);
    }
    return hasLow ? butterworthHighpass(order, params.lowHz, sampleRateHz)
                  : butterworthLowpass(order, params.highHz, sampleRateHz);
}

int sosFiltFiltDefaultPadlen(const std::vector<Biquad>& sos) {
    int zeroB2 = 0, zeroA2 = 0;
    for (const Biquad& q : sos) {
        if (q.b2 == 0.0) ++zeroB2;
        if (q.a2 == 0.0) ++zeroA2;
    }
    return 3 * (2 * int(sos.size()) + 1 - std::min(zeroB2, zeroA2));
}

void sosFiltFilt(const std::vector<Biquad>& sos, std::vector<float>& x, int padlen, EdgePadding padding) {
    const int n = int(x.size());
    if (n < 2 || sos.empty()) return;
    int edge = padlen < 0 ? sosFiltFiltDefaultPadlen(sos) : padlen;
    edge = std::clamp(edge, 0, n - 1);

    // scipy's even_ext / odd_ext / const_ext: `edge` samples past each end.
    // The mirrored samples are x[edge..1] on the left and x[n-2..n-1-edge]
    // on the right (the end sample itself is not repeated).
    std::vector<double> ext(size_t(n + 2 * edge));
    const double first = x[0], last = x[size_t(n - 1)];
    for (int i = 0; i < edge; ++i) {
        const double mirrored = x[size_t(edge - i)];
        ext[size_t(i)] = padding == EdgePadding::Even ? mirrored
                         : padding == EdgePadding::Odd ? 2.0 * first - mirrored
                                                       : first;
    }
    for (int i = 0; i < n; ++i) ext[size_t(edge + i)] = x[size_t(i)];
    for (int i = 0; i < edge; ++i) {
        const double mirrored = x[size_t(n - 2 - i)];
        ext[size_t(edge + n + i)] = padding == EdgePadding::Even ? mirrored
                                    : padding == EdgePadding::Odd ? 2.0 * last - mirrored
                                                                  : last;
    }

    const auto zi = sosFiltZi(sos);
    sosFilterPass(sos, zi, ext.front(), ext);   // forward
    std::reverse(ext.begin(), ext.end());
    sosFilterPass(sos, zi, ext.front(), ext);   // backward
    std::reverse(ext.begin(), ext.end());
    for (int i = 0; i < n; ++i) x[size_t(i)] = float(ext[size_t(edge + i)]);
}

} // namespace segy
