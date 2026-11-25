#include "analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <random>

#include "fft.h"
#include "segy_decode.h"
#include "segy_format.h"
#include "threadpool.h"

#include <atomic>

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

namespace {
// One pool for the spectral analyses, created on first use: a pool per call
// cost more than the work for small panels (one f-k per gather). The pool's
// parallelFor is safe to call from several threads at once.
ThreadPool& analysisPool() {
    static ThreadPool pool;
    return pool;
}
} // namespace

std::vector<std::vector<float>> computeXfAmplitudes(const std::function<void(int64_t, float*)>& fetch,
                                                    int samplesPerTrace, int64_t traceStart, int64_t traceEnd,
                                                    int gateStart, int gateEnd, const XfGrid& grid,
                                                    const std::function<void(int64_t)>& progress) {
    const int64_t count = std::max<int64_t>(0, traceEnd - traceStart);
    std::vector<std::vector<float>> spectra(static_cast<size_t>(count));
    gateStart = std::clamp(gateStart, 0, std::max(0, samplesPerTrace));
    gateEnd = std::clamp(gateEnd, gateStart, std::max(0, samplesPerTrace));
    const int gate = gateEnd - gateStart;
    if (count == 0 || gate < 2 || grid.bins <= 0 || !fetch) return spectra;
    const std::vector<float> window = cosineSquaredWindow(gate, 80.0);
    std::atomic<int64_t> done{0};
    ThreadPool& pool = analysisPool();
    pool.parallelFor(size_t(count), 16, [&](size_t begin, size_t end) {
        std::vector<float> trace(static_cast<size_t>(samplesPerTrace));
        for (size_t i = begin; i < end; ++i) {
            fetch(traceStart + int64_t(i), trace.data());
            spectra[i].resize(size_t(grid.bins));
            xfAmplitudeSpectrum(trace.data() + gateStart, gate, window, grid, spectra[i].data());
            const int64_t finished = done.fetch_add(1) + 1;
            if (progress && finished % 64 == 0) progress(finished);
        }
    });
    if (progress) progress(count);
    return spectra;
}

std::vector<FkPanel> splitIntoGathers(const std::vector<int32_t>& keys, int64_t firstTrace, int minTraces) {
    std::vector<FkPanel> panels;
    size_t runStart = 0;
    for (size_t i = 1; i <= keys.size(); ++i) {
        if (i == keys.size() || keys[i] != keys[runStart]) {
            if (int64_t(i - runStart) >= minTraces) {
                panels.push_back({firstTrace + int64_t(runStart), firstTrace + int64_t(i)});
            }
            runStart = i;
        }
    }
    return panels;
}

std::vector<FkPanel> slidingPanels(int64_t start, int64_t end, int width) {
    std::vector<FkPanel> panels;
    if (end <= start || width < 2) return panels;
    if (end - start <= width) return {{start, end}};
    const int64_t step = std::max<int64_t>(1, width / 2);
    int64_t s = start;
    for (; s + width <= end; s += step) panels.push_back({s, s + width});
    if (panels.back().end < end) panels.push_back({end - width, end}); // cover the tail
    return panels;
}

