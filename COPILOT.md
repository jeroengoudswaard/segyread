# Copilot wishlist

A living list of ideas and requirements to guide future work on TraceLab. **Performance is the highest priority:** the app must never feel sluggish. Keep interaction responsive, bound expensive work, and run substantial processing off the UI thread. Discuss open design choices together before settling implementation details.

1. **Customizable toolbar** — Allow buttons to be added to or removed from the toolbar. All functionality should also remain available from the appropriate menus, such as File, Edit, and Selection. *Status: done — show/hide per button (right-click the toolbar, or View > Toolbar), persisted; every button also has a menu entry. Reordering not yet supported.*

2. **Resizable octave-band display** — Allow the octave-band display to be resized, with its data resizing accordingly. Add a crosshair shared across all bands; its horizontal and vertical lines should extend to the x- and y-axes. Ensure axes are clearly shown with labels. *Status: done.*

3. **Processing ideas and SIGCOR** — Suggest useful processing steps and explore them collaboratively. Plan a noise-level estimator that separates signal from noise, to be called **SIGCOR**. The user will provide its description.

4. **Selection-box editing** — Cut, Copy, Paste, and Delete should operate on the selection box only. Their exact behavior is still to be discussed.

5. **F-K spectrum** — Generate an F-K spectrum, recognizing panels and producing an F-K spectrum for each panel. Since the 2D Fourier transform is complex, display nonnegative values derived from its magnitude or squared magnitude (the amplitude-versus-power choice remains to be discussed). The horizontal wavenumber axis should identify its units as 1/meters. Use red-yellow-green as the default colour map, with maximum amplitude in red and zero amplitude in green. Allow lines to be drawn on the F-K plot and display the corresponding velocity, `v = 2*pi*f/k`. Confirm whether `k` is angular wavenumber (rad/m) or cycles/m, since that determines the `2*pi` convention.

6. **Seismic block identifiers** — Allow foreground and background names in the seismic block identifier bar to be positioned at top-left, bottom-left, top-right, bottom-right, or off. Keep the names on the seismic canvas. *Status: done (View > Dataset Labels).*

7. **Colour maps** — Add more colour maps, including rainbow and red-yellow-green, and suggest other useful choices. Allow the colour map to be flipped from the menu. Show colour maps as live previews in the menu rather than only by name. *Status: done (right-click the amplitude legend).*

8. **Linear-event slope measurement** — Let the user draw a line on a panel and calculate its velocity from time and the x-axis in meters (for example, water velocity is commonly 1500 m/s). Show the slope value at the bottom of the trace information panel, in a separate section divided by a horizontal line.

9. **Automatic Gain Control (AGC)** — Add AGC with controls for window and level. *Status: done — toolbar toggle + View > Amplitude Processing (window ms, level % of full scale); display-time, not persisted.*

10. **Computed gain** — Support gain defined as time-gain pairs, so gain can vary as a function of time. *Status: done — time (ms) / gain (dB) pairs in View > Amplitude Processing, interpolated in dB.*

11. **Live subsetting and sorting** — Allow data to be subset and sorted on the fly, and provide a way to output the data with the new sort. *Status: first step done — File > Trace Header Ranges: edit Min/Max, sort on up to three idents, Apply writes a new dataset.*

12. **Spectral shaping** — Add spectral shaping as a processing step. Discuss the algorithm, controls, and performance needs before implementation.

13. **Autocorrelation** — Show the autocorrelation of the data in a popup window. The user parameter is the filter length L; the display runs from -L to +L, with the peak at t=0 in the middle. *Status: done — Analysis > Autocorrelation (autocorrelogram per trace plus average curve).*

14. **Signal/Noise** — Estimate signal and noise spectra of a section after SIPMAP SIGCOR (SPECTR and AUTCOR), with toggles for trace/signal/noise/S-N and the 1-sigma of the time shifts. *Status: done — Analysis > Signal/Noise; parameters in Preferences.* *Update (2026-11-25): rewritten as a port of the user's gold-standard MATLAB `sigcor_spectr_stndrd.m` (SPECTR/STNDRD): NSKIP = traces between the pair, symmetric IRANSK, streaming pair selection, normalized correlation on the gate overlap, rejection at SHMAX, NIPOL=1 whole-sample alignment, Kaiser β 5, noise = ½|X−Y|²; defaults NSKIP 3, IRANSK 2, SHMAX 20 ms, CCMIN 0.65. Checked against a transliteration of the MATLAB in segytest. Open: confirm against MATLAB output on real data (RNG equality with `randi` unverified).*

15. **Spectrum crosshair** — Crosshair with a value readout panel on the spectrum plots. *Status: done — Spectrum and Signal/Noise.*

