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

11. **Live subsetting and sorting** — Allow data to be subset and sorted on the fly, and provide a way to output the data with the new sort.

12. **Spectral shaping** — Add spectral shaping as a processing step. Discuss the algorithm, controls, and performance needs before implementation.

13. **Autocorrelation** — Show the autocorrelation of the data in a popup window. The user parameter is the filter length L; the display runs from -L to +L, with the peak at t=0 in the middle. *Status: done — Analysis > Autocorrelation (autocorrelogram per trace plus average curve).*