FkResult computeFk(const SigcorTraceFetch& fetch, int samplesPerTrace, const std::vector<FkPanel>& panels,
                   int gateStart, int gateEnd, double sampleIntervalUs, const std::function<void(int, int)>& progress) {
    FkResult res;
    gateStart = std::clamp(gateStart, 0, std::max(0, samplesPerTrace));
    gateEnd = std::clamp(gateEnd, gateStart, std::max(0, samplesPerTrace));
    const int nt = gateEnd - gateStart;
    if (nt < 8 || sampleIntervalUs <= 0.0 || !fetch || panels.empty()) return res;
    int64_t widest = 0;
    for (const FkPanel& panel : panels) widest = std::max(widest, panel.traceCount());
    if (widest < 2) return res;

    const size_t fftT = std::max<size_t>(64, nextPowerOfTwo(size_t(nt)));
    const size_t fftX = std::max<size_t>(32, nextPowerOfTwo(size_t(widest)));
    const size_t nf = fftT / 2 + 1;
    std::vector<double> power(nf * fftX, 0.0);
    const std::vector<float> timeWindow = cosineSquaredWindow(nt, 80.0);
    ThreadPool& pool = analysisPool();

    for (size_t p = 0; p < panels.size(); ++p) {
        if (progress) progress(int(p) + 1, int(panels.size()));
        const FkPanel& panel = panels[p];
        const int nx = int(panel.traceCount());
        if (nx < 2) continue;
        const std::vector<float> spaceWindow = cosineSquaredWindow(nx, 80.0);
        // Time FFT per trace, positive frequencies kept: spectra[x][f].
        std::vector<std::complex<float>> spectra(size_t(nx) * nf);
        pool.parallelFor(size_t(nx), 4, [&](size_t begin, size_t end) {
            std::vector<float> trace(static_cast<size_t>(samplesPerTrace));
            std::vector<std::complex<float>> column(fftT);
            for (size_t x = begin; x < end; ++x) {
                fetch(panel.traceAt(int64_t(x)), trace.data());
                double mean = 0.0;
                for (int i = 0; i < nt; ++i) mean += trace[size_t(gateStart + i)];
                mean /= nt;
                std::fill(column.begin(), column.end(), std::complex<float>(0.0f, 0.0f));
                const float w = spaceWindow[x];
                for (int i = 0; i < nt; ++i) {
                    column[size_t(i)] = float((trace[size_t(gateStart + i)] - mean) * timeWindow[size_t(i)] * w);
                }
                fft(column);
                std::copy_n(column.begin(), nf, spectra.begin() + std::ptrdiff_t(x * nf));
            }
        });
        // Space FFT per frequency. A forward FFT puts t = p x at k_fwd = -f p;
        // the column index flips that so positive dips land at positive k.
        // Each frequency row of `power` belongs to one worker: no races.
        pool.parallelFor(nf, 16, [&](size_t begin, size_t end) {
            std::vector<std::complex<float>> row(fftX);
            for (size_t f = begin; f < end; ++f) {
                std::fill(row.begin(), row.end(), std::complex<float>(0.0f, 0.0f));
                for (int x = 0; x < nx; ++x) row[size_t(x)] = spectra[size_t(x) * nf + f];
                fft(row);
                for (size_t m = 0; m < fftX; ++m) {
                    const int64_t kFwd = m < fftX / 2 ? int64_t(m) : int64_t(m) - int64_t(fftX);
                    const size_t col = size_t((-kFwd + int64_t(fftX / 2)) % int64_t(fftX));
                    power[f * fftX + col] += std::norm(std::complex<double>(row[m]));
                }
            }
        });
        ++res.panelsUsed;
    }
    if (res.panelsUsed == 0) return res;

    const double peak = *std::max_element(power.begin(), power.end());
    res.frequencies = int(nf);
    res.wavenumbers = int(fftX);
    res.frequencyStepHz = 1e6 / (double(fftT) * sampleIntervalUs);
    res.db.resize(power.size());
    for (size_t i = 0; i < power.size(); ++i) {
        res.db[i] = peak > 0.0 && power[i] > 0.0 ? std::max(-120.0f, float(10.0 * std::log10(power[i] / peak))) : -120.0f;
    }
    return res;
}

