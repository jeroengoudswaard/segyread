#include "analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <random>

#include "fft.h"
#include "segy_decode.h"
#include "segy_format.h"

namespace segy {

namespace {

// Bounds how much a "Full View" histogram/spectrum over a huge file has to
// decode -- both are meant as a quick-look diagnostic, not an exhaustive
// scan, so striding down to a fixed trace budget keeps them fast without
// materially changing the shape of either result for real seismic data
// (adjacent traces are highly correlated).
constexpr int64_t kHistogramMaxTraces = 500;
constexpr int64_t kSpectrumMaxTraces = 50;

std::vector<int64_t> strideTraceIndices(int64_t traceStart, int64_t traceEnd, int64_t traceCount, int64_t maxTraces) {
    std::vector<int64_t> indices;
    traceStart = std::clamp<int64_t>(traceStart, 0, std::max<int64_t>(0, traceCount - 1));
    traceEnd = std::clamp<int64_t>(traceEnd, traceStart + 1, traceCount);
    int64_t span = traceEnd - traceStart;
    if (span <= 0) return indices;
    int64_t step = std::max<int64_t>(1, span / maxTraces);
    for (int64_t t = traceStart; t < traceEnd; t += step) indices.push_back(t);
    return indices;
}

// One trace's decoded samples over [sampleStart, sampleEnd), clamped to the
// file's actual sample count.
std::vector<float> decodeTraceRange(const RenderContext& ctx, int64_t traceIdx, int32_t sampleStart,
                                     int32_t sampleEnd) {
    int samplesPerTrace = ctx.binHeader.samplesPerTrace;
    int s0 = std::clamp<int32_t>(sampleStart, 0, samplesPerTrace);
    int s1 = std::clamp<int32_t>(sampleEnd, s0, samplesPerTrace);
    std::vector<float> out(size_t(s1 - s0));
    if (out.empty()) return out;
    const uint8_t* fileBase = ctx.file->data();
    const uint8_t* traceBase = fileBase + kHeaderTotalSize + size_t(traceIdx) * ctx.traceStrideBytes;
    int sampleSize = sampleFormatSizeBytes(ctx.binHeader.formatCode);
    const uint8_t* sampleBase = traceBase + kTraceHeaderSize + size_t(s0) * size_t(sampleSize);
    decodeSamples(sampleBase, out.data(), out.size(), ctx.binHeader.formatCode);
    return out;
}

} // namespace

HistogramResult computeHistogram(const RenderContext& ctx, int64_t traceStart, int64_t traceEnd, int32_t sampleStart,
                                  int32_t sampleEnd, int numBins, float rangeMin, float rangeMax) {
    HistogramResult result;
    numBins = std::max(1, numBins);
    if (rangeMax <= rangeMin) rangeMax = rangeMin + 1.0f;
    result.rangeMin = rangeMin;
    result.rangeMax = rangeMax;
    result.binCenters.resize(size_t(numBins));
    float binWidth = (rangeMax - rangeMin) / float(numBins);
    for (int i = 0; i < numBins; ++i) result.binCenters[size_t(i)] = rangeMin + (float(i) + 0.5f) * binWidth;

    std::vector<int64_t> counts(size_t(numBins), 0);
    if (!ctx.file || !ctx.file->isOpen() || ctx.traceCount <= 0) return result;

    for (int64_t traceIdx : strideTraceIndices(traceStart, traceEnd, ctx.traceCount, kHistogramMaxTraces)) {
        std::vector<float> samples = decodeTraceRange(ctx, traceIdx, sampleStart, sampleEnd);
        for (float value : samples) {
            int bin = int((value - rangeMin) / binWidth);
            bin = std::clamp(bin, 0, numBins - 1);
            ++counts[size_t(bin)];
            ++result.sampleCount;
        }
    }

    result.percentOfTotal.resize(size_t(numBins), 0.0f);
    result.cumulativePercent.resize(size_t(numBins), 0.0f);
    if (result.sampleCount > 0) {
        double cumulative = 0.0;
        for (int i = 0; i < numBins; ++i) {
            double percent = 100.0 * double(counts[size_t(i)]) / double(result.sampleCount);
            result.percentOfTotal[size_t(i)] = float(percent);
            cumulative += percent;
            result.cumulativePercent[size_t(i)] = float(cumulative);
        }
    }
    return result;
}

SpectrumResult computeSpectrum(const RenderContext& ctx, int64_t traceStart, int64_t traceEnd, int32_t sampleStart,
                                int32_t sampleEnd, double sampleIntervalUs, int smoothPoints) {
    SpectrumResult result;
    if (!ctx.file || !ctx.file->isOpen() || ctx.traceCount <= 0 || sampleIntervalUs <= 0) return result;

    std::vector<int64_t> traceIndices = strideTraceIndices(traceStart, traceEnd, ctx.traceCount, kSpectrumMaxTraces);
    if (traceIndices.empty()) return result;

    int rawLength = std::max(0, sampleEnd - sampleStart);
    if (rawLength < 2) return result;
    size_t fftLength = nextPowerOfTwo(size_t(rawLength));
    size_t halfLength = fftLength / 2;

    std::vector<double> magnitudeSum(halfLength + 1, 0.0);
    int accumulated = 0;
    for (int64_t traceIdx : traceIndices) {
        std::vector<float> samples = decodeTraceRange(ctx, traceIdx, sampleStart, sampleEnd);
        if (samples.size() < 2) continue;

        std::vector<std::complex<float>> spectrum(fftLength, std::complex<float>(0.0f, 0.0f));
        // Hann window: reduces spectral leakage from the window's hard edges
        // (the FFT otherwise implicitly assumes the windowed segment
        // repeats periodically, which a raw rectangular cut does not).
        for (size_t i = 0; i < samples.size(); ++i) {
            double w = 0.5 - 0.5 * std::cos(2.0 * std::acos(-1.0) * double(i) / double(samples.size() - 1));
            spectrum[i] = std::complex<float>(samples[i] * float(w), 0.0f);
        }
        fft(spectrum);
        for (size_t k = 0; k <= halfLength; ++k) magnitudeSum[k] += std::abs(spectrum[k]);
        ++accumulated;
    }
    if (accumulated == 0) return result;

    double dt = sampleIntervalUs / 1'000'000.0; // seconds
    std::vector<double> magnitudeAvg(halfLength + 1);
    double referencePeak = 0.0;
    for (size_t k = 0; k <= halfLength; ++k) {
        magnitudeAvg[k] = magnitudeSum[k] / double(accumulated);
        referencePeak = std::max(referencePeak, magnitudeAvg[k]);
    }
    if (referencePeak <= 0.0) referencePeak = 1.0;

    result.frequencyHz.resize(halfLength + 1);
    result.amplitudeDb.resize(halfLength + 1);
    for (size_t k = 0; k <= halfLength; ++k) {
        result.frequencyHz[k] = float(double(k) / (double(fftLength) * dt));
        double ratio = magnitudeAvg[k] / referencePeak;
        result.amplitudeDb[k] = float(20.0 * std::log10(std::max(ratio, 1e-6)));
    }

    // Centered moving-average smoothing over the dB curve -- even widths
    // round up to the next odd one so the window has a well-defined center.
    if (smoothPoints > 1) {
        int window = smoothPoints % 2 == 0 ? smoothPoints + 1 : smoothPoints;
        int half = window / 2;
        std::vector<float> smoothed(result.amplitudeDb.size());
        for (size_t i = 0; i < result.amplitudeDb.size(); ++i) {
            int lo = std::max(0, int(i) - half);
            int hi = std::min(int(result.amplitudeDb.size()) - 1, int(i) + half);
            double sum = 0.0;
            for (int j = lo; j <= hi; ++j) sum += result.amplitudeDb[size_t(j)];
            smoothed[i] = float(sum / double(hi - lo + 1));
        }
        result.amplitudeDb = std::move(smoothed);
    }
    return result;
}

std::vector<float> normalizedAutocorrelation(const float* samples, int count, int maxLag) {
    if (count <= 0 || maxLag < 0) return {};
    maxLag = std::min(maxLag, count - 1);
    std::vector<double> r(size_t(maxLag) + 1, 0.0);
    for (int k = 0; k <= maxLag; ++k) {
        double sum = 0.0;
        for (int i = 0; i + k < count; ++i) sum += double(samples[i]) * double(samples[i + k]);
        r[size_t(k)] = sum;
    }
    std::vector<float> out(r.size(), 0.0f);
    if (r[0] > 0.0) {
        for (size_t k = 0; k < r.size(); ++k) out[k] = float(r[k] / r[0]);
    }
    return out;
}

std::vector<float> cosineSquaredWindow(int n, double flatPercent) {
    std::vector<float> w(size_t(std::max(n, 0)), 1.0f);
    if (n <= 2) return w;
    const int flatCount = int(std::lround(std::clamp(flatPercent, 0.0, 100.0) / 100.0 * n));
    const int ramp = (n - flatCount) / 2;
    const double halfPi = 0.5 * std::acos(-1.0);
    for (int i = 0; i < ramp; ++i) {
        // Sample centres, so neither end is exactly zero and the ramp is symmetric.
        double s = std::sin(halfPi * (double(i) + 0.5) / double(ramp));
        w[size_t(i)] = w[size_t(n - 1 - i)] = float(s * s);
    }
    return w;
}

XfGrid makeXfGrid(int gateSamples, double sampleIntervalUs) {
    XfGrid grid;
    if (gateSamples < 2 || sampleIntervalUs <= 0.0) return grid;
    grid.fftLength = int(std::max<size_t>(64, nextPowerOfTwo(size_t(gateSamples))));
    grid.fftBinHz = 1e6 / (double(grid.fftLength) * sampleIntervalUs);
    const double nyquistMilliHz = 5e8 / sampleIntervalUs; // Nyquist in mHz
    grid.binIntervalUs = int(std::clamp<long>(std::lround(grid.fftBinHz * 1000.0), 1, 32767));
    if (nyquistMilliHz / grid.binIntervalUs + 1.0 > 32767.0) {
        grid.binIntervalUs = int(std::min(32767.0, std::ceil(nyquistMilliHz / 32766.0)));
    }
    grid.bins = int(std::floor(nyquistMilliHz / grid.binIntervalUs + 1e-9)) + 1;
    return grid;
}

void xfAmplitudeSpectrum(const float* samples, int count, const std::vector<float>& window, const XfGrid& grid,
                         float* out) {
    if (grid.fftLength <= 0 || grid.bins <= 0) return;
    const size_t len = size_t(grid.fftLength);
    std::vector<std::complex<float>> buf(len, std::complex<float>(0.0f, 0.0f));
    count = std::min(count, grid.fftLength);
    double mean = 0.0;
    for (int i = 0; i < count; ++i) mean += samples[i];
    mean = count > 0 ? mean / count : 0.0;
    for (int i = 0; i < count; ++i) {
        float w = size_t(i) < window.size() ? window[size_t(i)] : 1.0f;
        buf[size_t(i)] = std::complex<float>(float((samples[i] - mean) * w), 0.0f);
    }
    fft(buf);
    const size_t half = len / 2;
    for (int k = 0; k < grid.bins; ++k) {
        double x = (double(k) * grid.binIntervalUs / 1000.0) / grid.fftBinHz; // fractional FFT bin
        size_t i0 = std::min(half, size_t(std::floor(x)));
        size_t i1 = std::min(half, i0 + 1);
        double frac = std::clamp(x - double(i0), 0.0, 1.0);
        out[k] = float(std::abs(buf[i0]) * (1.0 - frac) + std::abs(buf[i1]) * frac);
    }
}

std::vector<float> fractionalShift(const std::vector<float>& x, double shift) {
    const size_t n = x.size();
    if (n == 0 || shift == 0.0) return x;
    // Zero-padded to twice the length so the circular wrap-around of the
    // phase ramp only touches the padding, not the samples we return.
    const size_t len = nextPowerOfTwo(2 * n);
    std::vector<std::complex<float>> buf(len, std::complex<float>(0.0f, 0.0f));
    for (size_t i = 0; i < n; ++i) buf[i] = std::complex<float>(x[i], 0.0f);
    fft(buf);
    const double twoPi = 2.0 * std::acos(-1.0);
    for (size_t m = 0; m < len; ++m) {
        if (m == len / 2) {
            buf[m] *= float(std::cos(0.5 * twoPi * shift)); // Nyquist bin stays real
            continue;
        }
        double freq = m < len / 2 ? double(m) : double(m) - double(len); // signed bin
        double phase = twoPi * freq * shift / double(len);
        buf[m] *= std::complex<float>(float(std::cos(phase)), float(std::sin(phase)));
    }
    ifft(buf);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = buf[i].real();
    return out;
}

SigcorResult computeSigcor(const SigcorTraceFetch& fetch, int samplesPerTrace, int64_t traceStart, int64_t traceEnd,
                           int gateStart, int gateEnd, double sampleIntervalUs, const SigcorParams& params,
                           const std::function<void(int, int)>& progress) {
    SigcorResult res;
    traceStart = std::max<int64_t>(0, traceStart);
    gateStart = std::clamp(gateStart, 0, std::max(0, samplesPerTrace));
    gateEnd = std::clamp(gateEnd, gateStart, std::max(0, samplesPerTrace));
    const int n = gateEnd - gateStart;
    res.gateSamples = n;
    if (n < 8 || sampleIntervalUs <= 0.0 || !fetch) return res;

    const int nskip = std::max(1, params.nskip);
    const int iransk = std::max(0, params.iransk);
    const int64_t lastStart = traceEnd - 1 - nskip;
    if (lastStart < traceStart) return res;
    const int64_t startCount = lastStart - traceStart + 1;
    const int pairCount = int(std::min<int64_t>(startCount, std::max(1, params.maxPairs)));

    const double dtMs = sampleIntervalUs / 1000.0;
    // SHMAX of 0 or 1 sample: no shift estimate at all, the user has aligned
    // the data (same rule as SIGCOR).
    const int maxShift = std::max(0, int(std::lround(params.shmaxMs / dtMs)));
    const bool estimateShift = maxShift > 1;
    const bool autcor = params.option == SigcorOption::Autcor;

    std::mt19937 rng(params.seed);
    std::uniform_int_distribution<int> extraDistance(0, iransk);
    const std::vector<float> window = cosineSquaredWindow(n, params.flatPercent);

    // SPECTR: periodograms on the gate's own power-of-two length. AUTCOR:
    // correlations up to lag n - 1, so the final transform needs >= 2n - 1.
    const size_t fftLen = autcor ? nextPowerOfTwo(size_t(2 * n)) : nextPowerOfTwo(size_t(n));
    const size_t half = fftLen / 2;
    std::vector<double> sumTrace(autcor ? size_t(n) : half + 1, 0.0);
    std::vector<double> sumSignal(sumTrace.size(), 0.0);

    std::vector<float> traceA(static_cast<size_t>(samplesPerTrace)), traceB(static_cast<size_t>(samplesPerTrace));
    std::vector<double> a(static_cast<size_t>(n)), b(static_cast<size_t>(n));
    std::vector<double> lagValues;
    constexpr int kPad = 16; // context around the gate for the fractional shift
    std::vector<std::complex<float>> specA(fftLen), specB(fftLen);

    for (int p = 0; p < pairCount; ++p) {
        const int64_t i = traceStart + (pairCount == startCount
                                            ? int64_t(p)
                                            : int64_t((double(p) + 0.5) * double(startCount) / double(pairCount)));
        int64_t j = i + nskip + (iransk > 0 ? extraDistance(rng) : 0);
        if (j > traceEnd - 1) j = i + nskip;
        ++res.pairsTotal;
        if (progress) progress(p + 1, pairCount);
        fetch(i, traceA.data());
        fetch(j, traceB.data());

        double meanA = 0.0;
        for (int t = 0; t < n; ++t) meanA += (a[size_t(t)] = traceA[size_t(gateStart + t)]);
        meanA /= n;
        if (params.removeDc) {
            for (double& v : a) v -= meanA;
        }
        double energyA = 0.0;
        for (double v : a) energyA += v * v;
        if (energyA <= 0.0) {
            ++res.pairsDead;
            continue;
        }

        // Integer shift: maximum of the crosscorrelation of gate A with the
        // second trace over lags -SHMAX..+SHMAX. The second gate is gate A
        // extended by SHMAX on both sides; where the trace ends first (a
        // gate reaching the top or bottom of the trace), gate A is trimmed
        // by SHMAX there for this estimate only, so the full lag range
        // always fits. The spectra still use the whole gate.
        int bestLag = 0;
        double delta = 0.0;
        if (estimateShift) {
            const int lagMin = -maxShift, lagMax = maxShift;
            const int corrStart = std::max(gateStart, maxShift) - gateStart;              // offset into a
            const int corrEnd = std::min(gateEnd, samplesPerTrace - maxShift) - gateStart; // exclusive
            if (corrEnd - corrStart < 8) {
                ++res.pairsAtShiftLimit;
                continue;
            }
            // DC of trace B over the whole searched window.
            double meanB = 0.0;
            if (params.removeDc) {
                for (int t = gateStart + corrStart + lagMin; t < gateStart + corrEnd + lagMax; ++t) {
                    meanB += traceB[size_t(t)];
                }
                meanB /= double(corrEnd - corrStart + lagMax - lagMin);
            }
            lagValues.assign(size_t(lagMax - lagMin + 1), 0.0);
            double bestValue = -std::numeric_limits<double>::infinity();
            for (int k = lagMin; k <= lagMax; ++k) {
                double sum = 0.0, energyB = 0.0;
                for (int t = corrStart; t < corrEnd; ++t) {
                    double v = double(traceB[size_t(gateStart + t + k)]) - meanB;
                    sum += a[size_t(t)] * v;
                    energyB += v * v;
                }
                double value = sum;
                if (params.tcross == SigcorCrossCorrelator::Normalized) {
                    value = energyB > 0.0 ? sum / std::sqrt(energyB) : 0.0;
                }
                lagValues[size_t(k - lagMin)] = value;
                if (value > bestValue) {
                    bestValue = value;
                    bestLag = k;
                }
            }
            if (bestLag == lagMin || bestLag == lagMax) {
                ++res.pairsAtShiftLimit;
                continue;
            }
            // Parabolic interpolation through the maximum and its neighbours.
            double ym = lagValues[size_t(bestLag - 1 - lagMin)];
            double y0 = lagValues[size_t(bestLag - lagMin)];
            double yp = lagValues[size_t(bestLag + 1 - lagMin)];
            double curvature = ym - 2.0 * y0 + yp;
            if (curvature < 0.0) delta = std::clamp(0.5 * (ym - yp) / curvature, -0.5, 0.5);
        }

        // Gate B aligned to gate A: integer part by indexing, fraction by a
        // band-limited phase shift over the gate plus some context.
        std::vector<float> segment(size_t(n + 2 * kPad), 0.0f);
        for (int t = 0; t < n + 2 * kPad; ++t) {
            int src = gateStart + bestLag - kPad + t;
            if (src >= 0 && src < samplesPerTrace) segment[size_t(t)] = traceB[size_t(src)];
        }
        if (delta != 0.0) segment = fractionalShift(segment, delta);
        double meanBGate = 0.0;
        for (int t = 0; t < n; ++t) meanBGate += (b[size_t(t)] = segment[size_t(kPad + t)]);
        meanBGate /= n;
        if (params.removeDc) {
            for (double& v : b) v -= meanBGate;
        }
        double energyB = 0.0, cross = 0.0;
        for (int t = 0; t < n; ++t) {
            energyB += b[size_t(t)] * b[size_t(t)];
            cross += a[size_t(t)] * b[size_t(t)];
        }
        if (energyB <= 0.0) {
            ++res.pairsDead;
            continue;
        }
        const double rho = cross / std::sqrt(energyA * energyB);
        if (rho < params.ccmin) {
            ++res.pairsLowCorrelation;
            continue;
        }

        // Equalize energy (unit RMS per gate), then taper.
        const double scaleA = params.normalise ? 1.0 / std::sqrt(energyA / n) : 1.0;
        const double scaleB = params.normalise ? 1.0 / std::sqrt(energyB / n) : 1.0;
        std::fill(specA.begin(), specA.end(), std::complex<float>(0.0f, 0.0f));
        std::fill(specB.begin(), specB.end(), std::complex<float>(0.0f, 0.0f));
        for (int t = 0; t < n; ++t) {
            specA[size_t(t)] = float(a[size_t(t)] * scaleA * window[size_t(t)]);
            specB[size_t(t)] = float(b[size_t(t)] * scaleB * window[size_t(t)]);
        }
        fft(specA);
        fft(specB);
        if (!autcor) {
            for (size_t m = 0; m <= half; ++m) {
                std::complex<double> A(specA[m]), B(specB[m]);
                sumTrace[m] += 0.5 * (std::norm(A) + std::norm(B));
                sumSignal[m] += (A * std::conj(B)).real();
            }
        } else {
            // Time domain: trace autocorrelation (r_aa + r_bb) / 2 in the
            // real part, symmetrized crosscorrelation (c_ab(k) + c_ab(-k)) / 2
            // (the signal autocorrelation) in the imaginary part of one
            // inverse transform -- both are real and even.
            for (size_t m = 0; m < fftLen; ++m) {
                std::complex<double> A(specA[m]), B(specB[m]);
                double tracePart = 0.5 * (std::norm(A) + std::norm(B));
                double signalPart = (std::conj(A) * B).real();
                specA[m] = std::complex<float>(float(tracePart), float(signalPart));
            }
            ifft(specA);
            for (int k = 0; k < n; ++k) {
                sumTrace[size_t(k)] += specA[size_t(k)].real();
                sumSignal[size_t(k)] += specA[size_t(k)].imag();
            }
        }
        res.pairTrace.push_back(i);
        res.pairShiftMs.push_back(float((double(bestLag) + delta) * dtMs));
        res.pairCorrelation.push_back(float(rho));
        ++res.pairsUsed;
    }
    if (res.pairsUsed == 0) return res;

    const double invUsed = 1.0 / double(res.pairsUsed);
    res.tracePower.assign(half + 1, 0.0);
    res.signalPower.assign(half + 1, 0.0);
    if (!autcor) {
        for (size_t m = 0; m <= half; ++m) {
            res.tracePower[m] = sumTrace[m] * invUsed;
            res.signalPower[m] = sumSignal[m] * invUsed;
        }
    } else {
        // Averaged correlations -> power spectra: transform the even
        // sequences r[-(n-1)..n-1] (trace in real, signal in imaginary).
        std::vector<std::complex<float>> corr(fftLen, std::complex<float>(0.0f, 0.0f));
        for (int k = 0; k < n; ++k) {
            std::complex<float> v(float(sumTrace[size_t(k)] * invUsed), float(sumSignal[size_t(k)] * invUsed));
            corr[size_t(k)] = v;
            if (k > 0) corr[fftLen - size_t(k)] = v;
        }
        fft(corr);
        // FFT of (t + i s) with t, s real and even = T + i S, T and S real.
        for (size_t m = 0; m <= half; ++m) {
            res.tracePower[m] = corr[m].real();
            res.signalPower[m] = corr[m].imag();
        }
    }
    res.noisePower.resize(half + 1);
    res.frequencyHz.resize(half + 1);
    const double dtSeconds = sampleIntervalUs / 1e6;
    for (size_t m = 0; m <= half; ++m) {
        res.noisePower[m] = res.tracePower[m] - res.signalPower[m];
        res.frequencyHz[m] = float(double(m) / (double(fftLen) * dtSeconds));
    }

    double sum = 0.0;
    for (float s : res.pairShiftMs) sum += s;
    res.shiftMeanMs = sum / res.pairsUsed;
    double var = 0.0;
    for (float s : res.pairShiftMs) var += (s - res.shiftMeanMs) * (s - res.shiftMeanMs);
    res.shiftSigmaMs = res.pairsUsed > 1 ? std::sqrt(var / (res.pairsUsed - 1)) : 0.0;
    return res;
}

} // namespace segy
