// Wiggle-display geometry: pure computation, no GUI-toolkit dependency
// (mirrors chrome.h's split -- this answers "where do the lines go," the Qt
// shell answers "how do I draw that"). Deliberately no smoothing/
// interpolation, unlike renderer.cpp's variable-density path: a wiggle plot
// is straight line segments between actual sample values.
//
// Performance follows the same rule as the multi-resolution pyramid: cost
// is bounded by *screen pixels*, not file size, never by re-scanning raw
// data proportional to trace/sample count. Two independent decimations
// achieve that:
//   - trace axis: draw at most plotWidth/kMinPixelsPerTrace traces, evenly
//     spaced -- a file with 100,000 traces zoomed out fully still only
//     decodes a few hundred.
//   - sample axis: one vertex per raw sample when not much vertical
//     oversampling, or a min/max envelope per output pixel row otherwise
//     (same reason the pyramid keeps min/max per block, not just mean).
// Net worst case is O(plotWidth x plotHeight), independent of zoom level or
// file size -- wiggle mode never needs the pyramid at all, since the
// bounded point counts above are always cheap to exact-decode directly.
#pragma once
#include <cstdint>
#include <vector>
#include "renderer.h"

namespace segy {

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct WiggleTrace {
    float baselineX = 0.0f;      // plot-local pixel X of this trace's zero line
    std::vector<Point> linePoints; // plot-local pixel coords, increasing sample order
};

struct WiggleLayout {
    std::vector<WiggleTrace> traces;
};

// Minimum horizontal spacing (in pixels) enforced between adjacent selected
// trace baselines. A future Preferences "trace density" control would make
// this tunable; a fixed constant is enough for now.
constexpr int kMinPixelsPerTrace = 3;

// Computes wiggle-line geometry for the traces visible in `view`, in
// plot-local pixel coordinates (0,0 = top-left of the plotWidth x
// plotHeight plot area). See file comment above for the decimation rules.
// `gainDb` is applied on top of the pyramid's auto-computed global min/max
// clip -- the same amplitude-sensitivity gain the variable-density path
// uses (renderer.h), so switching display modes doesn't change how
// "loud" the data looks. `reversePolarity` flips every sample's sign
// before it's mapped to a pixel offset -- see DisplaySettings::
// reversePolarity (chrome.h), the same flag the Variable Density path reads.
WiggleLayout computeWiggleLayout(const RenderContext& ctx, const ViewRange& view, int plotWidth, int plotHeight,
                                  double gainDb = 0.0, bool reversePolarity = false);

} // namespace segy