void traceDerivative(std::vector<float>& trace, double sampleIntervalUs, double slopeDbPerOctave) {
    const size_t n = trace.size();
    if (n < 2 || sampleIntervalUs <= 0.0 || slopeDbPerOctave == 0.0) return;
    const double a = slopeDbPerOctave / (20.0 * std::log10(2.0)); // 6.0206 dB/octave = power 1
    const size_t len = nextPowerOfTwo(2 * n);
    double mean = 0.0;
    for (float v : trace) mean += v;
    mean /= double(n);
    std::vector<std::complex<float>> buf(len, std::complex<float>(0.0f, 0.0f));
    for (size_t i = 0; i < n; ++i) buf[i] = std::complex<float>(float(trace[i] - mean), 0.0f);
    fft(buf);
    const double df = 1e6 / (double(len) * sampleIntervalUs); // Hz per bin
    const double pi = std::acos(-1.0);
    const std::complex<double> phase = std::polar(1.0, a * 0.5 * pi); // i^a
    for (size_t m = 0; m < len; ++m) {
        if (m == 0) {
            buf[m] = 0.0f;
            continue;
        }
        const bool negative = m > len / 2;
        const double f = (negative ? double(len - m) : double(m)) * df;
        const double gain = std::pow(2.0 * pi * f, a);
        // (i w)^a for w > 0; its complex conjugate for the negative
        // frequencies keeps the output real. Nyquist (m = len/2): real part.
        std::complex<double> h = gain * (negative ? std::conj(phase) : phase);
        if (m == len / 2) h = std::complex<double>(h.real(), 0.0);
        buf[m] = std::complex<float>(std::complex<double>(buf[m]) * h);
    }
    ifft(buf);
    // Integration: the mean (DC) was taken out before the FFT, where it has
    // no finite gain -- but its integral is well defined: m t for a full
    // integral, m t^alpha / Gamma(alpha + 1) for a fractional one. Add it
    // back in the time domain; without it, integrating a derivative (whose
    // mean is generally not zero) lost a ramp.
    std::vector<double> out(n);
    const double dtSeconds = sampleIntervalUs * 1e-6;
    const double alpha = -a;
    const double dcScale = a < 0.0 ? mean / std::tgamma(alpha + 1.0) : 0.0;
    for (size_t i = 0; i < n; ++i) {
        out[i] = buf[i].real() + (a < 0.0 ? dcScale * std::pow(double(i) * dtSeconds, alpha) : 0.0);
    }
    double outMean = 0.0;
    for (double v : out) outMean += v;
    outMean /= double(n);
    // An integral is defined up to a constant: keep it centred on zero.
    for (size_t i = 0; i < n; ++i) trace[i] = float(out[i] - (a < 0.0 ? outMean : 0.0));
}

std::vector<float> normalizedCrossCorrelation(const float* a, const float* b, int count, int minLag, int maxLag) {
    if (count <= 0 || maxLag < minLag) return {};
    double ea = 0.0, eb = 0.0;
    for (int i = 0; i < count; ++i) {
        ea += double(a[i]) * a[i];
        eb += double(b[i]) * b[i];
    }
    std::vector<float> out(size_t(maxLag - minLag + 1), 0.0f);
    if (ea <= 0.0 || eb <= 0.0) return out;
    const double norm = 1.0 / std::sqrt(ea * eb);
    for (int k = minLag; k <= maxLag; ++k) {
        double sum = 0.0;
        const int lo = std::max(0, -k), hi = std::min(count, count - k);
        for (int i = lo; i < hi; ++i) sum += double(a[i]) * b[i + k];
        out[size_t(k - minLag)] = float(sum * norm);
    }
    return out;
}

double nearestSidePeakLag(const std::vector<float>& correlation, int minLag, double threshold) {
    const int n = int(correlation.size());
    const int zero = -minLag; // index of lag 0
    if (n < 3 || zero < 0 || zero >= n) return std::nan("");
    auto at = [&](int i) { return double(correlation[size_t(i)]); };
    // Walk outwards from lag 0 in one direction: past the central lobe (to
    // the first local minimum), then to the next local maximum.
    auto search = [&](int step) -> double {
        int i = zero;
        while (i + step > 0 && i + step < n - 1 && at(i + step) <= at(i)) i += step; // down the central lobe
        while (i + step > 0 && i + step < n - 1 && at(i + step) > at(i)) i += step;  // up to the side peak
        if (i == zero || i <= 0 || i >= n - 1) return std::nan("");
        if (at(i) < threshold || at(i) < at(i - 1) || at(i) < at(i + 1)) return std::nan("");
        const double ym = at(i - 1), y0 = at(i), yp = at(i + 1);
        const double curvature = ym - 2.0 * y0 + yp;
        const double delta = curvature < 0.0 ? std::clamp(0.5 * (ym - yp) / curvature, -0.5, 0.5) : 0.0;
        return double(i - zero) + delta;
    };
    const double right = search(+1), left = search(-1);
    if (std::isnan(right)) return left;
    if (std::isnan(left)) return right;
    return std::fabs(left) < std::fabs(right) ? left : right;
}

