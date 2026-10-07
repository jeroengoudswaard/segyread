// Dev utility: writes a synthetic SEG-Y file to disk (a few overlapping
// sinusoidal "reflectors" plus a wiggle term) for manually exercising
// segyviewer.exe end-to-end without needing a real data file.
// Usage: gen_fixture.exe [output.sgy] [traceCount] [samplesPerTrace]
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "synthetic_segy.h"

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "fixture.sgy";
    int64_t traceCount = argc > 2 ? std::atoll(argv[2]) : 4000;
    int samplesPerTrace = argc > 3 ? std::atoi(argv[3]) : 1500;
    auto f = segy::test::makeSyntheticSegy(traceCount, samplesPerTrace, segy::kIbmFloat32,
        [](int64_t t, int s) {
            // A few overlapping synthetic "reflectors" plus noise-ish wiggle,
            // enough to make the density plot visually distinguishable.
            double x = double(t);
            double y = double(s);
            double v = 0.0;
            v += 800.0 * std::sin((y - (200 + 80 * std::sin(x * 0.01))) * 0.05) * std::exp(-std::pow((y - (200 + 80 * std::sin(x * 0.01))) / 40.0, 2));
            v += 500.0 * std::sin((y - (700 + 150 * std::sin(x * 0.006 + 1.0))) * 0.04) * std::exp(-std::pow((y - (700 + 150 * std::sin(x * 0.006 + 1.0))) / 60.0, 2));
            v += 40.0 * std::sin(x * 0.7 + y * 0.3);
            return v;
        });
    FILE* out = std::fopen(path, "wb");
    if (!out) { std::printf("failed to open %s for write\n", path); return 1; }
    std::fwrite(f.bytes.data(), 1, f.bytes.size(), out);
    std::fclose(out);
    std::printf("wrote %s: %zu bytes, %lld traces x %d samples\n", path, f.bytes.size(), (long long)traceCount, samplesPerTrace);
    return 0;
}
