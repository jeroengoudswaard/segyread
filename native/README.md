# SEG-Y native rewrite (Windows + Linux, C++)

A from-scratch, performance-first SEG-Y reading/viewing application,
standalone and maintained for two targets: Windows and Linux, sharing
**one Qt6-based GUI shell** (`viewer/qt/viewer_qt.cpp`) between them.
Priorities, in order: **performance, correctness, maintainability,
simplicity** — every design choice below picks in that order when two of
them conflict.

It trades cross-platform-engine portability for direct platform API + mmap
+ AVX2 access, at the cost of a heavier GUI dependency (Qt6, dynamically
linked) than a from-scratch Win32/Xlib shell would need.

**GUI toolkit history**: this project originally had *two* platform-specific
shells — Win32/GDI on Windows, FLTK+raw-Xlib on Linux. That split was
deliberately replaced by a single Qt6 shell once Linux testing had
surfaced three real, non-trivial FLTK/Xlib bugs (see "GUI toolkit history:
why Qt, in the end" below) against zero equivalent bugs on Windows — a
signal that maintaining two independently-bugged GUI backends cost more
than the dependency weight of unifying on one more mature toolkit. The
historical bug writeups for both the old raw-Xlib and FLTK shells are kept
below since they're genuine, hard-won institutional knowledge (the same
class of bug — e.g. the progress-throttle data race — silently existed on
Windows too, just never triggered), even though the code that had those
bugs no longer exists.

## Why C++

A managed-runtime language has to go through typed-array bounds-checked
accessors, a tracing GC, and (if the decode work runs on a separate
worker/thread with its own heap) message-passing copies to move decoded
samples back to the UI. None of that is necessary for this workload: it's
pure numeric decode of a memory-mapped file, which is exactly what C++ with
direct pointer access, SIMD intrinsics, and OS-level memory mapping is for.
"Standalone portable app" also pointed away from anything needing a bundled
runtime (a managed-language VM, .NET, a Python interpreter) — a native,
statically-linked exe needs nothing but the OS.

## Platform split: shared core + one Qt shell

The codebase is one tree, not two branches-in-the-VCS-sense: portable logic
lives in files compiled on both platforms, and the only thing that
genuinely differs per platform now is how a file gets memory-mapped. The
GUI shell (`viewer/qt/viewer_qt.cpp`) is a single Qt6/C++ source compiled
identically for both targets:

| Concern | Shared? | Windows | Linux |
|---|---|---|---|
| Format parsing, decode, pyramid, thread pool, colormap | ✅ shared | — | — |
| View math + rendering (`viewer/renderer.*`) | ✅ shared | — | — |
| File loading (`viewer/loader.*`) | ✅ shared | — | — |
| Plot chrome layout/pixel math (`viewer/chrome.*`) | ✅ shared | — | — |
| GUI shell (window, menu, dialog, blit, threading) | ✅ shared | `viewer/qt/viewer_qt.cpp` | `viewer/qt/viewer_qt.cpp` |
| Memory-mapped file | interface shared, impl differs | `src/platform/segy_mmap_win32.cpp` (`CreateFileMappingW`) | `src/platform/segy_mmap_posix.cpp` (`mmap`) |
| Build system | ✅ shared | CMake (`CMakeLists.txt`), MSVC + a Qt6 install | CMake (`CMakeLists.txt`), g++ + `qt6-base-dev` |

`build.bat`/`build.sh` are now thin wrappers around the same
`CMakeLists.txt` (`cmake -S . -B <dir> && cmake --build <dir>`) rather than
each hand-invoking a compiler directly — CMake became a hard requirement
once the GUI shell moved to Qt6, since Qt's own tooling (AUTOMOC,
`qt_add_executable`) assumes it. Each script still only builds its own
platform's binaries (a Windows box has no Linux Qt/mmap headers to offer
g++, and vice versa); building the other target means running the other
script on that OS (or, for Linux, under WSL).

