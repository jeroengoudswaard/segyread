// Colormaps for signed seismic amplitude ("variable density" display).
// `divergingColormap` (red-white-blue) is the original/default; the others
// were added so the user can right-click the amplitude scale legend and
// pick a different one -- see AppState::display.colorScale and
// applyColorScale() below, which every rendering call site should go
// through instead of calling one of these directly.
#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace segy {

inline uint8_t clampByte(float v) {
    return uint8_t(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}

// `t` in [-1, 1]. Returns a packed 0x00RRGGBB (top byte unused) suitable for
// writing into a 32bpp BGRA/BGRX DIB after byte-order rearrangement by the
// caller -- same contract for every colormap function below.
inline void divergingColormap(float t, uint8_t* outB, uint8_t* outG, uint8_t* outR) {
    t = std::clamp(t, -1.0f, 1.0f);
    float r, g, b;
    if (t < 0.0f) {
        // blue -> white
        float u = 1.0f + t; // 0..1
        r = u; g = u; b = 1.0f;
    } else {
        // white -> red
        float u = 1.0f - t; // 1..0
        r = 1.0f; g = u; b = u;
    }
    *outR = clampByte(r * 255.0f);
    *outG = clampByte(g * 255.0f);
    *outB = clampByte(b * 255.0f);
}

// Same red/white "positive" half as divergingColormap, but black instead of
// blue for the negative half -- a diverging scale, not sequential (zero
// still renders white, matching the other three and matching how "0" reads
// on the amplitude scale legend).
inline void redWhiteBlackColormap(float t, uint8_t* outB, uint8_t* outG, uint8_t* outR) {
    t = std::clamp(t, -1.0f, 1.0f);
    float r, g, b;
    if (t < 0.0f) {
        float u = 1.0f + t; // black(0) -> white(1) as t goes -1..0
        r = u; g = u; b = u;
    } else {
        float u = 1.0f - t; // white(1) -> red as t goes 0..1
        r = 1.0f; g = u; b = u;
    }
    *outR = clampByte(r * 255.0f);
    *outG = clampByte(g * 255.0f);
    *outB = clampByte(b * 255.0f);
}

// Plain linear grayscale ramps across the whole [-1, 1] range (not
// diverging around a white zero, unlike the two colormaps above) -- the
// classic "SEG normal"/"SEG reverse" polarity-flip convention for a
// grayscale variable-density display.
inline void grayscaleWhiteToBlackColormap(float t, uint8_t* outB, uint8_t* outG, uint8_t* outR) {
    t = std::clamp(t, -1.0f, 1.0f);
    float u = (t + 1.0f) * 0.5f;      // 0 at t=-1, 1 at t=+1
    uint8_t gray = clampByte((1.0f - u) * 255.0f); // white at t=-1, black at t=+1
    *outR = *outG = *outB = gray;
}

inline void grayscaleBlackToWhiteColormap(float t, uint8_t* outB, uint8_t* outG, uint8_t* outR) {
    t = std::clamp(t, -1.0f, 1.0f);
    float u = (t + 1.0f) * 0.5f;      // 0 at t=-1, 1 at t=+1
    uint8_t gray = clampByte(u * 255.0f); // black at t=-1, white at t=+1
    *outR = *outG = *outB = gray;
}

// Linearly interpolates through an ordered list of RGB stops (each
// component 0..1) evenly spaced across [-1, 1] -- for a palette with more
// than two "arms," where the two-sided diverging formulas above no longer
// fit. `stops` must have at least 2 entries.
inline void multiStopColormap(float t, const float (*stops)[3], int stopCount, uint8_t* outB, uint8_t* outG,
                               uint8_t* outR) {
    t = std::clamp(t, -1.0f, 1.0f);
    float u = (t + 1.0f) * 0.5f * float(stopCount - 1); // 0..(stopCount-1)
    int i0 = std::clamp(int(std::floor(u)), 0, stopCount - 2);
    int i1 = i0 + 1;
    float frac = u - float(i0);
    float r = stops[i0][0] + (stops[i1][0] - stops[i0][0]) * frac;
    float g = stops[i0][1] + (stops[i1][1] - stops[i0][1]) * frac;
    float b = stops[i0][2] + (stops[i1][2] - stops[i0][2]) * frac;
    *outR = clampByte(r * 255.0f);
    *outG = clampByte(g * 255.0f);
    *outB = clampByte(b * 255.0f);
}

// Yellow (most negative) -> Red -> White (zero) -> Black -> Cyan (most
// positive), 5 evenly-spaced hues -- same "diverging, white at zero"
// convention as the two-color scales above, just with two extra stops on
// each arm instead of one.
inline void yellowRedWhiteBlackCyanColormap(float t, uint8_t* outB, uint8_t* outG, uint8_t* outR) {
    static const float kStops[5][3] = {
        {1.0f, 1.0f, 0.0f}, // yellow
        {1.0f, 0.0f, 0.0f}, // red
        {1.0f, 1.0f, 1.0f}, // white
        {0.0f, 0.0f, 0.0f}, // black
        {0.0f, 1.0f, 1.0f}, // cyan
    };
    multiStopColormap(t, kStops, 5, outB, outG, outR);
}

// AppState::display.colorScale (chrome.h's DisplaySettings) -- read fresh
// every frame, same convention as every other display toggle, so switching
// it just needs a repaint, not a re-render pipeline change.
enum class ColorScale {
    RedWhiteBlue,
    RedWhiteBlack,
    GrayscaleWhiteToBlack,
    GrayscaleBlackToWhite,
    YellowRedWhiteBlackCyan,
};

inline void applyColorScale(ColorScale scale, float t, uint8_t* outB, uint8_t* outG, uint8_t* outR) {
    switch (scale) {
        case ColorScale::RedWhiteBlack: redWhiteBlackColormap(t, outB, outG, outR); return;
        case ColorScale::GrayscaleWhiteToBlack: grayscaleWhiteToBlackColormap(t, outB, outG, outR); return;
        case ColorScale::GrayscaleBlackToWhite: grayscaleBlackToWhiteColormap(t, outB, outG, outR); return;
        case ColorScale::YellowRedWhiteBlackCyan: yellowRedWhiteBlackCyanColormap(t, outB, outG, outR); return;
        case ColorScale::RedWhiteBlue: default: divergingColormap(t, outB, outG, outR); return;
    }
}

} // namespace segy