namespace {

// Radix-2 complex FFT in double precision for SIGCOR, which ports a
// double-precision MATLAB reference (the shared float FFT would put a
// noise floor about 140 dB under the peak). Forward: X[k] = sum x[t]
// e^{-2 pi i k t / N}, as MATLAB's fft; inverse divides by N.
class DoubleFft {
public:
    explicit DoubleFft(size_t n) : n_(n), twiddle_(n / 2) {
        const double twoPi = 2.0 * std::acos(-1.0);
        for (size_t k = 0; k < n / 2; ++k) {
            twiddle_[k] = std::polar(1.0, -twoPi * double(k) / double(n));
        }
    }
    void forward(std::vector<std::complex<double>>& a) const { run(a, false); }
    void inverse(std::vector<std::complex<double>>& a) const {
        run(a, true);
        const double inv = 1.0 / double(n_);
        for (auto& v : a) v *= inv;
    }

private:
    void run(std::vector<std::complex<double>>& a, bool inverse) const {
        for (size_t i = 1, j = 0; i < n_; ++i) {
            size_t bit = n_ >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t halfLen = len / 2, step = n_ / len;
            for (size_t i = 0; i < n_; i += len) {
                for (size_t k = 0; k < halfLen; ++k) {
                    std::complex<double> w = twiddle_[k * step];
                    if (inverse) w = std::conj(w);
                    const std::complex<double> u = a[i + k], v = a[i + k + halfLen] * w;
                    a[i + k] = u + v;
                    a[i + k + halfLen] = u - v;
                }
            }
        }
    }
    size_t n_;
    std::vector<std::complex<double>> twiddle_;
};

// Modified Bessel function of the first kind, order 0 (power series).
double besselI0(double x) {
    const double q = 0.25 * x * x;
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 1000; ++k) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// MATLAB eps(x) for x >= 1: the spacing of doubles at x.
double epsAt(double x) { return std::nextafter(x, std::numeric_limits<double>::infinity()) - x; }

} // namespace

std::vector<double> kaiserWindow(int n, double beta) {
    if (n <= 0) return {};
    if (n == 1) return {1.0};
    std::vector<double> w(static_cast<size_t>(n));
    const double norm = besselI0(beta);
    for (int k = 0; k < n; ++k) {
        const double q = 2.0 * double(k) / double(n - 1) - 1.0;
        w[size_t(k)] = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - q * q))) / norm;
    }
    return w;
}

std::vector<int> sigcorSkipDraws(int64_t count, int nskip, int iransk, uint32_t seed) {
    std::vector<int> draws(size_t(std::max<int64_t>(0, count - 1)), nskip);
    if (iransk <= 0) return draws;
    std::mt19937 rng(seed);
    const int lo = nskip - iransk, span = 2 * iransk + 1;
    for (int& skip : draws) {
        // 53-bit uniform double in [0, 1) from two 32-bit outputs (genrand_res53).
        const double a = double(rng() >> 5), b = double(rng() >> 6);
        const double u = (a * 67108864.0 + b) / 9007199254740992.0;
        skip = lo + std::min(span - 1, int(u * span));
    }
    return draws;
}