**Verification status**: both targets have been built, run, and visually
checked against real rendered output (a synthetic fixture with known
"reflector" shapes), on the current Qt6 shell as well as historically on
the raw-Xlib and FLTK shells it replaced. The bugs below are kept as
historical record — they were found in the **original hand-rolled raw-Xlib
shell** (`viewer_x11.cpp`, since replaced first by FLTK, then by Qt6), once
WSL2 + Ubuntu 24.04 + `build-essential`/`libx11-dev` were available for
Linux testing, and that verification pass genuinely earned its keep: it
found three real bugs that code review alone had missed, all now moot
since the code they were in no longer exists, but the underlying lesson
(especially #3) is worth remembering for any future platform-specific code:

1. **Motion-event flood starving the load-completion signal.**
   `XSelectInput` requested `PointerMotionMask` (every pointer movement over
   the window, button or no button), rather than `ButtonMotionMask` (only
   while dragging). Combined with the event loop draining *all* pending X
   events before ever checking the background loader's wake-pipe, a stream
   of motion events could keep the loader's "file finished loading" signal
   from ever being noticed. Fixed by requesting `ButtonMotionMask` instead,
   only repainting on motion while actually dragging, and bounding the
   event-drain loop so the wake-pipe is checked periodically regardless of
   X event volume.
2. **Missing `XFlush`.** Xlib buffers drawing requests client-side and only
   sends them to the server on a round trip, a full buffer, or an explicit
   flush. `paint()` had none, so the *last* frame drawn before the app went
   idle could sit unsent in the client buffer indefinitely. Fixed by calling
   `XFlush` at the end of `paint()`.
3. **Data race on the progress-throttle timestamp.** `loaderThreadFunc`
   throttled progress callbacks using a plain `std::chrono::time_point`
   mutated from multiple pool worker threads without synchronization —
   undefined behavior. It happened to not visibly misbehave on Windows, but
   it was the same bug there too. Fixed by switching to an
   `std::atomic<int64_t>` millisecond counter — **this exact fix is still
   in `viewer_qt.cpp` today**, carried forward deliberately through both
   the FLTK shell and the Qt migration rather than re-introduced.

None of these were in the shared core (`segy_decode`, `segy_pyramid`,
`renderer.cpp`, `loader.cpp`) — that code passed its full test suite
identically on both MSVC and GCC without changes, and still does. All three
bugs were in the platform-specific GUI shell's event handling and
threading, exactly the kind of thing a test suite over pure logic can't
catch and only running the real thing surfaces. Take that as the argument
for why "written and reviewed" and "verified" are different claims — it's
also part of why the two independently-maintained shells were eventually
unified into one Qt6 shell (see "GUI toolkit history" below).

## What was scoped in vs. out

The original app has tiled on-demand pyramid caching, a spectrum/FFT
dialog, multi-dataset comparison slots, header-range filtering, and a
browser/web file picker path. Reproducing all of that at the same fidelity
in one pass wasn't realistic alongside "performance first, avoid premature
abstraction," so this rewrite covers the performance-critical core end to
end — file I/O, decode, the multi-resolution pyramid, and an interactive
density-plot viewer — and leaves out spectrum analysis, filtering, and
multi-dataset comparison. See "Trade-offs and honest limitations" below.

## Layout

```
native/
  include/        segy_format.h   on-disk layout, byte readers, header parsing (shared)
                  segy_decode.h   sample decode, scalar + AVX2 IBM float (shared)
                  segy_mmap.h     RAII memory-mapped file, interface only (shared)
                  segy_pyramid.h  multi-resolution min/max/mean pyramid (shared)
                  threadpool.h    persistent worker pool + parallelFor (shared)
                  colormap.h      diverging seismic amplitude colormap (shared)
  src/            segy_format.cpp, segy_decode.cpp, segy_pyramid.cpp (shared)
    platform/     segy_mmap_win32.cpp  -- Windows CreateFileMappingW impl
                  segy_mmap_posix.cpp  -- Linux mmap() impl
  viewer/         renderer.h/.cpp -- view math + rendering, variable-density (shared)
                  loader.h/.cpp   -- file open/validate/pyramid build (shared)
                  chrome.h/.cpp   -- time scale, color bar, grid lines: layout + pixel math (shared)
                  wiggle.h/.cpp   -- wiggle-display line/infill geometry (shared, Qt-free -- see "Design choices")
    qt/           viewer_qt.h/.cpp -- Qt6 GUI shell (window, menu bar, file dialog, canvas widget,
                                       rotated axis text via QPainter, background loading) -- shared
                                       by both platforms; app.manifest -- Windows PerMonitorV2 DPI
                                       awareness (see "Design choices")
  bench/          bench_main.cpp  -- microbenchmarks (shared)
  test/           test_main.cpp   -- correctness test harness (shared; also covers chrome.cpp's layout/tick logic)
                  synthetic_segy.h, gen_fixture_main.cpp -- test/dev fixtures (shared)
  CMakeLists.txt  builds segycore, segyviewercore, segyviewer (Qt6), segytest, segybench, gen_fixture
  build.bat       thin wrapper: configures + builds the Windows target via CMake
  build.sh        thin wrapper: configures + builds the Linux target via CMake
```

Build on Windows: install a Qt6 dev environment first (this project used
the vcpkg route: `vcpkg install qt6-base:x64-windows qtsvg:x64-windows` --
`qtsvg` is the vcpkg port name for the `Qt6::Svg` component the toolbar's
real icons need, see "Toolbar icons: from hand-drawn shapes to Tabler
Icons" below; then either set `VCPKG_ROOT` or edit the default path at the
top of `build.bat`) and CMake (`winget install -e --id Kitware.CMake`),
then run `native\build.bat` from a normal cmd/PowerShell prompt. Produces
`native\build-windows\Release\segytest.exe`, `segybench.exe`,
`segyviewer.exe`, and `gen_fixture.exe`, with the required Qt DLLs
(including `Qt6Svg.dll`) copied alongside `segyviewer.exe` automatically
(vcpkg's `VCPKG_APPLOCAL_DEPS`).

Build on Linux: `sudo apt install qt6-base-dev qt6-svg-dev cmake
build-essential`, then run `native/build.sh`. Produces the same four
binaries, without the `.exe` suffix, in `native/build-linux/`. Verified
under WSL2 + Ubuntu 24.04 with g++ 13.3 and Qt 6.4.2; see "Platform split"
above.

## Design choices

### Memory-mapped I/O, not buffered reads

`segy_mmap.h` declares `MappedFile` with a platform-neutral interface
(`open(std::filesystem::path)`, `data()`, `size()`); `segy_mmap_win32.cpp`
implements it with `CreateFileMappingW`/`MapViewOfFile`, `segy_mmap_posix.cpp`
with `open()`/`mmap()`. Either way the whole file becomes a flat
`const uint8_t*`; header parsing and sample decode read straight out of it
with zero copies into intermediate buffers, and the OS page cache — not
application code — handles read-ahead and caching. This also means opening
a file is O(1) regardless of size; there's no upfront "read the whole thing
into a buffer" step.

### Sample decode: hand-derived IBM-float conversion, scalar + AVX2

Format 1 (IBM System/370 hex float) is the historically dominant SEG-Y
sample format and has no hardware support on x86, making it the single
hottest loop in a SEG-Y reader. `segy_decode.cpp` implements it twice:

- **Scalar reference** (`ibmToIeeeScalar`): fully general — handles
  non-normalized mantissas via `_BitScanReverse` rather than assuming a
  bounded shift. This is the correctness oracle the tests and the AVX2
  fixup path both check against.
- **AVX2** (`ibmToIeeeAvx2`): processes 8 samples/instruction. It fast-paths
  the common case (mantissa normalized per the IBM convention: at most 3
  leading zero bits, true for effectively all real seismic data) using
  branchless vector compares and `_mm256_sllv_epi32` for the per-lane
  variable shift. Any lane where that assumption doesn't hold is detected
  via a mask and corrected with the scalar reference afterward — so the
  result is bit-for-bit identical to the scalar path for *any* input, not
  just the common case, with no accuracy trade-off.

**Derivation** (for the curious / for maintaining this code): an IBM float
encodes `value = (-1)^sign * (mantissa / 2^24) * 16^(exponent-64)`. Let
`shift` = the number of leading zero bits in the 24-bit mantissa before its
first set bit. Shifting the mantissa left by `shift` bits makes bit 23 the
implicit leading 1 of an IEEE mantissa; working through the algebra to match
the two representations gives:

```
ieee_biased_exponent = 4 * exponent - shift - 130
```

(worked example in `segy_decode.cpp` and cross-checked in
`testIbmFloatKnownValues`: `0x42100000` → 16.0). Values whose exponent falls
outside `[1, 254]` after this transform over/underflow IEEE754 single
precision (IBM float has a wider theoretical dynamic range) and are flushed
to signed zero/infinity — this can't happen for real trace amplitudes but is
handled explicitly rather than left as undefined bit-twiddling.

Formats 2 (int32), 3 (int16), 5 (IEEE float32), 6 (float64), 7 (int24), 8
(int8) are also implemented, each with its own correct decode rather than
special-casing one format and silently misinterpreting every other code as
raw IEEE float32 bytes. Unsupported/unrecognized format codes are rejected
at load time (`isSampleFormatSupported`) instead of silently decoding
garbage.

### The pyramid: single streaming pass, not a lazy tile cache

`segy_pyramid.h`/`.cpp` builds a stack of block-reduced levels (min/max/mean/
count per block, block size growing by `blockFactor=4` per axis per level)
in one parallel pass at load time, so rendering at any zoom level is an O(1)
array lookup per screen pixel instead of re-scanning raw data.

This is a deliberate simplification versus an on-demand tile cache with LRU
eviction: building the *whole* pyramid up front is simpler code and gives
predictable, jank-free pan/zoom (no cache-miss stalls), at the cost of
assuming the pyramid — not the raw file — fits in RAM. Because each level
shrinks by `blockFactor²=16`, the total pyramid size
is a small fraction of the raw file (geometric series, dominated by the
finest level: for a 10 GB file that's on the order of tens of MB), so this
is a safe trade for desktop-scale SEG-Y files. It would stop being safe for
files large enough that even the finest pyramid level doesn't fit in RAM;
that case isn't handled and would need the lazy-tile-cache design back.

The finest level is built directly from the mmap'd file, parallelized
across trace-block ranges (each worker decodes-and-reduces its own disjoint
trace range — the full decoded file is never materialized, only the much
smaller pyramid is kept resident). Coarser levels reduce the level below
using an **exact, count-weighted mean** (not a mean-of-means), so min/max/
mean are all exactly correct at every level regardless of ragged edges from
trace/sample counts that aren't multiples of the block factor — verified in
`testPyramidCoarseLevelsExact` against brute-force computation over raw
data.

### Rendering: pyramid lookup when zoomed out, on-the-fly decode when zoomed in

`viewer/renderer.cpp` (shared, GUI-toolkit-independent) picks one of two
paths per frame based on how many raw samples are visible:

- **Zoomed out** (visible raw samples > 8M): look up the coarsest pyramid
  level whose block size still resolves to ≤ 1 screen pixel, and colormap
  its mean. Cost is O(screen pixels), independent of file size.
- **Zoomed in** (≤ 8M raw samples visible): decode the visible trace/sample
  window on the fly, in parallel across traces, straight from the mmap. At
  this zoom level the visible window is small by construction, so this
  stays within an interactive frame budget (the 8M-sample cutoff was chosen
  from the decode benchmark below: ~8M samples is single-digit milliseconds
  even on one core).

The cutoff is on total visible raw samples (trace span × sample span), not
on a per-axis ratio, specifically to bound worst-case frame cost regardless
of window aspect ratio.

### Thread pool: persistent workers, not spawn-per-call

`threadpool.h` parks `hardware_concurrency()` worker threads on a condition
variable and hands out flat `parallelFor` ranges. A pyramid build is a
one-shot batch operation where thread-spawn overhead wouldn't matter, but
the interactive re-decode on every pan/zoom frame is latency-sensitive
enough that avoiding OS thread creation 60 times/second is worth the (small)
extra complexity of a persistent pool.

### Menu bar: one Qt `QMenuBar`, inert-by-default surface

Both platforms get the identical conventional File/Edit/Selection/View/Help
menu bar from one `QMenuBar`/`QAction` block in `viewer_qt.cpp`. Only
**File > Open** and **File > Exit** do anything; every other item (Undo,
Redo, Cut, Copy, Paste, Select All, Clear Selection, Zoom In/Out, Reset
View, Fit to Window, About) is a plain `addAction("...")` with nothing
connected to its `triggered` signal — clicking it is a true no-op, not a
disabled/grayed-out item. This is deliberate, not an oversight: the brief
asked for the *shape* of a normal desktop app's menu, not for those
features to exist yet, and present-but-inert reads as "normal Windows
application" rather than "feature list of what's missing." Qt's default
style already renders as a native-looking menu bar on both platforms (the
"windowsvista"-family style on Windows, a Fusion-like style on Linux) with
zero styling code needed.

### GUI toolkit history: why Qt, in the end

This project went through two prior GUI shells before settling on Qt6:

1. **A hand-rolled raw-Xlib shell on Linux, matching Win32/GDI on
   Windows** — direct platform API, no GUI toolkit dependency at all, in
   keeping with the project's original "stay close to the raw platform"
   stance. This is where the three historical bugs in "Platform split"
   above were found (motion-event flood, missing `XFlush`, the
   progress-throttle data race).
2. **FLTK on Linux** (Windows kept native Win32/GDI), adopted specifically
   to get a conventional File/Edit/Selection/View/Help menu bar without
   fighting a toolkit's opinionated look — GTK4 actively steers away from
   the classic dropdown-menu-bar style, and Qt at the time seemed like a
   much bigger rewrite than a menu bar justified. FLTK's default widget
   look was exactly the classic flat menu bar wanted, and it let a
   previously hand-rolled, buggy cross-thread wakeup (a self-pipe +
   `select()`) be replaced by FLTK's own `Fl::awake()`. But real-machine
   testing surfaced three more non-trivial bugs specific to FLTK's Xlib/Xft
   backend, none caught by code review:
   - **Blank canvas**: drawing into `fl_xid(window())` instead of FLTK's
     own `fl_window` current-draw-target global — `Fl_Double_Window`
     renders into an offscreen pixmap during `draw()`, so the wrong target
     produced no error, just nothing on screen.
   - **File > Open silently not opening**: `Fl_Native_File_Chooser`
     `dlopen()`s GTK3 at runtime on Linux; on a real Ubuntu 24.04 machine
     (GTK 3.24.41) that bridge called `gdk_x11_drawable_get_xid`, a GDK
     symbol removed from current GTK entirely. Worked around at the time by
     switching to `fl_file_chooser()` (FLTK's own dialog, no GTK
     dependency) — later made moot entirely by Qt's own `QFileDialog`.
   - **Rotated axis text silently failing after ~7 draws per frame**:
     `fl_draw(int angle, ...)` would render the first one or two rotated
     labels in a frame and then silently stop, confirmed by direct
     position-swap/call-count experiments to be some internal FLTK
     Xlib+Xft state issue, not a bug in this project's code. Worked around
     at the time with a fully manual render-horizontally-then-rotate-pixels
     implementation — since replaced by `QPainter::rotate()`, which is Qt's
     standard, heavily-used idiom for exactly this (see "Plot chrome"
     below).

   Three real, non-trivial bugs from testing Linux specifically (versus
   zero equivalent bugs from the same-depth testing on Windows) was a
   legitimate signal that FLTK's Xlib/Xft backend was less battle-tested
   than Win32/GDI for this project's needs — but the decision at the time
   was to fix each bug in place rather than migrate immediately, since a
   Qt migration was a materially bigger dependency and would cost the
   fully self-contained single-`.exe` property the Windows build had.
3. **Qt6, unifying both platforms into one shell** — the decision that was
   eventually made once the FLTK bug count kept growing and the project's
   direction shifted toward "a QC tool that needs to be reliable and will
   grow more UI over time," at which point the earlier objections (bigger
   dependency, losing the self-contained exe) were explicitly accepted as
   the right trade. This is documented here rather than treated as a
   contradiction of point 2's reasoning: the trade-off was re-evaluated
   against new information (three FLTK bugs, not zero; more UI coming, not
   less) and came out differently the second time, which is what
   re-evaluating a decision against new evidence is supposed to look like.
   Net effect: one shell instead of two, and every one of the three FLTK
   bugs above is structurally impossible to recur, since the FLTK code
   that had them no longer exists.

### Plot chrome: time scale (both sides), color bar, constant-time grid lines

Three elements sit around/over the density plot: a vertical time axis on
both the left and right (rotated labels + "Time (ms)" title), a color-bar
legend (positive/red at top, negative/blue at bottom), and horizontal
constant-time grid lines. All three are driven by a single `DisplaySettings`
struct (`showTimeScale`, `showColorBar`, `showGridLines`, all default
`true`), read fresh every frame by `chrome.cpp` — nothing else caches
whether an element is visible. That's deliberate: a future Edit >
Preferences dialog (not built yet, per the brief) only ever needs to flip
these booleans and ask for a redraw; it doesn't need to know anything about
layout, fonts, or the tick algorithm. `AppState::display` exists in both
shells today specifically so that hookup is the *only* thing Preferences
will need to do.

**Split between shared and platform-native code, and why**: the color bar
and grid lines are pure pixel arithmetic (fill a gradient, blend a row of
pixels toward a line color) — both platforms already share a `uint32_t`
pixel buffer with an identical `0x00RRGGBB` layout, so this lives entirely
in `chrome.cpp`, unit-tested, zero platform-specific code. Rotated *text*
is drawn by `viewer_qt.cpp`'s `drawRotatedLabel()`, one shared
implementation for both platforms using `QPainter`'s standard idiom:
`save()` → `translate(anchorX, centerY + textWidth/2)` → `rotate(-90)` →
`drawText(0, 0, text)` → `restore()`. `QFontMetrics::horizontalAdvance()`
supplies `chrome.h`'s `TextMetrics::measureWidth` callback the same way on
both platforms, since Qt's font metrics don't differ per-OS the way GDI vs.
FLTK's core-X-font measurement used to.

This replaced two previous, platform-specific implementations: a Win32
`LOGFONT` with `lfEscapement=900` (GDI's rotated-font support, which worked
fine), and — on Linux — a from-scratch manual pixel-rotation workaround
built to route around a real FLTK bug (`fl_draw(int angle, ...)` would
reliably render the first one or two rotated labels in a frame and then
silently stop after ~7 calls at nearby positions; see "GUI toolkit
history" above for how that was root-caused). `QPainter::rotate()` is a
single, mature, heavily-used code path shared by both platforms, so this
is a net simplification as well as a unification — strictly less
rotated-text code than either platform had before, and Qt's anti-aliased
`QPainter` text output looks better than the old Linux core-bitmap-font
fallback did.

**Tick interval selection** (`chrome.cpp`, tested in `test_main.cpp`): a
classic "1, 2, 5 × 10^k" sequence, indexed by a single integer so the code
can walk it in either direction without a lookup table. The algorithm picks
the *finest* interval from that sequence that still keeps the line count
at or under 11, then — using the platform-supplied `measureWidth`
callback — checks whether adjacent rotated labels would overlap along the
axis (a label's footprint along the axis is its *unrotated* pixel width);
if they would, it steps to the next coarser interval and rechecks, exactly
as specified ("if the labels start to overlap ... reduce the interval to
the next coarseness. Check if it now displays correctly"). This is
regression-tested directly: `testChromeTickCountNeverExceedsMax` sweeps many
view/canvas-size combinations checking the 11-line cap, and
`testChromeOverlapAvoidance` forces a short canvas and checks every adjacent
tick pair's pixel gap against its label's measured size.

**Time unit selection**: defaults to milliseconds; switches to seconds only
once the record would need 7+ digits in ms (>= 1,000,000 ms), and to
microseconds only when the whole record is under 1 ms (so values wouldn't
render as "0.0something"). Both thresholds are exercised in
`testChromeTimeUnitSelection`, including a "large" case built from
*realistic* SEG-Y field limits (`samplesPerTrace` and `sampleIntervalUs` are
each a 2-byte field in the real binary header, so 32767 is the actual
largest either can be — not an arbitrary large test number).

### Variable-density smoothing: bilinear, not nearest-value

`renderPyramidPath` and `renderExactPath` (`renderer.cpp`) originally picked
the single nearest pyramid-block/decoded-sample value per pixel. Both now
bilinearly interpolate between the 4 neighboring values instead, across
*both* the trace and time axes, for a smoother image — always on in
variable-density mode, no separate setting (nothing asked for a way to turn
it back off, so one wasn't added).

Two interpolation conventions, not one, because pyramid blocks and raw
decoded samples represent their value differently: a pyramid block's mean
is an *average over a range*, so it's treated as sitting at that block's
*center* (block `b` at continuous position `b+0.5`, the usual texel-center
convention); a decoded sample is a single point value that sits exactly at
its own integer position, no half-index shift. Using the block-center
convention for raw samples (or vice versa) would silently shift the image
by half a pixel at certain zoom levels — a real, easy-to-miss bug class for
this kind of resampling code, avoided here by keeping the two conventions
as named, separate helpers (`bilinearBlockIndex` / `bilinearPointIndex`)
rather than one "close enough" function doing both jobs. Neighbor indices
clamp at pyramid-level/decoded-buffer edges (repeat the edge value) rather
than reading out of bounds. Cost stays O(screen pixels) — same complexity
class as before, just ~4 memory reads and a lerp per pixel instead of one
lookup; see "Microbenchmarks" below for the measured impact.

### Wiggle display: bounded by screen pixels, never by file size

A second, distinct visualization mode — straight line segments between
consecutive sample amplitudes along each trace, deliberately with *no*
smoothing (the opposite call from variable density, on purpose: a wiggle
plot is supposed to show the actual sample-to-sample shape, not an
interpolated one) — with an optional infill (classic "variable area"
style: solid black, positive excursions only). Toggled via **View >
Seismic Display** (a real, working submenu, unlike the placeholder items
elsewhere in that menu) and a separate **View > Wiggle Infill** checkbox.

New shared files, `viewer/wiggle.h`/`.cpp`, follow the exact same split as
`chrome.h`/`.cpp`: pure geometry computation here (unit-tested from
`segytest`, no Qt link needed), the Qt shell (`viewer_qt.cpp`) is the only
place that actually calls `QPainter`. `chrome.cpp` itself needed only two
small changes for this: skip its own raster render and leave the plot area
white (the conventional wiggle-plot background) when in wiggle mode rather
than compute a raster image nobody will see, and auto-hide the color bar in
wiggle mode regardless of the user's own color-bar setting, since it
encodes amplitude-as-color and showing it next to a black-and-white wiggle
plot would be confusing clutter — that setting itself is untouched and
still respected in variable-density mode.

**Performance is the reason this took real design work, not just "draw a
polyline per trace."** A naive implementation decodes and draws every
visible trace at full sample resolution — cost scales with *file size and
zoom*, exactly the thing the multi-resolution pyramid exists to avoid for
the other display mode. Two independent decimations keep wiggle mode in
the same O(screen pixels) complexity class:

- **Trace axis**: draw at most `plotWidth / kMinPixelsPerTrace` traces
  (`kMinPixelsPerTrace = 3`, a fixed constant for now — a tunable
  "density/gain" Preferences control would be a natural follow-up, not
  needed yet), evenly spaced across the visible range. A 56,726-trace file
  zoomed out fully still only decodes a few hundred traces — confirmed via
  `segybench`, not just claimed (see below).
- **Sample axis**: one polyline vertex per raw sample when the visible
  range isn't much taller than the plot in pixels — literally "straight
  lines between samples," per the original ask — or, once zoomed out
  enough that many raw samples would map to one pixel row, a **min/max
  envelope per row** instead (2 vertices/row, alternating min-then-max and
  max-then-min so the outline doesn't snap back across the column each
  row). This is the standard technique for waveform displays at extreme
  zoom-out, and directly analogous to why the pyramid keeps min/max per
  block rather than only a mean.

Net worst case is O(plotWidth × plotHeight) — independent of file size or
zoom level, matching the raster paths' own complexity class. This also
means wiggle mode never needs the pyramid at all: the bounded point counts
above are always cheap to exact-decode directly from the mmap (reusing the
same `decodeSamples()` call `renderExactPath` already makes), regardless of
zoom level — simpler than the raster paths in that respect. Selected traces
decode in parallel via the existing `ThreadPool::parallelFor`.

Tested in `test_main.cpp` (a real temp file + `MappedFile`, not the
in-memory-buffer shortcut the chrome tests use, since `computeWiggleLayout`
always exact-decodes from a real mmap and never touches the pyramid):
trace count never exceeds the `plotWidth/kMinPixelsPerTrace` bound across
several view sizes; per-trace point count stays bounded even with a huge
visible sample range; every emitted point stays within plot-local pixel
bounds; and — checked directly against brute-force computation, the same
rigor as the pyramid's own coarse-level tests — the min/max envelope
reduction exactly preserves the true per-row extremes.

### Toolbar, status bar, box-select zoom, and gain

The status bar moved from a hand-drawn strip inside `SegyCanvas::paintEvent`
(originally at the top) to Qt's own `QStatusBar`, docked at the bottom by
`QMainWindow` automatically — manually reserving/drawing that strip had no
benefit once a toolbar needed the space below the menu bar anyway.
`SegyCanvas` pushes text to it via a plain pointer/callback handed over by
`MainWindow` after construction, not Qt signals/slots — a single,
controlled producer/consumer link doesn't need that machinery.

**Tool modes** (`ToolMode`: `Arrow`, `Pan`, `BoxSelect`) gate what
left-click does on the canvas; wheel-zoom and `'R'` reset are unaffected by
tool mode. Arrow (default) only hovers: it decodes the exact sample under
the cursor (`decodeSamples`, one sample) and parses that trace's header
with `segy::parseTraceHeader()` — already implemented, previously unused
anywhere in the viewer — to show `trace N (seq #S)  time Tms  amplitude A`
in the status bar. Pan and Box Select are checkable toolbar actions in a
`QActionGroup` with `ExclusionPolicy::ExclusiveOptional`, so clicking the
active one again deselects back to Arrow with no separate "Arrow" button
needed.

**Box select** stores its corners in data (trace/sample) space, not
pixels, converting to plot-local pixel coordinates fresh every
`paintEvent` — same reasoning as the density plot itself, so the box stays
correctly anchored across pan/zoom. Two clicks place opposite corners
(with a live rubber-band preview between them); once complete, clicking a
corner handle attaches it to the cursor and a second click releases it
(click-to-attach/release, not press-drag-release, per spec) — mouse
handlers derive the plot's pixel offset from `ChromeLayout::plotX`/
`plotWidth`, cached from the most recent `paintEvent` since only that
function computes it (the color bar/time-scale columns shift where the
plot itself starts). Right-click selects the box (turns it red) and shows
a `QPushButton` in the status bar's permanent-widget area; clicking it
clears the box.

**Zoom/Mooz** are one-shot toolbar actions, not modes. Zoom pushes the
current `ViewRange` onto `AppState::zoomHistory` and sets the view to the
box's extent (via the existing `clampView`); Mooz pops and restores the
last entry. Only the Zoom button pushes history — wheel-zoom, pan, and
reset don't — so "remember all zoom levels" means all *box-zoom* levels
specifically.

**Gain**: a `QSlider` (−6 to +24 dB, paired with a `QSpinBox` for direct
typing — see "Dark Pro" below for why it's no longer just a
`QDoubleSpinBox`) applies on top of the pyramid's auto-computed min/max
clip in both display modes — `renderer.cpp`'s `clipForGain()` divides the
clip by the linear gain (`10^(dB/20)`) once outside the per-pixel loop, so
it costs nothing in the hot path, and `wiggle.cpp` applies the identical
calculation (plus a fixed +6 dB perceptual offset applied only at the
`viewer_qt.cpp` call site — see "Dark Pro") so the two display modes read
as equally "loud" to the eye. 0 dB (default) means "trust auto-scaling
as-is"; manual full-scale control (as opposed to gain-on-top-of-auto-scale)
is a possible future addition, not this one.

Toolbar icons (magnifying-glass ± , hand, rectangle) are simple
hand-drawn `QPainter` shapes on a 24×24 `QPixmap` — this project has no
icon-asset pipeline, and four small glyphs didn't justify adding one.

**Two real bugs found while building this, neither visible from code
review:**
1. **Hover info and corner-dragging silently never fired.** Qt widgets
   only deliver `mouseMoveEvent` while a button is held, unless
   `setMouseTracking(true)` is set — fine for the old drag-only pan, but
   both hover (Arrow tool, no button held) and box-select's "click to
   attach, move freely, click to release" gesture need move events all the
   time. Confirmed by testing: corner drags silently did nothing (the
   corner's position only updates in `mouseMoveEvent`, which never ran)
   until `setMouseTracking(true)` was added to `SegyCanvas`'s constructor.
2. **Menu/toolbar/status-bar text stayed at 100% scale on a 150%-scaled
   Windows display** (canvas rendering itself was already correctly
   crisp/DPI-aware — see "Manually verifying the viewer" above for that
   separate, earlier fix). Root cause: Qt6's *default* high-DPI scale
   factor rounding policy snaps a fractional monitor scale factor to the
   nearest integer before applying it to fonts/layout, so 150% doesn't
   round to 1.5 — it rounds away from it entirely. Fixed with
   `QApplication::setHighDpiScaleFactorRoundingPolicy(PassThrough)`,
   called before constructing `QApplication`; confirmed by screenshot
   before/after on the same real 150%-scaled display. Applies equally on
   Linux (Ubuntu's fractional-scaling support reports the same kind of
   factor via xcb/XSETTINGS) even though it was found and confirmed on
   Windows.

### Dark Pro UI

After the toolbar/gain work above, three UI-direction mockups were built
(dark/DaVinci-Resolve-style, light/Figma-style, ribbon/Kingdom-style) and
"Dark Pro" was chosen. It's not just a palette swap: it relocates two
pieces of already-working functionality — the color bar and the live
trace/time/amplitude hover readout — off the canvas and status bar into a
new right-hand `QDockWidget`, alongside a placeholder-only QC Notes
section.

**App-wide theme**: a single QSS string, built in an anonymous namespace
in `viewer_qt.cpp` and applied once via `qApp->setStyleSheet(...)` in
`main()`, covers `QMainWindow`/`QMenuBar`/`QMenu`/`QToolBar`/`QStatusBar`/
`QPushButton`/`QDockWidget`/`QSlider`/`QSpinBox`. The seismic canvas paints
its own pixels directly and ignores the stylesheet entirely. Toolbar icons
(magnifier/hand/rectangle), which used to stroke in `Qt::black`, now stroke
in a light gray (`kIconStrokeColor`) so they stay visible on the dark
toolbar; `QToolButton:checked` gets the mockup's accent-tinted pill
background.

**Canvas: dark surround with an inset plot.** `SegyCanvas::paintEvent`
fills the whole widget with a near-black surround color first, then blits
the rendered chrome into a rect inset by a fixed `kCanvasMargin` (16px) on
every side, instead of at `(0, 0)`. `chrome.cpp`'s own layout math is
unaffected — it still renders into a buffer sized to the *inset*
width/height, which `viewer_qt.cpp` already computed and passed in before
this change. `dataCoordAt`/`pixelForData` (mouse hit-testing and
box-drawing) shift by the same margin so pan/zoom/box-select stay
correctly aligned with the cursor; deliberately not done via a
painter-wide `translate()`, since `pixelForData()`'s contract is to return
margin-inclusive *widget-local* pixels, used both for hit-testing raw
`event->pos()` and for drawing the box in that same space.

**Color bar → dock ("Amplitude Scale"), a relocation not a restyle.**
`chrome.cpp` still knows how to draw a color bar and its tests
(`testChromeColorBarPolarity`, `testChromeSettingsToggleOffMargins`) still
exercise that code — untouched. `viewer_qt.cpp` just never asks it to:
`paintEvent` builds a local `DisplaySettings` copy with `showColorBar`
forced to `false` before calling `renderFrameWithChrome`, reclaiming that
width for the plot. The View menu's existing `showColorBar` toggle now
drives the dock section's visibility instead (mirroring
`chrome.cpp`'s own "no color bar in wiggle mode" rule, so toggling display
mode doesn't leave a stale legend visible). The dock's `AmplitudeScaleWidget`
paints a vertical `QLinearGradient` sampled from `segy::divergingColormap`
plus `+clip`/`0`/`−clip` labels; `MainWindow::refreshAmplitudeScale()`
recomputes the gain-adjusted clip with the same small `rawClip`/
`linearGain`/`clip` formula `renderer.cpp`'s `clipForGain()` uses (small
enough to duplicate for this one caller rather than export) whenever the
gain control or the loaded file changes.

**Hover readout → dock ("Trace Info"), also a relocation.**
`SegyCanvas::statusLineText()` no longer has an Arrow-tool branch — the
status bar now always shows the same summary/box-guidance text regardless
of tool. The hover computation itself (exact single-sample decode via
`decodeSamples` + `segy::parseTraceHeader()`) moved into
`SegyCanvas::currentHoverInfo()`, reported to `MainWindow` through a new
`setHoverInfoCallback` (same plain-callback pattern as the existing
status-bar hookup) fired from `mouseMoveEvent`'s Arrow branch and cleared
from `leaveEvent`. `MainWindow::updateHoverInfo()` writes the four dock
labels (Trace/Seq #/Time/Amplitude), showing `--` placeholders when
nothing is hovered.

**Toolbar restructure**: the View menu's Variable Density/Wiggle submenu
is still there, now backed by member pointers (`variableDensityMenuAction_`/
`wiggleMenuAction_`) instead of locals, and a matching segmented pair of
toolbar buttons (`variableDensityToolbarAction_`/`wiggleToolbarAction_`) was
added in a *plain* `QActionGroup` — not `ExclusionPolicy::ExclusiveOptional`
like Pan/Box Select, since exactly one display mode is always active, never
neither. Each action's handler also checks its counterpart (menu → toolbar,
toolbar → menu), so both stay in sync regardless of which one the user
clicks. The gain control changed from a single `QDoubleSpinBox` to a
`QSlider` + `QSpinBox` pair (matching the mockup's bar), both wired through
one `MainWindow::setGainDb()` that updates `app_.display.gainDb` and both
widgets without feedback loops; the slider's `valueChanged` handler
rounds to the nearest multiple of 3 before calling it, since
`setSingleStep()` only affects arrow-key/wheel stepping, not dragging.

**Wiggle's perceptual gain offset.** After using the new toolbar's
segmented control to switch between modes at the same nominal dB setting,
wiggle traces read as visually weaker than the variable-density raster —
a wiggle line's "loudness" is judged by excursion width, which the eye
weighs differently than color saturation. Rather than changing
`computeWiggleLayout()`'s own default (0 dB) or its documented "switching
display modes doesn't change how loud the data looks" contract — both of
which its tests and `segybench` rely on — the fix is a fixed
`kWiggleGainBoostDb = 6.0` added only at the `viewer_qt.cpp` call site:
`computeWiggleLayout(..., app_.display.gainDb + kWiggleGainBoostDb)`. The
shared function, its tests, and every other caller are unaware this
happens.

**Right dock (`QDockWidget`, `Qt::RightDockWidgetArea`, fixed 260px,
`NoDockWidgetFeatures` so it can't be closed/floated/undocked)**, top to
bottom: Trace Info (the four labels above), QC Notes (a static "No issues
flagged on this line yet." placeholder — matches this project's existing
inert-placeholder convention; no backing functionality was asked for
beyond the mockup's empty state). The dock originally had a third
"Amplitude Scale" section too; it moved back onto the canvas shortly
after (see "Amplitude scale: from dock section to canvas overlay" below)
once it was actually seen next to real data.

No changes were needed to `renderer.*`, `chrome.*`, or any tested shared
file for any of the above — every relocation and gain adjustment is local
to `viewer_qt.cpp`, confirmed by an unchanged `segytest` result
(87,258 checks, 0 failures) after this work.

### Toolbar icons: from hand-drawn shapes to Tabler Icons

The toolbar's icons started as plain `QPainter` primitives (see "Toolbar,
status bar, box-select zoom, and gain" above) — fine for a magnifying
glass or a rectangle, but freehand-vectoring anything more specific (a
pan/grab hand, a waveform) meant guessing Bézier control points blind,
rebuilding, and screenshotting to see if the guess was right. That loop
produced a hand icon the user called "weird" and a wiggle icon that read
as a tangled scribble even after several tries.

The fix was to stop guessing and use real icons: [Tabler Icons](https://tabler.io/icons)
(MIT licensed, `https://github.com/tabler/tabler-icons`) ships every icon
as a small, plain SVG on a 24×24 grid — a straightforward license fit
(permissive, attributable, no runtime dependency beyond rendering) for a
project with no existing icon-asset pipeline. Candidates were pulled with
`curl` from the repo's raw SVG files, assembled into a comparison gallery
(an Artifact page showing each candidate at real toolbar size on the
app's actual dark chrome, plus enlarged) so the user could pick by eye
instead of by description, and the chosen ones' exact path data went
straight into the app.

**Rendering**: a new `renderSvgIcon(QString svg)` helper in `viewer_qt.cpp`
substitutes the toolbar's real stroke color for the SVG's `stroke="currentColor"`,
renders via `QSvgRenderer` (Qt6::Svg) into a `QPixmap`, and wraps that in
a `QIcon` — the icon-drawing functions (`iconHand`, `iconRectangle`,
`iconMagnifier`, `iconWiggleGlyph`, `iconChartArea`, `iconChartBar`,
`iconLock`, `iconCaretUpDown`, `iconSidebarCollapse`) now each just embed
one verbatim `<svg>...</svg>` string (a straight copy-paste from Tabler's
source, apart from the color substitution) rather than a page of
`QPainterPath` calls — directly WYSIWYG with the source icon instead of a
level of hand-translated indirection. This is a **new dependency**:
`Qt6::Svg` (vcpkg port `qtsvg`, apt package `qt6-svg-dev` — see "Build on
Windows/Linux" above), the first time this project has needed a Qt
component beyond `Widgets`.

**What got replaced, and with what** (all MIT, Tabler Icons):
- Pan → `hand-move`. Wiggle-mode toggle's "switch to Wiggle" state →
  `wave-sine`. Box Select → `rectangle`. Zoom/Mooz → `zoom-in`/`zoom-out`.
  The Variable-Density state icon is *not* from Tabler — it stayed a
  small `QLinearGradient` swatch sampling `segy::divergingColormap`
  directly (see "Toolbar restructure" above), since it previews the
  app's actual color scale rather than standing for a generic concept.
- Three new reserved-but-inert toolbar slots for features that don't
  exist yet, added at the same time since the user was already picking
  icons for them: **Spectrum** (`chart-area`) and **Histogram**
  (`chart-bar`), both `setEnabled(false)` with a "not yet implemented"
  tooltip — a disabled button says "not built yet" more clearly than a
  click that silently does nothing, matching the existing convention for
  the View menu's inert placeholders. **Lock** (`lock`/`lock-open`) is
  further along: a real two-state `QAction::setCheckable(true)` toggle
  that swaps its own icon on click (`iconLock(checked)`), with
  `QToolButton:checked`'s existing accent-bordered style giving it a
  "pressed down" look — nothing is actually locked by it yet (what it
  should lock is still undecided), but the toggle itself is real.
- The Gain control's plain-text " Gain " label became a small disclosure
  button (`caret-up-down`, also disabled for now) that will open a popup
  for more detailed gain settings once that exists.
- A new **sidebar toggle** (`layout-sidebar-left-collapse`, checkable,
  default checked) shows/hides the right dock — `dock_` has
  `NoDockWidgetFeatures` so it has no close button of its own; this
  toolbar action is the only way to hide or restore it
  (`connect(sidebarToggleAction_, &QAction::triggered, dock_, &QDockWidget::setVisible)`).
  Used exactly as named even though Tabler's own icon draws its panel on
  the left and this app's dock is on the right — the collapse-arrow
  meaning still reads fine unmirrored, and mirroring wasn't asked for.

**A real (if small) Linux-specific bug found and fixed along the way**:
depressing any checkable toolbar button visibly grew it by ~2px on
Ubuntu/Fusion-style widget rendering — `QToolButton:checked` added
`border: 1px solid #e8905a` but the base `QToolButton` rule had no
`border` at all, so the checked state's border expanded the button's box
instead of just recoloring an already-reserved one. Fixed by giving the
base rule `border: 1px solid transparent` — same box size in both states,
only the color changes on check. (Windows' native style already reserved
consistent border space regardless of the QSS, which is why this only
showed up when testing on Linux.)

### Amplitude scale: from dock section to canvas overlay

The dock's Amplitude Scale section (see "New right-hand dock" above)
looked wrong once actually seen next to real seismic data: a short, wide
swatch felt disconnected from the tall plot area it was describing. The
fix moves the legend back onto the canvas itself — not by re-enabling
`chrome.cpp`'s own color bar (still forced off, still untouched, same
`showColorBar = false` local-copy trick as before), but as a real Qt
child widget (`AmplitudeScaleWidget`, now owned by `SegyCanvas` instead of
`MainWindow`) floated on top of the rendered plot image in the seismic
display area's top-right corner, sized to ~70% of the plot's own height
and much narrower (an 8px gradient bar plus a slim label column, down
from a wide dock section) via `SegyCanvas::resizeEvent` →
`repositionAmplitudeScale()` — recalculated on every resize since Qt
composites child widgets on top of their parent's own painting
automatically, with no extra work needed in `paintEvent` itself. It
paints its own semi-transparent dark backing rect first
(`QColor(0x14, 0x14, 0x16, 200)`) so the legend stays legible regardless
of what seismic colors happen to be underneath it, and sets
`Qt::WA_TransparentForMouseEvents` since it's a legend, not a control.

`MainWindow` still owns the gain-adjusted clip math (same small
`rawClip`/`linearGain`/`clip` duplicate of `renderer.cpp`'s
`clipForGain()` as before) and the visibility rule (mirrors
`chrome.cpp`'s "no color bar in wiggle mode" behavior); it now hands both
to the canvas via one call, `canvas_->setAmplitudeScale(clip, visible)`,
instead of reaching into a dock-owned widget directly — geometry stays
inside `SegyCanvas`, where the plot's actual on-screen rect is known.

**Follow-up fix: the legend covered the plot's own right axis.** The
first version above floated the overlay at a fixed offset from the
canvas's right edge, which worked until `showTimeScale`'s right-side axis
column pushed chrome's own rendering wider than expected, and the overlay
ended up covering both the axis and part of the seismic image. Fixed by
reserving a real column for it instead of floating over content: a new
`SegyCanvas::plotAreaWidth()` subtracts a fixed
`kAmplitudeScaleColumnWidth` from the plot's width *whenever
`showColorBar` is on, independent of `seismicMode`* — every place that
used to compute `width() - 2*kCanvasMargin` for the plot (`paintEvent`,
`wheelEvent`, the pan branch of `mouseMoveEvent`) now calls
`plotAreaWidth()` instead, so chrome always renders into the narrower
area and the legend's reserved column never overlaps it. Driving the
reservation off `showColorBar` alone (not `seismicMode`) means the plot's
width stays *exactly* constant across Variable Density/Wiggle switches —
only whether the legend widget draws anything in that reserved space
changes, not the plot's own size, which was the actual ask ("the size of
the seismic window stays the same").

### Toolbar polish pass

A handful of smaller fixes/additions after using the Dark Pro toolbar for
a while:

- **Box Select's icon is now Tabler's own `rectangle`** (was a
  hand-drawn `p.drawRect()`), for the same consistency reason the other
  icons moved to Tabler.
- **Zoom/Mooz are now Tabler's `zoom-in`/`zoom-out`** (were a hand-drawn
  magnifying glass ± ), a matched pair from the same source as the rest.
- **The gain spin box was too wide** for its content (`-6 dB`..`24 dB`) --
  given a `setFixedWidth(64)`.
- **The gain slider's groove was nearly invisible** (`#17181a` on a
  `#2b2d30` toolbar -- barely any contrast): changed to `#4a4c50`,
  matching the border/hover tone already used elsewhere in the QSS. The
  orange handle stayed orange rather than switching to white: this app's
  Dark Pro palette already uses the accent color specifically to mean
  "interactive/active element" (checked toolbar buttons, the Lock
  toggle's pressed look, the active display-mode pill) — a white handle
  would read as a different, inconsistent signal.
- **A real Linux-specific bug**: depressing any checkable toolbar button
  visibly grew it by ~2px on this Ubuntu/Fusion-style build --
  `QToolButton:checked` added `border: 1px solid #e8905a` but the base
  `QToolButton` rule had no `border` at all, so the checked state's
  border expanded the button's box instead of recoloring an
  already-reserved one. Fixed by giving the base rule
  `border: 1px solid transparent` — identical box size in both states,
  only the color changes. (Windows' native style happened to already
  reserve consistent border space regardless of the QSS, which is why
  this only surfaced when testing on Linux.)
- **The sidebar toggle is right-aligned** on the toolbar, separated from
  the tool buttons by an expanding spacer widget
  (`QSizePolicy::Expanding` on a plain `QWidget` added via
  `toolbar->addWidget`) — reads as a standalone "view" control rather
  than grouped with Lock/Histogram/Spectrum to its left.

### Parameters dialogs: Histogram, Spectrum, Display Parameters

Three new toolbar buttons open real (modeless) dialogs, each modeled on
a specific screen from a reference seismic-interpretation tool the user
supplied as inspiration. All three follow the same scope as everything
else in this session's "not-yet-built features" work: the **settings UI**
is real and its values persist (`AppState::histogram`/`spectrum`/
`wigglePresentation`), but the **computation/rendering** each dialog
configures doesn't exist yet — no histogram is computed, no spectrum is
plotted, and only two of `DisplayParametersDialog`'s dozen-plus controls
(Gain, and the Variable Density/Wiggle radio) affect anything on screen.
This mirrors the Spectrum/Histogram toolbar buttons' own earlier
"disabled, not yet implemented" state, just one step further along now
that their parameter shapes are known.

**Shared pattern across all three** (`HistogramDialog`, `SpectrumDialog`,
`DisplayParametersDialog` in `viewer_qt.h`/`.cpp`): a single instance is
created lazily on first use and reused (`MainWindow` owns the pointer),
so edits stay "sticky" between opens like a real modeless parameters
palette; non-modal (`show()`, not `exec()`); an Apply button writes
settings without closing, matching the "tweak and observe" workflow these
reference dialogs themselves use; OK applies and closes; Cancel discards.
A "Selection" range option (where present) is disabled whenever there's
no complete box selected, refreshed in `showEvent()`.

- **Histogram Parameters**: Full View/Selection range, Number of Bins
  (`QSpinBox`, default 51, arrows step by 10 but typing accepts any
  value in range — `setSingleStep` only affects arrow/wheel stepping),
  Clip Type Data/User, and Min/Max fields. The Min/Max fields prefill
  from `app_.pyramid.globalMin/globalMax` whenever Data mode is
  (re)selected — a placeholder for the true "ignore the top/bottom 1% as
  outlier spikes" trimmed range asked for originally, which needs a real
  percentile pass over the samples that doesn't exist yet.
- **Spectrum Parameters**: Full View/Selection range, a Normalize group
  (`Absolute Max from Selected Curve` / `Relative Max to Each Curve` /
  `Absolute Max from User`, the last with its own value field) and a
  Smooth window-size dropdown — field names and the exact smoothing
  sizes (`1, 3, 5, 7, 9, 11, 15, 21, 25, 31`, note the widening gaps at
  the high end, not a uniform step) come directly from the reference
  tool's own Utilities > Normalize/Smooth menus.
- **Display Parameters** (opened from the toolbar's gain disclosure
  button, `caret-up-down`): modeled on a reference "PanelDisplayDialog"
  covering far more than gain — Variable Density/Wiggle Variable Area
  mode, Reverse Polarity/Center Line, Positive/Negative Fill,
  Wiggle/Positive Over Negative, two independent 4-way fill-style groups
  (Solid/PeakAmplitude/Varifill/Band) for "Variable Area Control" and
  "Wiggle Control", Variable Density Blending (enabled only in Variable
  Density mode), and five numeric fields (Gain, Percent Overlap, Fill
  Baseline, Wiggle/Var Minimum Spacing). Its button row matches the
  reference exactly: OK/Apply/Reset/Cancel (Reset re-reads
  `refreshFromAppState()`), unlike the other two dialogs' OK/Apply/Cancel.

**The two real, wired controls sync bidirectionally with the toolbar**,
not just one-way: editing Gain or the mode radio here updates the
toolbar's slider/spin box and the Seismic Display toggle/View menu
immediately (not just on the dialog's own OK/Apply), and the toolbar
changing either updates the dialog live while it's open. This needed two
small refactors:
- `MainWindow::setSeismicMode(mode)` is a new single place that sets
  `app_.display.seismicMode`, syncs both View-menu actions' checked
  state, refreshes the toolbar icon/amplitude-scale/canvas — replacing
  three near-identical copies of that same sequence (the two View-menu
  actions and the toolbar's own toggle button each used to do it inline).
  `DisplayParametersDialog`'s mode radio is a fourth caller now.
- Since `DisplayParametersDialog` has no reason to know about
  `MainWindow` (and `MainWindow` has no `Q_OBJECT`, so a
  `qobject_cast<MainWindow*>(window())` from the dialog wouldn't work
  anyway), it exposes `setGainChangedCallback`/`setModeChangedCallback`
  — plain `std::function` callbacks, the same producer/consumer pattern
  `SegyCanvas::setStatusWidgets`/`setHoverInfoCallback` already
  established — that `MainWindow` wires to `setGainDb`/`setSeismicMode`
  when it first creates the dialog. `MainWindow::setGainDb` and
  `refreshSeismicModeToolbarIcon` each also push a live refresh into the
  dialog (`if (displayParametersDialog_ && ...->isVisible())
  ...->refreshFromAppState()`) so external toolbar changes reach it
  immediately too, not just at next open.

**A real GUI-automation gotcha found while verifying these**: scripted
mouse clicks (`SetCursorPos`/`mouse_event` from a non-DPI-aware
PowerShell script) silently landed on the wrong control on this
150%-scaled Windows display, clicking through to the canvas underneath
the toolbar instead of the intended button — with no error, just nothing
visibly happening, which briefly looked like a code bug in the
newly-added dialogs. The fix wasn't in the app: switching the
verification method to Windows' UI Automation
(`System.Windows.Automation`, `FindFirst` by control `Name` +
`InvokePattern.Invoke()`) finds and clicks controls by identity instead
of screen coordinates, sidestepping DPI virtualization entirely, and
confirmed all three dialogs were working correctly the whole time. Same
underlying class of issue as the WSLg/`PrintWindow` DPI-virtualization
bugs recorded in "Manually verifying the viewer" below — worth reaching
for UI Automation over raw coordinate clicks for any future scripted
verification on this app.

### Split view: two independent panels, active-panel routing, and Lock

Two datasets (different files, or the same file twice) side by side, each
independently loadable/pannable/zoomable — modeled on a reference
"SeismicView" tool's dual-range/dual-foreground layout, scoped down to
its essential mechanic rather than that tool's full per-panel
Foreground/Background dataset-picker chrome.

**`Panel` struct**: `{ AppState app; SegyCanvas* canvas; }` — one per
side. `MainWindow` owns exactly two, `panelA_`/`panelB_`, both built at
startup and placed side by side in a `QSplitter` (`splitter_`, the new
central widget); panel B starts hidden, so single-panel use looks
identical to before split view existed. The toolbar's new **Split View**
toggle (`layout-columns` icon) just shows/hides panel B's canvas — no
other state changes when toggling it.

**Active-panel routing, not a rewrite of every handler**: `MainWindow`
keeps its existing `app_`/`canvas_` members, but they're now
`AppState*`/`SegyCanvas*` *aliases for whichever panel is active*
(`activePanel_`) rather than the one-and-only state. Every pre-existing
single-panel handler (menu wiggle-infill toggle, hover info, amplitude
scale's own single-canvas math, etc.) kept working with **no changes**
beyond the mechanical `app_.` → `app_->` this implies, since `canvas_`
was already a pointer. `SegyCanvas` itself needed **zero** changes either
— it was already parameterized over an `AppState&` reference, so
instantiating it twice was already supported; split view is purely a
`MainWindow`-side concern.

**"Active" = last clicked**: a new `SegyCanvas::setActivatedCallback`
fires at the top of `mousePressEvent` (before the existing tool-mode
logic, and even when nothing is loaded yet, so clicking an empty panel
before File > Open still targets it correctly).
`MainWindow::setActivePanel(Panel*)` re-points `app_`/`canvas_` and then
re-syncs every toolbar control (tool-mode buttons, gain slider/spin box,
seismic-mode toggle/menu) to that panel's own state, using plain
`setChecked()`/`setValue()` calls — these don't emit the `triggered`
signals the handlers below are keyed off, so re-syncing can't loop back
into applying a change. File > Open (`canvas_->startLoading(...)`) needed
no change at all: it already targets whichever panel `canvas_` aliases.

**Lock — the one button in this session that went from placeholder to
fully real**: `MainWindow::targetPanels()` returns just `{activePanel_}`
normally, or `{&panelA_, &panelB_}` when the Lock toggle is checked. The
handlers where "apply to both at once" is meaningful — tool mode
(Pan/Box Select), Gain, Seismic Display mode, Zoom, Mooz, Wiggle Infill,
and box deletion — loop over `targetPanels()` instead of touching
`app_`/`canvas_` directly; everything else (hover info, hitting a box's
corner, wheel-zoom, pan-drag) stays genuinely per-canvas since those are
driven directly by mouse events *on* a specific `SegyCanvas`, which
already only ever affects its own bound `AppState`. Zoom/Mooz, when
locked, still zoom **each target panel to its own** box/history, not a
shared one — the two panels can hold entirely different trace/sample
ranges, so copying one panel's zoom onto the other wouldn't make sense.

**Lock also follows pan, not just zoom range**: `SegyCanvas` takes a
`setViewChangedCallback`, fired after every mutation of `app_.view`
(wheel-zoom, drag-pan, box-zoom/Mooz, `'R'` reset) — not just the
tool-mode/gain/etc. handlers `targetPanels()` already covered, since pan
happens via direct mouse-drag on one specific canvas, with no toolbar
action in the loop to route through `targetPanels()`.
`MainWindow::syncLockedView(Panel* source)` is the callback body: no-ops
unless Lock is checked, then copies `source->app.view` onto the other
panel wholesale (not just an offset/delta — simplest correct behavior
when the two panels can hold different trace/sample ranges to begin
with) and re-clamps it against the other panel's own `foreground`
extent before redrawing. Both panels wire this the same way, so whichever
one you actually drag drives the other, symmetrically.

**Known, documented v1 simplifications** (all deliberate, not
oversights):
- The three parameters dialogs (Histogram/Spectrum/Display Parameters)
  each hold a `AppState&` reference bound permanently at construction —
  a plain C++ reference can't be rebound to the other panel later. They
  bind to whichever panel was active the *first* time each was opened,
  and stay bound to it even if you later switch panels or toggle Lock.
  Revisiting this (e.g. a dialog-per-panel, or an active-panel-aware
  reopen) is future work if it turns out to matter in practice.
- Zoom/Mooz/Del-button *enablement* (whether they're greyed out) and the
  shared status bar/dock always reflect whichever panel most recently
  changed or was interacted with — not lock-aware "enabled if either
  panel qualifies" logic. Minor UI polish, not a functional gap.

### Histogram and Spectrum: from settings-only to real computation

The Histogram/Spectrum parameters dialogs (see "Parameters dialogs" above)
started as settings-UI-only placeholders. They now actually compute and
plot a result, in a new Qt-free shared module —
`viewer/analysis.h`/`.cpp`, alongside renderer.cpp/chrome.cpp/wiggle.cpp,
same "GUI shell reuses tested logic" architecture as everything else in
this codebase — plus three new `segytest` cases (a known-distribution
histogram check, an out-of-range clamping check, and an FFT check that a
synthetic pure tone's peak lands at its exact expected frequency bin), the
same rigor as the pyramid/chrome/wiggle tests already there.

**`computeHistogram`**: decodes real samples (`segy::decodeSamples`, exact,
never the pyramid) from the requested trace/sample range — Full View or
the current box Selection, from `HistogramSettings::useSelection` — strided
down to `kHistogramMaxTraces` (500) traces if the range is larger, since
this is a quick-look diagnostic, not an exhaustive scan, and adjacent
traces are highly correlated in real seismic data. Bins into
`numBins` equal-width bins over `[rangeMin, rangeMax]` (`HistogramSettings`'
Data/User clip choice), clamping (not dropping) out-of-range values into
the edge bins, and returns both a percent-of-total-per-bin array and its
running cumulative sum.

**`computeSpectrum`**: decodes up to `kSpectrumMaxTraces` (50) traces from
the requested range, applies a Hann window (reduces the spectral leakage a
hard rectangular cut would otherwise introduce) and a hand-rolled iterative
radix-2 Cooley-Tukey FFT (zero-padded to the next power of 2 -- a full FFT
library would be overkill for a quick-look amplitude spectrum), averages
the magnitude across traces, and converts to dB relative to the averaged
spectrum's own peak. `SpectrumSettings::smoothPoints` then applies a
centered moving average over the dB curve. **Known simplification**: the
three Normalize modes (`Absolute Max from Selected Curve`/`Relative Max to
Each Curve`/`Absolute Max from User`) are real, working radio buttons, but
`computeSpectrum` always normalizes to the averaged spectrum's own peak
regardless of which is picked — distinguishing the three is real follow-up
work, not wired to the computation yet.

**Display**: two new plain `QWidget`s (`HistogramPlotWidget`,
`SpectrumPlotWidget` in `viewer_qt.h`/`.cpp`) — standalone top-level
windows, not `QDialog`s, since they're a result display with no OK/Cancel
semantics — paint the result with plain `QPainter` calls (bars +
cumulative line for the histogram; a single line for the spectrum) rather
than reusing chrome.cpp's tick-generation logic, which is written for the
density plot's specific axis conventions, not a generic small chart.
`MainWindow::computeAndShowHistogram`/`computeAndShowSpectrum` build a
`segy::RenderContext` from whichever `AppState` the dialog is bound to
(via a shared `makeRenderContextFor()` helper, also now used by
`SegyCanvas::makeRenderContext()` to avoid a second copy of the same
five-line struct fill) and call into `analysis.h`, wired to fire on
`HistogramDialog`/`SpectrumDialog`'s existing OK/Apply buttons via a new
`setAppliedCallback()` — the same plain-`std::function` pattern as
`setGainChangedCallback`/`setModeChangedCallback`.

**A real Qt gotcha found while chasing "the radio buttons don't work"**:
`QRadioButton`/`QCheckBox` styled with *any* QSS property (even just
`color`) lose their indicator's hit-testable geometry unless the
stylesheet also gives `::indicator` an explicit size — Qt's style engine
switches to fully custom rendering as soon as any QSS touches the widget,
and without indicator geometry it draws (and hit-tests) a zero-size
indicator. The dark-theme QSS added `QRadioButton, QCheckBox { color:
... }` for the three new parameters dialogs but never gave `::indicator`
a size, so every radio/checkbox in them was effectively unclickable.
Fixed by adding explicit `QRadioButton::indicator`/`QCheckBox::indicator`
rules (14×14px, rounded for radio/square for checkbox, with `:checked` and
`:disabled` states) to the same QSS block.

### Text (EBCDIC) and Binary header viewers

Two more toolbar buttons, `EbcdicHeaderWidget`/`BinaryHeaderWidget` in
`viewer_qt.h`/`.cpp` — read-only popups, and unlike
Histogram/Spectrum/Display Parameters, they hold no settings at all, so
each just re-populates from whichever panel is active on *every* click
rather than binding permanently to one panel at first open.

**Text header**: `segy::decodeEbcdicText()` (already existed, already
covered by its own `segytest` case) decodes the file's 3200-byte EBCDIC
text header to a single 3200-character string with **no embedded CR/LF at
all** — the on-disk convention is a fixed 40×80 grid, not paragraphs, so
`EbcdicHeaderWidget::setHeaderText()` reinserts a newline every 80
characters purely for display. The widget's `QPlainTextEdit` is
`setFixedSize()` to the exact pixel dimensions of an 80×40 monospace grid
(computed from `QFontMetrics`, plus a little headroom for the frame) —
deliberately not resizable, since showing more or fewer than 80 columns
would misrepresent the file's actual fixed-width layout, not just look
different. Styled white-on-black (the text edit specifically, not the
window chrome around it, which stays the app's usual dark grey) — the
classic terminal look this kind of fixed-width mainframe-era text is
normally read in.

**A real encoding bug found (and fixed) here**: `decodeEbcdicText()`'s
lookup table maps each EBCDIC byte to a **Latin-1** code point (0-255,
one byte in, one character out), but the first version of this widget
built the `QString` with `QString::fromStdString()`, which decodes as
**UTF-8**. Any decoded byte >= 128 (not rare in a real text header, since
EBCDIC's own punctuation/national-character codepoints land there) is an
invalid or partial UTF-8 sequence, corrupting everything from that byte
onward. Fixed by using `QString::fromLatin1(text.data(), text.size())`
instead, which is the correct one-byte-per-character decode matching what
the lookup table actually produces.

**Binary header**: originally an 8-field key/value list (whatever
`segy::BinaryHeader` happens to parse for rendering's own purposes), now a
proper `QTableWidget` with **Byte | Description | Value** columns showing
every field SEG-Y rev1 assigns a meaning to — 29 fields in total, decoded
directly from the raw mmap'd bytes with the existing `readI16BE`/
`readI32BE` readers (a new `BinHeaderFieldSpec` table in `viewer_qt.cpp`,
not through `segy::BinaryHeader`, which only exists to serve the renderer
and was never meant to grow to cover every field). The `3261-3500` block
(unassigned in rev1) is deliberately left out, per the original ask to
"drop the unassigned." A handful of enumerated-code fields (data sample
format, trace sorting code, measurement system, impulse signal polarity)
get their meaning annotated next to the raw number, e.g. `4 (Horizontally
stacked)`; the rest show their number as-is rather than risk guessing at
a meaning not confidently known. **Known gap**: rev2's additional fields
(beyond byte 3506) aren't included — added from general knowledge of the
rev1 spec without the user's own reference doc in hand; flag anything
missing or wrong and it's a quick table edit, not a redesign.

**Dataset name in every title, consistently**: a new `datasetTitleSuffix()`
helper appends `" — filename.sgy"` to a window's title whenever
`AppState::loaded` is true — a new `AppState::filePath` field (set in
`SegyCanvas::startLoading()` once the load succeeds) is all this needed.
Applied to the **main window** (refreshed from `refreshToolChrome()`,
which already runs after every load-completion/active-panel-switch) and
every popup that shows one panel's data: the Text/Binary header viewers,
the Histogram/Spectrum parameter dialogs and their result plots, and
Display Parameters — the same convention everywhere, which matters more
than usual now that split view means two panels can genuinely hold two
different files at once.

**Icons**: `file-description` (Text Header) and `file-digit` (Binary
Header), both Tabler Icons, MIT licensed, picked from the same
gallery-of-real-candidates workflow as every other icon in this app (see
"Toolbar icons: from hand-drawn shapes to Tabler Icons" above). Originally
`align-justified`/`binary`; swapped after a later gallery review to a pair
that visually pairs with each other (both a "page" glyph with a
distinguishing mark) rather than two unrelated shapes.

All of the above (white-on-black EBCDIC with real decoded content, dataset
names in both popup and main-window titles, the full annotated binary
header table) was confirmed with real screenshots against an actual
survey file, not just UI Automation — the earlier verification gap this
section used to describe is resolved.

### Icon size, and the first real Preferences: toolbar/drawer position

**Icon size**: all toolbar icons dropped to 85% of their nominal 24px size
with one line, `toolbar_->setIconSize(QSize(20, 20))` — `QToolBar` applies
its icon size to every action uniformly, so none of the individual
icon-drawing functions needed to change.

**Edit > Preferences** is this app's first actual persisted, user-facing
settings screen (`PreferencesDialog` in `viewer_qt.h`/`.cpp`) — unlike
every other `*Settings` struct in this file (Histogram, Spectrum,
`WigglePresentationSettings`, ...), which lives on a per-panel `AppState`
and resets with a fresh session, the new `Preferences` struct is app-wide
and persisted to **`$HOME/.segyread_settings`** via `QSettings(path,
QSettings::IniFormat)` — an explicit, visible-on-disk path rather than
`QSettings`' own platform-default location (registry on Windows,
`~/.config/<org>/<app>.conf` on Linux), since the request was specifically
for a known, inspectable file.

**Toolbar Position** (`QComboBox`: Left/Top/Bottom/Right, default Left)
maps directly to `Qt::ToolBarArea` — `QMainWindow::addToolBar(area,
toolbar_)` on an *already-added* toolbar just moves it, no need to remove
and re-add. `toolbar_` had to be promoted from a constructor-local
variable to a `MainWindow` member for this (`applyToolbarPosition()` needs
it later); the ~20 existing `toolbar->...` call sites inside the
constructor were left alone via a local `QToolBar* toolbar = toolbar_;`
alias, rather than a 20-site mechanical rename for no functional benefit.

**Drawer Position** (two `QRadioButton`s: Left/Right) is the right dock's
position (`Qt::DockWidgetArea`, same `addDockWidget()`-on-an-existing-dock
relocate trick) — "drawer" is just this feature request's own name for the
dock that "Right dock"/"sidebar" refer to elsewhere in this file, not a
separate concept. It's only freely choosable when the toolbar is Top or
Bottom; with the toolbar Left or Right, the drawer is forced to the
opposite side, and the dialog's radio buttons disable themselves,
pre-selected to whichever side that forces — so what Apply/OK is about to
do is never a surprise. `MainWindow::applyToolbarPosition()` is the single
place that enforces this (it updates `prefs_.drawerPosition` itself, not
just the live dock placement, before applying it) specifically so a
Left/Right toolbar can never leave a stale, inconsistent drawer side sitting
in `prefs_` ready to be written to disk by a later `savePreferences()` —
the dialog's own `updateDrawerControlState()` enforces the identical rule
for what the UI shows before Apply/OK is even clicked, so the two can't
disagree.

**Known limitation, not chased further given effort spent on other
verification this session**: opening the Preferences dialog specifically
via UI Automation (menu-click, then keyboard mnemonics) was unreliable in
the same way this file's other automation notes describe, and wasn't
confirmed with a screenshot the way most other features here were. The
toolbar's actual repositioning *was* confirmed visually (it renders
correctly on the left by default), and `PreferencesDialog` itself follows
the exact same lazy-create/non-modal/Apply-without-closing shape as
Histogram/Spectrum/Display Parameters, already screenshot-confirmed
working earlier in this file's history.

### Plain button rows instead of `QDialogButtonBox`

Every settings dialog (Histogram, Spectrum, Display Parameters,
Preferences) originally built its OK/Apply/(Reset/)Cancel row from a
`QDialogButtonBox`, which reorders buttons per platform convention and, on
this Linux build's style, drew a small icon on each button. Both were
unwanted — the user wants a fixed, predictable order (OK first, Cancel
last, whatever else in between) with no icons. Fixed by a small shared
helper, `addButtonRow(QDialog*, QVBoxLayout*, const
std::vector<std::pair<QString, std::function<void()>>>&)` in
`viewer_qt.cpp`'s anonymous namespace, that lays out plain `QPushButton`s
in a right-aligned `QHBoxLayout` in exactly the order given. All four
dialogs now call it instead of constructing a `QDialogButtonBox`; the
`#include <QDialogButtonBox>` was removed as no longer used anywhere.

### Gain control: vertical-toolbar rework, and a right-click exact-value entry

The Gain `QSlider` (the orange-handled one) only makes sense laid out
horizontally — when the toolbar is docked Left/Right, a vertical slider
would read as a completely different control, not the same one rotated,
and its horizontal-track handle styling doesn't survive that rotation
either. A vertical toolbar instead shows a plain +3 dB/-3 dB step pair
(`gainUpAction_`/`gainDownAction_`, Tabler `chevron-up`/`chevron-down`
icons). `gainValueLabel_`, the dB readout between them, went through a few
iterations and ended up as the *one* gain control shown in both
orientations — sitting next to the slider in horizontal mode too — which
made the separate `QSpinBox` that used to pair with the slider redundant
(removed entirely); right-clicking the label (`Qt::CustomContextMenu` +
`QInputDialog::getDouble`) opens an exact-value entry for gains other than
multiples of 3, formatted by `formatGainDb()` (one decimal place, trailing
`.0` trimmed so whole values still read as plain `12dB`).

Two bugs surfaced while building this, both worth recording since they're
non-obvious Qt behavior, not this app's own logic:

- **`QToolBar::orientationChanged` doesn't reliably fire from
  `addToolBar(area, ...)`**: the original design toggled the slider/step-pair
  visibility from that signal, synced once explicitly after connecting it
  (since the signal only fires on an actual change, not for whatever
  orientation the toolbar already happens to be in in the current
  constructor). In practice, on Windows, the slider and the step-pair both
  stayed visible at once regardless. Fixed by dropping the signal entirely
  and driving the visibility swap directly from `ToolbarPosition` inside
  `MainWindow::applyToolbarPosition()` — the enum that already decides the
  dock area, so no separate signal is needed at all.
- **Hiding a toolbar-added widget via its own `setVisible(false)` doesn't
  stick**: `QToolBar::addWidget()` wraps the widget in a `QWidgetAction`,
  and the toolbar's next layout pass (triggered by the very
  `addToolBar(area, ...)` call above) re-shows the widget as long as that
  wrapping action is still visible — silently undoing a plain
  `gainSlider_->setVisible(false)` and explaining why the slider kept
  reappearing in vertical mode. Fixed by capturing the `QAction*` that
  `addWidget()` returns (`gainSliderAction_`) and toggling *that* instead.

`gainValueLabel_`'s fixed width (so its box lines up flush with the icon
column instead of leaving dead space beside it) is computed from another
button's actual rendered `sizeHint()` — but not at the label's own
construction time, which runs too early in the constructor for the
toolbar's layout to have settled and undersized it. It's (re)computed
inside `applyToolbarPosition()` instead, once every action in the toolbar
actually exists, against `sidebarToggleAction_`'s widget specifically (any
icon button's width should be identical, but that one was confirmed
visually correct first).

### Sidebar toggle alignment follows toolbar orientation

The sidebar/drawer show-hide toggle is pushed to the toolbar's far end by
an expanding spacer widget ahead of it. That spacer's size policy was
`(Expanding, Preferred)`, which only pushes along the *horizontal* axis —
correct when the toolbar is Top/Bottom, but with the toolbar Left/Right
(vertical `QToolBar` layout) it did nothing, leaving the toggle wherever it
fell in icon order instead of at the bottom. Changed to `(Expanding,
Expanding)` so it pushes along whichever axis `QToolBar` is actually
laying widgets out on — right-aligned when horizontal, bottom-aligned when
vertical, matching how the rest of the toolbar's own orientation-driven
behavior already works.

### Split view: even 50/50 default, right-click for split options

Two follow-ups to the original split-view feature. First: showing panel B
via the Split View toggle used to leave `QSplitter` at whatever handle
position it last remembered (often a sliver) instead of an even split --
`QSplitter` doesn't default to 50/50 just because a previously-hidden pane
becomes visible again. Fixed with `MainWindow::resetSplitToHalf()`
(`splitter_->setSizes({total/2, total-total/2})` along whichever axis the
splitter is currently oriented on), called right after showing panel B.

Second: right-clicking the Split View toolbar button (a `QMenu` on
`QWidget::customContextMenuRequested`, since the button itself is a plain
`QAction` with no menu of its own) offers two options that didn't need a
permanent toolbar slot: **Set Split 50%** (calls the same
`resetSplitToHalf()`), and a toggle between **Vertical Split**/**Horizontal
Split** (flips `splitter_->orientation()` between `Qt::Horizontal`, the
original side-by-side default, and `Qt::Vertical`, panels stacked
top/bottom) — re-centering the split either way, since a stale handle
position from one orientation isn't a sensible split fraction in the
other.

### "No data selected" flash for Spectrum/Histogram with nothing loaded

Clicking Spectrum or Histogram with no file loaded used to just open their
parameters dialog against an empty/default `AppState` — not a crash, but
not meaningful either. Both toolbar actions now check `app_->loaded` first
and, if nothing's loaded, call `MainWindow::flashStatusMessage("No data
selected")` instead of opening the dialog: an orange status-bar message
(bottom-left) for 2 seconds, then back to whatever the active panel's
canvas would normally show there.

The color needed a second attempt: the first version tried an inline
`<span style="color:...">` in the text passed to
`QStatusBar::showMessage()`, on the assumption that its message label
auto-detects rich text like a plain `QLabel` does — it doesn't, and the
raw markup showed up literally as text. Fixed by setting the color via the
status bar's own stylesheet instead (`statusBar()->setStyleSheet("QStatusBar
{ color: #e8905a; }")`, which takes precedence over the app-wide
`QStatusBar` QSS rule for as long as it's set), cleared by a
`QTimer::singleShot(2000, ...)` that also calls `canvas_->refreshStatusBar()`
to restore the normal message.

### UI Scaling preference (Linux manual override)

A new `Preferences::uiScalePercent` (double, default 100), added
specifically for Linux: unlike Windows, which is already per-monitor
DPI-aware, there's no reliable way to *detect* the desktop's own scaling on
Linux in every environment — WSLg in particular reports none at all (see
the QT_QPA_PLATFORM/xcb workaround this file already documents for other
WSLg-specific gaps), so a fractional-scaling desktop under WSLg always
rendered at 100% no matter what the host was actually set to. The new
Preferences control is a `QSpinBox` (50-300%, step 25) the user sets to
match their desktop manually; it's disabled outright on Windows
(`#ifdef Q_OS_WIN`) with a tooltip explaining why, since Windows already
handles this itself and a second, manual override could only make the two
disagree.

Applying it is the interesting part: Qt only reads `QT_SCALE_FACTOR` while
constructing `QApplication`, which is far too late to react to a value
`MainWindow` loads normally via `loadPreferences()`. So `main()` reads the
same `$HOME/.segyread_settings` file directly, via a throwaway `QSettings`
instance, *before* `QApplication app(argc, argv)` is constructed, and calls
`qputenv("QT_SCALE_FACTOR", ...)` if the stored value isn't 100. This is
safe without an app instance yet because `QSettings` with an explicit
`IniFormat` path is just file I/O, no `QCoreApplication` required. One
consequence, called out in both the code comment and the Preferences
dialog's own tooltip/hint label: a changed value only takes effect after
restarting the app, not live.

### Header-based select/sort: the backend (`segy_select.h`), no UI yet

The next requested feature -- letting the user filter/reorder a loaded
file's traces by trace-header field values (a reference tool's
"IdentSelectSort" dialog: Ident/Min/Max/Inc rows, a Sort-by chain) -- was
deliberately built backend-first and performance-verified before any
dialog, given how central "select/sort after load" is to how this class of
tool actually gets used. Three new pieces in `include/segy_select.h` /
`src/segy_select.cpp` (part of `segycore`, Qt-free, fully covered by
`segytest`):

- **`scanTraceHeaders()`**: parses every trace's 240-byte header (via the
  existing `parseTraceHeader()`) into `TraceHeaderColumns`, a struct-of-
  arrays (one `vector<int32_t>` per field) rather than an array of
  `TraceHeader` structs -- selection and sorting each scan one field across
  every trace, and columnar storage keeps that a sequential memory scan
  instead of striding through a 7-int struct per trace for a single field.
  Parallelized across the existing `ThreadPool` the same no-synchronization,
  disjoint-output-range way `buildFinestLevel` already is.
- **`selectTraces()`**: given a list of `IdentCriterion{field, min, max,
  inc}`, returns the original-file trace indices where every criterion
  matches (AND across criteria). `inc` mirrors the reference tool's own
  convention -- a stride over the ident's *value* domain (keep it if
  `(value - min) % inc == 0`), not over trace position.
- **`sortTraceIndices()`**: stable-sorts a list of trace indices by a
  priority-ordered list of ident fields, mirroring the reference tool's
  chained "Sort by" rows.

**The one change this needed in already-tested, shared code**: `buildPyramid()`
(`segy_pyramid.h`/`.cpp`) computed each trace's file offset as a direct
`t * traceStrideBytes` -- no indirection, because nothing before this
needed one. `BuildParams` gained one optional field,
`traceIndexMap` (null = today's identity behavior, byte-for-byte
unchanged); when set, the finest level's inner loop reads
`traceIndexMap[t]` instead of `t` for the file offset, while `t` itself
still drives the pyramid's own block/level structure. This is what lets a
filtered *and reordered* selection become a real pyramid -- rebuilt by
the exact same parallelized decode-and-reduce pass an ordinary load uses --
without ever duplicating the file or touching disk again: the mmap'd
file's pages are already resident in the OS page cache from the original
load (a trace's header lives in the same page as the start of its own
sample data, so even the header scan above never re-hits disk either).

**Verified, not just argued**: `testPyramidTraceIndexMapMatchesDirectSubsetBuild`
builds a pyramid two ways -- once over a source file with an arbitrary,
non-contiguous, duplicate-containing `traceIndexMap`, and once by
physically constructing a second synthetic file whose traces are already
in that exact selected/reordered order with no map at all -- and asserts
every level's min/max/mean/count match. That's the actual guarantee this
feature rests on: reading trace *i* through the map is bit-identical to
the file having really contained the selection in that order.

**Performance, measured with a new `segybench` case** (`benchHeaderSelectSort`,
same 20,000-trace x 1,500-sample synthetic file as the existing pyramid
benchmark, so its numbers are directly comparable to that baseline):

```
Windows:
Header scan (all traces)                    0.674 ms       6793.8 MB/s         29.7 Msamples/s
Select (1 criterion)                        0.072 ms   (10004 of 20000 traces selected)
Sort (1 key)                                0.338 ms   (10004 indices sorted)
Pyramid rebuild over selection              14.487 ms       4109.5 MB/s       1035.9 Msamples/s

Linux / WSL2:
Header scan (all traces)                    0.746 ms       6138.8 MB/s         26.8 Msamples/s
Select (1 criterion)                        0.112 ms   (10004 of 20000 traces selected)
Sort (1 key)                                0.320 ms   (10004 indices sorted)
Pyramid rebuild over selection             19.374 ms       3072.9 MB/s        774.6 Msamples/s
```

Scanning, selecting and sorting together cost under 1.2 ms combined on both
platforms -- immaterial next to the rebuild. And the rebuild itself tracks
the *selected* count: ~50% of the file selected here costs roughly half of
the full 16-thread build's 23-37 ms (see the Microbenchmarks section
below), not the same as a full reload. Selecting a smaller fraction of a
large file is strictly cheaper than the original load; reordering all of
it costs the same as the original load once. Confirms the design goal from
the original investigation: this is bounded by the *result* size, not the
source file's, and adds no new disk I/O.

**Not built yet, on purpose**: the Selection/Sort dialog UI itself, and
wiring a `MainWindow`/`Panel` up to hold a "selected view" pyramid
alongside (or instead of) the file's full one. This section is the
backend those will call into once designed -- see the ident-label-rows
mockup and the earlier investigation for the UI-side plan.

### Multi-box selection: any number of boxes, right-click to pick the active one

The original box-select feature (a single `SelectionBox` on `AppState`,
Delete-marks-then-removes-it) became `std::vector<SelectionBox>
selectionBoxes` plus three transient fields (`activeBoxIndex`,
`draggingBoxIndex`, `draggingCorner` -- moved off the struct itself onto
`AppState`, since only one box is ever mid-drag at a time). Each box gets
a `colorIndex`, assigned once at creation from a monotonic counter
(`nextBoxColorIndex`) into a fixed 10-color palette (`boxColorForIndex()`)
that cycles past its own length rather than reassigning colors as boxes
are added/removed -- a box's color stays meaningful (matches its
Histogram/Spectrum plot, below) even after other boxes are deleted.

Interaction changes from the single-box version: clicking empty space
with `BoxSelect` active always starts a **new** box now, regardless of how
many already exist -- only clicking an existing *complete* box's corner
handle drags it (`hitTestCorner()` now searches every box and reports
which one). Finishing a box's second corner auto-selects it as active
(you just drew it, it's what Zoom/Histogram/Spectrum/Delete should act on
next); right-clicking any complete box's rect re-selects it as active
without needing to finish a new one first. `MainWindow::refreshToolChrome()`
and `zoomToBox()` route through `activeBox()`/`activeBoxMut()` -- two
tiny helpers that centralize the "is there a valid active box" bounds
check every one of those call sites (plus the two Selection-dialog
`showEvent`s and `computeAndShowHistogram`/`Spectrum`) would otherwise
repeat.

Drawing: the active box gets a 2px outline at full opacity; inactive
boxes get a 1px outline at ~67% opacity, so the active one visibly pops
without inactive ones disappearing. All boxes are drawn inside a
`painter.setClipRect()` scoped to the actual plot area (`chrome.plotX`/
`plotWidth` inset by `kCanvasMargin`) -- a box's data-space coordinates
don't move when you pan/zoom, only their pixel projection does, so
without clipping a box could visibly spill into the axis columns or dark
surround once a zoom/pan pushed it outside the current view.

### Histogram/Spectrum: regular axis ticks, gridlines, menus, and color-by-source

Both result windows got the same treatment, reusing the new
`axis_ticks.h` module (below) instead of the ad hoc "4 evenly-spaced
lines" each used to draw independently:

- **Both axes now use `computeNiceAxisTicks()`** (max 11 lines, coarsened
  on overlap) instead of a fixed 4-or-5-line split -- Spectrum's dB axis
  in particular used to show whatever `floor/ceil`-derived endpoints fell
  out of the data (e.g. `-80, -59, -38, -16, 5`); it now shows regular
  steps (`-80, -70, -60, ..., 0`), identical in spirit to the seismic
  canvas's own Time axis.
- **Vertical gridlines** (the X axis) were added to both charts, plus tick
  marks (short perpendicular lines at the axis edge) on top of the
  existing horizontal gridlines -- three small shared drawing helpers
  (`drawVerticalGridlines`, `drawHorizontalGridlinesAndLabels`,
  `drawXAxisTicksAndLabels`) do this identically for Histogram's two
  charts and Spectrum's one, rather than four copies of the same loop.
- **A File/Edit/View/Utilities menu bar** on each window -- both are plain
  `QWidget`s, not `QMainWindow`, so this is a `QMenuBar` added as an
  ordinary child widget in a `QVBoxLayout` above the actual chart (which
  moved into its own inner widget, `HistogramChartArea`/
  `SpectrumChartArea`, for exactly this reason: painting onto `this`
  stopped being correct once something else needed the top of the
  window). File > Close; Edit is intentionally empty, matching this app's
  own convention for a menu bar slot with nothing to do yet; View > Grid
  Lines is a real, working toggle; Utilities > Parameters... reopens the
  same `HistogramDialog`/`SpectrumDialog` the toolbar's own buttons do --
  `MainWindow::openHistogramDialog()`/`openSpectrumDialog()` were factored
  out of the toolbar actions' lambdas specifically so the toolbar button
  and this menu item don't duplicate the lazy-create/show/raise logic.
- **Bars/line color reflects where the data came from**: white for Full
  View, or the originating selection box's own color for a Selection --
  `resolveAnalysisSource()` (one function, called by both
  `computeAndShowHistogram` and `computeAndShowSpectrum`) resolves the
  Full View/Selection radio into both the trace/sample range *and* this
  color in one place, returning an `AnalysisSource` the two callers just
  read from instead of each re-deriving the range and separately guessing
  a color.
- **Cumulative Percent switched from a smoothed line to the same bins as
  Percent of Total** (bar-for-bar, same `barSlot` positions, just scaled
  to `cumulativePercent` instead of `percentOfTotal`), and its background
  is now literally `Qt::black` rather than this app's usual dark canvas
  tone -- both per the original request. The old smoothed-curve version
  had an unrelated real bug worth recording: `painter.drawRect(bottomChart)`
  for the border was drawn right after the bar loop above it without
  resetting the brush, so it silently inherited the bars' orange fill,
  painting the whole chart solid orange behind the curve. Rebuilding the
  chart from shared helpers (which each manage their own pen/brush state)
  avoided reintroducing that class of bug rather than just patching the
  one call site.

### `axis_ticks.h`: one "nice tick" implementation, not two

The seismic canvas's Time axis and the Histogram/Spectrum plots above
both want the same behavior -- finest "nice" (1-2-5 x 10^k) interval with
at most 11 lines, coarsened just enough that labels stop overlapping --
so that logic was extracted from `chrome.cpp` (which had it inline,
Time-axis-specific) into a small, generic, Qt-free module,
`viewer/axis_ticks.h`/`.cpp` (part of `segyviewercore`, tested by
`segytest`, callable from Qt code and from `segytest` alike):
`computeNiceAxisTicks(rangeMin, rangeMax, pixelSpan, maxLines,
measureWidth, formatValue = nullptr)` returns a plain `vector<AxisTick>`
(value + pixel position + label), with no notion of "time" or "samples"
baked in. `chrome.cpp`'s own tick computation now just converts its
sample range to display-unit space and calls this, unchanged in behavior
(verified: every existing Chrome test -- tick-count cap, overlap
avoidance, unit switching -- passes identically before and after the
refactor) and shorter by about 30 lines for it.

### EBCDIC text header: detect plain-ASCII headers instead of mangling them

A real user-reported bug: `decodeEbcdicText()` unconditionally ran every
text header through the EBCDIC-to-ASCII table, but not every SEG-Y file's
text header is actually EBCDIC -- files written by modern, non-mainframe
tools (the reported case: a file exported by OpendTect) sometimes write
plain ASCII instead, and nothing in the file format itself flags which
one was used. Running ASCII bytes through the EBCDIC table produces pure
gibberish. Fixed by detecting which encoding a header actually is first:
count bytes in ASCII letter/digit ranges (`0-9`, `A-Z`, `a-z`) versus
EBCDIC's own letter/digit ranges (`0xC1-0xE9`, `0xF0-0xF9`) -- entirely
disjoint ranges, so real text in either encoding lands almost exclusively
in one count or the other -- and trust ASCII when it wins. Counting
letters/digits specifically, rather than "printable ASCII" broadly, matters:
EBCDIC space is `0x40`, which happens to equal ASCII `'@'`, so a genuine
EBCDIC header that's mostly blank-padded (common -- often only the first
few of its 40 lines hold real text) would otherwise get misread as "mostly
printable ASCII" purely from its own padding. Covered by a new test,
`testEbcdicDecodeDetectsAlreadyAsciiHeader`, alongside the existing
EBCDIC-path test (both pass; the padding-collision case specifically is
what the existing test's all-`0x40`-plus-two-bytes buffer already
exercised, just needed the letter/digit-only counting to not trip over it).

### File > Open: a Linux-specific "dialog opens blank" bug, and a default filter

Two smaller fixes to `SegyOpenDialog`:

- **Reported on Linux**: clicking File > Open showed the dialog briefly,
  then it visually disappeared, reappearing only after moving the main
  window. Root cause: `MainWindow::openFile()` constructed and `exec()`'d
  the new modal dialog directly inside the "Open..." `QAction`'s
  `triggered` handler -- i.e. synchronously, while the menu that was just
  clicked was still in the middle of closing. Under (at least) WSLg's xcb
  compositor, that raced the menu's own closing X11 events against the new
  dialog's first paint, leaving it mapped but unpainted until a later,
  unrelated window-manager event (moving the parent sends a
  `ConfigureNotify`) forced a redraw. Fixed with the standard remedy for
  this class of bug: `QTimer::singleShot(0, this, [...]{ ... })` defers
  the dialog's construction to the next event-loop turn, letting the menu
  fully close first -- an imperceptible delay that sidesteps the race
  entirely.
- **Default filter**: the Filter box now starts at `*.sgy *.segy *.SGY
  *.SEGY` instead of a bare `*` -- multiple space-separated glob patterns
  at once, the same convention a native file-open dialog's own filter box
  uses. `QFileInfo::fileName()` only ever returns one path component, so
  parsing this needed two small helpers, `filterDirectory()`/
  `filterPatterns()`, splitting on the last `/` and then on spaces;
  `navigateTo()` now carries the current pattern(s) into a newly-entered
  directory instead of resetting to `*`, matching how a native dialog's
  filter survives changing folders.

### Code organization: toolbar icons split into their own file

`viewer_qt.cpp` had grown very large over the course of this app's
features (past 3200 lines). The toolbar icon factories (`iconMagnifier`,
`iconLock`, `iconChevronUp`, ... -- about a dozen functions, roughly 300
lines including their embedded SVG source) were the cleanest extraction
target: pure Qt drawing plus `segy::divergingColormap`, with zero
dependency on `AppState`/`SegyCanvas`/`MainWindow` or any other type
defined in `viewer_qt.h`. Moved verbatim to `viewer_qt.h/.cpp`'s new
sibling files, `viewer/qt/viewer_qt_icons.h`/`.cpp`, declared as ordinary
`namespace segyqt` functions (not anonymous-namespace/internal-linkage
like before, since they're now called from a different translation unit)
and included back into `viewer_qt.cpp`. Shrinks the main file by about
300 lines for essentially zero behavioral risk -- nothing about how the
icons are drawn or referenced changed, confirmed by a full rebuild and a
real screenshot showing every toolbar icon rendering identically to
before.

### Zoomed-out rendering: trace decimation instead of pyramid-block averaging

`renderExactPath` used to only handle views small enough to decode every
visible trace within `kExactDecodeSampleBudget`; anything wider fell back to
`renderPyramidPath`, which colored each pixel by bilinearly interpolating a
pyramid block statistic (originally `mean`, briefly changed to peak
magnitude — see git history) across its 4 nearest blocks. Both variants had
the same structural problem: blending *statistics of* nearby traces, rather
than real trace data, either washed out amplitude contrast (mean) or made
the coarse block grid itself visible as a "staircase" once zoomed out far
enough (either statistic) — reported as the display looking "blotchy."

Replaced entirely with decimation: `renderExactPath` now always decodes real
trace samples at full vertical resolution, but once the visible trace range
would need more samples than the budget to decode in full, it decodes only
every `stride`-th trace instead (`stride` chosen so total decoded samples
stay within budget, also capped at ~2x the pixel width since decoding more
traces than screen columns can distinguish would only waste time). Each
*decoded* trace is still its true, unaveraged samples — a heavily
zoomed-out view now reads as "fewer real traces shown," not "the same
traces blurred together." Screen columns map to the nearest decoded trace
by nearest-neighbor (not bilinear blend): blending real samples from two
traces several skipped traces apart would recreate the same smearing
decimation is meant to avoid, so the display instead steps cleanly from one
real trace to the next as the view crosses the midpoint between them.
Vertically, sample-to-pixel mapping is unchanged (bilinear, as before) —
only the trace axis's coarsening strategy changed.

This made `renderPyramidPath`, `pickLevel`, and the block-oriented
`bilinearBlockIndex` helper entirely dead code (nothing else in the
codebase called them — confirmed via search before deleting), so they were
removed rather than left unused. `Pyramid`/`PyramidLevel` themselves are
untouched and still built/used for their global min/max (clip defaults) and
by the wiggle display's own, unrelated min/max envelope reduction
(`wiggle.cpp`) — only the variable-density color-fill path stopped
consuming per-block statistics.

### Color scale selection: five palettes, right-click the amplitude legend

`include/colormap.h` grew from one hardcoded diverging colormap to a
`segy::ColorScale` enum (`RedWhiteBlue`, `RedWhiteBlack`,
`GrayscaleWhiteToBlack`, `GrayscaleBlackToWhite`,
`YellowRedWhiteBlackCyan`) dispatched through `applyColorScale()`. The first
four are 2-3 stop gradients; the fifth reuses a new generic
`multiStopColormap()` N-stop linear interpolator (yellow → red → white →
black → cyan) rather than writing a fifth bespoke lerp. `DisplaySettings`
carries the active scale (`colorScale`, default `RedWhiteBlue`) and both
render paths read it fresh every frame, same as every other display
toggle.

Picked via a right-click context menu on the amplitude-scale legend
(`AmplitudeScaleWidget`) — previously that widget was
`Qt::WA_TransparentForMouseEvents` (deliberately click-through, so canvas
interactions underneath weren't blocked); that flag was removed to let it
receive the right-click, which is safe since the legend sits in a corner
overlay the canvas's own mouse handling doesn't otherwise need. The menu is
a plain `QActionGroup`-checked list, scoped to that one panel only (not
Lock-aware like the toolbar's gain/mode controls) since it's triggered from
one specific panel's own overlay widget, not a shared control — there's
never ambiguity about which panel it means.

Also swapped two toolbar icons this pass, unrelated to color scales but
same session: the "Display Parameters" action's caret-up-down icon (looked
like a generic sort control, not seismic-display-parameters-specific) for
Tabler's "adjustments" glyph (three vertical sliders); and added
`iconLayoutRows()` so the Split View button's icon actually flips to match
the current split orientation (columns icon for a horizontal split,
rows icon for vertical) instead of staying fixed regardless of state.

### Interactive clip dialog: manual asymmetric clip via histogram + draggable lines

Added `segy::ManualClip` (`renderer.h`): an optional override
(`enabled`, `posMagnitude`, `negMagnitude` — both always-positive
magnitudes, one per sign) that composes with `gainDb` rather than replacing
it. A shared `effectiveClipPosNeg(rawClip, gainDb, manualClip, &clipPos,
&clipNeg)` helper (used identically by `renderPyramidPath` and
`renderExactPath` so the two paths can't drift apart) picks
`manualClip`'s two magnitudes over the pyramid's symmetric global-extremes
clip when enabled, then applies `clipForGain` to each side independently —
so the toolbar gain slider keeps working exactly as before regardless of
whether a manual clip is active. `DisplaySettings::manualClip` carries the
live value; `DisplaySettings::clipLocked` is a plumbed-through bool
reserved for a future "keep this scale fixed across reprocessing" feature
but not enforced by any code path yet.

UI: a new "Clip..." entry on the amplitude legend's right-click menu (next
to the color-scale picker) opens a non-modal `ClipDialog` — a histogram
(`ClipHistogramWidget`, reusing `segy::computeHistogram` over the
*currently displayed* trace/sample viewport, a third scoping mode distinct
from the Histogram/Spectrum dialogs' Full View/Selection) with two
draggable vertical lines marking `-negMagnitude`/`+posMagnitude`. Each line
is stored as a positive magnitude on its own side of zero, so by
construction they can never cross or swap sides. A "Symmetrical" checkbox
(default on) makes dragging or typing either value mirror it into the
other; turning it back on snaps both to whichever magnitude was larger,
rather than leaving them silently mismatched until the next drag. Every
drag and every spin-box edit pushes straight into
`AppState::display.manualClip` and repaints live (via the existing
`SegyCanvas::notifyStateChanged()` → `MainWindow::refreshToolChrome()` →
`refreshAmplitudeScale()` hookup, the same path gain changes already used —
no new callback plumbing needed). Apply re-bins the dialog's own histogram
around the current clip range (so dragging near an edge reveals more
headroom); Cancel restores whatever `ManualClip` was in effect when the
dialog opened; OK does the same as Apply and closes.

`AmplitudeScaleWidget::setClipRange(posClip, negClip)` (replacing the old
single-value `setClip`) draws the gradient/labels asymmetrically: the
zero point sits at `posClip/(posClip+negClip)` of the bar's height instead
of always at the midpoint, so a lopsided clip is visually honest about it.
`MainWindow::refreshAmplitudeScale()` computes both sides the same way
`effectiveClipPosNeg` does (duplicated rather than exported for this one
caller, matching this file's existing convention for small shared math).

### Box delete: middle-click instead of a status-bar button

The status-bar "Del" button (delete the right-click-selected box) was
replaced with a middle-click on the canvas: `SegyCanvas::handleBoxSelectPress`
now handles `Qt::MiddleButton` by deleting `activeBoxMut(app_)` directly and
repainting, and the button/its wiring were removed from `MainWindow`
entirely rather than left as a second, redundant way to do the same thing.
Scoped to the clicked panel only (not Lock-routed like the old button was)
since box selection itself is already inherently per-panel — right-clicking
to select an active box only ever sets that one panel's own
`activeBoxIndex`.

### Display Parameters: wiring up Reverse Polarity, Centerline, and fill sides

Three of `DisplayParametersDialog`'s checkboxes went from stored-but-unread
placeholders to real rendering behavior:

- **Reverse Polarity** flips the sign of every displayed sample. Since it
  needs to affect both display modes identically, it moved from
  `WigglePresentationSettings` (Qt-shell-only, wiggle-specific) to
  `segy::DisplaySettings::reversePolarity` (chrome.h) — the same struct
  `colorScale`/`manualClip`/`gainDb` already live on and that both
  `renderFrame` (variable density) and `computeWiggleLayout` (wiggle) now
  take an explicit `reversePolarity` parameter for, applied as `value =
  -value` right before clip/color in the former and inside
  `pixelXForValue` in the latter — after gain/clip magnitude is computed
  (which stays symmetric either way), so only which color/side a sample
  maps to changes, not how "loud" it reads.
- **Centerline** draws each wiggle trace's own zero-amplitude line
  (`tr.baselineX`, full plot height, light gray) before its wiggle
  polyline, so the actual trace stays the visually dominant line at each
  crossing.
- **Positive Fill** / **Negative Fill** now independently gate which side
  of the baseline gets filled, instead of the fill being an unconditional
  "positive side only" whenever the View menu's master Wiggle Infill
  toggle was on.

Wiring up real fill sides surfaced a pre-existing bug in the fill geometry,
made obvious once Centerline made the true zero-crossing visible right next
to the fill: a segment that crosses the baseline was filled as a single
quad built from `(baselineX, y_i)` to `(x_{i+1}, y_{i+1})`, i.e. it
implicitly assumed the crossing fell exactly at one segment endpoint's own
Y — the fill polygon never actually computed *where* the trace crosses
zero. Whenever the true crossing fell partway through the segment instead,
the filled lobe's tip visibly bulged past the point where the wiggle line
itself reaches zero. Fixed by computing the exact linearly-interpolated
crossing (`t = a0 / (a0 - a1)`, `yCross = y0 + t*(y1-y0)`, where `a0`/`a1`
are the two endpoints' signed distance from the baseline) and splitting the
segment into up-to-two triangles at that point — one per side, each only
emitted if that side's fill is enabled. A same-side segment (both endpoints
positive, or both negative) still short-circuits to a single plain quad, no
crossing math needed.

### Histogram/Spectrum: default to Selection when a box is active

`HistogramDialog`/`SpectrumDialog::showEvent` used to only *gate* the
Selection radio's availability (disabled with no complete active box,
falling back to Full View only if Selection was already checked while
becoming unavailable) — otherwise leaving whatever was checked from the
dialog's previous open. Changed to actively default to whichever the
current box state actually supports every time the dialog opens: Selection
when a box is active, Full View otherwise. Opening either dialog right
after selecting a box no longer needs an extra click to switch the radio
over.

### Flip Horizontal: mirroring around the untouched shared render paths

New toolbar toggle (`iconArrowsHorizontal`, Tabler's "arrows-horizontal") that
mirrors the display left-right, backed by `DisplaySettings::flipHorizontal`.
Implemented entirely in the Qt shell rather than teaching `renderer.cpp`/
`chrome.cpp`/`wiggle.cpp` about it, so none of that tested shared code
changed:

- **Variable density**: after `renderFrameWithChrome` fills the normal,
  unmirrored raster, `SegyCanvas::paintEvent` reverses each row's plot-region
  pixels in place (`std::reverse` over `[chrome.plotX, chrome.plotX +
  chrome.plotWidth)`) before blitting to the `QImage`. Grid lines are
  unaffected since each is blended as a uniform value across the whole row.
- **Wiggle**: each `WiggleTrace`'s `baselineX` and every line-point's `x` are
  mirrored (`plotWidth - x`) right after `computeWiggleLayout` returns, before
  building the fill/centerline/polyline geometry — a true mirror-image
  reflection, so a trace's own excursion direction flips along with its
  position (a peak that pointed right now points left, at its new, also
  mirrored, screen column), consistent with what mirroring the raster image
  does implicitly.
- **Mouse/coordinate mapping**: `SegyCanvas::dataCoordAt`/`pixelForData` (the
  single choke point for hover, box-select, and corner-drag) un-mirror/
  mirror the pixel coordinate around the plot's own width before/after the
  normal `view`-based mapping, so hovering, drawing, and dragging box corners
  all land on the correct real trace regardless of flip state — this is also
  what makes the dock's Trace Info panel automatically correct with no
  separate fix, since it already reads its trace/sample position through
  `dataCoordAt`.
- **Wheel-zoom and pan**: `wheelEvent` un-mirrors the cursor's pixel position
  before calling `zoomAt` (renderer.cpp), so scroll-to-zoom still centers on
  the real trace under the cursor. Panning needs the opposite fix — negating
  `dx` before calling `panFromAnchor` — since (confirmed by working through
  concrete pixel numbers, not just symbolically) leaving `dx` unmodified
  would shift the mirrored view in the direction *opposite* the drag.

### Producing and displaying a new dataset: Calculator, Bandpass, Save SEG-Y, Octave Bands

Until now the app only ever *read* SEG-Y. These four features (all approved
via mockups first) needed a way to produce a new dataset and show it, so
that machinery was built once and shared:

- **`segy_writer.h/.cpp`** (`segycore`) — `writeSegyFile()` writes a
  complete, valid SEG-Y: EBCDIC text header (new `encodeEbcdicText()`, built
  as the exact inverse of the existing decode table rather than a second
  hand-typed one), binary header (new `writeBinaryHeader()`, inverse of
  `parseBinaryHeader`), then each trace's 240-byte header copied verbatim
  from the source plus its samples. **Output is always IEEE float32 (format
  5)**, never re-quantized into the source's original format — sidesteps
  needing an IBM-float encoder and loses nothing computed samples need.
  Round-trip tested through the normal read path (`MappedFile`,
  `parseBinaryHeader`, `decodeEbcdicText`, `decodeSamples`).
- **`fft.h/.cpp`** — the radix-2 FFT moved out of `analysis.cpp` (verified
  behavior-preserving: Spectrum's test still passes) and gained `ifft`
  (conjugate trick). **`bandpass.h/.cpp`** — `applyBandpassFilter()`: FFT,
  multiply by a Hanning (raised-cosine) tapered trapezoid gain mask mirrored
  around Nyquist so real input stays real, inverse FFT. Tested on
  bin-aligned sines (passband tone reconstructed to <0.01, out-of-band tone
  and an all-stop filter suppressed).
- **Displaying a result**: rather than a parallel in-memory-dataset code
  path, Calculator/Bandpass write the result to a scratch `.sgy` under the OS
  temp dir (`segyread_scratch/<name>.sgy`) and load it through the existing,
  tested `startLoading()`/pyramid pipeline into **`panelA_`** (the left/only
  panel, per the spec). `startLoading` gained an optional
  `processingHistory` argument stored on `AppState::processingHistory`
  (empty for File > Open) so a chain of derived datasets keeps a record.
- **Edit > Calculator**: Add/Subtract two loaded panels' datasets (each
  operand combo lists whichever panels are loaded), default name
  `A_Plus_B`/`A_Minus_B` using the real dataset names (file stems), editable
  until you type in it. Clipped to the overlap if sizes differ. Dataset A's
  trace headers carry over. Undo/Redo removed from Edit as asked.
- **Edit > Processing > Bandpass**: four corner spin boxes (default
  5-10-50-60 Hz, kept ordered by pushing neighbors), applied to every trace
  of the *active* panel at Apply time.
- **File > Save SEG-Y**: writes the active panel's dataset with a
  synthesized 40x80 text header (`buildSegyTextHeader`, shared with the two
  above): idents, input data, numbered processing history, output dataset.
  The dialog previews that exact text.
- **Octave Band Display** (toolbar, `columns-3` icon): filters the *visible*
  traces/samples (current view) of the chosen Left/Right panel into
  unfiltered + 2^N Hz octave bands (min/max are power-of-2 combos), each with
  a 15% taper past its edges, rendered **variable-density** (per the mockup
  feedback), each band auto-clipped to its own peak. Capped at 80 evenly
  spaced traces per band — a look tool, not an exhaustive filter.
- **Known limitation**: Calculator/Bandpass/Save run synchronously on the UI
  thread behind a wait cursor (Histogram/Spectrum's existing precedent), but
  unlike those they must touch every trace, so a huge file will freeze the UI
  for its duration. Backgrounding it like file loading is the follow-up.

### Building against Qt6 or Qt5 (Rocky/RHEL 8 "compatible" build)

`CMakeLists.txt` picks Qt6 when found (Windows/vcpkg, Ubuntu) and falls back
to Qt5 >= 5.15 (Rocky/RHEL 8's AppStream only has Qt5); force one with
`-DSEGY_QT_MAJOR=5|6`. It uses `qt_add_executable`/`qt_standard_project_setup`
only under Qt6, and links `stdc++fs` only for GCC < 9 (never MSVC).
`cmake_minimum_required` dropped 3.21 -> 3.16. The one Qt5-only source change
is enabling `AA_EnableHighDpiScaling` (Qt6 has it always on); the Qt6-looking
APIs (`QActionGroup::ExclusionPolicy`, `horizontalAdvance`,
`setHighDpiScaleFactorRoundingPolicy`, `QWheelEvent::position`) all exist in
Qt 5.15.

`build.sh` keeps the two toolchains in separate directories so their CMake
caches never collide (a shared cache broke both): Ubuntu/Debian ->
`build-linux/` (Qt6), Rocky/RHEL -> `build-linux-compatible/` (Qt5, uses
gcc-toolset if installed). `--install-deps` and `--clean` are opt-in; a plain
run never installs packages or deletes anything. **The Qt5 path is verified
only by the user's Rocky container, not in CI** -- Ubuntu (Qt6) and Windows
(Qt6) both build and pass all 87,685 checks.

## Microbenchmarks (`segybench`)

Measured on the same physical machine (16 logical cores, AVX2 available),
both natively on Windows and under WSL2 for Linux. Reproduce with
`segybench [path-to-a-real-segy-file]`.

```
Windows (MSVC /O2 /arch:AVX2):
== IBM float32 decode throughput ==
Scalar (general bit-scan)                 114.615 ms        558.4 MB/s        146.4 Msamples/s
AVX2 (8-wide, common-case fast path)       19.672 ms       3253.3 MB/s        852.8 Msamples/s

== Pyramid build throughput (synthetic file, 20,000 traces x 1,500 samples, IBM float) ==
1 thread                                   57.606 ms       2066.1 MB/s        520.8 Msamples/s
16 threads (hardware_concurrency)          23.313 ms       5105.5 MB/s       1286.9 Msamples/s

Linux / WSL2 (g++ 13.3 -O2 -mavx2):
== IBM float32 decode throughput ==
Scalar (general bit-scan)                  95.223 ms        672.1 MB/s        176.2 Msamples/s
AVX2 (8-wide, common-case fast path)       20.920 ms       3059.2 MB/s        802.0 Msamples/s

== Pyramid build throughput (same synthetic file) ==
1 thread                                   76.511 ms       1555.6 MB/s        392.1 Msamples/s
16 threads (hardware_concurrency)          31.633 ms       3762.6 MB/s        948.4 Msamples/s
```

Wiggle layout computation (added alongside the wiggle display feature; see
"Design choices"), measured the same way, synthetic 50,000-trace x
2,000-sample file, fully zoomed out (the worst case for trace decimation)
into a 1600x900 plot:

```
Windows: Wiggle layout   2.954 ms   (533 traces drawn out of 50,000 in file, ~959,400 points/frame)
Linux:   Wiggle layout   2.319 ms   (533 traces drawn out of 50,000 in file, ~959,400 points/frame)
```

533 traces drawn regardless of the file having 50,000 — confirms the
trace-decimation bound (`plotWidth/kMinPixelsPerTrace` = 1600/3 ≈ 533) is
actually being hit, not just theorized. Bilinear interpolation's cost on
the existing raster paths (see "Design choices") was measured qualitatively
via the status line's live render-time readout rather than added as a
dedicated `segybench` case: it stayed in the same single-digit-millisecond
range as before (nearest-value) at typical window sizes on both platforms.

Takeaways:
- **AVX2 gives ~5.5-6x** over the (already nontrivial, fully-general) scalar
  decoder, on both compilers.
- **Multithreading gives ~2.5-2.7x, not 16x**, going from 1 to 16 threads on
  the pyramid build, on both platforms. This is memory-bandwidth-bound, not
  compute-bound: one core can already decode at ~3+ GB/s via AVX2, so 16
  cores decoding concurrently would need on the order of 50 GB/s of memory
  bandwidth to scale linearly, which exceeds typical desktop memory
  subsystems. Reported as measured rather than tuned to look better — this
  is the honest ceiling for a streaming decode+reduce workload on this
  hardware.
- The Windows/Linux numbers are close (within ~15-20%) but not identical,
  as expected: same silicon, but WSL2 runs Linux in a lightweight VM with
  its own scheduler behavior, and the two are different compilers (MSVC vs
  GCC) with different codegen for the same intrinsics.
- Pass a real `.sgy` file path as `argv[1]` to also get a memory-mapped
  sequential page-touch throughput number for that file.

## Correctness test harness (`segytest`)

Self-contained (no external test framework, matching the "standalone,
portable" brief); exit code is the failure count. Current status: **87,685
checks, 0 failures — identically on MSVC/Windows and GCC 13.3/Linux**,
including the AVX2-vs-scalar bit-exactness check on both compilers'
codegen. Coverage:

- IBM-float conversion against hand-derived known values, plus a 20,000-case
  encode-then-decode round trip via an independently-written reference
  encoder (`ieeeToIbmBitsReference`) so the test doesn't share bugs with the
  implementation it's checking.
- AVX2 output checked bit-for-bit against the scalar reference across
  randomized inputs of varying lengths (exercises both the 8-wide path and
  the scalar tail).
- Binary/trace header field parsing at exact byte offsets.
- EBCDIC→ASCII text header decode.
- `decodeSamples` for every supported format code (IEEE float32/float64,
  int32/int16/int24/int8).
- Pyramid finest-level and coarser-level stats checked against brute-force
  computation over raw synthetic data, including **ragged edges** (trace/
  sample counts not multiples of the block factor) and exact global min/max.
- Degenerate inputs (zero traces/samples) don't crash.
- Plot chrome (`chrome.cpp`): the 11-line cap across many view/canvas-size
  combinations, overlap-avoidance coarsening under a deliberately short
  canvas, ms/s/µs unit-selection thresholds (including at real SEG-Y field
  limits, not arbitrary large numbers), color-bar polarity, and that
  disabling each `DisplaySettings` flag collapses its margin to zero width.
- Wiggle geometry (`wiggle.cpp`): trace-count decimation never exceeds its
  bound across several view sizes, per-trace point count stays bounded even
  with a huge visible sample range, every emitted point stays within
  plot-local bounds, and the min/max envelope reduction is checked directly
  against brute-force computation over synthetic data (same rigor as the
  pyramid's own coarse-level tests) — see "Design choices" for why this
  needs a real temp file + `MappedFile` rather than the in-memory-buffer
  shortcut the chrome tests use.

Run: `native\build-windows\Release\segytest.exe` (Windows) or
`native/build-linux/segytest` (Linux).

## Manually verifying the viewer

Both platforms now run the same `viewer_qt.cpp` shell; `gen_fixture[.exe]
[out.sgy]` writes a synthetic 4,000-trace × 1,500-sample file with
sinusoidal "reflectors" for a quick visual check without a real dataset on
either platform. Confirmed on both: `segyviewer[.exe] [path.sgy]` opens a
file at startup or via File > Open / Ctrl+O (`QFileDialog`); the menu bar
(File/Edit/Selection/View/Help) renders correctly with only File > Open and
File > Exit wired to anything; the density plot, color bar, and
both-sided rotated time scale (including the dynamic-coarsening behavior —
shrinking the window's height until the tick interval visibly jumps from
200ms to 500ms) all render correctly and pixel-identically in structure
between platforms (mirrored scales, same colors, same rotation direction);
and mouse-drag pan, wheel zoom, and 'r'-to-reset all update the status line
with a live per-frame render time.

**Windows-specific setup gotchas found and fixed while verifying**, neither
visible from code review:

1. **`segyviewer.exe` crashed instantly on launch** (`0xC0000409`, a
   stack-cookie/`__fastfail` failure inside `Qt6Core.dll` itself, confirmed
   via the Windows Application event log). Root cause: vcpkg's automatic
   post-build DLL copy (`VCPKG_APPLOCAL_DEPS`) only walks the executable's
   direct PE import dependencies (`Qt6Core.dll`, `Qt6Gui.dll`,
   `Qt6Widgets.dll`, transitively ICU/zlib/freetype/etc.) — it doesn't know
   about Qt's *platform plugin* (`platforms/qwindows.dll`), which
   `QGuiApplication` loads dynamically via `QPluginLoader` at startup.
   Without any usable platform plugin, Qt has no windowing backend and
   crashes hard in its own init code rather than failing gracefully.
   `windeployqt` normally handles this, but vcpkg's Qt6 port doesn't ship
   it. Fixed with an explicit `add_custom_command(... POST_BUILD)` in
   `CMakeLists.txt` that copies `qwindows.dll` into a `platforms/`
   subdirectory next to the built exe, deriving the source path from
   `Qt6::Widgets`'s own `IMPORTED_LOCATION` rather than hardcoding a vcpkg
   path.
2. **Blurry, apparently-cropped rendering on a 150%-scale display**: the
   built exe had no Windows application manifest declaring DPI awareness,
   so Windows silently bitmap-scaled the whole window for compatibility —
   the app itself only ever drew at 1200×800 physical pixels while Windows
   stretched that to ~1800×1200 on screen. Confirmed via
   `GetWindowDpiAwarenessContext` (returned `DPI_AWARENESS_UNAWARE`-shaped
   behavior) and by comparing `GetClientRect`'s reported size against the
   actual DPI scale factor. Fixed by embedding
   `viewer/qt/app.manifest` (declares `PerMonitorV2` via
   `<dpiAwareness>`) as a source file on the `segyviewer` CMake target —
   MSVC's linker embeds a `.manifest` file added to a target's sources
   automatically. After the fix, `GetWindowDpiAwarenessContext` correctly
   reports `PER_MONITOR_AWARE`, and the app renders crisply at native
   physical resolution instead of being upscaled.

**Screenshot-tooling artifacts found while verifying on both platforms**
(worth recording since they cost real debugging time and could otherwise
be mistaken for rendering bugs in a future session):

- **On Linux under WSLg, `PrintWindow`-based screenshots of the
  Windows-side RDP proxy window silently dropped the right edge of wide
  windows** — the right-side time scale and "Time (ms)" title appeared
  completely missing in every such screenshot, with no black margin at
  all, indistinguishable from a real layout bug. Debug `fprintf`s
  confirmed the layout math (`chrome.rightScaleX`, `rightScaleWidth`) was
  correct, and a `painter.fillRect()` probe filling the exact reserved
  margin also failed to show up in the `PrintWindow` capture. Ground
  truth came from a small standalone Xlib tool (`XGetWindowAttributes` +
  `XGetImage`/`XGetPixel`, dumped as a raw pixel file and reconstructed
  into a `.png` on the Windows side) grabbing the actual X11 window
  directly, bypassing WSLg's RDP-based window mirroring entirely — this
  showed everything rendering correctly. **Lesson**: don't trust
  `PrintWindow` captures of WSLg-proxied windows for anything near a wide
  window's right edge; grab the X11 window directly instead.
- **On native Windows, `PrintWindow` also appeared to crop the right
  edge** immediately after the DPI-manifest fix above — because the
  bitmap passed to `PrintWindow` was sized from `GetWindowRect` in a
  *different, non-DPI-aware calling process* (a PowerShell script), and
  Win32's rect-query APIs silently DPI-virtualize their result based on
  the *calling* thread's own awareness context when querying a
  higher-awareness target window — not a real app bug. Confirmed by
  retrying with a bitmap deliberately sized for the true physical
  (DPI-scaled) resolution, which then captured the full window correctly.
  **Lesson**: when screenshotting a per-monitor-DPI-aware window from a
  script/tool that isn't itself declared per-monitor-aware, don't trust
  `GetWindowRect`/`GetClientRect` for sizing the capture buffer — either
  make the caller per-monitor-aware too (`SetThreadDpiAwarenessContext`)
  or just over-allocate.

**Historical FLTK-era bugs** (blank canvas from the wrong Xlib drawable,
`Fl_Native_File_Chooser`'s GTK ABI break, `fl_draw(angle,...)` failing
after repeated calls) are documented in "GUI toolkit history" above rather
than here, since that code no longer exists — kept for institutional
memory, not as open items.

**Environment quirks specific to WSLg** (not application bugs, confirmed
across multiple debugging sessions): `CopyFromScreen`-based screenshots
intermittently miss newly composited content right after a resize
(`PrintWindow` on the target's own HDC is reliable); WSLg can leave a
frozen "ghost" proxy window behind after its Linux process exits, which
still responds to `PrintWindow`/`MoveWindow` — match on the
`(Ubuntu-24.04)`-suffixed window title to find the window actually backed
by a live process; and scripted `xdotool` clicks on this app's menu bar
were never reliable under this specific WSLg/Weston setup, making
hand-testing the practical way to confirm interactive menu items.

**Variable-density smoothing and wiggle display** were both visually
confirmed on both platforms: smoothing via direct comparison against the
pre-change nearest-value screenshots at the same zoom level (no hard pixel
edges, same overall shape); wiggle via a real 4,000-trace synthetic fixture
at both a wide zoomed-out view (dense, as expected — see "Design choices"
for why that's realistic rather than a bug) and zoomed into a few dozen
traces (individual lines and positive-only black infill both correct), plus
the 56,726-trace shaped fixture fully zoomed out specifically to confirm
trace decimation keeps it fast (1.19ms, matching the `segybench` number).
The **View > Seismic Display / Wiggle Infill menu items' actual dropdown
was not captured on screen** — a mouse click on the "View" menu bar item
was confirmed to register (it highlights correctly, proving clicks do
reach the menu bar under this WSLg/xcb setup, unlike the FLTK-era
`xdotool` unreliability noted above), but the open dropdown itself is a
separate override-redirect X11 window invisible to a per-window
`XGetImage` capture, and capturing the X11 root window directly to see it
failed with a `BadMatch` under this specific Xwayland setup. What *was*
confirmed instead: the underlying `DisplaySettings` field each menu item
flips renders correctly in both states (verified by exercising it
directly), which is the part with real logic; the menu wiring itself is a
few lines of standard `QAction`/`QActionGroup` code with no exotic risk.
Treat the final "click Wiggle in the open dropdown" step as the one
specific thing not directly observed, not the feature logic behind it.

### Dataset pool: Foreground/Background

Until now each panel's `AppState` held exactly one loaded dataset's fields
(`file`, `binHeader`, `pyramid`, etc.) directly, embedded. That assumption
is gone: a new `Dataset` struct bundles those fields (plus a `name` and
`processingHistory`, both moved here from `AppState`), and `AppState` now
holds `foreground`/`background` as `std::shared_ptr<Dataset>` — never
`nullptr` (defaults to a fresh empty `Dataset`, so every existing call site
that read e.g. `pyramid.globalMin` without a `loaded` check first, relying
on a zero default, stays safe unchanged). Switching a slot to an
already-loaded dataset is an O(1) pointer swap, never a reload — the whole
point of a pool.

**Invariant: never mutate a `Dataset` in place once constructed.** It may
be shared with the pool and/or another panel's other slot; always build a
new one and reassign the `shared_ptr` (see `SegyCanvas::startLoading`'s
completion handler). This was a real bug caught while writing the
refactor, before it ever ran: the original code mutated
`app_.foreground->file = std::move(...)` directly, which would have
corrupted whichever pool entry/other slot still pointed at the *previous*
dataset.

Converting the ~120 call sites that read the moved fields (every render
call, every analysis/Calculator/Bandpass/Save-SEGY/hover-info site) was
done as a scripted, exactly-anchored rename (`app_.binHeader` →
`app_.foreground->binHeader`, etc., one prefix+field pair at a time,
excluding `ctx.`/`result.` which are different, unrelated structs with
same-named fields) rather than by hand — the compiler then enforces
completeness (any missed spot is a compile error, not a silent bug) — this
is also why `AppState::loaded`/`loading` were deliberately left as
independent plain `bool` fields rather than derived from `foreground !=
nullptr`: the existing code already had a window (during/after a failed
load) where `loaded` and "does `foreground` point at something real"
weren't quite the same thing, and preserving that exact timing mattered
more than the derivation being conceptually cleaner.

**UI**: two small boxes per panel ("Foreground N"/"Background N"),
overlaid on the canvas (same technique as `AmplitudeScaleWidget`) at the
top, flush with that panel's own plot left axis (`lastPlotX_`) — so in
split view, panel 2's boxes land near the screen's middle (where its own
plot begins), not the window's actual left edge, and both panels' boxes
stay at the top regardless of split orientation (each is simply
positioned relative to its own panel). Each box's small list-icon button
opens `DatasetPickerDialog` (OK/Delete/Cancel, matching the reference
tool this was modeled on) scoped to that slot; `MainWindow::datasetPool_`
is the shared list every load (File > Open, Calculator, Bandpass) adds to
via `SegyCanvas::setDatasetLoadedCallback`. Delete is blocked (button
disabled, not just refused on click) for any dataset currently assigned
to *any* panel's Foreground or Background — including the slot the
dialog was opened for itself, so you can't delete a dataset out from
under the very box you're editing without first picking something else
and confirming OK. Background only accepts a synthetic "(None)" entry in
its own picker (Foreground always needs something to render).

Verified with a real screenshot, not just compile+test: loaded a real
file via the command line, confirmed the Foreground box showed its name
and Background showed "—". That caught a real ordering bug the type
system couldn't — `startLoading`'s completion handler called the
pool-registration callback (which refreshes the boxes) *before* actually
assigning the new dataset to `app_.foreground`, so the box kept reading
the previous (empty) dataset's name and never updated. Fixed by swapping
the order: `setForegroundDataset` first, pool registration after.

**Phase 1 scope, confirmed explicitly before starting**: Background is
purely a held reference right now — nothing reads `app_.background` for
rendering or analysis. What it should actually *do* (overlay? ghost?
difference display?) is an intentionally separate, later decision.

### App icon, and why a console window used to pop up alongside the GUI

The real app icon (`native/assets/icons/`) replaces the placeholder
seismic-wave icon on both platforms: `app_icon.qrc` embeds
`assets/icons/png/segyread-256.png` as a Qt resource (`:/app_icon.png`,
`CMAKE_AUTORCC ON`), set via `app.setWindowIcon(...)` in `main()` — this
covers the taskbar/window-switcher icon on both platforms. Windows also
gets `app.rc` (`IDI_ICON1 ICON "...segyread.ico"`), a Windows-only
resource compiled straight into the `.exe` itself, since Explorer/the
taskbar read an exe's *own* icon resource before the process has even
started painting a window — the Qt resource alone would leave a brief
flash of the default icon.

Separately: launching `segyviewer.exe` used to always pop a console
window behind the GUI. Cause: `qt_add_executable`/`add_executable` had
no `WIN32` flag, so the linker defaulted to the Console subsystem — a
GUI app with an unwanted console, not a logging/debug feature. Fixed by
adding `WIN32` to both executable-creation calls in `CMakeLists.txt`.
Verified directly rather than just by eye: read the built `.exe`'s PE
header subsystem field via `[System.BitConverter]` in PowerShell (2 =
GUI, 3 = Console) before and after the fix.

### Background tasks: Calculator/Bandpass/Save SEG-Y off the UI thread

These (and every future heavy operation) now run on a background
`std::thread`, with the status bar showing a label and a `QProgressBar`
(`statusBar()->addPermanentWidget(...)`) — the same real estate the
existing pyramid-build progress text already used, not a new piece of
UI. `MainWindow::runBackgroundTask(label, backgroundWork, uiCompletion)`
is the one entry point all of them share: it flips a `backgroundTaskRunning_`
guard (a second heavy operation while one's already running just gets a
flashed status message, not a crash or silent queue), starts a
33ms-interval `QTimer` to repaint the progress bar from
`std::atomic<int64_t>` done/total counters the background closure
updates, runs `backgroundWork` on a detached thread, and — via
`QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)` back onto
the UI thread, same pattern `SegyCanvas::startLoading` already used for
pyramid building — stops the timer and calls `uiCompletion` once done.

**Thread-safety rule, easy to get wrong given the Dataset pool above**:
every background closure captures `std::shared_ptr<Dataset>` snapshots
taken *synchronously on the UI thread before the thread is spawned* —
never reads through `app_.foreground`/`app_.background` from the
background thread itself, since the user is free to switch a panel's
dataset mid-task. The `shared_ptr`'s own refcounting then keeps that
exact `Dataset` (mmap, pyramid, everything) alive for the task's
duration even if the pool or every panel drops its reference to it
while the task is still running.

### Toolbar overflow: a chevron + count badge, fused to the toolbar

When the window's too short (toolbar docked Left/Right) or too narrow
(Top/Bottom) to show every toolbar button, the ones that don't fit are
hidden and reachable instead through a chevron button fused to the
bottom/right edge of whatever's currently visible, with a small
accent-colored badge showing how many actions are hidden
(`renderOverflowIcon()`) — "Option A" from a round of mockups the user
reviewed and picked (chevron + count badge), over a plainer list-icon
button that was tried first. Clicking it rebuilds a `QMenu` from scratch
each time from whatever's currently hidden: a fresh proxy `QAction` per
hidden item (icon/text/checkable/checked copied from the real one,
`triggered` forwarded to the real action via `connect(proxy,
&QAction::triggered, action, &QAction::trigger)`) rather than reusing
the real actions directly, since a `QWidgetAction`'s widget can only
ever live in one place at a time and would just move into the menu.

`MainWindow::updateToolbarOverflow()` runs after every resize
(deferred via `QTimer::singleShot(0, ...)` — `height()`/`width()` queried
synchronously inside `resizeEvent` itself can still reflect the
*previous* layout pass) and after `applyToolbarPosition()`. It measures
fit **empirically**, not by predicting it: show every candidate action,
force a real layout pass (`toolbar_->layout()->activate()`), then read
back each candidate's *actual resulting geometry* and hide whichever
ones (and everything after them — position is monotonic along the
toolbar's axis) come back positioned past the toolbar's own real
height/width. `QAction::setVisible()` alone was found, by logging both,
to leave the underlying `QToolButton`'s own `isVisible()` lagging behind
the action's, so the hide/show helper sets both explicitly.

That empirical approach replaced an earlier version that predicted fit
by summing each candidate's cached `sizeHint()` — repeatedly wrong by a
margin that tracked inter-item spacing and separator widgets
`QToolBarLayout` adds but `sizeHint()` doesn't report anywhere queryable.
The failure mode was quiet and easy to misdiagnose: Qt marked an action
"visible" with a plausible, non-stale-looking cached geometry, and then
just never painted it — no native overflow chevron, no error, no size
mismatch visible from the outside except careful geometry-level logging.
**The expanding spacer that pushes the sidebar-toggle button to the
toolbar's far end (see its own construction comment) has to stay hidden
through the entire measurement pass, not just once overflow is already
known.** Showing it during measurement was a real, user-reported bug
(not a hypothetical): with every candidate visible and a window tall
enough to comfortably fit all of them, the spacer's `Expanding` policy
still claimed *all* of the toolbar's unused height during that pass —
exactly its intended cosmetic job once the final layout is known, but
premature here — which pushed the sidebar-toggle button's *measured* Y
position down to where the spacer's far end would be, past the actual
budget, so it measured as "doesn't fit" and vanished into the overflow
menu on windows that had more than enough room for it. Fixed by forcing
the spacer hidden for the whole measurement pass regardless of prior
state, only re-showing it (and re-`activate()`-ing the layout so the
resulting push is resolved synchronously, not left to Qt's own lazy
relayout on some later paint) once the fit check confirms nothing
actually overflowed.

**The overflow button itself is manually positioned, not
`toolbar->addWidget()`'d into `QToolBarLayout` like every other toolbar
widget here.** It went through two earlier designs first: added as a
normal `QToolBarLayout` child the same way as every candidate (broke for
the same `sizeHint()`-sum reason above, before that was fixed), then
moved to the status bar's permanent-widget area to sidestep
`QToolBarLayout` entirely (worked, but wasn't the look the user actually
picked from the mockups). Once the empirical fit check above replaced
the `sizeHint()`-sum approach for candidates, moving it back into the
toolbar as a normal child seemed safe — except geometry-level debug
logging then showed a third, independent `QToolBarLayout` quirk:
**the layout's actual last one or two children reliably come back with
stale, never-laid-out `(0,0,100,30)` geometry, regardless of overflow
state or available space.** This wasn't a new bug the button introduced
— logging every candidate's own geometry alongside it showed
Lock/Sidebar (always the last two entries in `toolbarOverflowCandidates_`)
exhibiting the exact same stale rect *whenever they themselves were
hidden and therefore the layout's actual last children* — it had been
there the whole time, just never visibly mattered before because the
affected items happened to always be ones already hidden. Rather than
chase a fourth `QToolBarLayout` workaround, the button is now parented to
`toolbar_` but kept out of its layout altogether and positioned by hand
in `updateToolbarOverflow()`, using the always-valid geometry of
`zoomAction_`'s widget (the first candidate, never hidden) as a
same-style reference for its own size/offset — the same manual-overlay
technique this file already uses for `DatasetSlotWidget`/
`AmplitudeScaleWidget` on the canvas.

### Idents: trace-header reference rows/plot/overlay lines

Modeled on a reference seismic tool's own Idents submenu (two screenshots:
small rows of trace-header values above/below the section, a line-graph
"ident plot" at the top, and checkable Display at Top/Bottom/On Seismic
lists) and scoped to this app's own decoded fields: `IdentField`
(`viewer_qt.h`) covers exactly the 7 fields `segy::TraceHeader`/
`parseTraceHeader` already decode (Trace Sequence Line/File, Field
Record, Trace Number, CDP, X, Y) — no abstract numbered-slot indirection
like the reference tool had, since nothing else is decoded for a slot to
map onto. `IdentSettings` (per-panel, on `AppState::idents`, read fresh
every frame like `DisplaySettings` — **not persisted**, confirmed with
the user: idents reset to all-off every session) holds which fields show
at top (max 4), bottom (max 2), as the one ident plot (top only — a
bottom plot was in the reference tool but the user said it "can be
regretted"), and as each of two colored overlay lines drawn directly on
the seismic (cyan/red).

**The toolbar's Idents button** (`identsAction_`, new `iconIdents()` in
`viewer_qt_icons.*` — hand-drawn, not a Tabler icon, since nothing in
that set covers this app-specific concept) opens a `QMenu` built fresh
from the active panel's `app_->idents` every time, same "rebuild, don't
keep in sync live" approach as `toolbarMoreMenu_`. Display at Top/Bottom
are independent checkable actions per field; checking a 5th/3rd is
refused (`flashStatusMessage`, action left unchanged) rather than
evicting the oldest — simplest correct behavior given the whole `QMenu`
is thrown away the instant it closes, so there's no stale state to
revert. Ident Plot and each overlay line's field picker are
single-select-or-none, via an `ExclusiveOptional QActionGroup` (the same
policy already used for Pan/Box Select) — clicking the already-checked
entry again clears it to `-1`.

**Canvas margin: independent top/bottom insets.** The existing
`kCanvasMargin` (16px, the "Dark Pro" dark surround) was applied
symmetrically on all four sides everywhere in `SegyCanvas`. Idents need
the top/bottom insets to grow independently as rows/the plot are toggled
on, without touching the (unchanged) left/right margin — `identInsets()`
counts the enabled fields/plot and returns the extra top/bottom pixels to
reserve, recomputed fresh on every call (cheap: a handful of bool reads)
rather than cached, so there's no stale-cache window between an
Idents-menu toggle and the next mouse/paint event reading it. Every
existing site that derived the *vertical* inset from `kCanvasMargin`
alone now adds `identInsets().top`/`.bottom`: `paintEvent`'s image blit
origin and inset height, the time-scale tick/title Y positions, the
wiggle-mode `painter.translate`, the selection-box clip rect,
`dataCoordAt`/`pixelForData` (hover/box-select/pan/zoom all route through
these two), and the pan/wheel handlers' own inset-height math. This was
the single riskiest part of the feature — every site was enumerated
up front from a full-file read rather than discovered mid-implementation,
and verified by screenshot afterward (see below) rather than trusted on
inspection alone, since a silent off-by-`identInsets()` bug here would
show up as "pan/zoom/hover don't track the mouse correctly," not a crash.
The Foreground/Background dataset-slot boxes (`repositionDatasetSlots()`)
move too — from a fixed `y=2` to `identInsets().top + 2`, which reduces
to exactly the original position when no idents are enabled, and stays
pinned the same 14px above the image's actual top edge as the inset
grows. `SegyCanvas::refreshIdentLayout()` (public) is what the Idents
menu's handlers call after writing into `app_.idents` — `update()` alone
wouldn't reposition the amplitude-scale legend or the dataset-slot boxes,
since `repositionAmplitudeScale()`/`repositionDatasetSlots()` are
otherwise only reached from `resizeEvent`/conditionally from `paintEvent`.

**Trace-axis tick positions** are a new, Qt-shell-only helper
(`identTracePositions()`, independent of `chrome.h`'s Y-axis/time tick
system, which is unrelated): `count` evenly-spaced trace indices across
the visible view, clamped and adjacent-duplicates collapsed. Two
densities are used deliberately: ~12 for the text rows (readable, matches
the reference's sparse labels) and one per plot-width pixel column for
the ident plot and the two overlay lines (smooth continuous line — text
rows have no such smoothness need, so there was no reason to reuse the
same sparse count for both).

**Overlay lines**: a field's raw header value is reinterpreted as a time
in ms (inverse of the formula `currentHoverInfo()` already uses for its
own `timeMs`), converted to a Y pixel via `pixelForData()` — the exact
function selection-box corners already use, no new coordinate math.
Confirmed with the user: a sampled point whose reinterpreted time falls
outside the visible sample range breaks the line into a gap (new
`QPainterPath` subpath on the next in-range point) rather than clipping
it flat at the plot's edge, so an out-of-range stretch reads as "no data
here," not as a real flat value.

Verified with a real screenshot (not just compile+test, given the margin
threading above): loaded the fixture, turned on 3 top rows + the bottom
row + the ident plot + a cyan overlay line via temporarily-hardcoded
`IdentSettings` defaults (reverted before committing — the Idents menu
itself wasn't click-tested interactively, see "Trade-offs"). Confirmed:
rows/plot render with correct per-trace values at correct tick positions,
the dataset-slot boxes visibly shifted down to track the new top inset,
the cyan line drew as a clean diagonal precisely bounded within the
image area (strong indirect evidence `pixelForData`'s inset math is
correct, since any offset bug would have shown the line starting above
or extending past the image), and toggling everything back off reverted
the layout pixel-for-pixel to the pre-feature baseline.

## Trade-offs and honest limitations

- **No header-range filtering** (selecting/loading only traces matching a
  header-field predicate) — out of scope for this pass. Spectrum/FFT
  analysis and multi-dataset combination (Calculator) are implemented; see
  their own sections above.
- **Whole-pyramid-in-RAM assumption** (see above) instead of an on-demand
  tile cache — simpler and jank-free, but doesn't scale to files whose
  pyramid alone exceeds available RAM.
- **Coordinate scaler (trace header bytes 71-72) is not applied to X/Y**,
  matching the original app's behavior — noted here rather than silently
  changed, since fixing it would change displayed coordinates.
- **AVX2 is required to launch `segyviewer[.exe]`/`segybench[.exe]` as
  built** (`-mavx2`/`/arch:AVX2`, set in `CMakeLists.txt`), even though the
  decode routines themselves have a correct runtime-gated scalar fallback
  (`cpuSupportsAvx2()`). Rebuild without that flag for pre-Haswell
  (pre-2013) CPUs.
- **No longer a self-contained single-exe on Windows**: the Qt6 migration
  (see "GUI toolkit history" above) explicitly traded away the static-CRT,
  copy-one-.exe-anywhere property the old Win32/GDI shell had. Distributing
  `segyviewer.exe` now means shipping it alongside its Qt DLLs and the
  `platforms/qwindows.dll` plugin (see "Manually verifying the viewer" for
  why that plugin specifically needs its own deployment step) — this was
  an explicit, accepted trade-off, not an oversight.
- **File > Exit and the Edit/Selection/View/Help dummy items are not
  automated-tested** on either platform — File > Open is confirmed by hand
  on both (see "Manually verifying the viewer"); the rest use the same
  trivial `QAction` pattern and are judged low-risk but not individually
  confirmed. Scripted `xdotool` clicks on this menu bar were never reliable
  under WSLg/Weston, so hand-testing is the practical way to close this out
  on Linux.
- **`QFileDialog` wasn't specifically stress-tested for the appear/
  disappear/reappear flicker the old `fl_file_chooser()` dialog showed
  under WSLg** — no evidence of similar behavior was observed, but this
  wasn't deliberately re-tested for that specific symptom after the
  migration, so treat "moot" as likely, not confirmed.
- **No Edit > Preferences UI yet** — by design, per the brief. `DisplaySettings`
  exists and is read fresh every frame by `viewer_qt.cpp`, so a future
  Preferences dialog only needs to flip its booleans/enum and redraw; no
  rendering code should need to change.
- **Color bar has no numeric labels** (just the gradient) — the brief asked
  for "positive at top, negative at bottom," not specific amplitude values;
  adding min/max or tick labels to it would be a small, separate follow-up
  if wanted.
- **Wiggle's trace spacing (`kMinPixelsPerTrace`) and amplitude scale are
  fixed constants**, not yet user-tunable — a "density/gain" control is a
  natural Preferences follow-up (same `DisplaySettings`-is-read-fresh
  pattern as everything else), just not built yet since nothing asked for
  it specifically.
- **View > Seismic Display / Wiggle Infill menu items were not manually
  clicked** — see "Manually verifying the viewer" for exactly what was and
  wasn't confirmed and why the gap is judged low-risk.
- **Toolbar icons are simple hand-drawn shapes at a fixed 24×24 pixel
  resolution** (see "Toolbar, status bar, box-select zoom, and gain") —
  correctly sized at high DPI (the rounding-policy fix scales them along
  with everything else) but not as crisp as they'd be with an explicit
  high-resolution variant; a cosmetic follow-up, not a functional gap.
- **View menu's Zoom In/Out/Reset View/Fit to Window items are still
  inert placeholders**, unaffected by the new toolbar — the toolbar's own
  Zoom/Mooz/Pan/Box Select are the real, working zoom controls now; wheel
  zoom and `'R'` reset remain the other two working ways to change the
  view.
- **The dock's QC Notes section is a static placeholder** ("No issues
  flagged on this line yet.", no backing functionality) — matches the
  mockup's own empty state and this project's existing inert-placeholder
  convention; nothing beyond the label was asked for.
- **Dark Pro's rounded corners/drop-shadow on the canvas inset (from the
  mockup) were skipped** — meaningfully more work for a raster-blitted
  surface than the flat inset that shipped; a possible cosmetic follow-up,
  not silently dropped.
- **The 6 dB wiggle-vs-variable-density perceptual gain offset is a fixed
  constant (`kWiggleGainBoostDb`), not user-tunable** — chosen by eye
  against one real dataset; if a different dataset or display makes it
  read as over/under-corrected, the fix is one constant in `viewer_qt.cpp`,
  not a rearchitecture.
- **Toolbar overflow order is fixed** (the toolbar's own button order,
  least-recently-added hidden first) — not user-reorderable, and not
  based on actual usage frequency. Matches every other fixed-order
  toolbar in this app; a user-customizable toolbar is an explicitly
  planned follow-up, not built yet.
- **A second background task (Calculator/Bandpass/Save SEG-Y/Octave
  Bands) while one's already running is refused with a flashed status
  message**, not queued — simple and safe, but means starting a second
  heavy operation means waiting for the first to finish first, with no
  visible queue or cancel button for the one in progress.
- **The Idents menu wasn't interactively click-tested** (QMenu/QAction
  mechanics — checking a box, the max-4/max-2 refusal, the exclusive
  pickers clearing on re-click) — the risky part (margin/inset math,
  actual row/plot/overlay-line rendering) was verified by screenshot with
  temporarily-hardcoded settings; the menu's own plumbing is standard,
  low-risk Qt code already proven elsewhere in this file (`toolbarMoreMenu_`,
  the amplitude-scale legend's menu), but not independently confirmed by
  hand.
- **Idents' overlay lines need a header field that actually holds a
  time-like value to be useful** — none of the 7 decoded fields
  (sequence numbers, CDP, X/Y) naturally are one; the feature reinterprets
  whatever raw value is there as milliseconds, which only makes visual
  sense for data where a field was deliberately repurposed to carry a
  pick/marker time (a real legacy workflow this was modeled on, but not
  something this app's own synthetic test fixture demonstrates).
- **No bottom ident plot** (only top) — per the user's own call ("can be
  regretted"), not an oversight.