16. **x-f display** — Toggle on the seismic display: amplitude spectrum (dB, 0 to Nyquist) of every trace in view, variable density, green -40 dB to red 0 dB by default; toggling back restores the seismic and the selection boxes. *Status: done — toolbar/View > x-f Display.*

17. **Lock View (decided, see 32)** — The user wants a way to fix an axis range set via Axis Ranges so zoom/unzoom can no longer change it. Conflicts with the existing Lock button, which links the two panels. *Status: parked — decide whether this is a separate Lock View button or a new meaning for Lock.*

18. **f-k display** — Separate window: one f-k plot per gather in view (or averaged), selectable panel and distance idents with scalar, velocity fan. *Status: done — Analysis > f-k display.*

19. **Read Attribute SEG-Y** — Separate input for velocity models and other attributes (interval, average, RMS velocity, delta, epsilon, eta), tagged at load, listed in the dataset list, not shown automatically, meant as background. *Status: done — File > Read Attribute SEG-Y; Background display itself is still to come.*

20. **Trace Inspector (open)** — Per-trace header table (name, byte offset, value, description) plus the trace's samples with a small wiggle, after the user's RadExPro screenshot. Header definition selectable: SEG-Y rev 1.0, rev 2.0, Shell V2.1 (default; from the user's "SEGY trace identing" document, bytes 1-240). *Status: done — File > Trace Inspector.*

## Ideas to implement (registered 2026-11-24, to clarify with the user before building)

21. **Send Zoom** — send one panel's zoom to the other. *Decided: a one-off action, matched on trace index and time.* *Status: done (2026-11-24).*
22. **Calculator does not always show all data sets** — bug: its dataset choice is incomplete. *Decided: pick inputs from the whole dataset list, not just what panels A and B show.* *Status: done (2026-11-24).*
23. **Send Display** — send scaling, colour scale and display method to the other panel. *Decided: everything, including gain, AGC and the manual clip.* *Status: done (2026-11-24).*
24. **Crosshairs between two windows** — linked crosshair across the panels, on/off, default on. *Decided: linked on time and trace index, default on.* *Status: done (2026-11-24).*
25. **Zoom without a box** — clicking Zoom with no box selected turns on "Draw a box to zoom". *Decided: Box Select turns on and the view zooms automatically once the box is drawn.* *Status: done (2026-11-24).*
26. **Autocorrelation: zoom** — zoom option in the autocorrelation window. *Decided: the same three buttons as the spectra (Zoom, Unzoom, Box Select).* *Status: done (2026-11-24).*
27. **Autocorrelation: lag window** — let the user set the +/- lag window, possibly asymmetric. *Decided: two fields, -lag and +lag (ms).* *Status: done (2026-11-24).*
28. **Peak-to-peak distance** — automatic estimate of the lag (nearest peak with a cross-correlation of 50%, if it exists), or a delta-T measuring tool. *Decided: automatic on the average autocorrelation (first side peak >= 0.5), plus a two-click delta-T measuring tool.* *Status: done (2026-11-24).*
29. **Spectrum of the autocorrelation** — shown below it, default off. *Decided: in dB, same chart as Spectrum, below the autocorrelation, default off.* *Status: done (2026-11-24).*
30. **XCOR in the calculator** — cross-correlation of two datasets, same display as the autocorrelation. *Decided: panel A x panel B trace by trace, shown as a popup like the autocorrelation (no new dataset).* *Status: done (2026-11-24).*
31. **Verify diff/integrate** — check the trace differentiation/integration functionality. *Decided: test differentiate-then-integrate gives back the original; remove f = 0 (DC) from the test input first.* *Status: done (2026-11-24).*
32. **Lock functionality** — work on Lock (see 17). *Decided: two separate buttons — Lock Panels (today's linking) and Lock View (fixes the axis range so zoom/unzoom cannot change it).* *Status: done (2026-11-24).*
33. **Always some headers at top** — ident rows always present at the top. *Decided: fall back to the sequence number when no sort ident is found; the user can always switch them off.* *Status: done (2026-11-24).*
34. **Auto-pick seabed.** *Decided: first strong amplitude per trace above a threshold (% of the trace maximum, default 30%), refined by cross-correlation with neighbouring traces; the result is an editable horizon (like 36/37); writing it to a header (e.g. bytes 57-60, seabed TWT, in a new dataset) is an option, not the default.* *Status: done (2026-11-24) — Horizons > Auto-pick Seabed; Write Horizon to Header as the option.*
35. **Flatten** — on an ident or a picked horizon. *Decided: flatten on an ident (e.g. seabed TWT) or on a picked horizon; by default a display you switch on/off, with "create a new dataset" as an option; the horizon goes to a fixed time the user picks, default the horizon's minimum.* *Status: done (2026-11-24) — Horizons > Flatten (display toggle, or a new dataset).*
36. **Horizon picking, point to point** — straight lines between picks; use the trace-to-trace autocovariance to fill in between, with a single trust value between 0.3 and 1. *Decided: clicks place points joined by straight lines; between them each pick is adjusted by correlation with the previous trace within a search window around the line (default +/-20 ms, adjustable); trust is the threshold (default 0.7): correlation >= trust takes the adjusted pick, below it the pick stays on the line; pick on peak, trough or zero crossing, default peak.* *Status: done (2026-11-24) — toolbar Pick Horizon.*
37. **Picks in memory** — several user horizons; output of an event as IDENT/TIME. *Decided: several horizons with name and colour, in memory per dataset, kept between sessions by exporting; export as CSV with a header row, one line per trace: trace number, CDP and time (ms), plus FFID and offset for prestack; picks can be read back in.* *Status: done (2026-11-24) — Horizon Manager, CSV export/import.*
38. **Time-depth conversion** — with a velocity function loaded, T-D or D-T conversion; default output sampling 4 ms (time) or 8 m (depth). *Decided: interval, average and RMS velocity as input (RMS converted to interval with Dix); the velocity SEG-Y is matched to the seismic on CDP, interpolated between CDPs; its own sampling is allowed (linear interpolation); output is a new dataset, depth below sea level without datum correction, default 8 m (depth) or 4 ms (time), adjustable.* *Also: each dataset gets a domain flag, time or depth (SEG-Y does not record it), default time; T-D/D-T is undefined without it.* *Status: done (2026-11-24) — Processing > Time-Depth Conversion; View > Data Domain.*

39. **f-k filter, spectral whitening, predictive deconvolution** — the simplest useful versions. *Status: done (2026-11-25) — Processing > f-k Filter (reject below a velocity, 128-trace overlapping panels), Spectral Whitening (From/To/Smoothing), Deconvolution (operator, gap, white noise).*

40. **Attributes** — envelope, instantaneous phase and frequency, phase rotation. *Status: done (2026-11-25) — Processing > Attributes; only Phase Rotation asks a question (the angle).*

41. **Statistical phase estimate** — estimate the wavelet's constant phase from the seismic itself, Ockham's razor style: the best rotation gives the simplest seabed / main reflectors. *Ideas only (2026-11-25), not decided:* scan -180..180 degrees and score each rotation by (a) kurtosis / varimax norm of the rotated data in a window (the classic: van der Baan's maximum-kurtosis phase), (b) seabed symmetry: the rotated seabed wavelet is symmetric and its main lobe peaks positive (or negative per polarity convention), (c) minimum entropy / sparsity (L1/L2) of the rotated trace or of its deconvolved version, (d) lateral consistency: pick the rotation that makes the auto-picked seabed amplitude most stable along the line. Report the angle with a confidence curve (score vs. angle) per window, let the user apply it with Phase Rotation. *Update (2026-11-25): done — Analysis > Phase Estimate: maximum kurtosis (van der Baan) and minimum entropy (Wiggins varimax, per trace) as alternatives, plus their agreement; seabed symmetry dropped (does not work in practice). Low-order entropy measures prefer a 90-degree wavelet and were rejected.*

42. **One-sided colour scale** — velocities, instantaneous frequency, envelope: the smallest value no longer has to be <= 0. *Status: done (2026-11-25) — automatic min..max for one-sided data; the Clip dialog takes any Min/Max.*

43. **Bandpass shapes** — separate low and high slopes (dB/octave) for the Butterworth, and a trapezoidal (Ormsby) bandpass. *Status: done (2026-11-25) — Bandpass Filter > Type.*

44. **f-k range, pass or reject** *Status: done (2026-11-25).* **Decon type** (gapped or spiking) *done (2026-11-25).*

45. **Amplitude processing as processing** — Processing > Amplitude Processing writes a dataset; the AGC button stays display-only (window and level on right-click). *Status: done (2026-11-25).*

46. **Optional toolbar buttons** — a minimal default toolbar, plus buttons the user can add for quick needs. *Status: done (2026-11-25) — 24 optional buttons in Preferences > Toolbar Buttons.*

47. **Zippiness pass on processing** — *done (2026-11-25): f-k filter 62 -> 25 s, attributes 21 -> 12 s, f-k display 4.5 -> 1.5 s on the 6 GB file; mutes preserved; robust default clip.*


## Round of 2026-11-26 (user list 1-17)

48. **Swap Foreground and Background** (list item 1). *Decided: one action swaps the two datasets of the active panel.* *Status: done (2026-11-26).*

49. **Curtain comparison** (list item 2) — with a Background dataset set, the Background is drawn left of a vertical divider and the Foreground right of it; drag the divider like a curtain to pull the Foreground over the Background. *Status: done (2026-11-26).*

50. **f-k display: say what Auto chose** (list item 3). *Status: done (2026-11-26).*

51. **Auto-pick Seabed only in the Horizon Manager** (list item 4). *Status: done (2026-11-26).*

52. **Dataset names**: Foreground/Background boxes show the start and end of a long name with "..." in the middle; hovering shows the full name (list item 5). *Status: done (2026-11-26).*

53. **Octave Bands window**: normal minimise/maximise buttons (list item 6). *Status: done (2026-11-26).*

54. **Signal/Noise details**: rows closer together (list item 7). *Status: done (2026-11-26).*

55. **Open with analysis** (list item 8) — scan the file before reading it: which headers hold what, read a subselection (trace range + every Nth, and min/max/step on header keys), override the trace-header byte mapping, save and load the mapping as a template. Rename "Shell V2.1" to "SEG-Y v2.1". *Status: done (2026-11-26).*

56. **Autocorrelation** (list items 9-11): remove the spectrum option; the auto lag threshold is the user's (default 0.3); measure a lag with a measuring tool. *Status: done (2026-11-26).*

57. **Scale bar** (list item 12) — black/white bar of about 5-10 % of the plot width, rounded to a nice number (100, 200, 500, 1000 ...), m switching to km from 10 000 m; bottom right, top right when the dataset names are bottom right. Also in the autocorrelation window. *Decided: trace spacing from CDP X/Y; without coordinates the user types a trace spacing.* *Status: done (2026-11-26).*

58. **Recompute turns orange** when a setting in a result window changed (list item 13). *Status: done (2026-11-26).*

59. **Cross-correlation** as an analysis next to the autocorrelation (list item 14). *Status: done (2026-11-26).*

60. **Spectra to CSV and back** (list item 15): save the spectrum and the Signal/Noise spectra as CSV; load a saved spectrum into the spectrum window. *Status: done (2026-11-26).*

61. **Spectrum manager** (list item 16): each spectrum (box) is listed and can be switched on/off. *Status: done (2026-11-26).*

62. **Peak frequency** in the spectrum's right-hand bar (list item 17). *Status: done (2026-11-26).*

## From the comparison with Ewoud's segy_reader (2026-11-26)

63. **Ewoud, easy group** — field types per header field (2I/4I/unsigned/IEEE/IBM/8-byte, own byte order); a trace-header mapping saved and loaded as a template; header positions read from the text header as a proposal; header scan with your own fields, Copy table and an "out of date" note; rev 2 data start and extended text headers; files with an undecodable sample format still show their headers; raw binary header view; scroll bars on the seismic; a notes list ("never guess without saying so"). Built together with item 55. *Status: done (2026-11-26).*

64. **Ewoud, medium (later)** — little-endian files with "retry with the other byte order"; colour maps from `.cmap` files (Petrel seismic, Earth model, Turbo...); x-axis by a header (CDP, shotpoint); PNG export at a fixed size; Basemap from a point scan of source/receiver/midpoint X/Y coloured by a header; overlay curve from a header expression (whitelist parser, no eval; could feed header math).

65. **Ewoud, optional** — 3D bin grid from the text header (corners, bin size, azimuth, derived 4th corner) and fold maps per cell. Outside the 2D scope for now; the user likes them, so kept as options.

66. **Fence instead of an always-on curtain** — a toolbar button, right-click for horizontal or vertical, the grip movable along the fence. *Status: done (2026-11-27).*

67. **Reset in Open with Analysis > Selection** — right-click a value to go back to the data's own min/max/step. *Status: done (2026-11-27).*

68. **Dataset Manager** — remove datasets, and choose which ones the Foreground/Background boxes list; a displayed one that is removed or unticked leaves the panel empty. *Status: done (2026-11-27).*

69. **Keep the zoom** when the displayed dataset changes or is processed, if compatible; otherwise full view. *Status: done (2026-11-27).*

70. **Header names on the ident rows** — left of the plot; the right panel of a split on the right. *Status: done (2026-11-27).*

71. **Print and Export** — preview on A4/Letter/A3, PDF/PNG/JPG, elements on/off, colours and font sizes, title/client/survey from the text header (editable), fit to page only, templates applied every time Print opens. Batch export is a separate flow (later). *Decided 2026-11-27 with the mockup.* *Status: done (2026-11-27) — File > Print and Export (Ctrl+P). Next: text boxes and logo images ("+ Add"), wiggle printing, batch export.*