SigcorResult computeSigcor(const SigcorTraceFetch& fetch, int samplesPerTrace, int64_t traceStart, int64_t traceEnd,
                           int gateStart, int gateEnd, double sampleIntervalUs, const SigcorParams& params,
                           const std::function<void(int, int)>& progress) {
    SigcorResult res;
    traceStart = std::max<int64_t>(0, traceStart);
    traceEnd = std::max(traceStart, traceEnd);
    gateStart = std::clamp(gateStart, 0, std::max(0, samplesPerTrace));
    gateEnd = std::clamp(gateEnd, gateStart, std::max(0, samplesPerTrace));
    const int n = gateEnd - gateStart; // MATLAB nGate
    res.gateSamples = n;
    if (sampleIntervalUs <= 0.0 || !fetch) {
        res.error = "The dataset has no sample interval.";
        return res;
    }
    if (params.nskip < 0 || params.iransk < 0) {
        res.error = "Skipped traces and random variation must not be negative.";
        return res;
    }
    if (params.iransk > params.nskip) {
        res.error = "The random variation (IRANSK) must not exceed the skipped traces (NSKIP).";
        return res;
    }
    const double dtMs = sampleIntervalUs / 1000.0;
    const int sh = int(std::lround(params.shmaxMs / dtMs)); // MATLAB round(SHMax/dt)
    res.shmaxSamples = sh;
    if (sh < 1) {
        res.error = "Max shift must cover at least one sample.";
        return res;
    }
    if (2 * sh + 3 > n) {
        res.error = "The time gate is too short for the max shift: it needs at least " + std::to_string(2 * sh + 3) +
                    " samples (2 x max shift + 3).";
        return res;
    }
    const bool autcor = params.option == SigcorOption::Autcor;
    // SPECTR: MATLAB's default NFFT = 2^nextpow2(nGate). AUTCOR: lags up to
    // n - 1 need a transform of at least 2n - 1.
    const size_t fftLen = autcor ? nextPowerOfTwo(size_t(2 * n)) : nextPowerOfTwo(size_t(n));
    const size_t half = fftLen / 2; // nFreq = half + 1
    res.fftLength = int(fftLen);

    // Streaming pair selection: one draw per current trace 2..nTraces, an
    // earlier partner only when the history reaches back far enough.
    const int64_t nTraces = traceEnd - traceStart;
    const std::vector<int> draws = sigcorSkipDraws(nTraces, params.nskip, params.iransk, params.seed);
    std::vector<SigcorPair> candidates;
    candidates.reserve(draws.size());
    for (int64_t c = 1; c < nTraces; ++c) {
        const int skip = draws[size_t(c - 1)];
        const int64_t earlier = c - (skip + 1);
        if (earlier < 0) continue;
        SigcorPair pair;
        pair.earlierTrace = traceStart + earlier;
        pair.currentTrace = traceStart + c;
        pair.actualSkip = skip;
        candidates.push_back(pair);
    }
    res.pairsCandidate = int(candidates.size());
    const size_t maxPairs = size_t(std::max(1, params.maxPairs));
    if (candidates.size() > maxPairs) {
        // An even subset (bounded work on huge views), in trace order.
        for (size_t k = 0; k < maxPairs; ++k) {
            res.pairs.push_back(candidates[size_t((double(k) + 0.5) * double(candidates.size()) / double(maxPairs))]);
        }
    } else {
        res.pairs = std::move(candidates);
    }
    res.pairsTotal = int(res.pairs.size());
    if (res.pairs.empty()) return res;

    const std::vector<double> taper = kaiserWindow(n, params.kaiserBeta);
    const DoubleFft transform(fftLen);
    const size_t bins = autcor ? size_t(n) : half + 1; // accumulated per block: lags or frequencies
    // Fixed-size blocks, summed in order afterwards: the result does not
    // depend on the thread count.
    constexpr size_t kBlock = 32;
    const size_t blockCount = (res.pairs.size() + kBlock - 1) / kBlock;
    std::vector<std::vector<double>> blockTrace(blockCount), blockSignal(blockCount), blockNoise(blockCount);
    std::atomic<int> done{0};
    const int total = res.pairsTotal;

    analysisPool().parallelFor(blockCount, 1, [&](size_t blockBegin, size_t blockEnd) {
        std::vector<float> traceA(static_cast<size_t>(samplesPerTrace)), traceB(static_cast<size_t>(samplesPerTrace));
        std::vector<double> lagValue(size_t(2 * sh + 1));
        std::vector<double> xs(static_cast<size_t>(n)), ys(static_cast<size_t>(n));
        std::vector<std::complex<double>> specX(fftLen), specY(fftLen);
        for (size_t block = blockBegin; block < blockEnd; ++block) {
            std::vector<double>& sumT = blockTrace[block];
            std::vector<double>& sumS = blockSignal[block];
            std::vector<double>& sumN = blockNoise[block];
            sumT.assign(bins, 0.0);
            sumS.assign(bins, 0.0);
            sumN.assign(autcor ? 0 : bins, 0.0);
            const size_t pairEnd = std::min(res.pairs.size(), (block + 1) * kBlock);
            for (size_t pi = block * kBlock; pi < pairEnd; ++pi) {
                SigcorPair& pair = res.pairs[pi];
                if (progress) progress(++done, total);
                fetch(pair.earlierTrace, traceA.data());
                fetch(pair.currentTrace, traceB.data());
                const float* x = traceA.data() + gateStart; // MATLAB xRaw
                const float* y = traceB.data() + gateStart; // MATLAB yRaw

                // findCorrelationPeak: normalised correlation of the
                // overlapping raw gates at every integer lag. Positive lag
                // compares x(1:end-lag) with y(1+lag:end).
                int bestIndex = -1;
                double best = 0.0;
                for (int lag = -sh; lag <= sh; ++lag) {
                    const int xFirst = lag >= 0 ? 0 : -lag;
                    const int yFirst = lag >= 0 ? lag : 0;
                    const int count = n - std::abs(lag);
                    double sxy = 0.0, sxx = 0.0, syy = 0.0;
                    for (int t = 0; t < count; ++t) {
                        const double xv = x[xFirst + t], yv = y[yFirst + t];
                        sxy += xv * yv;
                        sxx += xv * xv;
                        syy += yv * yv;
                    }
                    const double denom = std::sqrt(sxx * syy);
                    const double c = denom > 0.0 ? sxy / denom : std::numeric_limits<double>::quiet_NaN();
                    lagValue[size_t(lag + sh)] = c;
                    if (!std::isnan(c) && (bestIndex < 0 || c > best)) { // MATLAB max: first maximum, NaN omitted
                        best = c;
                        bestIndex = lag + sh;
                    }
                }
                if (bestIndex < 0) {
                    pair.correlation = std::numeric_limits<double>::quiet_NaN();
                    pair.status = SigcorPairStatus::Dead;
                    continue;
                }
                pair.discreteLag = bestIndex - sh;
                pair.parabolicLag = pair.discreteLag;
                pair.correlation = best;
                if (bestIndex == 0 || bestIndex == 2 * sh) {
                    pair.status = SigcorPairStatus::AtShiftLimit;
                    continue;
                }
                const double cm = lagValue[size_t(bestIndex - 1)], c0 = best, cp = lagValue[size_t(bestIndex + 1)];
                const double curvature = cm - 2.0 * c0 + cp;
                if (std::isfinite(curvature) && std::fabs(curvature) > epsAt(std::max(1.0, std::fabs(c0)))) {
                    const double delta = 0.5 * (cm - cp) / curvature;
                    if (std::fabs(delta) <= 1.0) pair.parabolicLag = pair.discreteLag + delta;
                }
                if (!std::isfinite(best) || best < params.ccmin) {
                    pair.status = SigcorPairStatus::LowCorrelation;
                    continue;
                }

                // NIPOL = 1: apply the nearest whole-sample shift (MATLAB
                // round: halves away from zero). The overlap stays at x's
                // own positions in a zero-filled gate, y moved onto it.
                const int lag = int(std::round(pair.parabolicLag));
                pair.appliedLag = lag;
                const int count = n - std::abs(lag);
                const int xFirst = lag >= 0 ? 0 : -lag;
                const int yFirst = lag >= 0 ? lag : 0;
                double meanX = 0.0, meanY = 0.0;
                if (params.removeDc) {
                    for (int t = 0; t < count; ++t) {
                        meanX += x[xFirst + t];
                        meanY += y[yFirst + t];
                    }
                    meanX /= count;
                    meanY /= count;
                }
                double energyX = 0.0, energyY = 0.0;
                for (int t = 0; t < count; ++t) {
                    xs[size_t(t)] = double(x[xFirst + t]) - meanX;
                    ys[size_t(t)] = double(y[yFirst + t]) - meanY;
                    energyX += xs[size_t(t)] * xs[size_t(t)];
                    energyY += ys[size_t(t)] * ys[size_t(t)];
                }
                double scaleX = 1.0, scaleY = 1.0;
                if (params.normalise) { // rmsNormalise: unit RMS, an all-zero overlap stays zero
                    const double rmsX = std::sqrt(energyX / count), rmsY = std::sqrt(energyY / count);
                    scaleX = rmsX > 0.0 ? 1.0 / rmsX : 0.0;
                    scaleY = rmsY > 0.0 ? 1.0 / rmsY : 0.0;
                }
                std::fill(specX.begin(), specX.end(), std::complex<double>(0.0, 0.0));
                std::fill(specY.begin(), specY.end(), std::complex<double>(0.0, 0.0));
                for (int t = 0; t < count; ++t) {
                    const int pos = xFirst + t; // MATLAB idx
                    specX[size_t(pos)] = xs[size_t(t)] * scaleX * taper[size_t(pos)];
                    specY[size_t(pos)] = ys[size_t(t)] * scaleY * taper[size_t(pos)];
                }
                transform.forward(specX);
                transform.forward(specY);
                if (!autcor) {
                    for (size_t m = 0; m <= half; ++m) {
                        const std::complex<double> X = specX[m], Y = specY[m];
                        sumT[m] += 0.5 * (std::norm(X) + std::norm(Y));
                        sumS[m] += (X * std::conj(Y)).real();
                        sumN[m] += 0.5 * std::norm(X - Y); // the non-negative form of T - S
                    }
                } else {
                    // Time domain: trace autocorrelation (r_xx + r_yy) / 2 in
                    // the real part, the symmetrised crosscorrelation (the
                    // signal autocorrelation) in the imaginary part of one
                    // inverse transform -- both are real and even.
                    for (size_t m = 0; m < fftLen; ++m) {
                        const std::complex<double> X = specX[m], Y = specY[m];
                        specX[m] = {0.5 * (std::norm(X) + std::norm(Y)), (X * std::conj(Y)).real()};
                    }
                    transform.inverse(specX);
                    for (int k = 0; k < n; ++k) {
                        sumT[size_t(k)] += specX[size_t(k)].real();
                        sumS[size_t(k)] += specX[size_t(k)].imag();
                    }
                }
                pair.status = SigcorPairStatus::Accepted;
            }
        }
    });

    std::vector<double> sumT(bins, 0.0), sumS(bins, 0.0), sumN(autcor ? 0 : bins, 0.0);
    for (size_t b = 0; b < blockCount; ++b) {
        for (size_t m = 0; m < bins; ++m) {
            sumT[m] += blockTrace[b][m];
            sumS[m] += blockSignal[b][m];
            if (!autcor) sumN[m] += blockNoise[b][m];
        }
    }
    std::vector<double> lags;
    for (const SigcorPair& pair : res.pairs) {
        switch (pair.status) {
        case SigcorPairStatus::Accepted:
            ++res.pairsUsed;
            lags.push_back(pair.parabolicLag * dtMs);
            break;
        case SigcorPairStatus::AtShiftLimit: ++res.pairsAtShiftLimit; break;
        case SigcorPairStatus::LowCorrelation: ++res.pairsLowCorrelation; break;
        case SigcorPairStatus::Dead: ++res.pairsDead; break;
        }
    }
    if (res.pairsUsed == 0) return res;

    const double invUsed = 1.0 / double(res.pairsUsed);
    res.tracePower.assign(half + 1, 0.0);
    res.signalPower.assign(half + 1, 0.0);
    res.noisePower.assign(half + 1, 0.0);
    if (!autcor) {
        for (size_t m = 0; m <= half; ++m) {
            res.tracePower[m] = sumT[m] * invUsed;
            res.signalPower[m] = sumS[m] * invUsed;
            res.noisePower[m] = sumN[m] * invUsed;
        }
    } else {
        // Averaged correlations -> power spectra: transform the even
        // sequences r[-(n-1)..n-1] (trace in real, signal in imaginary).
        std::vector<std::complex<double>> corr(fftLen, std::complex<double>(0.0, 0.0));
        for (int k = 0; k < n; ++k) {
            const std::complex<double> v(sumT[size_t(k)] * invUsed, sumS[size_t(k)] * invUsed);
            corr[size_t(k)] = v;
            if (k > 0) corr[fftLen - size_t(k)] = v;
        }
        transform.forward(corr);
        for (size_t m = 0; m <= half; ++m) {
            res.tracePower[m] = corr[m].real();
            res.signalPower[m] = corr[m].imag();
            // T - S is the mean of |X - Y|^2 / 2 here too; only roundoff
            // can take it below zero.
            res.noisePower[m] = std::max(0.0, res.tracePower[m] - res.signalPower[m]);
        }
    }
    res.frequencyHz.resize(half + 1);
    const double dtSeconds = sampleIntervalUs / 1e6;
    for (size_t m = 0; m <= half; ++m) res.frequencyHz[m] = float(double(m) / (double(fftLen) * dtSeconds));

    double sum = 0.0;
    for (double s : lags) sum += s;
    res.shiftMeanMs = sum / double(lags.size());
    double var = 0.0;
    for (double s : lags) var += (s - res.shiftMeanMs) * (s - res.shiftMeanMs);
    res.shiftSigmaMs = lags.size() > 1 ? std::sqrt(var / double(lags.size() - 1)) : 0.0;
    return res;
}

} // namespace segy
