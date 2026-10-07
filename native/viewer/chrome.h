// Plot "chrome": the time scale (both sides), color bar, and constant-time
// grid lines drawn around/over the density plot. Shared between platforms
// wherever the work is pure pixel arithmetic (color bar fill, grid lines,
// layout math, "nice" tick interval selection); only actually drawing the
// *rotated* axis text is platform-native (see native/README.md for why --
// short version: real rotated, anti-aliased text needs GDI's rotated font
// on Windows or FLTK's fl_draw(angle,...) on Linux, and hand-rolling a
// portable rotated font would look worse than either, which matters when
// "cleanliness of interface" is the top priority for this feature).
//
// Toggling: DisplaySettings is the single on/off switch for each element,
// read fresh every frame -- there is deliberately no caching of "is this
// visible" anywhere else. That's what makes this Preferences-ready: a
// future Edit > Preferences dialog only ever needs to flip these booleans
// and request a redraw; it doesn't need to know anything about layout.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include "colormap.h"
#include "renderer.h"

namespace segy {

enum class SeismicDisplayMode { VariableDensity, Wiggle };

struct DisplaySettings {
    bool showTimeScale = true;
    bool showColorBar = true;
    bool showGridLines = true;
    SeismicDisplayMode seismicMode = SeismicDisplayMode::VariableDensity;
    bool wiggleInfill = true;
    // Mirrors the display left-right. Entirely a Qt-shell concern (like
    // wiggleInfill above) -- renderer.cpp/chrome.cpp render the normal,
    // unmirrored raster/geometry, and the Qt shell (viewer_qt.cpp) mirrors
    // the finished pixels/wiggle coordinates and un-mirrors screen<->data
    // coordinate conversion (mouse hover, box select, zoom-at-cursor, pan)
    // to match, rather than teaching the shared render paths about it --
    // keeps this out of the tested core entirely.
    bool flipHorizontal = false;
    // Applied on top of the pyramid's auto-computed (or manualClip's, below)
    // clip -- 0 dB means "trust that base clip as-is."
    double gainDb = 0.0;
    // Right-click the amplitude scale legend to change this -- see
    // colormap.h's ColorScale/applyColorScale.
    ColorScale colorScale = ColorScale::RedWhiteBlue;
    // Right-click the amplitude scale legend > Clip... -- see
    // renderer.h's ManualClip for what posMagnitude/negMagnitude mean and
    // how gainDb still composes with it.
    ManualClip manualClip;
    // Right-click the amplitude scale legend > Lock Scale -- reserved for a
    // future reprocessing/data-change feature to check before silently
    // re-deriving the auto clip out from under a manual one; not enforced
    // anywhere yet since nothing currently changes a loaded file's data.
    bool clipLocked = false;
    // Display Parameters' "Reverse Polarity" -- flips the sign of every
    // displayed sample. Lives here (not the Qt shell's per-mode wiggle
    // settings) because it applies identically to both Variable Density
    // and Wiggle, both of which read DisplaySettings fresh every frame.
    bool reversePolarity = false;
};

struct TimeTick {
    int pixelY = 0;      // Y within the canvas's "below the status bar" area
    std::string label;   // formatted tick value in the chosen display unit, e.g. "1000"
};

struct ChromeLayout {
    int plotX = 0, plotWidth = 0;
    int colorBarX = 0, colorBarWidth = 0;
    // Left side, left to right: [title column][columnGap][numbers column][columnGap] then plotX.
    // Right side, left to right: rightScaleX, [columnGap][numbers column][columnGap][title column].
    int leftScaleX = 0, leftScaleWidth = 0;
    int rightScaleX = 0, rightScaleWidth = 0;
    int titleColumnWidth = 0;  // width reserved for the rotated "Time (ms)" title, each side
    int numbersColumnWidth = 0;
    int columnGap = 0;
    std::vector<TimeTick> ticks;  // shared by grid lines and both scale columns
    std::string axisTitle;        // e.g. "Time (ms)", "Time (\xC2\xB5s)" (UTF-8 micro sign), "Time (s)"
    double lastRenderMs = 0.0;
};

// The platform provides these because measuring rendered text width needs a
// live font context (a selected HFONT on Windows, fl_font() on Linux).
struct TextMetrics {
    std::function<int(const std::string&)> measureWidth; // pixel width, unrotated, in the axis font
    int fontHeightPx = 12; // full font height (ascent+descent) of the axis font
};

// Fills canvasPixels (canvasWidth x canvasHeight, row-major, packed
// 0x00RRGGBB -- the same layout renderFrame() uses) with the density plot,
// the color bar, and grid lines: everything here is pure pixel
// manipulation, so it's identical on both platforms. Returns the computed
// layout, including the tick list and axis title text, so the caller can
// draw the rotated labels with its own native text API.
ChromeLayout renderFrameWithChrome(const RenderContext& ctx, const ViewRange& view,
                                    const DisplaySettings& settings, const TextMetrics& metrics,
                                    uint32_t* canvasPixels, int canvasWidth, int canvasHeight);

} // namespace segy
