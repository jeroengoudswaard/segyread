// Toolbar icon factories (Tabler Icons, MIT licensed -- see
// native/README.md) and the box-select cursor. Split out of viewer_qt.cpp
// specifically because these are self-contained -- pure Qt drawing plus
// segy::divergingColormap, no dependency on AppState/SegyCanvas/MainWindow
// -- so moving them here shrinks the main file with essentially zero risk
// of breaking anything that actually depends on app state.
#pragma once
#include <QCursor>
#include <QIcon>

namespace segyqt {

QIcon iconMagnifier(bool plus);
QIcon iconChartArea();
QIcon iconChartBar();
QIcon iconAdjustments();
QIcon iconSidebarCollapse();
QIcon iconFullView();
QIcon iconLayoutColumns();
QIcon iconLayoutRows();
QIcon iconFileDescription();
QIcon iconFileDigit();
QIcon iconLock(bool locked);
QIcon iconChevronUp();
QIcon iconChevronDown();
QIcon iconHand();
QIcon iconRectangle();
QIcon iconWiggleGlyph();
QIcon iconVariableDensityGlyph();
QIcon iconArrowsHorizontal();
QIcon iconXf();
QIcon iconLockView();
QIcon iconPick();
QIcon iconColumns3();
QIcon iconList();
QIcon iconIdents();
QIcon iconAgc();
QIcon iconAutocorrelation();
// A 24x24 stroked icon from SVG path data alone (Tabler style: stroke 2,
// round caps), e.g. "M3 12h18". Used by the optional toolbar buttons.
QIcon iconFromPaths(const QStringList& paths);

// Box-select cursor: a plain arrow with a small rectangle badge, matching
// "a pointer with a little rectangle."
QCursor makeBoxSelectCursor();

} // namespace segyqt
