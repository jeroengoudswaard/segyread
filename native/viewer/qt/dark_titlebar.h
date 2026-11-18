// Dark window title bars on Windows, matching the app's dark theme. Every
// top-level window (main window, dialogs, result windows) gets
// DWMWA_USE_IMMERSIVE_DARK_MODE when it is first shown, via one application-
// wide event filter -- so new dialogs never need to remember to opt in.
// Kept in its own file so <windows.h> (and its min/max macros) stay out of
// viewer_qt.cpp. A no-op on other platforms, where the window manager owns
// the title bar.
#pragma once

class QApplication;

namespace segyqt {

void installDarkTitleBars(QApplication& app);

} // namespace segyqt
