#include "viewer_qt_icons.h"

#include "colormap.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPolygon>
#include <QSvgRenderer>

namespace segyqt {

namespace {

const QColor kIconStrokeColor(0xc7, 0xc9, 0xcc);

// Renders a small embedded SVG (real icons from Tabler Icons,
// https://tabler.io/icons, MIT licensed -- see native/README.md) to a
// QIcon, substituting the toolbar's actual stroke color for the SVG's
// `currentColor` so it matches the hand-drawn glyphs below exactly.
// QSvgRenderer (Qt6::Svg) avoids having to hand-port each icon's elliptical
// arc segments into QPainterPath calls, which was the previous approach's
// real problem -- editing a raw path string here is directly WYSIWYG with
// the source icon, unlike guessing Bezier control points from a preview.
QIcon renderSvgIcon(QString svg) {
    svg.replace(QStringLiteral("currentColor"), kIconStrokeColor.name());
    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pm(24, 24);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    renderer.render(&p);
    p.end();
    return QIcon(pm);
}

} // namespace

// Tabler Icons "zoom-in"/"zoom-out" (https://tabler.io/icons/icon/zoom-in),
// MIT licensed -- a matched pair sharing the same lens, so the two only
// differ by the missing vertical stroke in zoom-out's "-" vs. zoom-in's "+".
QIcon iconMagnifier(bool plus) {
    static const QString kSvgZoomIn = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M3 10a7 7 0 1 0 14 0a7 7 0 1 0 -14 0" />
          <path d="M7 10l6 0" />
          <path d="M10 7l0 6" />
          <path d="M21 21l-6 -6" />
        </svg>
    )");
    static const QString kSvgZoomOut = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M3 10a7 7 0 1 0 14 0a7 7 0 1 0 -14 0" />
          <path d="M7 10l6 0" />
          <path d="M21 21l-6 -6" />
        </svg>
    )");
    return renderSvgIcon(plus ? kSvgZoomIn : kSvgZoomOut);
}

