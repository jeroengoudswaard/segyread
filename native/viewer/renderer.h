// Platform-independent view state and rendering logic, shared by the Win32
// and Linux (X11) shells. Nothing in this file touches a windowing API --
// it only knows about the pyramid, the memory-mapped file, and a raw pixel
// buffer, so it is exercised identically (and can eventually be unit
// tested) on both platforms. See win32/viewer_win32.cpp and
// linux/viewer_x11.cpp for the thin per-platform shells that own a window,
// pump events, and call into this.
#pragma once
#include <cstdint>
#include "colormap.h"
#include "segy_format.h"
#include "segy_pyramid.h"
#include "segy_mmap.h"
#include "threadpool.h"

namespace segy {

struct ViewRange {
    double traceStart = 0, traceEnd = 0;
    double sampleStart = 0, sampleEnd = 0;
};

// Everything renderFrame() needs; owned by the platform shell's app state,
// passed in by reference so this header never owns platform resources.
struct RenderContext {
    const MappedFile* file = nullptr;
    const Pyramid* pyramid = nullptr;
    BinaryHeader binHeader{};
    int64_t traceCount = 0;
    size_t traceStrideBytes = 0;
    ThreadPool* pool = nullptr;
};

// Caps how many samples renderExactPath decodes for one frame. When the
// visible trace range would need more than this many samples decoded at
// full density, traces are decimated (every Nth one decoded, at full
// per-trace vertical resolution) rather than substituting coarser
// pyramid-block statistics -- see native/README.md, "Zoomed-out rendering."
// Chosen so a worst-case decode (~8M samples, ~32MB through the IBM-float
// AVX2 path) stays a few milliseconds even single-threaded -- see
// native/README.md benchmarks.
constexpr double kExactDecodeSampleBudget = 8'000'000.0;

// Overrides the pyramid-auto-computed global min/max clip with an explicit
// one, independently for positive and negative excursions -- set from the
// amplitude scale legend's right-click "Clip..." dialog (viewer_qt.cpp).
// `posMagnitude`/`negMagnitude` are always positive numbers: a sample at
// exactly +posMagnitude (or -negMagnitude) maps to the color scale's +1 (or
// -1) endpoint. Both still pass through gainDb the same as the
// auto-computed clip would (see clipForGain in renderer.cpp) -- this
// replaces the *base* clip, not the whole gain pipeline, so the toolbar
// gain slider keeps working identically after a manual clip is set.
// Symmetric (posMagnitude == negMagnitude) reproduces today's plain-gain
// behavior exactly; `enabled = false` is the default, unmanaged case.
struct ManualClip {
    bool enabled = false;
    float posMagnitude = 1.0f;
    float negMagnitude = 1.0f;
};

void resetView(ViewRange& view, int64_t traceCount, int samplesPerTrace);
void clampView(ViewRange& view, int64_t traceCount, int samplesPerTrace);

// Adjusts `view` to zoom by `factor` (<1 = zoom in, >1 = zoom out) centered
// on client-area pixel (px, py) within a `width` x `height` viewport.
void zoomAt(ViewRange& view, int64_t traceCount, int samplesPerTrace, int width, int height,
            int px, int py, double factor);

// Shifts `view` by a drag of (dxPixels, dyPixels) measured from `anchor`,
// clamped to the file bounds. Platform shells call this on every
// WM_MOUSEMOVE / MotionNotify while dragging, passing the view captured at
// drag-start as `anchor` so repeated small moves don't accumulate error.
void panFromAnchor(ViewRange& view, const ViewRange& anchor, int64_t traceCount, int samplesPerTrace,
                    int width, int height, int dxPixels, int dyPixels);

// Renders `ctx`'s data for `view` into `pixels` (row-major, width*height,
// packed 0x00RRGGBB -- i.e. bytes [B,G,R,0] in memory on a little-endian
// machine, which is what both GDI's BI_RGB and a 32-bit X11 TrueColor
// visual expect). Always decodes real trace samples at full vertical
// resolution; decimates which traces it decodes (rather than averaging
// blocks of them) once the visible range is too wide to decode every one
// within kExactDecodeSampleBudget. Returns the render time in milliseconds
// (shown in the status line as a live performance readout).
//
// `gainDb` is applied on top of the pyramid's auto-computed global min/max
// clip (0 = trust auto-scaling as-is); see DisplaySettings::gainDb in
// chrome.h for why it's a multiplier on the clip rather than a separate
// scaling mode.
// `reversePolarity` flips the sign of every displayed sample -- applied
// after gain/clip magnitude is computed (which stays symmetric either way),
// only the color each value maps to changes. Wiggle mode applies the same
// flip to its own amplitude-to-pixel mapping (wiggle.h's computeWiggleLayout)
// so the two display modes never show opposite polarity for the same data.
double renderFrame(const RenderContext& ctx, const ViewRange& view, uint32_t* pixels, int width, int height,
                    double gainDb = 0.0, ColorScale colorScale = ColorScale::RedWhiteBlue,
                    ManualClip manualClip = ManualClip{}, bool reversePolarity = false);

} // namespace segy
