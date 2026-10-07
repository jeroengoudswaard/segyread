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
QIcon iconColumns3();

// Box-select cursor: a plain arrow with a small rectangle badge, matching
// "a pointer with a little rectangle."
QCursor makeBoxSelectCursor();

} // namespace segyqt
