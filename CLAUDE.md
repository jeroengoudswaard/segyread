# CLAUDE.md

Guidance for Claude (and other AI assistants) working on this repository:
TraceLab, a native Qt6 C++ 2D SEG-Y viewer and QC tool for a geophysicist
user. **2D only, simpler is better** — 3D interpretation is out of scope
(that is what Petrel is for).

- `native/README.md` — the design record: every feature, why it is built
  the way it is, what was verified and how. **Update it with every change
  under `native/`**, proactively.
- `COPILOT.md` — the numbered wishlist (items 1–38) with decisions and
  status. Add new ideas there; mark them done with a date.
- This file — how to work here, plus the open observations from the
  performance and UX passes (November 2026).

## Ground rules

- **Performance first.** The app must never feel sluggish. Bound work by
  screen pixels, not file size; run anything substantial in a background
  task (`MainWindow::runBackgroundTask`); never read every trace header on
  the UI thread. Check large-file behaviour with the profiler (below).
- **No PRs, no `v*` tags, no manual workflow runs until December 2026**
  (GitHub Actions storage is exhausted). Plain pushes to `main` are fine;
  `release-windows.yml` only runs on tags, PRs and manual dispatch.
- **Never edit sources with PowerShell `Get-Content`/`Set-Content`** (PS 5.1
  corrupts UTF-8). Use the Edit tool or a Python script with
  `encoding='utf-8', newline='\n'`.
- **Keep `viewer_qt.cpp` pure ASCII.** MSVC builds without `/utf-8`, so a
  literal `·` or `–` in a narrow string turns into mojibake. Write
  `"\xc2\xb7"` / `"\xe2\x80\x93"` escapes. Beware: bash heredocs in this
  environment turn `\\n` and `\\x..` into real characters — write Python
  edit scripts with the Write tool instead.
- **Only stage `native/` (and `COPILOT.md`/`CLAUDE.md`).** The user's own
  files (`.claude/`, `.vscode/`, release zips) are sometimes already staged;
  check `git status` before committing so they don't get swept in.
- Commit messages end with the `Co-Authored-By` line from the session's
  attribution reminder. The user writes Dutch, answers in English are fine.

## Build, test, run

- Build: `cmake --build native/build-windows --config Release -- -m`
  (MSVC, Qt 6.11 from vcpkg at `C:\Users\Jeroen\Dev\vcpkg`).
- Tests: `native/build-windows/Release/segytest.exe` (88k checks; keep at 0
  failures). Benchmarks: `segybench.exe` (`segybench spectral`).
- If `segyviewer.exe` is running the link fails; rename the locked exe
  (`segyviewer.oldHHMMSS.exe`) and rebuild.
- Test data: `PARAGON_DEGHOST_12.5M_..._STACK.SGY` (15082 traces, 2501
  samples, IBM float, repo root), `Seismic.sgy`,
  `C:\Users\Jeroen\ODData\F3_Demo_Start\Seismics\Prestack_seismic.sgy`.
  Large-file tests: `C:\Users\Jeroen\segy_perf\paragon_x40.sgy` (6 GB,
  603k traces; PARAGON tiled 40×) and `paragon_x10.sgy` (1.5 GB).
- **Profiler:** set `SEGYREAD_PROFILE=<file>`; it logs loads, background
  tasks, paint stages ≥ 5 ms and every UI-thread stall ≥ 50 ms with the
  action that caused it (a modal dialog counts as one long event).
- **Screenshot verification** (no built-in automation): a temporary hook in
  `main()` (env `SEGYREAD_DEBUG_ACTION="Action text|!accept|!open:path|!dump:file"`,
  `SEGYREAD_DEBUG_STEP_MS`) triggers menu actions by text, accepts modal
  dialogs, opens files and dumps the menu tree; plus PowerShell
  `PrintWindow` capture scripts. **Remove the hook before committing.**
  Popup menus can't be captured; synthetic clicks don't reach modal dialogs.
