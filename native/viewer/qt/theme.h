// UI themes (see native/README.md, "Themes"): Dark (the default) and
// Windows Grey. The whole UI is written once, in the dark palette; in
// Windows Grey every dark colour is mapped to its counterpart, both in
// style sheets (themed()) and in painted colours (tc()). The theme is read
// once at startup (View > Theme stores it and restarts the app).
#pragma once
#include <QColor>
#include <QString>

namespace segyqt {

enum class UiTheme { Dark, WindowsGrey };

UiTheme uiTheme();
void setUiTheme(UiTheme theme); // call before any widget is created
bool lightTheme();              // Windows Grey

// A style sheet written in the dark palette, in the active theme.
QString themed(QString css);
// A dark-palette colour in the active theme (alpha kept). Colours not in
// the palette (data colours: seismic, curves, horizons) pass unchanged.
QColor tc(const QColor& dark);

} // namespace segyqt