// Tabler Icons "chart-area" (https://tabler.io/icons/icon/chart-area), MIT
// licensed -- Spectrum toolbar button (not wired to a feature yet).
QIcon iconChartArea() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M4 19l16 0" />
          <path d="M4 15l4 -6l4 2l4 -5l4 4l0 5l-16 0" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "chart-bar" (https://tabler.io/icons/icon/chart-bar), MIT
// licensed -- Histogram toolbar button (not wired to a feature yet).
QIcon iconChartBar() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M3 13a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v6a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -6" />
          <path d="M15 9a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v10a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -10" />
          <path d="M9 5a1 1 0 0 1 1 -1h4a1 1 0 0 1 1 1v14a1 1 0 0 1 -1 1h-4a1 1 0 0 1 -1 -1l0 -14" />
          <path d="M4 20h14" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "adjustments" (https://tabler.io/icons/icon/adjustments),
// MIT licensed -- the Display Parameters disclosure button. Replaced
// caret-up-down (read as "sort"/"expand", not "settings") per user
// preference from an icon-options gallery review; three vertical sliders
// at different heights previews the dialog's actual content (gain, mode,
// fill style) rather than implying an unrelated action.
QIcon iconAdjustments() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M4 10a2 2 0 1 0 4 0a2 2 0 0 0 -4 0" />
          <path d="M6 4v4" />
          <path d="M6 12v8" />
          <path d="M10 16a2 2 0 1 0 4 0a2 2 0 0 0 -4 0" />
          <path d="M12 4v10" />
          <path d="M12 18v2" />
          <path d="M16 7a2 2 0 1 0 4 0a2 2 0 0 0 -4 0" />
          <path d="M18 4v1" />
          <path d="M18 9v11" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "layout-sidebar-left-collapse"
// (https://tabler.io/icons/icon/layout-sidebar-left-collapse), MIT
// licensed -- shows/hides the right dock. Used unmirrored exactly as
// named even though our panel is on the right (the source icon's own
// panel is drawn on the left); the collapse-arrow meaning still reads
// fine, and mirroring wasn't asked for.
QIcon iconSidebarCollapse() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M4 6a2 2 0 0 1 2 -2h12a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-12a2 2 0 0 1 -2 -2l0 -12" />
          <path d="M9 4v16" />
          <path d="M15 10l-2 2l2 2" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "layout-columns" (https://tabler.io/icons/icon/layout-columns),
// MIT licensed -- Split View toolbar toggle.
QIcon iconLayoutColumns() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M4 6a2 2 0 0 1 2 -2h12a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-12a2 2 0 0 1 -2 -2l0 -12" />
          <path d="M12 4l0 16" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "layout-rows" (https://tabler.io/icons/icon/layout-rows),
// MIT licensed -- Split View toolbar toggle when the split is currently
// stacked top/bottom (Vertical Split from the button's own right-click
// menu) instead of side by side, so the icon keeps depicting the actual
// divider orientation rather than always showing the vertical-divider
// default.
QIcon iconLayoutRows() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M4 6a2 2 0 0 1 2 -2h12a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-12a2 2 0 0 1 -2 -2l0 -12" />
          <path d="M4 12l16 0" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "columns-3" (https://tabler.io/icons/icon/columns-3), MIT
// licensed -- Octave Band Display toolbar button (a row of side-by-side
// panels).
QIcon iconColumns3() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M3 4a1 1 0 0 1 1 -1h16a1 1 0 0 1 1 1v16a1 1 0 0 1 -1 1h-16a1 1 0 0 1 -1 -1v-16" />
          <path d="M9 3v18" />
          <path d="M15 3v18" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "list" (https://tabler.io/icons/icon/list), MIT licensed --
// the Foreground/Background dataset box's "pick a dataset" button.
QIcon iconList() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M9 6l11 0" />
          <path d="M9 12l11 0" />
          <path d="M9 18l11 0" />
          <path d="M5 6l0 .01" />
          <path d="M5 12l0 .01" />
          <path d="M5 18l0 .01" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Hand-drawn (no matching Tabler icon for this app-specific concept) --
// Idents toolbar button: a small zigzag line (the ident plot) over two
// short rows (the ident text rows), echoing the feature's own layout --
// see native/README.md.
QIcon iconIdents() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M3 9l4 -4l4 5l4 -6l4 3l2 -1" />
          <path d="M4 15l16 0" />
          <path d="M4 19l16 0" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "arrows-horizontal" (https://tabler.io/icons/icon/arrows-horizontal),
// MIT licensed -- Flip Horizontal toolbar button.
QIcon iconArrowsHorizontal() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M7 8l-4 4l4 4" />
          <path d="M17 8l4 4l-4 4" />
          <path d="M3 12l18 0" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "file-description"
// (https://tabler.io/icons/icon/file-description), MIT licensed -- Text/
// EBCDIC Header viewer toolbar button. Replaced align-justified (plain text
// lines, no document framing) per user preference from an icon-options
// gallery review.
QIcon iconFileDescription() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M14 3v4a1 1 0 0 0 1 1h4" />
          <path d="M17 21h-10a2 2 0 0 1 -2 -2v-14a2 2 0 0 1 2 -2h7l5 5v11a2 2 0 0 1 -2 2" />
          <path d="M9 17h6" />
          <path d="M9 13h6" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "file-digit" (https://tabler.io/icons/icon/file-digit), MIT
// licensed -- Binary Header viewer toolbar button. Replaced binary (loose
// "01" digit pairs) per user preference from the same gallery review, so it
// now visually pairs with file-description's document framing.
QIcon iconFileDigit() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M14 3v4a1 1 0 0 0 1 1h4" />
          <path d="M9 13a1 1 0 0 1 1 -1h1a1 1 0 0 1 1 1v3a1 1 0 0 1 -1 1h-1a1 1 0 0 1 -1 -1l0 -3" />
          <path d="M17 21h-10a2 2 0 0 1 -2 -2v-14a2 2 0 0 1 2 -2h7l5 5v11a2 2 0 0 1 -2 2" />
          <path d="M15 12v5" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "lock"/"lock-open" (https://tabler.io/icons/icon/lock), MIT
// licensed -- Lock toolbar toggle: when checked, split view's toolbar/menu
// actions apply to both panels at once instead of just the active one
// (see MainWindow::targetPanels()).
QIcon iconLock(bool locked) {
    static const QString kSvgLocked = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M5 13a2 2 0 0 1 2 -2h10a2 2 0 0 1 2 2v6a2 2 0 0 1 -2 2h-10a2 2 0 0 1 -2 -2v-6" />
          <path d="M11 16a1 1 0 1 0 2 0a1 1 0 0 0 -2 0" />
          <path d="M8 11v-4a4 4 0 1 1 8 0v4" />
        </svg>
    )");
    static const QString kSvgOpen = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M5 13a2 2 0 0 1 2 -2h10a2 2 0 0 1 2 2v6a2 2 0 0 1 -2 2h-10a2 2 0 0 1 -2 -2l0 -6" />
          <path d="M11 16a1 1 0 1 0 2 0a1 1 0 1 0 -2 0" />
          <path d="M8 11v-5a4 4 0 0 1 8 0" />
        </svg>
    )");
    return renderSvgIcon(locked ? kSvgLocked : kSvgOpen);
}

// Tabler Icons "chevron-up"/"chevron-down"
// (https://tabler.io/icons/icon/chevron-up), MIT licensed -- the vertical
// toolbar's Gain +/- buttons (see the gain-toolbar-controls comment near
// MainWindow's construction for why a slider is replaced by these when the
// toolbar is docked Left/Right).
QIcon iconChevronUp() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M6 15l6 -6l6 6" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

QIcon iconChevronDown() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M6 9l6 6l6 -6" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "hand-move" (https://tabler.io/icons/icon/hand-move), MIT
// licensed, embedded verbatim apart from currentColor substitution.
QIcon iconHand() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M8 13v-8.5a1.5 1.5 0 0 1 3 0v7.5" />
          <path d="M11 11.5v-2a1.5 1.5 0 0 1 3 0v2.5" />
          <path d="M14 10.5a1.5 1.5 0 0 1 3 0v1.5" />
          <path d="M17 11.5a1.5 1.5 0 0 1 3 0v4.5a6 6 0 0 1 -6 6h-2h.208a6 6 0 0 1 -5.012 -2.7l-.196 -.3c-.312 -.479 -1.407 -2.388 -3.286 -5.728a1.5 1.5 0 0 1 .536 -2.022a1.867 1.867 0 0 1 2.28 .28l1.47 1.47" />
          <path d="M2.541 5.594a13.487 13.487 0 0 1 2.46 -1.427" />
          <path d="M14 3.458c1.32 .354 2.558 .902 3.685 1.612" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Tabler Icons "rectangle" (https://tabler.io/icons/icon/rectangle), MIT
// licensed.
QIcon iconRectangle() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M3 7a2 2 0 0 1 2 -2h14a2 2 0 0 1 2 2v10a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-10" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// Seismic Display toggle button: its icon always shows the mode a click
// switches *to*, not the current one (play/pause convention) -- Tabler
// Icons "wave-sine" (https://tabler.io/icons/icon/wave-sine), MIT
// licensed, for Wiggle...
QIcon iconWiggleGlyph() {
    static const QString kSvg = QStringLiteral(R"(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"
             fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M21 12h-2c-.894 0 -1.662 -.857 -1.761 -2c-.296 -3.45 -.749 -6 -2.749 -6s-2.5 3.582 -2.5 8s-.5 8 -2.5 8s-2.452 -2.547 -2.749 -6c-.1 -1.147 -.867 -2 -1.763 -2h-2" />
        </svg>
    )");
    return renderSvgIcon(kSvg);
}

// ...and a small vertically-graded red/white/blue swatch for Variable
// Density -- the same segy::divergingColormap the density plot and the
// dock's Amplitude Scale widget already use, so the icon looks like an
// actual sample of what that mode renders (and happens to read as a
// Dutch flag with soft transitions between the bands).
QIcon iconVariableDensityGlyph() {
    QPixmap pm(24, 24);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    QRectF swatch(4, 3, 16, 18);
    QLinearGradient gradient(swatch.topLeft(), swatch.bottomLeft());
    constexpr int kStops = 6;
    for (int i = 0; i <= kStops; ++i) {
        float t = 1.0f - 2.0f * float(i) / float(kStops);
        uint8_t r, g, b;
        segy::divergingColormap(t, &b, &g, &r);
        gradient.setColorAt(double(i) / kStops, QColor(r, g, b));
    }
    p.setPen(QPen(kIconStrokeColor, 1));
    p.setBrush(gradient);
    p.drawRoundedRect(swatch, 3, 3);
    p.end();
    return QIcon(pm);
}

QCursor makeBoxSelectCursor() {
    QPixmap pm(24, 24);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPolygon arrow;
    arrow << QPoint(1, 1) << QPoint(1, 15) << QPoint(5, 11) << QPoint(8, 18) << QPoint(10, 17) << QPoint(7, 10)
          << QPoint(12, 10);
    p.setPen(Qt::black);
    p.setBrush(Qt::white);
    p.drawPolygon(arrow);
    QPen pen(Qt::black);
    pen.setWidth(1);
    p.setPen(pen);
    p.setBrush(QColor(255, 220, 0));
    p.drawRect(QRect(13, 1, 9, 7));
    p.end();
    return QCursor(pm, 1, 1);
}

} // namespace segyqt