- Settings live in `~/.segyread_settings` (INI, keys under `[General]`,
  e.g. `uiTheme=windowsGrey`). Back it up before tests that change it.
- Local portable package: see "Packaging" below.

## Architecture in one paragraph

Qt-free core libraries (`segycore`: SEG-Y format/mmap/decode/select/writer;
`segyviewercore`: renderer, pyramid loader, chrome, analysis, FFT, IIR
filters, horizons, depth conversion, trace-header definitions) are covered
by `segytest`. The Qt shell is `native/viewer/qt/viewer_qt.{h,cpp}` (one
large file), icons in `viewer_qt_icons.*`, themes in `theme.*`. Datasets are
`shared_ptr<Dataset>` in a pool; computed results are written as float32
SEG-Y to `%TEMP%\segyread_scratch` with `streamTraces`/
`segy::writeSegyFileChunked` (chunked, never the whole output in RAM) and
loaded back. The time/depth domain flag rides along in the processing
history (`DOMAIN: DEPTH`).

## UI conventions (from the UX pass)

- **One zoom model everywhere** (seismic and every `ZoomableChart`): wheel
  at the cursor (one gesture = one history step), left-drag rectangle
  zooms on release, double-click = previous zoom, right-click = Previous
  Zoom / Full View / Axis Ranges…, middle-drag pans, Backspace / Home (R).
  Don't add per-window zoom buttons again.
- **Menus**: File, View, Analysis, Processing, Horizons, Window, Help. No
  placeholder items that do nothing. Ellipsis only when a dialog asks for
  input first. Analyses open their result directly; parameters live in the
  result window.
- **Words**: tooltips "Name: what it does"; re-run buttons "Recompute";
  labels without colons; trace numbers shown to users are 1-based; status
  and info lines separate parts with " · ".
- **Colours**: write UI colours in the dark palette and pass them through
  `themed()` (style sheets) / `tc()` (painted `QColor`s) from `theme.h`, or
  use the `ThemedColor` constants (`kAccentColor`, …). Data colours
  (seismic, curves, horizons) are never themed. Windows Grey = native
  `windows11` style + Windows 11 light palette; the theme is read once at
  startup (View › Theme restarts the app).
- Icons: Tabler Icons SVGs rendered at 24/48/72 px (`renderSvgIcon`).

## Packaging (local zip)

vcpkg's Qt has no `windeployqt`. A working portable folder is: `TraceLab.exe`
(= `segyviewer.exe`) + every `*.dll` from `native/build-windows/Release` +
`platforms/qwindows.dll` + `styles/qmodernwindowsstyle.dll` (from
`vcpkg/installed/x64-windows/Qt6/plugins/styles`, needed for Windows Grey) +
the MSVC runtime DLLs (`vcruntime140.dll`, `vcruntime140_1.dll`,
`msvcp140*.dll` from `VC/Redist/MSVC/<ver>/x64/Microsoft.VC145.CRT`) +
`native/packaging/README-windows.txt` as `README.txt`. Zip it as
`Shockwave-TraceLab-windows-x64.zip`. Don't commit the zip.

## Performance status (6 GB file, 603k traces, 16 threads)

| Operation | Time |
|---|---|
| Load + pyramid | 4.2–5.7 s (~1 GB/s, I/O bound) |
| Repaint, full view | 25–80 ms (first paint after load up to ~140 ms) |
| Wiggle, full view | ~150 ms per paint (still the slowest paint) |
| x-f display | 0.36 s (at most 4096 traces, stride shown) |
| Flatten (whole file, streamed) | 7.9 s |
| Auto-pick seabed | 0.9 s |
| Trace Header Ranges | 3.0 s (cold headers) |
| f-k, full view | 1.8 s |
| Signal/Noise | 1.1 s (old implementation, 2000 pairs; the SIGCOR reference port of 2026-11-25 is not yet re-measured: up to 20 000 pairs, parallel) |

