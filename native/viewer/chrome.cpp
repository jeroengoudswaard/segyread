#include "chrome.h"
#include "axis_ticks.h"
#include "colormap.h"

#include <cstring>
#include <algorithm>

namespace segy {

namespace {

struct TimeUnit {
    const char* suffix;
    double msPerUnit; // 1 unit = this many ms
};

// Defaults to ms; only deviates when ms would make the numbers "ridiculous"
// -- so large the tick values need 7+ digits (switch to seconds), or so
// small the whole record is under 1ms (switch to microseconds, since every
// value would otherwise render as "0.0..." something).
TimeUnit chooseTimeUnit(double totalRecordMs) {
    if (totalRecordMs >= 1'000'000.0) return {"s", 1000.0};
    if (totalRecordMs < 1.0) return {"\xC2\xB5s", 0.001}; // UTF-8 for U+00B5 MICRO SIGN
    return {"ms", 1.0};
}

} // namespace

ChromeLayout renderFrameWithChrome(const RenderContext& ctx, const ViewRange& view,
                                    const DisplaySettings& settings, const TextMetrics& metrics,
                                    uint32_t* canvasPixels, int canvasWidth, int canvasHeight) {
    ChromeLayout result;
    if (canvasWidth <= 0 || canvasHeight <= 0 || !canvasPixels) return result;

    double sampleIntervalMs = ctx.binHeader.sampleIntervalUs / 1000.0;
    double totalRecordMs = double(ctx.binHeader.samplesPerTrace) * sampleIntervalMs;
    TimeUnit unit = chooseTimeUnit(totalRecordMs);
    result.axisTitle = std::string("Time (") + unit.suffix + ")";

    // Color bar encodes amplitude-as-color, which wiggle mode doesn't use --
    // showing it next to a black-and-white wiggle plot would be confusing
    // clutter, so it's auto-hidden regardless of the user's showColorBar
    // setting (which is untouched and still respected in variable-density
    // mode; this is just mode-appropriate display, not a setting change).
    bool effectiveShowColorBar = settings.showColorBar && settings.seismicMode == SeismicDisplayMode::VariableDensity;

    // --- Margins ---
    const int kColorBarWidth = effectiveShowColorBar ? 16 : 0;
    const int kGap = 3;
    int scaleWidth = 0;
    int titleColW = 0;
    int numbersColW = 0;
    if (settings.showTimeScale) {
        // Rotated text: the space a rotated string needs *across* the axis
        // is its unrotated font height (plus a little slack for glyphs
        // wider than the nominal cell), not its length -- the length runs
        // *along* the axis instead. So each column (numbers, title) is one
        // font-height wide, regardless of how long the longest label is.
        titleColW = metrics.fontHeightPx + 2;
        numbersColW = metrics.fontHeightPx + 2;
        scaleWidth = titleColW + kGap + numbersColW + kGap;
    }

    result.colorBarX = 0;
    result.colorBarWidth = kColorBarWidth;
    result.leftScaleX = kColorBarWidth;
    result.leftScaleWidth = scaleWidth;
    result.titleColumnWidth = titleColW;
    result.numbersColumnWidth = numbersColW;
    result.columnGap = kGap;
    result.plotX = kColorBarWidth + scaleWidth;
    result.rightScaleWidth = scaleWidth;
    result.rightScaleX = canvasWidth - scaleWidth;
    result.plotWidth = std::max(0, result.rightScaleX - result.plotX);

    // --- Density plot, rendered into a scratch buffer then copied into
    // place -- skipped entirely in wiggle mode (the caller draws wiggle
    // traces itself, on top; no point spending time on a raster render
    // that would just be covered up). The plot area is left white, the
    // conventional wiggle-plot background, instead.
    if (result.plotWidth > 0) {
        static std::vector<uint32_t> scratch; // reused across frames; single UI thread only
        scratch.assign(size_t(result.plotWidth) * size_t(canvasHeight), 0);
        if (settings.seismicMode == SeismicDisplayMode::VariableDensity) {
            result.lastRenderMs =
                renderFrame(ctx, view, scratch.data(), result.plotWidth, canvasHeight, settings.gainDb,
                            settings.colorScale, settings.manualClip, settings.reversePolarity);
        } else {
            std::fill(scratch.begin(), scratch.end(), 0x00FFFFFFu);
        }
        for (int y = 0; y < canvasHeight; ++y) {
            std::memcpy(canvasPixels + size_t(y) * size_t(canvasWidth) + size_t(result.plotX),
                        scratch.data() + size_t(y) * size_t(result.plotWidth),
                        size_t(result.plotWidth) * sizeof(uint32_t));
        }
    }

    // --- Color bar: vertical gradient, positive (red) at top, negative (blue) at bottom ---
    if (effectiveShowColorBar) {
        for (int y = 0; y < canvasHeight; ++y) {
            float t = 1.0f - 2.0f * float(y) / float(std::max(1, canvasHeight - 1));
            uint8_t r, g, b;
            applyColorScale(settings.colorScale, t, &b, &g, &r);
            uint32_t px = (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
            uint32_t* row = canvasPixels + size_t(y) * size_t(canvasWidth);
            for (int x = 0; x < kColorBarWidth; ++x) row[x] = px;
        }
    }

    // --- Ticks: nice interval, <= 11 lines, coarsened just enough that
    // rotated labels don't overlap along the axis -- see axis_ticks.h,
    // shared with every other numeric axis in the app that wants the same
    // regular, non-overlapping behavior (Histogram/Spectrum's plots).
    if (result.plotWidth > 0 && canvasHeight > 0 && sampleIntervalMs > 0.0) {
        double startUnit = view.sampleStart * sampleIntervalMs / unit.msPerUnit;
        double endUnit = view.sampleEnd * sampleIntervalMs / unit.msPerUnit;
        constexpr int kMaxLines = 11;
        std::vector<AxisTick> axisTicks =
            computeNiceAxisTicks(startUnit, endUnit, canvasHeight, kMaxLines, metrics.measureWidth);
        result.ticks.reserve(axisTicks.size());
        for (const AxisTick& tk : axisTicks) result.ticks.push_back({tk.pixelPos, tk.label});
    }

    // --- Grid lines: blended directly into the plot region so the
    // underlying data stays faintly visible underneath. ---
    if (settings.showGridLines) {
        constexpr uint8_t kGr = 0x20, kGg = 0x20, kGb = 0x20;
        for (const TimeTick& tk : result.ticks) {
            if (tk.pixelY < 0 || tk.pixelY >= canvasHeight) continue;
            uint32_t* row = canvasPixels + size_t(tk.pixelY) * size_t(canvasWidth) + size_t(result.plotX);
            for (int x = 0; x < result.plotWidth; ++x) {
                uint32_t src = row[x];
                uint8_t sr = uint8_t(src >> 16), sg = uint8_t(src >> 8), sb = uint8_t(src);
                row[x] = (uint32_t((sr + kGr) / 2) << 16) | (uint32_t((sg + kGg) / 2) << 8) | uint32_t((sb + kGb) / 2);
            }
        }
    }

    return result;
}

} // namespace segy