Signal/Noise follows the user's MATLAB reference `sigcor_spectr_stndrd.m`
(see the README section); keep `testSigcorAgainstReference` passing when
changing it.

Not yet measured through the GUI on the 6 GB file: Bandpass (its dialog is
non-modal, the hook couldn't accept it), Calculator, Time-Depth.

## Open observations (to do / could be better)

**Inconsistencies left in the UI**
- Decimal separators are mixed: spin boxes use the system locale ("0,30")
  while labels use a dot ("0.30"). Pick one (probably `QLocale::c()` for
  numbers everywhere).
- "Full View" in the Spectrum/Histogram parameters means the whole dataset,
  whereas the other analyses use the visible view. Unify (view or box).
- Autocorrelation and Octave Bands trace axes are still 0-based; Octave
  Bands' x-axis end labels are clipped ("1508" for 15081).
- "Colour" and "Color" are both used. Pick one.
- Custom-painted text uses several pixel sizes (10–12 px) next to the
  system font; a small type scale would tidy this.
- Parameter dialogs use OK/Apply/Cancel, result windows use Recompute;
  fine, but the Display Parameters dialog is still bound to the panel it
  was first opened from (known limitation).
- Theme switching needs a restart (icons and inline styles are built once).
- Toolbar buttons can be hidden but not reordered.
- The gain shortcut `+` needs Shift on a US keyboard.

**Functional gaps in what exists**
- The Background dataset slot can be chosen but is not drawn (velocity as
  colour under wiggles is the obvious use).
- Horizons live in memory only (export CSV to keep them); picks made on a
  flattened display stay on the flattened copy; no undo while picking.
- Seabed auto-pick handles dead and muted traces, but steep seabed with a
  weak first break is untested on real data other than PARAGON.
- Time-depth conversion: velocities assumed m/s, input start time assumed
  0 (delay recording time ignored), no datum correction (as decided).
- x-f on a huge view shows every Nth trace (by design); hovering reports
  the real trace number.

**Not verified by test or screenshot**
- Theme-switch restart flow (`QProcess::startDetached`), Report a Bug
  actually opening a mail client, middle-drag pan, double-click on the
  seismic, f-k grouped drag-zoom, Send Zoom/Display, Lock View, Read
  Attribute, Trace Inspector "Follow cursor", Write Horizon to Header.

## Missing compared with other 2D tools (simplest and most useful first)

1. **Basemap**: a small map of the line(s) from CDP X/Y, with line
   intersections — orientation and line ties.
2. **SEG-Y import options**: override header byte positions and the sample
   format, little-endian data, SU files, rev 2 extended text headers.
3. **Axes and output**: x-axis by shotpoint / CDP / distance, km scale bar,
   export to PNG/PDF at a fixed scale (traces/cm, cm/s).
4. **Draw the Background dataset** (velocity colour behind wiggles).
5. ~~Attributes~~ (done, Processing > Attributes; one-sided data such as
   envelope and instantaneous frequency now spans its own min..max) — next:
   sweetness, RMS amplitude in a window.
6. **Simple processing**: top mute, spectral shaping (COPILOT item 12).
   Done: f-k filter, spectral whitening, predictive deconvolution — all
   with one whole-trace design window; time-variant windows, f-k polygons
   and gather-aware f-k panels are the obvious next steps.
7. **Interpretation**: fault sticks, undo while picking, a ruler for
   distance, dip and apparent velocity (COPILOT item 8).
8. **Sessions**: remember open datasets, horizons and views between runs.
9. **Header math**: set header values from a formula.
10. Batch / command-line conversion (lower priority).

Phase: Analysis > Phase Estimate (COPILOT item 41) — max kurtosis and
min entropy (varimax), plus their agreement; next could be time windows
(seabed vs. deep) and a time-variant rotation.

Also still open from `COPILOT.md`: 3 (processing ideas), 4 (cut/copy/paste
on the selection box — the Edit menu was removed until this exists), 5
(F-K line drawing), 8 (slope measurement), 12 (spectral shaping).
