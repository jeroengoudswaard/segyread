#include "viewer_qt.h"

#include "axis_ticks.h"
#include "segy_decode.h"
#include "viewer_qt_icons.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDate>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPolygon>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTableWidget>
#include <QToolButton>
#include <QTimer>
#include <QtGlobal>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace segyqt {
namespace {

// "Dark Pro" palette (see the approved UI mockup) -- kept as named
// constants rather than scattered hex literals since several places
// (icons, canvas surround, amplitude scale widget) need to agree on them.
constexpr int kCanvasMargin = 16; // dark surround around the inset plot
// Reserved column for the amplitude-scale legend (see
// SegyCanvas::plotAreaWidth) -- a fixed width subtracted from the plot's
// own width whenever showColorBar is on, *regardless* of seismicMode, so
// the plot doesn't resize when toggling between Variable Density and
// Wiggle (only whether the legend widget itself draws anything in that
// reserved space changes). 12px of that is breathing room between the
// plot/chrome's own right axis and the legend, not the legend itself.
constexpr int kAmplitudeScaleColumnWidth = 46 + 12;
// Idents (see native/README.md): fixed pixel sizes rather than font-metric-
// derived, so SegyCanvas::identInsets() stays callable from contexts with
// no painter/font (wheelEvent, mouseMoveEvent) just as cheaply as it is
// from paintEvent.
constexpr int kIdentRowHeight = 18;  // one text-row ident
constexpr int kIdentPlotHeight = 54; // ident line-plot + its min/max labels
constexpr int kIdentRowGap = 2;      // gap between stacked ident elements
// Wiggle traces read as visually weaker than the variable-density raster at
// the same nominal gain (a wiggle line's "loudness" is judged by excursion
// width, not color saturation) -- boosting wiggle's effective gain by a
// fixed offset compensates, purely a display-side adjustment local to this
// call site (computeWiggleLayout's own default/tests are untouched).
// Started at 6 dB, raised to 9 dB after still reading weak by comparison.
constexpr double kWiggleGainBoostDb = 9.0;
const QColor kCanvasSurroundColor(0x14, 0x14, 0x16);
const QColor kAccentColor(0xe8, 0x90, 0x5a);
const QColor kSecondaryTextColor(0x9a, 0x9c, 0xa3);

// One color per selection box (AppState::selectionBoxes), assigned once at
// creation from SelectionBox::colorIndex and never reassigned -- so
// Histogram/Spectrum can show "the same color as the box it came from" and
// have that stay meaningful even after other boxes are deleted. Cycles
// once more boxes exist than colors (no hard limit on box count).
QColor boxColorForIndex(int colorIndex) {
    static const QColor kPalette[] = {
        QColor(255, 220, 0),   // yellow
        QColor(80, 200, 255),  // cyan
        QColor(140, 255, 120), // green
        QColor(255, 140, 220), // pink
        QColor(180, 140, 255), // purple
        QColor(255, 150, 80),  // orange
        QColor(120, 255, 220), // teal
        QColor(255, 255, 255), // white
        QColor(255, 90, 90),   // red
        QColor(160, 200, 255), // pale blue
    };
    constexpr int kPaletteSize = int(sizeof(kPalette) / sizeof(kPalette[0]));
    return kPalette[((colorIndex % kPaletteSize) + kPaletteSize) % kPaletteSize];
}

// The box Delete/Zoom/Histogram/Spectrum/the Selection radio buttons all
// act on -- null if none is right-click-selected, or the selected index
// went stale (e.g. right after that box was deleted). Centralizes the
// bounds check every one of those call sites would otherwise repeat.
const SelectionBox* activeBox(const AppState& app) {
    if (app.activeBoxIndex < 0 || app.activeBoxIndex >= int(app.selectionBoxes.size())) return nullptr;
    return &app.selectionBoxes[size_t(app.activeBoxIndex)];
}
SelectionBox* activeBoxMut(AppState& app) {
    if (app.activeBoxIndex < 0 || app.activeBoxIndex >= int(app.selectionBoxes.size())) return nullptr;
    return &app.selectionBoxes[size_t(app.activeBoxIndex)];
}

// Resolves a Histogram/Spectrum dialog's Full View/Selection radio into an
// actual trace/sample range plus the color its plot should draw in (white
// for Full View, the active box's own color for Selection) -- shared by
// computeAndShowHistogram/computeAndShowSpectrum so the two don't
// duplicate this range/color resolution.
struct AnalysisSource {
    int64_t traceStart, traceEnd;
    int32_t sampleStart, sampleEnd;
    QColor color;
};
AnalysisSource resolveAnalysisSource(const AppState& app, bool useSelection) {
    AnalysisSource src{0, app.foreground->traceCount, 0, app.foreground->binHeader.samplesPerTrace, Qt::white};
    if (!useSelection) return src;
    const SelectionBox* box = activeBox(app);
    if (!box || !box->complete) return src;
    src.traceStart = int64_t(std::min(box->traceA, box->traceB));
    src.traceEnd = int64_t(std::max(box->traceA, box->traceB));
    src.sampleStart = int32_t(std::min(box->sampleA, box->sampleB));
    src.sampleEnd = int32_t(std::max(box->sampleA, box->sampleB));
    src.color = boxColorForIndex(box->colorIndex);
    return src;
}

// Forward declaration: defined below (shared by Histogram/Spectrum's own
// axis drawing), but ClipHistogramWidget::paintEvent -- earlier in the file
// -- needs it too.
void drawXAxisTicksAndLabels(QPainter& painter, const QRect& chartRect, const std::vector<segy::AxisTick>& ticks,
                              const QColor& textColor, const std::function<int(const std::string&)>& measureWidth);

// Fixed pixel size (not point size): chrome.cpp's layout math reserves
// column widths in exact pixels via TextMetrics, so the axis font's actual
// rendered size must not silently drift with DPI/point-size conversion.
QFont axisFont() {
    QFont font;
    font.setPixelSize(13);
    return font;
}

// QPainter's standard rotated-label idiom (translate to the anchor, rotate,
// draw at the local origin) -- replaces both the Windows GDI LOGFONT
// rotated-font path and the manual pixel-rotation workaround the FLTK shell
// needed after fl_draw(angle,...) turned out to be unreliable (see
// native/README.md). One shared, mature implementation instead of two.
// `anchorX` is the near edge (closest to the plot) and `centerY` is the
// vertical center of the label run, matching how ChromeLayout's fields are
// already defined by chrome.cpp.
//
// After the -90 degree rotation, drawText(0,0,...)'s glyphs extend from
// -ascent to +descent *across* anchorX, not from anchorX to anchorX+height
// -- i.e. most of the text's thickness falls *before* anchorX, not after
// it. chrome.cpp's column widths (fontHeightPx-based) assume the opposite:
// that a column starting at anchorX is where the label's thickness begins.
// Shifting the anchor forward by the font's ascent makes the visible text
// span [anchorX, anchorX+fontHeightPx], matching that assumption -- without
// this, the left-side title/numbers bleed into whatever sits before their
// reserved column (confirmed: the left axis title overlapping the color
// bar, since it has zero margin before it).
void drawRotatedLabel(QPainter& painter, const QFont& font, int anchorX, int centerY, const std::string& text) {
    if (text.empty()) return;
    QString qtext = QString::fromStdString(text);
    QFontMetrics fm(font);
    int textWidth = fm.horizontalAdvance(qtext);
    painter.save();
    painter.setFont(font);
    painter.translate(anchorX + fm.ascent(), centerY + textWidth / 2);
    painter.rotate(-90);
    painter.drawText(0, 0, qtext);
    painter.restore();
}

// Idents (see native/README.md): short display label and raw-value
// accessor for each of the 7 fields segy::TraceHeader already decodes --
// kept together so a new IdentField enumerator is a compile error in both
// places (a switch with no default) until handled, not a silent gap.
const char* identFieldLabel(IdentField f) {
    switch (f) {
        case IdentField::TraceSequenceLine: return "Seq (Line)";
        case IdentField::TraceSequenceFile: return "Seq (File)";
        case IdentField::FieldRecord: return "Field Record";
        case IdentField::TraceNumber: return "Trace Number";
        case IdentField::Cdp: return "CDP";
        case IdentField::X: return "X";
        case IdentField::Y: return "Y";
    }
    return "";
}

int32_t identFieldValue(const segy::TraceHeader& th, IdentField f) {
    switch (f) {
        case IdentField::TraceSequenceLine: return th.traceSequenceLine;
        case IdentField::TraceSequenceFile: return th.traceSequenceFile;
        case IdentField::FieldRecord: return th.fieldRecord;
        case IdentField::TraceNumber: return th.traceNumber;
        case IdentField::Cdp: return th.cdp;
        case IdentField::X: return th.x;
        case IdentField::Y: return th.y;
    }
    return 0;
}

// Evenly-spaced trace indices across the visible view, clamped into
// [0, traceCount), with adjacent duplicates collapsed (a heavily zoomed-in
// view can otherwise request more positions than there are distinct
// traces to show). `count` is deliberately different per caller -- see
// native/README.md: ~12 for the readable text rows, one per plot pixel
// column for the smooth ident plot/overlay lines.
std::vector<int64_t> identTracePositions(const segy::ViewRange& view, int64_t traceCount, int count) {
    std::vector<int64_t> result;
    if (traceCount <= 0 || count <= 0) return result;
    result.reserve(size_t(count));
    double span = view.traceEnd - view.traceStart;
    for (int i = 0; i < count; ++i) {
        double frac = count == 1 ? 0.5 : double(i) / double(count - 1);
        int64_t idx = int64_t(std::lround(view.traceStart + frac * span));
        idx = std::clamp<int64_t>(idx, 0, traceCount - 1);
        if (result.empty() || result.back() != idx) result.push_back(idx);
    }
    return result;
}

// Toolbar overflow "More" button icon: the chevron glyph (iconChevronDown(),
// same one used for Gain -3dB) with a small accent-colored badge showing
// how many actions are currently hidden, when any are -- the "Option A"
// mockup look the user picked (chevron fused to the toolbar + a count
// badge) over the plain list icon the status-bar version originally
// shipped with. Regenerated fresh each time updateToolbarOverflow() runs,
// since the count changes with the window size.
QIcon renderOverflowIcon(int hiddenCount) {
    QPixmap pm = iconChevronDown().pixmap(24, 24);
    if (hiddenCount > 0) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QRectF badgeRect(12, 0, 12, 12);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xe8, 0x90, 0x5a)); // Dark Pro accent
        p.drawEllipse(badgeRect);
        QFont font = p.font();
        font.setPixelSize(9);
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(0x1e, 0x1f, 0x22)); // dark text, readable on the accent orange
        p.drawText(badgeRect, Qt::AlignCenter, QString::number(std::min(hiddenCount, 99)));
    }
    return QIcon(pm);
}

// " -- filename.sgy" once a file is loaded, otherwise empty -- appended to
// every window title that shows data for one specific panel (the main
// window, and every popup/dialog below), so which dataset a given window
// is showing is always visible and titled the same way everywhere.
QString datasetTitleSuffix(const AppState& app) {
    if (!app.loaded || app.foreground->filePath.empty()) return QString();
    return QString::fromUtf8(" \xe2\x80\x94 ") + QString::fromStdString(app.foreground->filePath.filename().string());
}

// The vertical toolbar's gain value label (between the up/down arrows) can
// show a non-multiple-of-3 value once set via its right-click "exact value"
// dialog, unlike the toolbar/spin box widgets either side of it, which are
// int-only -- one decimal place, with a trailing ".0" trimmed so the common
// case (whole dB values) still reads as plain "12dB".
QString formatGainDb(double db) {
    double rounded = std::round(db * 10.0) / 10.0;
    QString text = QString::number(rounded, 'f', 1);
    if (text.endsWith(".0")) text.chop(2);
    return text + "dB";
}

// A left-to-right button row in a fixed, deterministic order -- every
// settings dialog in this app wants OK first and Cancel last with
// whatever else goes in between, but QDialogButtonBox reorders its
// buttons per platform convention (and some styles draw a small icon on
// each) instead of respecting insertion order, fighting that requirement
// directly. Plain QPushButtons in a QHBoxLayout sidestep both.
void addButtonRow(QDialog* dialog, QVBoxLayout* layout,
                   const std::vector<std::pair<QString, std::function<void()>>>& buttons) {
    QHBoxLayout* row = new QHBoxLayout();
    row->addStretch();
    for (const auto& entry : buttons) {
        QPushButton* button = new QPushButton(entry.first);
        std::function<void()> onClick = entry.second;
        QObject::connect(button, &QPushButton::clicked, dialog, onClick);
        row->addWidget(button);
    }
    layout->addLayout(row);
}

// Shared by SegyCanvas::makeRenderContext() and MainWindow's
// compute-and-show Histogram/Spectrum methods -- same fields either way,
// so kept in one place rather than two copies drifting apart.
segy::RenderContext makeRenderContextFor(AppState& app) {
    segy::RenderContext ctx;
    ctx.file = &app.foreground->file;
    ctx.pyramid = &app.foreground->pyramid;
    ctx.binHeader = app.foreground->binHeader;
    ctx.traceCount = app.foreground->traceCount;
    ctx.traceStrideBytes = app.foreground->traceStrideBytes;
    ctx.pool = &app.pool;
    return ctx;
}

// Builds the exact 3200-character (40 lines x 80 cols, "C 1 "/"C10 "-style
// prefixes matching the SEG-Y convention) EBCDIC-ready text used by
// Calculator/Bandpass/Save SEG-Y -- see native/README.md. Shared by all
// three so the header format can't drift between them. `inputBlocks` is one
// or more pre-formatted "FILE: ... TRACES: ..." blocks (Calculator has two,
// everything else has one); `processingSteps` becomes a numbered list,
// oldest first (AppState::processingHistory plus whatever step produced
// this particular output). The result is exactly kTextHeaderSize (3200)
// ASCII characters -- segy::encodeEbcdicText still pads/truncates
// defensively, but callers shouldn't rely on that.
QString buildSegyTextHeader(const QStringList& inputBlocks, const std::vector<std::string>& processingSteps,
                             const QString& outputName, int64_t outTraces, int outSamplesPerTrace,
                             double outSampleIntervalUs) {
    QStringList lines;
    lines << "CLIENT:                              JOB:                     LINE:";
    lines << "AREA:                                SURVEY:";
    lines << QString("PROCESSED BY: SEGYREAD VIEWER (NATIVE)          DATE: %1")
                 .arg(QDate::currentDate().toString("yyyy-MM-dd"));
    lines << "";
    lines << "INPUT DATA";
    for (const QString& block : inputBlocks) lines << ("  " + block);
    lines << "";
    lines << "PROCESSING HISTORY (MOST RECENT LAST)";
    int step = 1;
    for (const std::string& entry : processingSteps) {
        lines << QString("  %1. %2").arg(step++).arg(QString::fromStdString(entry));
    }
    lines << "";
    lines << "OUTPUT DATASET";
    lines << ("  NAME:      " + outputName);
    lines << QString("  TRACES:    %1       SAMPLES/TRACE: %2     SAMPLE INTERVAL: %3 US")
                 .arg(outTraces)
                 .arg(outSamplesPerTrace)
                 .arg(qint64(outSampleIntervalUs));
    lines << "  FORMAT:    5 (IEEE FLOATING POINT)";

    QString out;
    out.reserve(3200);
    for (int i = 0; i < 40; ++i) {
        QString content = i < lines.size() ? lines[i] : QString();
        QString line = QString("C%1 %2").arg(i + 1, 2).arg(content);
        line = line.size() > 80 ? line.left(80) : line.leftJustified(80, ' ');
        out += line;
    }
    return out;
}

// Where Calculator/Bandpass write their generated SEG-Y file before loading
// it into panelA_ -- a per-app scratch folder under the OS temp directory,
// not a user-chosen path (those two features have no file-save prompt; Save
// SEG-Y is what writes a dataset somewhere permanent). `outputName` becomes
// the file's stem, sanitized to filesystem-safe characters since it's
// user-editable text in both dialogs.
std::filesystem::path scratchSegyPath(const QString& outputName) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "segyread_scratch";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    QString safe = outputName;
    for (QChar& c : safe) {
        if (!c.isLetterOrNumber() && c != QChar('_') && c != QChar('-') && c != QChar('.')) c = QChar('_');
    }
    if (safe.isEmpty()) safe = "dataset";
    return dir / (safe.toStdString() + ".sgy");
}

// Renders one octave-band panel for the Octave Band Display window: one
// column per trace in `traces` (nearest-neighbor, not blended -- see
// native/README.md's reasoning for the main canvas's own trace decimation,
// which applies identically here), auto-clipped to that band's own peak
// magnitude since amplitude varies wildly band-to-band. `Format_RGB32` packs
// 0x00RRGGBB, matching renderFrame()'s own convention.
QImage rasterizeOctaveBand(const std::vector<std::vector<float>>& traces, int width, int height,
                            segy::ColorScale colorScale) {
    QImage img(width, height, QImage::Format_RGB32);
    if (traces.empty() || traces[0].empty()) {
        img.fill(Qt::white);
        return img;
    }
    float clip = 1e-6f;
    for (const std::vector<float>& tr : traces) {
        for (float v : tr) clip = std::max(clip, std::fabs(v));
    }
    int samplesPerTrace = int(traces[0].size());
    for (int y = 0; y < height; ++y) {
        int s = std::clamp(int((double(y) + 0.5) / height * samplesPerTrace), 0, samplesPerTrace - 1);
        uint32_t* row = reinterpret_cast<uint32_t*>(img.scanLine(y));
        for (int x = 0; x < width; ++x) {
            int t = std::clamp(int((double(x) + 0.5) / width * traces.size()), 0, int(traces.size()) - 1);
            float value = traces[size_t(t)][size_t(s)];
            uint8_t r, g, b;
            segy::applyColorScale(colorScale, value / clip, &b, &g, &r);
            row[x] = (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        }
    }
    return img;
}

} // namespace

AmplitudeScaleWidget::AmplitudeScaleWidget(QWidget* parent) : QWidget(parent) {}

void AmplitudeScaleWidget::setClipRange(float posClip, float negClip) {
    posClip_ = posClip > 0.0f ? posClip : 1.0f;
    negClip_ = negClip > 0.0f ? negClip : 1.0f;
    update();
}

void AmplitudeScaleWidget::setColorScale(segy::ColorScale scale) {
    colorScale_ = scale;
    update();
}

void AmplitudeScaleWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    int w = width();
    int h = height();

    // Semi-transparent dark backing chip so the legend stays legible
    // sitting on top of whatever seismic colors happen to be underneath it
    // (this widget is an overlay on the plot, not a dock section anymore).
    painter.fillRect(rect(), QColor(0x14, 0x14, 0x16, 200));

    QFontMetrics fm(painter.font());
    int textH = fm.height();
    constexpr int kPad = 6;
    int barW = 8;
    int barX = kPad;
    int barY = kPad + textH / 2;
    int barH = h - 2 * (kPad + textH / 2);

    // Same colormap the plot itself uses (segy::applyColorScale), so the
    // legend always matches what's actually on screen -- sampled at a
    // handful of stops rather than reimplementing the curve as a CSS-like
    // gradient string. 8 stops exactly lands on all 5 breakpoints of the
    // Yellow-Red-White-Black-Cyan scale (spaced every 2 stops), so it's
    // never faceted, just fewer samples than strictly needed for the
    // plain two-color scales.
    //
    // posClip_/negClip_ can differ (a manual asymmetric clip -- see
    // ClipDialog), in which case "0" isn't at the vertical center: each
    // half of the gradient is sampled over its own [0, clip] range, split
    // at whatever fraction posClip_/(posClip_+negClip_) actually is.
    float totalClip = posClip_ + negClip_;
    float zeroFrac = totalClip > 0.0f ? posClip_ / totalClip : 0.5f; // 0 sits this far down from the top
    QLinearGradient gradient(0, barY, 0, barY + barH);
    constexpr int kStopsPerHalf = 8;
    for (int i = 0; i <= kStopsPerHalf; ++i) {
        float frac = float(i) / float(kStopsPerHalf);  // 0 (top of this half) .. 1 (zero line)
        float t = 1.0f - frac;                          // +1 (top) .. 0 (zero line), for the positive half
        uint8_t r, g, b;
        segy::applyColorScale(colorScale_, t, &b, &g, &r);
        gradient.setColorAt(double(frac) * zeroFrac, QColor(r, g, b));
    }
    for (int i = 0; i <= kStopsPerHalf; ++i) {
        float frac = float(i) / float(kStopsPerHalf);  // 0 (zero line) .. 1 (bottom of this half)
        float t = -frac;                                // 0 (zero line) .. -1 (bottom), for the negative half
        uint8_t r, g, b;
        segy::applyColorScale(colorScale_, t, &b, &g, &r);
        gradient.setColorAt(double(zeroFrac) + double(frac) * (1.0 - zeroFrac), QColor(r, g, b));
    }
    painter.fillRect(QRectF(barX, barY, barW, barH), gradient);

    painter.setPen(kSecondaryTextColor);
    int textX = barX + barW + kPad;
    int textW = w - textX - kPad;
    QString topLabel = QString("+%1").arg(int(posClip_));
    QString bottomLabel = QString::fromUtf8("\xe2\x88\x92") + QString::number(int(negClip_)); // U+2212 minus
    int zeroY = barY + int(zeroFrac * barH);
    painter.drawText(QRect(textX, 0, textW, textH), Qt::AlignLeft | Qt::AlignTop, topLabel);
    painter.drawText(QRect(textX, zeroY - textH / 2, textW, textH), Qt::AlignLeft | Qt::AlignVCenter, "0");
    painter.drawText(QRect(textX, h - textH, textW, textH), Qt::AlignLeft | Qt::AlignBottom, bottomLabel);
}

ClipHistogramWidget::ClipHistogramWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(160);
}

void ClipHistogramWidget::setHistogram(const segy::HistogramResult& result) {
    result_ = result;
    update();
}

void ClipHistogramWidget::setClipValues(float posClip, float negClip) {
    posClip_ = std::max(posClip, 1e-6f);
    negClip_ = std::max(negClip, 1e-6f);
    update();
}

void ClipHistogramWidget::setSymmetrical(bool symmetrical) { symmetrical_ = symmetrical; }

QRect ClipHistogramWidget::chartRect() const {
    constexpr int kMarginLeft = 12, kMarginRight = 12, kMarginTop = 10, kMarginBottom = 22;
    return QRect(kMarginLeft, kMarginTop, width() - kMarginLeft - kMarginRight,
                 height() - kMarginTop - kMarginBottom);
}

int ClipHistogramWidget::xForValue(float value) const {
    QRect chart = chartRect();
    if (result_.rangeMax <= result_.rangeMin) return chart.left();
    float frac = (value - result_.rangeMin) / (result_.rangeMax - result_.rangeMin);
    return chart.left() + int(std::clamp(frac, 0.0f, 1.0f) * chart.width());
}

float ClipHistogramWidget::valueForX(int x) const {
    QRect chart = chartRect();
    float frac = chart.width() > 0 ? float(x - chart.left()) / float(chart.width()) : 0.0f;
    frac = std::clamp(frac, 0.0f, 1.0f);
    return result_.rangeMin + frac * (result_.rangeMax - result_.rangeMin);
}

int ClipHistogramWidget::hitTestLine(int x) const {
    constexpr int kHitRadius = 8;
    int negX = xForValue(-negClip_);
    int posX = xForValue(posClip_);
    if (std::abs(x - negX) <= kHitRadius) return 0;
    if (std::abs(x - posX) <= kHitRadius) return 1;
    return -1;
}

void ClipHistogramWidget::setValueFromDrag(int lineIndex, float value) {
    // Each line is a positive magnitude on its own side of zero, so the two
    // literally cannot cross -- clamp away from zero (never quite reaching
    // it) instead of letting either collapse to nothing.
    constexpr float kMinMagnitude = 1e-3f;
    if (lineIndex == 0) {
        negClip_ = std::max(-value, kMinMagnitude);
        if (symmetrical_) posClip_ = negClip_;
    } else {
        posClip_ = std::max(value, kMinMagnitude);
        if (symmetrical_) negClip_ = posClip_;
    }
    update();
    if (onChanged) onChanged(posClip_, negClip_);
}

void ClipHistogramWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    draggingLine_ = hitTestLine(event->pos().x());
}

void ClipHistogramWidget::mouseMoveEvent(QMouseEvent* event) {
    if (draggingLine_ < 0) return;
    setValueFromDrag(draggingLine_, valueForX(event->pos().x()));
}

void ClipHistogramWidget::mouseReleaseEvent(QMouseEvent*) { draggingLine_ = -1; }

void ClipHistogramWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kCanvasSurroundColor);
    QRect chart = chartRect();

    if (result_.percentOfTotal.empty()) {
        painter.setPen(kSecondaryTextColor);
        painter.drawText(rect(), Qt::AlignCenter, "No data.");
        return;
    }

    QFont font = axisFont();
    painter.setFont(font);
    QFontMetrics fm(font);
    auto measureWidth = [&fm](const std::string& s) {
        return s.empty() ? 0 : fm.horizontalAdvance(QString::fromStdString(s));
    };

    painter.setPen(QColor(0x3c, 0x3f, 0x41));
    painter.drawRect(chart);

    int numBins = int(result_.percentOfTotal.size());
    float maxPercent = 0.0f;
    for (float p : result_.percentOfTotal) maxPercent = std::max(maxPercent, p);
    maxPercent = std::max(1.0f, maxPercent * 1.15f);

    painter.setPen(Qt::NoPen);
    painter.setBrush(kAccentColor);
    float barSlot = float(chart.width()) / float(numBins);
    for (int i = 0; i < numBins; ++i) {
        float barH = result_.percentOfTotal[size_t(i)] / maxPercent * chart.height();
        QRectF barRect(chart.left() + i * barSlot, chart.bottom() - barH, std::max(1.0f, barSlot - 1.0f), barH);
        painter.drawRect(barRect);
    }

    constexpr int kMaxLines = 11;
    std::vector<segy::AxisTick> xTicks =
        segy::computeNiceAxisTicks(result_.rangeMin, result_.rangeMax, chart.width(), kMaxLines, measureWidth);
    drawXAxisTicksAndLabels(painter, chart, xTicks, kSecondaryTextColor, measureWidth);

    // The two draggable clip lines, drawn last so they sit on top of the bars.
    auto drawLine = [&](float value, bool active) {
        int x = xForValue(value);
        QPen pen(active ? Qt::white : kAccentColor);
        pen.setWidth(active ? 2 : 1);
        painter.setPen(pen);
        painter.drawLine(x, chart.top(), x, chart.bottom());
        constexpr int kHandle = 6;
        painter.setBrush(active ? Qt::white : kAccentColor);
        painter.drawRect(x - kHandle / 2, chart.top() - kHandle, kHandle, kHandle);
    };
    drawLine(-negClip_, draggingLine_ == 0);
    drawLine(posClip_, draggingLine_ == 1);
}

ClipDialog::ClipDialog(AppState& app, QWidget* parent) : QDialog(parent), app_(app) {
    setWindowTitle("Clip");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* hint = new QLabel(
        "Drag either line, or type exact values below. The seismic display updates live.");
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #9a9ca3; font-size: 11px;");
    layout->addWidget(hint);

    histogramWidget_ = new ClipHistogramWidget(this);
    layout->addWidget(histogramWidget_, 1);

    QHBoxLayout* spinRow = new QHBoxLayout();
    spinRow->addWidget(new QLabel("Min (-):"));
    negSpin_ = new QDoubleSpinBox();
    negSpin_->setRange(1e-3, 1e12);
    negSpin_->setDecimals(3);
    spinRow->addWidget(negSpin_);
    spinRow->addStretch();
    symmetricalCheck_ = new QCheckBox("Symmetrical");
    symmetricalCheck_->setChecked(true);
    spinRow->addWidget(symmetricalCheck_);
    spinRow->addStretch();
    spinRow->addWidget(new QLabel("Max (+):"));
    posSpin_ = new QDoubleSpinBox();
    posSpin_->setRange(1e-3, 1e12);
    posSpin_->setDecimals(3);
    spinRow->addWidget(posSpin_);
    layout->addLayout(spinRow);

    histogramWidget_->setSymmetrical(true);
    connect(symmetricalCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        histogramWidget_->setSymmetrical(checked);
        if (checked) {
            // Turning it back on snaps to symmetric around whichever of
            // the two current values is larger, rather than silently
            // leaving them mismatched until the next drag.
            float magnitude = std::max(posSpin_->value(), negSpin_->value());
            applyLive(float(magnitude), float(magnitude), false);
        }
    });

    histogramWidget_->onChanged = [this](float posClip, float negClip) { applyLive(posClip, negClip, false); };
    connect(posSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        float negClip = symmetricalCheck_->isChecked() ? float(v) : float(negSpin_->value());
        applyLive(float(v), negClip, false);
    });
    connect(negSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        float posClip = symmetricalCheck_->isChecked() ? float(v) : float(posSpin_->value());
        applyLive(posClip, float(v), false);
    });

    addButtonRow(this, layout,
                 {
                     {"OK",
                      [this]() {
                          if (onChanged_) onChanged_();
                          accept();
                      }},
                     {"Apply",
                      [this]() {
                          recomputeHistogram(-negSpin_->value(), posSpin_->value());
                          if (onChanged_) onChanged_();
                      }},
                     {"Cancel",
                      [this]() {
                          app_.display.manualClip = original_;
                          if (onChanged_) onChanged_();
                          reject();
                      }},
                 });

    resize(480, 380);
}

void ClipDialog::setChangedCallback(std::function<void()> callback) { onChanged_ = std::move(callback); }

void ClipDialog::applyLive(float posClip, float negClip, bool /*fromSymmetricalDrag*/) {
    posClip = std::max(posClip, 1e-6f);
    negClip = std::max(negClip, 1e-6f);
    app_.display.manualClip.enabled = true;
    app_.display.manualClip.posMagnitude = posClip;
    app_.display.manualClip.negMagnitude = negClip;

    posSpin_->blockSignals(true);
    negSpin_->blockSignals(true);
    posSpin_->setValue(double(posClip));
    negSpin_->setValue(double(negClip));
    posSpin_->blockSignals(false);
    negSpin_->blockSignals(false);
    histogramWidget_->setClipValues(posClip, negClip);

    if (onChanged_) onChanged_();
}

void ClipDialog::recomputeHistogram(float rangeMin, float rangeMax) {
    if (!app_.loaded) return;
    // "The displayed traces" -- the current on-screen viewport, not the
    // whole file or a selection box (a third, dialog-local scoping
    // distinct from Histogram/Spectrum's Full View/Selection radio).
    int64_t traceStart = int64_t(std::max(0.0, app_.view.traceStart));
    int64_t traceEnd = int64_t(std::min(double(app_.foreground->traceCount), app_.view.traceEnd));
    int32_t sampleStart = int32_t(std::max(0.0, app_.view.sampleStart));
    int32_t sampleEnd = int32_t(std::min(double(app_.foreground->binHeader.samplesPerTrace), app_.view.sampleEnd));
    constexpr int kBins = 101;
    segy::HistogramResult result = segy::computeHistogram(makeRenderContextFor(app_), traceStart, traceEnd,
                                                            sampleStart, sampleEnd, kBins, rangeMin, rangeMax);
    histogramWidget_->setHistogram(result);
}

void ClipDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!app_.loaded) return;
    original_ = app_.display.manualClip;

    float rawClip = std::max(std::fabs(app_.foreground->pyramid.globalMin), std::fabs(app_.foreground->pyramid.globalMax));
    float posClip = app_.display.manualClip.enabled ? app_.display.manualClip.posMagnitude : rawClip;
    float negClip = app_.display.manualClip.enabled ? app_.display.manualClip.negMagnitude : rawClip;

    // Bin over a bit wider than the initial clip so both lines start with
    // visible headroom on either side to drag into, not pinned to the
    // chart's own edges.
    recomputeHistogram(-negClip * 1.3f, posClip * 1.3f);
    posSpin_->setValue(double(posClip));
    negSpin_->setValue(double(negClip));
    histogramWidget_->setClipValues(posClip, negClip);
}

HistogramDialog::HistogramDialog(AppState& app, QWidget* parent) : QDialog(parent), app_(app) {
    setWindowTitle("Histogram Parameters");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QGroupBox* rangeGroup = new QGroupBox("Range");
    QHBoxLayout* rangeLayout = new QHBoxLayout(rangeGroup);
    fullViewRadio_ = new QRadioButton("Full View");
    selectionRadio_ = new QRadioButton("Selection");
    fullViewRadio_->setChecked(true);
    QButtonGroup* rangeButtons = new QButtonGroup(this);
    rangeButtons->addButton(fullViewRadio_);
    rangeButtons->addButton(selectionRadio_);
    rangeLayout->addWidget(fullViewRadio_);
    rangeLayout->addWidget(selectionRadio_);
    layout->addWidget(rangeGroup);

    QHBoxLayout* binsLayout = new QHBoxLayout();
    binsLayout->addWidget(new QLabel("Number of Bins:"));
    binsSpinBox_ = new QSpinBox();
    binsSpinBox_->setRange(2, 500);
    binsSpinBox_->setValue(app_.histogram.numBins);
    binsSpinBox_->setSingleStep(10); // arrows jump by 10; typing still accepts any value in range
    binsLayout->addWidget(binsSpinBox_);
    binsLayout->addStretch();
    layout->addLayout(binsLayout);

    QGroupBox* clipGroup = new QGroupBox("Clip Type");
    QHBoxLayout* clipLayout = new QHBoxLayout(clipGroup);
    dataClipRadio_ = new QRadioButton("Data");
    userClipRadio_ = new QRadioButton("User");
    dataClipRadio_->setChecked(true);
    QButtonGroup* clipButtons = new QButtonGroup(this);
    clipButtons->addButton(dataClipRadio_);
    clipButtons->addButton(userClipRadio_);
    clipLayout->addWidget(dataClipRadio_);
    clipLayout->addWidget(userClipRadio_);
    layout->addWidget(clipGroup);

    QHBoxLayout* minMaxLayout = new QHBoxLayout();
    minMaxLayout->addWidget(new QLabel("Min:"));
    minValueSpin_ = new QDoubleSpinBox();
    minValueSpin_->setRange(-1e9, 1e9);
    minValueSpin_->setDecimals(2);
    minMaxLayout->addWidget(minValueSpin_);
    minMaxLayout->addWidget(new QLabel("Max:"));
    maxValueSpin_ = new QDoubleSpinBox();
    maxValueSpin_->setRange(-1e9, 1e9);
    maxValueSpin_->setDecimals(2);
    minMaxLayout->addWidget(maxValueSpin_);
    layout->addLayout(minMaxLayout);

    connect(dataClipRadio_, &QRadioButton::toggled, this, [this](bool) { updateClipFieldsEnabled(); });
    updateClipFieldsEnabled();

    addButtonRow(this, layout,
                 {
                     {"OK",
                      [this]() {
                          applySettings();
                          if (onApplied_) onApplied_();
                          accept();
                      }},
                     {"Apply",
                      [this]() {
                          applySettings();
                          if (onApplied_) onApplied_();
                      }},
                     {"Cancel", [this]() { reject(); }},
                 });
}

void HistogramDialog::setAppliedCallback(std::function<void()> callback) { onApplied_ = std::move(callback); }

void HistogramDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // "Selection" only makes sense with a complete active box to compute
    // over -- fall back to Full View if there isn't one (none right-clicked
    // yet, or it was deleted since).
    const SelectionBox* box = activeBox(app_);
    bool hasCompleteActiveBox = box && box->complete;
    selectionRadio_->setEnabled(hasCompleteActiveBox);
    // Defaults to whichever the current box state actually supports --
    // Selection when a box is active, Full View when none is -- rather than
    // just gating availability and leaving whatever was checked last time,
    // so opening the dialog right after selecting a box doesn't require an
    // extra click to switch the radio over.
    if (hasCompleteActiveBox) {
        selectionRadio_->setChecked(true);
    } else {
        fullViewRadio_->setChecked(true);
    }
}

void HistogramDialog::updateClipFieldsEnabled() {
    bool userClip = !dataClipRadio_->isChecked();
    minValueSpin_->setEnabled(userClip);
    maxValueSpin_->setEnabled(userClip);
    if (!userClip) {
        // Prefill with the data's own range so switching to "User" starts
        // from a sensible value instead of 0 -- true 1st/99th-percentile
        // trimming (ignoring outlier spikes, per the original ask) needs a
        // real pass over the samples that doesn't exist yet (see
        // native/README.md); this uses the pyramid's already-computed
        // true min/max as a placeholder in the meantime.
        minValueSpin_->setValue(app_.loaded ? app_.foreground->pyramid.globalMin : 0.0);
        maxValueSpin_->setValue(app_.loaded ? app_.foreground->pyramid.globalMax : 0.0);
    }
}

void HistogramDialog::applySettings() {
    app_.histogram.useSelection = selectionRadio_->isChecked();
    app_.histogram.numBins = binsSpinBox_->value();
    app_.histogram.useUserClip = userClipRadio_->isChecked();
    app_.histogram.userMin = minValueSpin_->value();
    app_.histogram.userMax = maxValueSpin_->value();
}

// Odd-numbered smoothing window sizes, matching a reference frequency-
// spectrum tool's own Smooth submenu exactly (not a simple step-by-2
// range -- it widens the gaps at the high end: 11, 15, 21, 25, 31).
const int kSpectrumSmoothPoints[] = {1, 3, 5, 7, 9, 11, 15, 21, 25, 31};

SpectrumDialog::SpectrumDialog(AppState& app, QWidget* parent) : QDialog(parent), app_(app) {
    setWindowTitle("Spectrum Parameters");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QGroupBox* rangeGroup = new QGroupBox("Range");
    QHBoxLayout* rangeLayout = new QHBoxLayout(rangeGroup);
    fullViewRadio_ = new QRadioButton("Full View");
    selectionRadio_ = new QRadioButton("Selection");
    fullViewRadio_->setChecked(true);
    QButtonGroup* rangeButtons = new QButtonGroup(this);
    rangeButtons->addButton(fullViewRadio_);
    rangeButtons->addButton(selectionRadio_);
    rangeLayout->addWidget(fullViewRadio_);
    rangeLayout->addWidget(selectionRadio_);
    layout->addWidget(rangeGroup);

    QGroupBox* normalizeGroup = new QGroupBox("Normalize");
    QVBoxLayout* normalizeLayout = new QVBoxLayout(normalizeGroup);
    absoluteSelectedRadio_ = new QRadioButton("Absolute Max from Selected Curve");
    relativeEachRadio_ = new QRadioButton("Relative Max to Each Curve");
    absoluteUserRadio_ = new QRadioButton("Absolute Max from User");
    absoluteSelectedRadio_->setChecked(true);
    QButtonGroup* normalizeButtons = new QButtonGroup(this);
    normalizeButtons->addButton(absoluteSelectedRadio_);
    normalizeButtons->addButton(relativeEachRadio_);
    normalizeButtons->addButton(absoluteUserRadio_);
    normalizeLayout->addWidget(absoluteSelectedRadio_);
    normalizeLayout->addWidget(relativeEachRadio_);
    QHBoxLayout* userValueLayout = new QHBoxLayout();
    userValueLayout->addWidget(absoluteUserRadio_);
    userValueSpin_ = new QDoubleSpinBox();
    userValueSpin_->setRange(-1e9, 1e9);
    userValueSpin_->setDecimals(2);
    userValueLayout->addWidget(userValueSpin_);
    normalizeLayout->addLayout(userValueLayout);
    layout->addWidget(normalizeGroup);

    connect(absoluteUserRadio_, &QRadioButton::toggled, this, [this](bool checked) { userValueSpin_->setEnabled(checked); });
    updateUserValueEnabled();

    QHBoxLayout* smoothLayout = new QHBoxLayout();
    smoothLayout->addWidget(new QLabel("Smooth (points):"));
    smoothCombo_ = new QComboBox();
    for (int points : kSpectrumSmoothPoints) smoothCombo_->addItem(QString::number(points), points);
    smoothLayout->addWidget(smoothCombo_);
    smoothLayout->addStretch();
    layout->addLayout(smoothLayout);

    addButtonRow(this, layout,
                 {
                     {"OK",
                      [this]() {
                          applySettings();
                          if (onApplied_) onApplied_();
                          accept();
                      }},
                     {"Apply",
                      [this]() {
                          applySettings();
                          if (onApplied_) onApplied_();
                      }},
                     {"Cancel", [this]() { reject(); }},
                 });
}

void SpectrumDialog::setAppliedCallback(std::function<void()> callback) { onApplied_ = std::move(callback); }

void SpectrumDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    const SelectionBox* box = activeBox(app_);
    bool hasCompleteActiveBox = box && box->complete;
    selectionRadio_->setEnabled(hasCompleteActiveBox);
    // Same reasoning as HistogramDialog::showEvent: default to whatever the
    // current box state supports, not whatever was checked last time.
    if (hasCompleteActiveBox) {
        selectionRadio_->setChecked(true);
    } else {
        fullViewRadio_->setChecked(true);
    }
}

void SpectrumDialog::updateUserValueEnabled() { userValueSpin_->setEnabled(absoluteUserRadio_->isChecked()); }

void SpectrumDialog::applySettings() {
    app_.spectrum.useSelection = selectionRadio_->isChecked();
    if (absoluteSelectedRadio_->isChecked()) {
        app_.spectrum.normalizeMode = SpectrumNormalizeMode::AbsoluteSelectedCurve;
    } else if (relativeEachRadio_->isChecked()) {
        app_.spectrum.normalizeMode = SpectrumNormalizeMode::RelativeEachCurve;
    } else {
        app_.spectrum.normalizeMode = SpectrumNormalizeMode::AbsoluteFromUser;
    }
    app_.spectrum.userNormalizeValue = userValueSpin_->value();
    app_.spectrum.smoothPoints = smoothCombo_->currentData().toInt();
}

namespace {
// Shared by DisplayParametersDialog's two 4-way style groups (Variable
// Area Control / Wiggle Control) -- same options, separate groups.
QGroupBox* buildFillStyleGroup(const QString& title, QRadioButton* (&radios)[4], QWidget* parentDialog,
                                QButtonGroup* group) {
    QGroupBox* box = new QGroupBox(title);
    QVBoxLayout* boxLayout = new QVBoxLayout(box);
    const char* labels[4] = {"Solid", "PeakAmplitude", "Varifill", "Band"};
    for (int i = 0; i < 4; ++i) {
        radios[i] = new QRadioButton(labels[i], box);
        group->addButton(radios[i]);
        boxLayout->addWidget(radios[i]);
    }
    radios[0]->setChecked(true);
    Q_UNUSED(parentDialog);
    return box;
}
} // namespace

DisplayParametersDialog::DisplayParametersDialog(AppState& app, QWidget* parent) : QDialog(parent), app_(app) {
    setWindowTitle("Display Parameters");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);

    // Top row: display mode (real, synced with the toolbar/View menu) plus
    // two not-yet-wired placeholder checkboxes.
    QHBoxLayout* topLayout = new QHBoxLayout();
    variableDensityRadio_ = new QRadioButton("Variable Density");
    wiggleRadio_ = new QRadioButton("Wiggle Variable Area");
    QButtonGroup* modeButtons = new QButtonGroup(this);
    modeButtons->addButton(variableDensityRadio_);
    modeButtons->addButton(wiggleRadio_);
    topLayout->addWidget(variableDensityRadio_);
    topLayout->addWidget(wiggleRadio_);
    reversePolarityCheck_ = new QCheckBox("Reverse Polarity");
    centerLineCheck_ = new QCheckBox("Center Line");
    topLayout->addWidget(reversePolarityCheck_);
    topLayout->addWidget(centerLineCheck_);
    topLayout->addStretch();
    layout->addLayout(topLayout);

    connect(variableDensityRadio_, &QRadioButton::toggled, this, [this](bool checked) {
        segy::SeismicDisplayMode mode = checked ? segy::SeismicDisplayMode::VariableDensity
                                                 : segy::SeismicDisplayMode::Wiggle;
        if (onModeChanged_) onModeChanged_(mode);
        updateBlendingEnabled();
    });

    QHBoxLayout* columnsLayout = new QHBoxLayout();

    // Left column: fill/wiggle style checkboxes and the two 4-way style
    // groups.
    QVBoxLayout* leftColumn = new QVBoxLayout();
    QHBoxLayout* fillRow = new QHBoxLayout();
    positiveFillCheck_ = new QCheckBox("Positive Fill");
    negativeFillCheck_ = new QCheckBox("Negative Fill");
    positiveFillCheck_->setChecked(true);
    fillRow->addWidget(positiveFillCheck_);
    fillRow->addWidget(negativeFillCheck_);
    leftColumn->addLayout(fillRow);

    QHBoxLayout* wiggleRow = new QHBoxLayout();
    wiggleTraceCheck_ = new QCheckBox("Wiggle");
    positiveOverNegativeCheck_ = new QCheckBox("Positive Over Negative");
    wiggleTraceCheck_->setChecked(true);
    positiveOverNegativeCheck_->setChecked(true);
    wiggleRow->addWidget(wiggleTraceCheck_);
    wiggleRow->addWidget(positiveOverNegativeCheck_);
    leftColumn->addLayout(wiggleRow);

    QHBoxLayout* styleGroupsLayout = new QHBoxLayout();
    QButtonGroup* variableAreaButtons = new QButtonGroup(this);
    styleGroupsLayout->addWidget(
        buildFillStyleGroup("Variable Area Control", variableAreaStyleRadios_, this, variableAreaButtons));
    QButtonGroup* wiggleControlButtons = new QButtonGroup(this);
    styleGroupsLayout->addWidget(
        buildFillStyleGroup("Wiggle Control", wiggleControlStyleRadios_, this, wiggleControlButtons));
    leftColumn->addLayout(styleGroupsLayout);

    QHBoxLayout* blendingLayout = new QHBoxLayout();
    blendingLayout->addWidget(new QLabel("Variable Density Blending:"));
    blendingSpin_ = new QDoubleSpinBox();
    blendingSpin_->setRange(0.0, 100.0);
    blendingLayout->addWidget(blendingSpin_);
    leftColumn->addLayout(blendingLayout);
    leftColumn->addStretch();
    columnsLayout->addLayout(leftColumn);

    // Right column: the numeric fields, Gain first since that's the one
    // real (already-wired) control among them.
    QVBoxLayout* rightColumn = new QVBoxLayout();
    auto numericRow = [&](const QString& label, QDoubleSpinBox*& spin, double min, double max) {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel(label));
        spin = new QDoubleSpinBox();
        spin->setRange(min, max);
        row->addWidget(spin);
        rightColumn->addLayout(row);
    };
    numericRow("Gain:", gainSpin_, -6.0, 24.0);
    numericRow("Percent Overlap:", percentOverlapSpin_, 0.0, 1000.0);
    numericRow("Fill Baseline:", fillBaselineSpin_, -1e9, 1e9);
    numericRow("Wiggle Minimum Spacing:", wiggleMinSpacingSpin_, 0.0, 100.0);
    numericRow("Var Minimum Spacing:", varMinSpacingSpin_, 0.0, 100.0);
    rightColumn->addStretch();
    columnsLayout->addLayout(rightColumn);

    layout->addLayout(columnsLayout);

    addButtonRow(this, layout,
                 {
                     {"OK",
                      [this]() {
                          applySettings();
                          accept();
                      }},
                     {"Apply", [this]() { applySettings(); }},
                     {"Reset", [this]() { refreshFromAppState(); }},
                     {"Cancel", [this]() { reject(); }},
                 });

    refreshFromAppState();
}

void DisplayParametersDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    refreshFromAppState();
}

void DisplayParametersDialog::updateBlendingEnabled() { blendingSpin_->setEnabled(variableDensityRadio_->isChecked()); }

void DisplayParametersDialog::setGainChangedCallback(std::function<void(double)> callback) {
    onGainChanged_ = std::move(callback);
}

void DisplayParametersDialog::setModeChangedCallback(std::function<void(segy::SeismicDisplayMode)> callback) {
    onModeChanged_ = std::move(callback);
}

void DisplayParametersDialog::refreshFromAppState() {
    variableDensityRadio_->setChecked(app_.display.seismicMode == segy::SeismicDisplayMode::VariableDensity);
    wiggleRadio_->setChecked(app_.display.seismicMode == segy::SeismicDisplayMode::Wiggle);
    gainSpin_->setValue(app_.display.gainDb);
    const WigglePresentationSettings& w = app_.wigglePresentation;
    reversePolarityCheck_->setChecked(app_.display.reversePolarity);
    centerLineCheck_->setChecked(w.centerLine);
    positiveFillCheck_->setChecked(w.positiveFill);
    negativeFillCheck_->setChecked(w.negativeFill);
    wiggleTraceCheck_->setChecked(w.wiggleTrace);
    positiveOverNegativeCheck_->setChecked(w.positiveOverNegative);
    variableAreaStyleRadios_[int(w.variableAreaStyle)]->setChecked(true);
    wiggleControlStyleRadios_[int(w.wiggleControlStyle)]->setChecked(true);
    blendingSpin_->setValue(w.variableDensityBlending);
    percentOverlapSpin_->setValue(w.percentOverlap);
    fillBaselineSpin_->setValue(w.fillBaseline);
    wiggleMinSpacingSpin_->setValue(w.wiggleMinimumSpacing);
    varMinSpacingSpin_->setValue(w.varMinimumSpacing);
    updateBlendingEnabled();
}

void DisplayParametersDialog::applySettings() {
    // Gain and seismicMode are the two real, already-wired settings here;
    // route Gain through the callback so the toolbar slider/spin box stay
    // in sync too, same single-source-of-truth pattern as any other gain
    // change.
    if (onGainChanged_) onGainChanged_(gainSpin_->value());
    app_.display.reversePolarity = reversePolarityCheck_->isChecked();
    WigglePresentationSettings& w = app_.wigglePresentation;
    w.centerLine = centerLineCheck_->isChecked();
    w.positiveFill = positiveFillCheck_->isChecked();
    w.negativeFill = negativeFillCheck_->isChecked();
    w.wiggleTrace = wiggleTraceCheck_->isChecked();
    w.positiveOverNegative = positiveOverNegativeCheck_->isChecked();
    for (int i = 0; i < 4; ++i) {
        if (variableAreaStyleRadios_[i]->isChecked()) w.variableAreaStyle = WiggleFillStyle(i);
        if (wiggleControlStyleRadios_[i]->isChecked()) w.wiggleControlStyle = WiggleFillStyle(i);
    }
    w.variableDensityBlending = blendingSpin_->value();
    w.percentOverlap = percentOverlapSpin_->value();
    w.fillBaseline = fillBaselineSpin_->value();
    w.wiggleMinimumSpacing = wiggleMinSpacingSpin_->value();
    w.varMinimumSpacing = varMinSpacingSpin_->value();
}

CalculatorDialog::CalculatorDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Calculator");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* aRow = new QHBoxLayout();
    aRow->addWidget(new QLabel("Dataset A:"));
    datasetACombo_ = new QComboBox();
    aRow->addWidget(datasetACombo_, 1);
    layout->addLayout(aRow);

    QHBoxLayout* bRow = new QHBoxLayout();
    bRow->addWidget(new QLabel("Dataset B:"));
    datasetBCombo_ = new QComboBox();
    bRow->addWidget(datasetBCombo_, 1);
    layout->addLayout(bRow);

    QHBoxLayout* opRow = new QHBoxLayout();
    opRow->addWidget(new QLabel("Operation:"));
    addRadio_ = new QRadioButton("Add");
    subtractRadio_ = new QRadioButton("Subtract");
    addRadio_->setChecked(true);
    QButtonGroup* opGroup = new QButtonGroup(this);
    opGroup->addButton(addRadio_);
    opGroup->addButton(subtractRadio_);
    opRow->addWidget(addRadio_);
    opRow->addWidget(subtractRadio_);
    opRow->addStretch();
    layout->addLayout(opRow);

    QHBoxLayout* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel("Output Name:"));
    outputNameEdit_ = new QLineEdit();
    nameRow->addWidget(outputNameEdit_, 1);
    layout->addLayout(nameRow);

    connect(datasetACombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { updateDefaultOutputName(); });
    connect(datasetBCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { updateDefaultOutputName(); });
    connect(addRadio_, &QRadioButton::toggled, this, [this](bool) { updateDefaultOutputName(); });
    connect(outputNameEdit_, &QLineEdit::textEdited, this, [this](const QString&) { outputNameEdited_ = true; });

    addButtonRow(this, layout,
                 {
                     {"OK",
                      [this]() {
                          emitApply();
                          accept();
                      }},
                     {"Apply", [this]() { emitApply(); }},
                     {"Cancel", [this]() { reject(); }},
                 });
}

void CalculatorDialog::refreshPanels(const QString& nameA, bool loadedA, const QString& nameB, bool loadedB) {
    datasetACombo_->clear();
    datasetBCombo_->clear();
    if (loadedA) {
        datasetACombo_->addItem(nameA, 0);
        datasetBCombo_->addItem(nameA, 0);
    }
    if (loadedB) {
        datasetACombo_->addItem(nameB, 1);
        datasetBCombo_->addItem(nameB, 1);
    }
    // Defaults to A vs. B when both exist, rather than leaving both combos
    // pointed at the same one -- combining a dataset with itself is legal
    // (Add doubles it, Subtract zeroes it) but not the useful default case.
    if (loadedA && loadedB) datasetBCombo_->setCurrentIndex(1);
    outputNameEdited_ = false;
    updateDefaultOutputName();
}

void CalculatorDialog::setApplyCallback(
    std::function<void(int, int, bool, QString)> callback) {
    onApply_ = std::move(callback);
}

void CalculatorDialog::updateDefaultOutputName() {
    if (outputNameEdited_) return;
    QString a = datasetACombo_->currentText();
    QString b = datasetBCombo_->currentText();
    if (a.isEmpty() || b.isEmpty()) return;
    QString op = addRadio_->isChecked() ? "Plus" : "Minus";
    outputNameEdit_->setText(QString("%1_%2_%3").arg(a, op, b));
}

void CalculatorDialog::emitApply() {
    if (!onApply_ || datasetACombo_->count() == 0 || datasetBCombo_->count() == 0) return;
    int a = datasetACombo_->currentData().toInt();
    int b = datasetBCombo_->currentData().toInt();
    onApply_(a, b, subtractRadio_->isChecked(), outputNameEdit_->text());
}

BandpassDialog::BandpassDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Bandpass Filter");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* cornerRow = new QHBoxLayout();
    auto corner = [&](const QString& label, QDoubleSpinBox*& spin, double defaultValue) {
        QVBoxLayout* col = new QVBoxLayout();
        col->addWidget(new QLabel(label));
        spin = new QDoubleSpinBox();
        spin->setRange(0.0, 100000.0);
        spin->setSuffix(" Hz");
        spin->setValue(defaultValue);
        col->addWidget(spin);
        cornerRow->addLayout(col);
    };
    // Defaults per the original ask: 5-10-50-60 Hz.
    corner("Low Cut", lowCutSpin_, 5.0);
    corner("Low Pass", lowPassSpin_, 10.0);
    corner("High Pass", highPassSpin_, 50.0);
    corner("High Cut", highCutSpin_, 60.0);
    layout->addLayout(cornerRow);

    QHBoxLayout* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel("Output Name:"));
    outputNameEdit_ = new QLineEdit();
    nameRow->addWidget(outputNameEdit_, 1);
    layout->addLayout(nameRow);

    for (QDoubleSpinBox* spin : {lowCutSpin_, lowPassSpin_, highPassSpin_, highCutSpin_}) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
            enforceOrdering();
            updateDefaultOutputName();
        });
    }
    connect(outputNameEdit_, &QLineEdit::textEdited, this, [this](const QString&) { outputNameEdited_ = true; });

    addButtonRow(this, layout,
                 {
                     {"OK",
                      [this]() {
                          emitApply();
                          accept();
                      }},
                     {"Apply", [this]() { emitApply(); }},
                     {"Cancel", [this]() { reject(); }},
                 });
}

void BandpassDialog::refreshSource(const QString& sourceName, bool loaded) {
    sourceName_ = loaded ? sourceName : QString();
    outputNameEdited_ = false;
    updateDefaultOutputName();
}

void BandpassDialog::setApplyCallback(std::function<void(segy::BandpassParams, QString)> callback) {
    onApply_ = std::move(callback);
}

void BandpassDialog::enforceOrdering() {
    double a = lowCutSpin_->value();
    double b = lowPassSpin_->value();
    double c = highPassSpin_->value();
    double d = highCutSpin_->value();
    // Push forward, then back, so a change to any one corner drags only the
    // minimum necessary neighbors along with it instead of rejecting the
    // edit outright.
    b = std::max(b, a);
    c = std::max(c, b);
    d = std::max(d, c);
    c = std::min(c, d);
    b = std::min(b, c);
    a = std::min(a, b);
    auto setQuiet = [](QDoubleSpinBox* spin, double v) {
        if (spin->value() == v) return;
        spin->blockSignals(true);
        spin->setValue(v);
        spin->blockSignals(false);
    };
    setQuiet(lowCutSpin_, a);
    setQuiet(lowPassSpin_, b);
    setQuiet(highPassSpin_, c);
    setQuiet(highCutSpin_, d);
}

void BandpassDialog::updateDefaultOutputName() {
    if (outputNameEdited_ || sourceName_.isEmpty()) return;
    outputNameEdit_->setText(QString("%1_Bandpass_%2_%3_%4_%5")
                                  .arg(sourceName_)
                                  .arg(lowCutSpin_->value(), 0, 'g', 4)
                                  .arg(lowPassSpin_->value(), 0, 'g', 4)
                                  .arg(highPassSpin_->value(), 0, 'g', 4)
                                  .arg(highCutSpin_->value(), 0, 'g', 4));
}

void BandpassDialog::emitApply() {
    if (!onApply_) return;
    segy::BandpassParams params;
    params.lowCut = lowCutSpin_->value();
    params.lowPass = lowPassSpin_->value();
    params.highPass = highPassSpin_->value();
    params.highCut = highCutSpin_->value();
    onApply_(params, outputNameEdit_->text());
}

SaveSegyDialog::SaveSegyDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Save SEG-Y");
    setModal(false);
    resize(560, 640);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* fileRow = new QHBoxLayout();
    fileRow->addWidget(new QLabel("Save As:"));
    fileNameEdit_ = new QLineEdit();
    fileRow->addWidget(fileNameEdit_, 1);
    QPushButton* browseButton = new QPushButton("Browse...");
    connect(browseButton, &QPushButton::clicked, this, [this]() { browseForFile(); });
    fileRow->addWidget(browseButton);
    layout->addLayout(fileRow);

    datasetSummaryLabel_ = new QLabel();
    datasetSummaryLabel_->setStyleSheet("color: #9a9ca3;");
    layout->addWidget(datasetSummaryLabel_);

    layout->addWidget(new QLabel("EBCDIC Text Header Preview:"));
    ebcdicPreview_ = new QPlainTextEdit();
    ebcdicPreview_->setReadOnly(true);
    ebcdicPreview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(9);
    ebcdicPreview_->setFont(mono);
    ebcdicPreview_->setStyleSheet("background: #f4f2ec; color: #1b1b1b;");
    layout->addWidget(ebcdicPreview_, 1);

    addButtonRow(this, layout,
                 {
                     {"Save",
                      [this]() {
                          if (fileNameEdit_->text().trimmed().isEmpty()) return;
                          if (onSave_) onSave_(fileNameEdit_->text());
                          accept();
                      }},
                     {"Cancel", [this]() { reject(); }},
                 });
}

void SaveSegyDialog::setPreview(const QString& defaultFileName, const QString& datasetSummary,
                                 const QString& ebcdicPreview) {
    fileNameEdit_->setText(defaultFileName);
    datasetSummaryLabel_->setText(datasetSummary);
    ebcdicPreview_->setPlainText(ebcdicPreview);
}

void SaveSegyDialog::setSaveCallback(std::function<void(QString)> callback) { onSave_ = std::move(callback); }

void SaveSegyDialog::browseForFile() {
    QString path = QFileDialog::getSaveFileName(this, "Save SEG-Y", fileNameEdit_->text(), "SEG-Y files (*.sgy)");
    if (!path.isEmpty()) fileNameEdit_->setText(path);
}

OctaveBandDialog::OctaveBandDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Octave Band Display");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* toolbarRow = new QHBoxLayout();
    toolbarRow->addWidget(new QLabel("Source:"));
    sourceCombo_ = new QComboBox();
    toolbarRow->addWidget(sourceCombo_);
    toolbarRow->addWidget(new QLabel("Min Freq:"));
    minFreqCombo_ = new QComboBox();
    toolbarRow->addWidget(minFreqCombo_);
    toolbarRow->addWidget(new QLabel("Max Freq:"));
    maxFreqCombo_ = new QComboBox();
    toolbarRow->addWidget(maxFreqCombo_);
    // Only powers of 2, per the original ask ("increments of 2^N") -- a
    // QComboBox of exact values enforces that directly, where a QSpinBox's
    // single linear step could not.
    for (int hz : {1, 2, 4, 8, 16, 32, 64, 128, 256, 512}) {
        minFreqCombo_->addItem(QString::number(hz) + " Hz", hz);
        maxFreqCombo_->addItem(QString::number(hz) + " Hz", hz);
    }
    minFreqCombo_->setCurrentText("2 Hz");
    maxFreqCombo_->setCurrentText("128 Hz");
    QPushButton* applyButton = new QPushButton("Apply");
    connect(applyButton, &QPushButton::clicked, this, [this]() { emitApply(); });
    toolbarRow->addWidget(applyButton);
    toolbarRow->addStretch();
    layout->addLayout(toolbarRow);

    QScrollArea* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    bandsRow_ = new QWidget();
    QHBoxLayout* bandsLayout = new QHBoxLayout(bandsRow_);
    bandsLayout->setSpacing(0);
    bandsLayout->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(bandsRow_);
    layout->addWidget(scroll, 1);

    connect(sourceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { emitApply(); });

    resize(960, 640);
}

void OctaveBandDialog::refreshPanels(const QString& nameA, bool loadedA, const QString& nameB, bool loadedB) {
    int previous = sourceCombo_->currentData().isValid() ? sourceCombo_->currentData().toInt() : 0;
    sourceCombo_->blockSignals(true);
    sourceCombo_->clear();
    if (loadedA) sourceCombo_->addItem("Left Panel: " + nameA, 0);
    if (loadedB) sourceCombo_->addItem("Right Panel: " + nameB, 1);
    int restoreIndex = sourceCombo_->findData(previous);
    sourceCombo_->setCurrentIndex(restoreIndex >= 0 ? restoreIndex : 0);
    sourceCombo_->blockSignals(false);
}

void OctaveBandDialog::setSourcePanel(int panelIndex) {
    int index = sourceCombo_->findData(panelIndex);
    if (index < 0) return;
    sourceCombo_->blockSignals(true);
    sourceCombo_->setCurrentIndex(index);
    sourceCombo_->blockSignals(false);
}

void OctaveBandDialog::setResult(const OctaveBandResult& result) {
    QLayout* oldLayout = bandsRow_->layout();
    QHBoxLayout* bandsLayout = new QHBoxLayout();
    bandsLayout->setSpacing(0);
    bandsLayout->setContentsMargins(0, 0, 0, 0);
    for (const OctaveBandResult::Band& band : result.bands) {
        QWidget* col = new QWidget();
        col->setStyleSheet("border-right: 1px solid #3c3f41;");
        QVBoxLayout* colLayout = new QVBoxLayout(col);
        colLayout->setContentsMargins(4, 4, 4, 4);
        QLabel* labelWidget = new QLabel(band.label);
        labelWidget->setAlignment(Qt::AlignCenter);
        labelWidget->setStyleSheet("font-size: 11px;");
        colLayout->addWidget(labelWidget);
        QLabel* imageWidget = new QLabel();
        imageWidget->setPixmap(QPixmap::fromImage(band.image));
        colLayout->addWidget(imageWidget);
        bandsLayout->addWidget(col);
    }
    delete oldLayout;
    bandsRow_->setLayout(bandsLayout);
}

void OctaveBandDialog::setApplyCallback(std::function<void(int, int, int)> callback) { onApply_ = std::move(callback); }

void OctaveBandDialog::emitApply() {
    if (!onApply_ || sourceCombo_->count() == 0) return;
    onApply_(sourceCombo_->currentData().toInt(), minFreqCombo_->currentData().toInt(),
             maxFreqCombo_->currentData().toInt());
}

namespace {
// Shared File/Edit/View/Utilities menu bar for the Histogram/Spectrum
// result windows -- both need the identical structure (File > Close; Edit
// intentionally empty, matching this app's own convention for a menu with
// nothing to do yet; View > Grid Lines; Utilities > Parameters...), so
// it's built once here instead of duplicated per window.
QMenuBar* buildResultWindowMenuBar(QWidget* window, std::function<void(bool)> onGridLinesToggled,
                                    std::function<void()> onParameters) {
    QMenuBar* menuBar = new QMenuBar(window);
    QMenu* fileMenu = menuBar->addMenu("&File");
    QAction* closeAction = fileMenu->addAction("&Close");
    QObject::connect(closeAction, &QAction::triggered, window, &QWidget::close);
    menuBar->addMenu("&Edit");
    QMenu* viewMenu = menuBar->addMenu("&View");
    QAction* gridAction = viewMenu->addAction("&Grid Lines");
    gridAction->setCheckable(true);
    gridAction->setChecked(true);
    QObject::connect(gridAction, &QAction::toggled, window, std::move(onGridLinesToggled));
    QMenu* utilitiesMenu = menuBar->addMenu("&Utilities");
    QAction* paramsAction = utilitiesMenu->addAction("&Parameters...");
    QObject::connect(paramsAction, &QAction::triggered, window, std::move(onParameters));
    return menuBar;
}

// Draws one numeric axis's gridlines (if `gridLinesVisible`), a border
// rect, and tick marks + labels -- shared shape between Histogram's shared
// X (Amplitude) axis, its two independent Y axes, and Spectrum's X/Y axes.
// `ticks` are in the axis's own pixel space (0 at chart.left()/top()); this
// only handles the *vertical* (X-axis) case, drawn once per chart rect
// since Histogram has two charts sharing one X axis.
void drawVerticalGridlines(QPainter& painter, const QRect& chartRect, const std::vector<segy::AxisTick>& ticks,
                            const QColor& gridColor, bool gridLinesVisible) {
    if (!gridLinesVisible) return;
    painter.setPen(gridColor);
    for (const segy::AxisTick& tk : ticks) {
        int x = chartRect.left() + tk.pixelPos;
        painter.drawLine(x, chartRect.top(), x, chartRect.bottom());
    }
}

// Y-axis gridlines + labels for one chart, ticks measured bottom-up
// (tk.pixelPos 0 = chartRect.bottom()).
void drawHorizontalGridlinesAndLabels(QPainter& painter, const QRect& chartRect,
                                       const std::vector<segy::AxisTick>& ticks, const QColor& gridColor,
                                       const QColor& textColor, int labelColumnWidth, bool gridLinesVisible) {
    for (const segy::AxisTick& tk : ticks) {
        int y = chartRect.bottom() - tk.pixelPos;
        if (gridLinesVisible) {
            painter.setPen(gridColor);
            painter.drawLine(chartRect.left(), y, chartRect.right(), y);
        }
        painter.setPen(textColor);
        painter.drawText(QRect(0, y - 8, labelColumnWidth, 16), Qt::AlignRight | Qt::AlignVCenter,
                          QString::fromStdString(tk.label));
    }
}

// X-axis tick marks (short perpendicular lines at the chart's bottom edge)
// plus their numeric labels, shared by the bottom-most chart on both
// Histogram and Spectrum.
void drawXAxisTicksAndLabels(QPainter& painter, const QRect& chartRect, const std::vector<segy::AxisTick>& ticks,
                              const QColor& textColor,
                              const std::function<int(const std::string&)>& measureWidth) {
    painter.setPen(textColor);
    for (const segy::AxisTick& tk : ticks) {
        int x = chartRect.left() + tk.pixelPos;
        painter.drawLine(x, chartRect.bottom(), x, chartRect.bottom() + 4);
        int labelW = measureWidth(tk.label) + 12;
        painter.drawText(QRect(x - labelW / 2, chartRect.bottom() + 6, labelW, 18), Qt::AlignHCenter,
                          QString::fromStdString(tk.label));
    }
}
} // namespace

HistogramChartArea::HistogramChartArea(QWidget* parent) : QWidget(parent) {}

void HistogramChartArea::setResult(const segy::HistogramResult& result, QColor sourceColor) {
    result_ = result;
    sourceColor_ = sourceColor;
    update();
}

void HistogramChartArea::setGridLinesVisible(bool visible) {
    gridLinesVisible_ = visible;
    update();
}

void HistogramChartArea::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kCanvasSurroundColor);
    QColor gridColor(0x3c, 0x3f, 0x41);

    if (result_.percentOfTotal.empty()) {
        painter.setPen(kSecondaryTextColor);
        painter.drawText(rect(), Qt::AlignCenter,
                          "No data yet -- open a file, then click Apply or OK in Histogram Parameters.");
        return;
    }

    QFont font = axisFont();
    painter.setFont(font);
    QFontMetrics fm(font);
    auto measureWidth = [&fm](const std::string& s) {
        return s.empty() ? 0 : fm.horizontalAdvance(QString::fromStdString(s));
    };

    int w = width(), h = height();
    int marginLeft = 60, marginRight = 20, marginTop = 30, marginBottom = 54, gap = 36;
    int chartW = w - marginLeft - marginRight;
    int chartH = (h - marginTop - marginBottom - gap) / 2;
    QRect topChart(marginLeft, marginTop, chartW, chartH);
    QRect bottomChart(marginLeft, marginTop + chartH + gap, chartW, chartH);

    int numBins = int(result_.percentOfTotal.size());
    float maxPercent = 0.0f;
    for (float p : result_.percentOfTotal) maxPercent = std::max(maxPercent, p);
    maxPercent = std::max(1.0f, maxPercent * 1.15f); // headroom so the tallest bar doesn't touch the top

    constexpr int kMaxLines = 11;
    // Shared X (Amplitude) axis ticks -- same interval, used for both
    // charts' vertical gridlines and the one labeled row at the bottom.
    std::vector<segy::AxisTick> xTicks =
        segy::computeNiceAxisTicks(result_.rangeMin, result_.rangeMax, chartW, kMaxLines, measureWidth);

    // --- Percent of Total: bars ---
    painter.setPen(kSecondaryTextColor);
    painter.drawText(QRect(0, topChart.top() - 22, w, 20), Qt::AlignHCenter, "Percent of Total");
    std::vector<segy::AxisTick> yTicksTop =
        segy::computeNiceAxisTicks(0.0, double(maxPercent), topChart.height(), kMaxLines, measureWidth);
    drawHorizontalGridlinesAndLabels(painter, topChart, yTicksTop, gridColor, kSecondaryTextColor, marginLeft - 6,
                                      gridLinesVisible_);
    drawVerticalGridlines(painter, topChart, xTicks, gridColor, gridLinesVisible_);
    painter.setPen(gridColor);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(topChart);

    painter.setPen(Qt::NoPen);
    painter.setBrush(sourceColor_);
    float barSlot = float(chartW) / float(numBins);
    for (int i = 0; i < numBins; ++i) {
        float barH = result_.percentOfTotal[size_t(i)] / maxPercent * chartH;
        QRectF barRect(topChart.left() + i * barSlot, topChart.bottom() - barH, std::max(1.0f, barSlot - 1.0f), barH);
        painter.drawRect(barRect);
    }

    // --- Cumulative Percent: same bins as above (not a smoothed curve),
    // pure black background per the original request ---
    painter.setPen(kSecondaryTextColor);
    painter.drawText(QRect(0, bottomChart.top() - 22, w, 20), Qt::AlignHCenter, "Cumulative Percent");
    painter.fillRect(bottomChart, Qt::black);
    std::vector<segy::AxisTick> yTicksBottom =
        segy::computeNiceAxisTicks(0.0, 100.0, bottomChart.height(), kMaxLines, measureWidth);
    drawHorizontalGridlinesAndLabels(painter, bottomChart, yTicksBottom, gridColor, kSecondaryTextColor,
                                      marginLeft - 6, gridLinesVisible_);
    drawVerticalGridlines(painter, bottomChart, xTicks, gridColor, gridLinesVisible_);
    painter.setPen(gridColor);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bottomChart);

    painter.setPen(Qt::NoPen);
    painter.setBrush(sourceColor_);
    for (int i = 0; i < numBins; ++i) {
        float barH = result_.cumulativePercent[size_t(i)] / 100.0f * chartH;
        QRectF barRect(bottomChart.left() + i * barSlot, bottomChart.bottom() - barH, std::max(1.0f, barSlot - 1.0f),
                        barH);
        painter.drawRect(barRect);
    }

    // Shared X axis: tick marks + labels at the very bottom, axis title below that.
    drawXAxisTicksAndLabels(painter, bottomChart, xTicks, kSecondaryTextColor, measureWidth);
    painter.setPen(kSecondaryTextColor);
    painter.drawText(QRect(marginLeft, h - 20, chartW, 20), Qt::AlignHCenter, "Amplitude");
}

HistogramPlotWidget::HistogramPlotWidget(QWidget* parent) : QWidget(parent) {
    setWindowTitle("Histogram");
    resize(700, 540);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    chart_ = new HistogramChartArea(this);
    layout->addWidget(buildResultWindowMenuBar(
        this, [this](bool checked) { chart_->setGridLinesVisible(checked); },
        [this]() {
            if (onParameters_) onParameters_();
        }));
    layout->addWidget(chart_, 1);
}

void HistogramPlotWidget::setResult(const segy::HistogramResult& result, QColor sourceColor) {
    chart_->setResult(result, sourceColor);
}

void HistogramPlotWidget::setParametersCallback(std::function<void()> callback) { onParameters_ = std::move(callback); }

SpectrumChartArea::SpectrumChartArea(QWidget* parent) : QWidget(parent) {}

void SpectrumChartArea::setResult(const segy::SpectrumResult& result, QColor sourceColor) {
    result_ = result;
    sourceColor_ = sourceColor;
    update();
}

void SpectrumChartArea::setGridLinesVisible(bool visible) {
    gridLinesVisible_ = visible;
    update();
}

void SpectrumChartArea::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kCanvasSurroundColor);
    QColor gridColor(0x3c, 0x3f, 0x41);

    if (result_.amplitudeDb.empty() || result_.frequencyHz.back() <= 0.0f) {
        painter.setPen(kSecondaryTextColor);
        painter.drawText(rect(), Qt::AlignCenter,
                          "No data yet -- open a file, then click Apply or OK in Spectrum Parameters.");
        return;
    }

    QFont font = axisFont();
    painter.setFont(font);
    QFontMetrics fm(font);
    auto measureWidth = [&fm](const std::string& s) {
        return s.empty() ? 0 : fm.horizontalAdvance(QString::fromStdString(s));
    };

    int w = width(), h = height();
    int marginLeft = 60, marginRight = 20, marginTop = 30, marginBottom = 54;
    QRect chart(marginLeft, marginTop, w - marginLeft - marginRight, h - marginTop - marginBottom);

    float minDb = 0.0f, maxDb = 0.0f;
    for (float db : result_.amplitudeDb) {
        minDb = std::min(minDb, db);
        maxDb = std::max(maxDb, db);
    }
    minDb = std::floor(minDb / 10.0f) * 10.0f - 10.0f;
    maxDb = std::max(maxDb, 0.0f) + 5.0f;
    float maxFreq = result_.frequencyHz.back();

    constexpr int kMaxLines = 11;
    std::vector<segy::AxisTick> yTicks =
        segy::computeNiceAxisTicks(double(minDb), double(maxDb), chart.height(), kMaxLines, measureWidth);
    std::vector<segy::AxisTick> xTicks =
        segy::computeNiceAxisTicks(0.0, double(maxFreq), chart.width(), kMaxLines, measureWidth);

    painter.setPen(kSecondaryTextColor);
    painter.drawText(QRect(0, chart.top() - 22, w, 20), Qt::AlignHCenter, "Amplitude (dB) vs. Frequency (Hz)");
    drawHorizontalGridlinesAndLabels(painter, chart, yTicks, gridColor, kSecondaryTextColor, marginLeft - 6,
                                      gridLinesVisible_);
    drawVerticalGridlines(painter, chart, xTicks, gridColor, gridLinesVisible_);
    painter.setPen(gridColor);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(chart);

    QPolygonF curve;
    for (size_t i = 0; i < result_.amplitudeDb.size(); ++i) {
        float x = chart.left() + result_.frequencyHz[i] / maxFreq * chart.width();
        float y = chart.bottom() - (result_.amplitudeDb[i] - minDb) / (maxDb - minDb) * chart.height();
        curve << QPointF(x, y);
    }
    QPen linePen(sourceColor_);
    linePen.setWidthF(1.5);
    painter.setPen(linePen);
    painter.drawPolyline(curve);

    drawXAxisTicksAndLabels(painter, chart, xTicks, kSecondaryTextColor, measureWidth);
    painter.setPen(kSecondaryTextColor);
    painter.drawText(QRect(marginLeft, h - 20, chart.width(), 20), Qt::AlignHCenter, "Frequency (Hz)");
}

SpectrumPlotWidget::SpectrumPlotWidget(QWidget* parent) : QWidget(parent) {
    setWindowTitle("Spectrum");
    resize(700, 480);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    chart_ = new SpectrumChartArea(this);
    layout->addWidget(buildResultWindowMenuBar(
        this, [this](bool checked) { chart_->setGridLinesVisible(checked); },
        [this]() {
            if (onParameters_) onParameters_();
        }));
    layout->addWidget(chart_, 1);
}

void SpectrumPlotWidget::setResult(const segy::SpectrumResult& result, QColor sourceColor) {
    chart_->setResult(result, sourceColor);
}

void SpectrumPlotWidget::setParametersCallback(std::function<void()> callback) { onParameters_ = std::move(callback); }

namespace {
// Splits "<dir>/<pattern>[ <pattern>...]" into the directory and the list
// of glob patterns after the last '/' -- QFileInfo::fileName() only ever
// returns a single path component, but the filter box's default (and
// anything the user types to widen it) is deliberately several
// space-separated patterns at once (e.g. "*.sgy *.segy *.SGY *.SEGY"), the
// same multi-extension convention a native file-open dialog's own filter
// box uses.
QString filterDirectory(const QString& filterText) {
    int lastSlash = filterText.lastIndexOf('/');
    return lastSlash >= 0 ? filterText.left(lastSlash) : QString();
}
QStringList filterPatterns(const QString& filterText) {
    int lastSlash = filterText.lastIndexOf('/');
    QString tail = lastSlash >= 0 ? filterText.mid(lastSlash + 1) : filterText;
    QStringList patterns = tail.split(' ', Qt::SkipEmptyParts);
    if (patterns.isEmpty()) patterns << "*";
    return patterns;
}
} // namespace

DatasetPickerDialog::DatasetPickerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Select Dataset");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);
    list_ = new QListWidget();
    layout->addWidget(list_, 1);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int) { updateButtonsEnabled(); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (okButton_->isEnabled()) okButton_->click();
    });

    QHBoxLayout* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    okButton_ = new QPushButton("OK");
    QPushButton* deleteButton = new QPushButton("Delete");
    QPushButton* cancelButton = new QPushButton("Cancel");
    deleteButton_ = deleteButton;
    buttonRow->addWidget(okButton_);
    buttonRow->addWidget(deleteButton);
    buttonRow->addWidget(cancelButton);
    layout->addLayout(buttonRow);

    connect(okButton_, &QPushButton::clicked, this, [this]() {
        int row = list_->currentRow();
        if (row < 0 || row >= int(datasets_.size())) return;
        if (onPicked_) onPicked_(datasets_[size_t(row)] ? datasets_[size_t(row)] : std::make_shared<Dataset>());
        accept();
    });
    connect(deleteButton, &QPushButton::clicked, this, [this]() {
        int row = list_->currentRow();
        if (row < 0 || row >= int(datasets_.size())) return;
        std::shared_ptr<Dataset> target = datasets_[size_t(row)];
        if (!target || (isInUse_ && isInUse_(target))) return; // belt-and-suspenders; button should already be disabled
        if (onDeleted_) onDeleted_(target);
        datasets_.erase(datasets_.begin() + row);
        delete list_->takeItem(row);
        updateButtonsEnabled();
    });
    connect(cancelButton, &QPushButton::clicked, this, [this]() { reject(); });

    resize(360, 320);
}

void DatasetPickerDialog::setDatasets(const std::vector<std::shared_ptr<Dataset>>& datasets,
                                       const std::shared_ptr<Dataset>& current, bool allowNone,
                                       std::function<bool(const std::shared_ptr<Dataset>&)> isInUse) {
    isInUse_ = std::move(isInUse);
    datasets_.clear();
    list_->clear();

    int currentRow = -1;
    if (allowNone) {
        datasets_.push_back(nullptr);
        list_->addItem("(None)");
        if (!current || current->name.empty()) currentRow = 0;
    }
    for (const std::shared_ptr<Dataset>& ds : datasets) {
        datasets_.push_back(ds);
        list_->addItem(QString::fromStdString(ds->name));
        if (ds == current) currentRow = int(datasets_.size()) - 1;
    }
    list_->setCurrentRow(currentRow);
    updateButtonsEnabled();
}

void DatasetPickerDialog::setCallbacks(std::function<void(std::shared_ptr<Dataset>)> onPicked,
                                        std::function<void(std::shared_ptr<Dataset>)> onDeleted) {
    onPicked_ = std::move(onPicked);
    onDeleted_ = std::move(onDeleted);
}

void DatasetPickerDialog::updateButtonsEnabled() {
    int row = list_->currentRow();
    bool hasSelection = row >= 0 && row < int(datasets_.size());
    okButton_->setEnabled(hasSelection);
    bool isNoneRow = hasSelection && !datasets_[size_t(row)];
    bool canDelete = hasSelection && !isNoneRow && !(isInUse_ && isInUse_(datasets_[size_t(row)]));
    deleteButton_->setEnabled(canDelete);
}

DatasetSlotWidget::DatasetSlotWidget(const QString& label, QWidget* parent) : QWidget(parent), label_(label) {
    setFixedSize(160, 34);

    pickButton_ = new QPushButton(this);
    pickButton_->setIcon(iconList());
    pickButton_->setIconSize(QSize(12, 12));
    pickButton_->setFixedSize(18, 18);
    pickButton_->setFlat(true);
    pickButton_->setCursor(Qt::PointingHandCursor);
    pickButton_->setToolTip("Select dataset...");
    pickButton_->setStyleSheet(
        "QPushButton { background: transparent; border: none; } "
        "QPushButton:hover { background: #3c3f41; border-radius: 2px; }");
    pickButton_->move(width() - 22, 14);
    connect(pickButton_, &QPushButton::clicked, this, [this]() { if (onPick_) onPick_(); });
}

void DatasetSlotWidget::setLabel(const QString& label) {
    label_ = label;
    update();
}

void DatasetSlotWidget::setDatasetName(const QString& name) {
    datasetName_ = name;
    update();
}

void DatasetSlotWidget::setPickCallback(std::function<void()> callback) { onPick_ = std::move(callback); }

void DatasetSlotWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QRect r = rect().adjusted(0, 0, -1, -1);
    painter.fillRect(r, QColor(0x2b, 0x2d, 0x30));
    painter.setPen(QColor(0x3c, 0x3f, 0x41));
    painter.drawRect(r);

    QFont captionFont = font();
    captionFont.setPixelSize(9);
    painter.setFont(captionFont);
    painter.setPen(QColor(0x9a, 0x9c, 0xa3));
    painter.drawText(QRect(4, 1, width() - 8, 12), Qt::AlignLeft | Qt::AlignVCenter, label_);

    QFont nameFont = font();
    nameFont.setPixelSize(11);
    painter.setFont(nameFont);
    painter.setPen(QColor(0xd4, 0xd4, 0xd8));
    QString shown = datasetName_.isEmpty() ? QStringLiteral("—") : datasetName_;
    QRect nameRect(4, 15, width() - 8 - 20, 16);
    painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter.fontMetrics().elidedText(shown, Qt::ElideRight, nameRect.width()));
}

SegyOpenDialog::SegyOpenDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Open SEG-Y File");
    setModal(true);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* loadOptionsLabel = new QLabel("Load Options:");
    loadOptionsLabel->setStyleSheet("font-style: italic; color: #9a9ca3;");
    layout->addWidget(loadOptionsLabel);

    QHBoxLayout* rangeRow = new QHBoxLayout();
    rangeCombo_ = new QComboBox();
    rangeCombo_->addItem("traceno");
    rangeRow->addWidget(new QLabel("Range:"));
    rangeRow->addWidget(rangeCombo_);
    rangeRow->addWidget(new QLabel("Min:"));
    minSpin_ = new QSpinBox();
    minSpin_->setRange(1, 1000000);
    minSpin_->setValue(1);
    rangeRow->addWidget(minSpin_);
    rangeRow->addWidget(new QLabel("Max (0 = all):"));
    maxSpin_ = new QSpinBox();
    maxSpin_->setRange(0, 1000000);
    maxSpin_->setValue(0);
    rangeRow->addWidget(maxSpin_);
    rangeRow->addStretch();
    layout->addLayout(rangeRow);

    // Present but not wired to real behavior -- see the class comment.
    auto addPlaceholderCheck = [&](const QString& text) {
        QCheckBox* check = new QCheckBox(text);
        check->setToolTip("Not implemented yet -- has no effect on the load.");
        layout->addWidget(check);
        return check;
    };
    useDefaultsCheck_ = addPlaceholderCheck("Use Default Select/Sort Values");
    clipAbsCheck_ = addPlaceholderCheck("Clip at abs max/min");

    keepPrevClipCheck_ = new QCheckBox("Use clipping from previously loaded window");
    keepPrevClipCheck_->setToolTip(
        "Unchecked (default): gain resets to 0 dB so this file gets a fresh "
        "auto-scaled clip. Checked: keeps whatever gain the previously "
        "loaded file was left at.");
    layout->addWidget(keepPrevClipCheck_);

    QLabel* browseLabel = new QLabel("Open Seismic File:");
    browseLabel->setStyleSheet("font-style: italic; color: #9a9ca3; margin-top: 8px;");
    layout->addWidget(browseLabel);

    layout->addWidget(new QLabel("Filter"));
    // Default to SEG-Y's usual extensions specifically (both cases, since
    // ext3/ext4 filesystems are case-sensitive) rather than "*" -- the user
    // can still widen or replace this by editing the box directly.
    filterEdit_ = new QLineEdit(QDir::homePath() + "/*.sgy *.segy *.SGY *.SEGY");
    connect(filterEdit_, &QLineEdit::returnPressed, this, &SegyOpenDialog::refreshLists);
    layout->addWidget(filterEdit_);

    QHBoxLayout* listsRow = new QHBoxLayout();
    QVBoxLayout* dirColumn = new QVBoxLayout();
    dirColumn->addWidget(new QLabel("Directories"));
    dirList_ = new QListWidget();
    dirColumn->addWidget(dirList_);
    listsRow->addLayout(dirColumn);

    QVBoxLayout* fileColumn = new QVBoxLayout();
    fileColumn->addWidget(new QLabel("Files"));
    fileList_ = new QListWidget();
    fileColumn->addWidget(fileList_);
    listsRow->addLayout(fileColumn);
    layout->addLayout(listsRow);

    connect(dirList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) { navigateTo(item->text()); });
    connect(fileList_, &QListWidget::currentTextChanged, this, [this](const QString& name) {
        if (name.isEmpty()) return;
        selectionEdit_->setText(QDir(filterDirectory(filterEdit_->text())).absoluteFilePath(name));
    });
    connect(fileList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { tryAccept(); });

    layout->addWidget(new QLabel("Selection"));
    selectionEdit_ = new QLineEdit();
    connect(selectionEdit_, &QLineEdit::returnPressed, this, &SegyOpenDialog::tryAccept);
    layout->addWidget(selectionEdit_);

    addButtonRow(this, layout,
                 {
                     {"OK", [this]() { tryAccept(); }},
                     {"Filter", [this]() { refreshLists(); }},
                     {"Help",
                      [this]() {
                          QMessageBox::information(
                              this, "Open SEG-Y File",
                              "Double-click a directory to browse into it, or edit Filter directly "
                              "and click Filter to jump to a path/pattern. Click a file to select "
                              "it, or type/paste a full path into Selection. Then click OK.");
                      }},
                     {"Cancel", [this]() { reject(); }},
                 });

    refreshLists();
    resize(420, 560);
}

std::filesystem::path SegyOpenDialog::selectedPath() const {
    return std::filesystem::path(selectionEdit_->text().toStdString());
}

bool SegyOpenDialog::keepPreviousClip() const { return keepPrevClipCheck_->isChecked(); }

void SegyOpenDialog::refreshLists() {
    QString filterText = filterEdit_->text();
    QDir dir(filterDirectory(filterText));
    if (!dir.exists()) return;

    dirList_->clear();
    dirList_->addItem(".");
    dirList_->addItem("..");
    for (const QString& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        dirList_->addItem(name);
    }

    fileList_->clear();
    for (const QString& name : dir.entryList(filterPatterns(filterText), QDir::Files, QDir::Name)) {
        fileList_->addItem(name);
    }
}

void SegyOpenDialog::navigateTo(const QString& dirName) {
    QString filterText = filterEdit_->text();
    QDir dir(filterDirectory(filterText));
    if (!dir.cd(dirName)) return;
    // Carries the current pattern(s) into the new directory, same as a
    // native file dialog -- changing folder shouldn't reset the filter.
    filterEdit_->setText(dir.absolutePath() + "/" + filterPatterns(filterText).join(' '));
    refreshLists();
}

void SegyOpenDialog::tryAccept() {
    QFileInfo info(selectionEdit_->text());
    if (info.exists() && info.isFile()) {
        accept();
    } else {
        QMessageBox::warning(this, "Open SEG-Y File", "Select a valid file first.");
    }
}

PreferencesDialog::PreferencesDialog(const Preferences& initial, QWidget* parent) : QDialog(parent) {
    setWindowTitle("Preferences");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* toolbarRow = new QHBoxLayout();
    toolbarRow->addWidget(new QLabel("Toolbar Position:"));
    toolbarPositionCombo_ = new QComboBox();
    toolbarPositionCombo_->addItem("Left", int(ToolbarPosition::Left));
    toolbarPositionCombo_->addItem("Top", int(ToolbarPosition::Top));
    toolbarPositionCombo_->addItem("Bottom", int(ToolbarPosition::Bottom));
    toolbarPositionCombo_->addItem("Right", int(ToolbarPosition::Right));
    toolbarPositionCombo_->setCurrentIndex(int(initial.toolbarPosition));
    toolbarRow->addWidget(toolbarPositionCombo_);
    toolbarRow->addStretch();
    layout->addLayout(toolbarRow);

    QGroupBox* drawerGroup = new QGroupBox("Drawer Position");
    QHBoxLayout* drawerLayout = new QHBoxLayout(drawerGroup);
    drawerLeftRadio_ = new QRadioButton("Left");
    drawerRightRadio_ = new QRadioButton("Right");
    (initial.drawerPosition == DrawerPosition::Left ? drawerLeftRadio_ : drawerRightRadio_)->setChecked(true);
    QButtonGroup* drawerButtons = new QButtonGroup(this);
    drawerButtons->addButton(drawerLeftRadio_);
    drawerButtons->addButton(drawerRightRadio_);
    drawerLayout->addWidget(drawerLeftRadio_);
    drawerLayout->addWidget(drawerRightRadio_);
    layout->addWidget(drawerGroup);

    connect(toolbarPositionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { updateDrawerControlState(); });
    updateDrawerControlState();

    QHBoxLayout* scaleRow = new QHBoxLayout();
    scaleRow->addWidget(new QLabel("UI Scaling:"));
    uiScaleSpin_ = new QSpinBox();
    uiScaleSpin_->setRange(50, 300);
    uiScaleSpin_->setSingleStep(25);
    uiScaleSpin_->setSuffix("%");
    uiScaleSpin_->setValue(int(initial.uiScalePercent));
#ifdef Q_OS_WIN
    // Windows already scales the whole UI per-monitor on its own -- offering
    // a second, manual override here would just let the two disagree.
    uiScaleSpin_->setEnabled(false);
    uiScaleSpin_->setToolTip("Windows already scales the UI automatically based on your display settings.");
#else
    uiScaleSpin_->setToolTip(
        "Matches the UI to your desktop's scaling when it can't be detected automatically "
        "(e.g. under WSLg). Takes effect after restarting the app.");
#endif
    scaleRow->addWidget(uiScaleSpin_);
    scaleRow->addStretch();
    layout->addLayout(scaleRow);
#ifndef Q_OS_WIN
    QLabel* scaleHint = new QLabel("Takes effect after restarting the app.");
    scaleHint->setStyleSheet("color: #9a9ca3; font-size: 11px;");
    layout->addWidget(scaleHint);
#endif

    addButtonRow(this, layout,
                 {
                     {"OK",
                      [this]() {
                          applySettings();
                          accept();
                      }},
                     {"Apply", [this]() { applySettings(); }},
                     {"Cancel", [this]() { reject(); }},
                 });
}

void PreferencesDialog::setAppliedCallback(std::function<void(const Preferences&)> callback) {
    onApplied_ = std::move(callback);
}

void PreferencesDialog::updateDrawerControlState() {
    auto toolbarPos = ToolbarPosition(toolbarPositionCombo_->currentData().toInt());
    bool toolbarIsHorizontal = toolbarPos == ToolbarPosition::Top || toolbarPos == ToolbarPosition::Bottom;
    drawerLeftRadio_->setEnabled(toolbarIsHorizontal);
    drawerRightRadio_->setEnabled(toolbarIsHorizontal);
    if (!toolbarIsHorizontal) {
        // Toolbar Left forces the drawer Right and vice versa -- pre-select
        // whichever that is while the controls are disabled, so what will
        // happen on Apply/OK is never a surprise.
        (toolbarPos == ToolbarPosition::Left ? drawerRightRadio_ : drawerLeftRadio_)->setChecked(true);
    }
}

Preferences PreferencesDialog::currentPreferences() const {
    Preferences prefs;
    prefs.toolbarPosition = ToolbarPosition(toolbarPositionCombo_->currentData().toInt());
    prefs.drawerPosition = drawerLeftRadio_->isChecked() ? DrawerPosition::Left : DrawerPosition::Right;
#ifdef Q_OS_WIN
    prefs.uiScalePercent = 100.0;
#else
    prefs.uiScalePercent = uiScaleSpin_->value();
#endif
    return prefs;
}

void PreferencesDialog::applySettings() {
    if (onApplied_) onApplied_(currentPreferences());
}

EbcdicHeaderWidget::EbcdicHeaderWidget(QWidget* parent) : QWidget(parent) {
    setWindowTitle("Text Header (EBCDIC)");
    // Matches the app's dark chrome around the text area itself, which is
    // deliberately pure black/white instead -- the classic terminal look
    // this kind of fixed-width mainframe text is normally read in, not
    // just "some more text on the app's usual dark-grey background."
    setStyleSheet("background-color: #1e1f22;");
    QVBoxLayout* layout = new QVBoxLayout(this);
    textEdit_ = new QPlainTextEdit(this);
    textEdit_->setReadOnly(true);
    textEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    textEdit_->setStyleSheet("background-color: black; color: white; border: none;");
    QFont mono("monospace");
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(10);
    textEdit_->setFont(mono);
    layout->addWidget(textEdit_);
    layout->setContentsMargins(0, 0, 0, 0);

    // Exactly 80x40 characters, fixed -- the on-disk header has no CR/LF to
    // rewrap around, so letting the user resize this to a different column
    // count would show something that isn't the file's actual layout.
    QFontMetrics fm(mono);
    int charWidth = fm.horizontalAdvance('X');
    int frameExtra = 2 * textEdit_->frameWidth() + 12; // scrollbar/margin headroom
    setFixedSize(charWidth * 80 + frameExtra, fm.lineSpacing() * 40 + frameExtra);
}

void EbcdicHeaderWidget::setHeaderText(const QString& rawText) {
    QStringList lines;
    for (int i = 0; i < rawText.size(); i += 80) lines << rawText.mid(i, 80);
    textEdit_->setPlainText(lines.join('\n'));
}

namespace {

// SEG-Y rev1 binary file header: every field the standard actually assigns
// a meaning to (the 3261-3500 range, and everything past 3506, is reserved/
// unassigned in rev1 and deliberately left out per the original request --
// "drop the unassigned"). `offset` is 0-based from the binary header's own
// start (add 3201 for the 1-based absolute file byte). `length` is 2 or 4.
// Decoded straight from the raw mmap'd bytes with the existing
// readI16BE/readI32BE readers, not through segy::BinaryHeader, which only
// parses the handful of fields the renderer itself actually needs.
struct BinHeaderFieldSpec {
    int offset;
    int length;
    const char* description;
};

constexpr BinHeaderFieldSpec kBinHeaderFields[] = {
    {0, 4, "Job identification number"},
    {4, 4, "Line number"},
    {8, 4, "Reel number"},
    {12, 2, "Data traces per ensemble"},
    {14, 2, "Auxiliary traces per ensemble"},
    {16, 2, "Sample interval [\xc2\xb5s]"},
    {18, 2, "Sample interval, original field recording [\xc2\xb5s]"},
    {20, 2, "Samples per data trace"},
    {22, 2, "Samples per data trace, original field recording"},
    {24, 2, "Data sample format code"},
    {26, 2, "Ensemble fold"},
    {28, 2, "Trace sorting code"},
    {30, 2, "Vertical sum code"},
    {32, 2, "Sweep frequency at start [Hz]"},
    {34, 2, "Sweep frequency at end [Hz]"},
    {36, 2, "Sweep length [ms]"},
    {38, 2, "Sweep type code"},
    {40, 2, "Trace number of sweep channel"},
    {42, 2, "Sweep trace taper length at start [ms]"},
    {44, 2, "Sweep trace taper length at end [ms]"},
    {46, 2, "Taper type"},
    {48, 2, "Correlated data traces"},
    {50, 2, "Binary gain recovered"},
    {52, 2, "Amplitude recovery method"},
    {54, 2, "Measurement system"},
    {56, 2, "Impulse signal polarity"},
    {58, 2, "Vibratory polarity code"},
    {300, 2, "SEG-Y format revision number"},
    {302, 2, "Fixed length trace flag"},
    {304, 2, "Number of extended textual file header records"},
};

// A handful of the fields above are enumerated codes, not raw magnitudes --
// annotated with their meaning the same way the format-code field already
// was before this table existed. Everything else just shows its number;
// guessing at meanings for codes not confidently known wasn't worth the
// risk of showing something wrong.
QString annotateBinHeaderValue(int offset, int32_t value) {
    switch (offset) {
        case 24: // Data sample format code
            switch (value) {
                case segy::kIbmFloat32: return "IBM float32";
                case segy::kInt32: return "32-bit integer";
                case segy::kInt16: return "16-bit integer";
                case segy::kFixedGain: return "Fixed gain (obsolete, unsupported)";
                case segy::kIeeeFloat32: return "IEEE float32";
                case segy::kIeeeFloat64: return "IEEE float64";
                case segy::kInt24: return "24-bit integer";
                case segy::kInt8: return "8-bit integer";
                default: return "Unknown";
            }
        case 28: // Trace sorting code
            switch (value) {
                case -1: return "Other";
                case 0: return "Unknown";
                case 1: return "As recorded";
                case 2: return "CDP ensemble";
                case 3: return "Single fold continuous profile";
                case 4: return "Horizontally stacked";
                case 5: return "Common source";
                case 6: return "Common receiver";
                case 7: return "Common offset";
                case 8: return "Common mid-point";
                case 9: return "Common conversion point";
                default: return QString();
            }
        case 54: // Measurement system
            return value == 1 ? "Meters" : value == 2 ? "Feet" : QString();
        case 56: // Impulse signal polarity
            return value == 1 ? "Increasing pressure = negative"
                   : value == 2 ? "Increasing pressure = positive"
                                : QString();
        default:
            return QString();
    }
}

} // namespace

BinaryHeaderWidget::BinaryHeaderWidget(QWidget* parent) : QWidget(parent) {
    setWindowTitle("Binary Header");
    setStyleSheet("background-color: #1e1f22;"); // matches the app's dark chrome, same as EbcdicHeaderWidget
    QVBoxLayout* layout = new QVBoxLayout(this);
    constexpr int kFieldCount = sizeof(kBinHeaderFields) / sizeof(kBinHeaderFields[0]);
    table_ = new QTableWidget(kFieldCount, 3, this);
    table_->setHorizontalHeaderLabels({"Byte", "Description", "Value"});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(table_);
    resize(560, 700);
}

void BinaryHeaderWidget::setHeader(const uint8_t* binHeaderBase) {
    constexpr int kFieldCount = sizeof(kBinHeaderFields) / sizeof(kBinHeaderFields[0]);
    for (int row = 0; row < kFieldCount; ++row) {
        const BinHeaderFieldSpec& field = kBinHeaderFields[row];
        int fileStart = 3201 + field.offset;
        int fileEnd = fileStart + field.length - 1;
        int32_t value = field.length == 4 ? segy::readI32BE(binHeaderBase + field.offset)
                                           : int32_t(segy::readI16BE(binHeaderBase + field.offset));
        QString valueText = QString::number(value);
        QString annotation = annotateBinHeaderValue(field.offset, value);
        if (!annotation.isEmpty()) valueText += QString(" (%1)").arg(annotation);

        table_->setItem(row, 0, new QTableWidgetItem(QString("%1-%2").arg(fileStart).arg(fileEnd)));
        table_->setItem(row, 1, new QTableWidgetItem(QString::fromUtf8(field.description)));
        table_->setItem(row, 2, new QTableWidgetItem(valueText));
    }
}

SegyCanvas::SegyCanvas(AppState& app, QWidget* parent) : QWidget(parent), app_(app) {
    setFocusPolicy(Qt::StrongFocus); // accept focus so keyPressEvent (R = reset) reaches us
    setAutoFillBackground(false);
    // Without this, mouseMoveEvent only fires while a button is held --
    // fine for the old drag-only pan, but hover info (Arrow tool) and the
    // box-select corner drag (click to attach, move with no button held,
    // click again to release) both need move events all the time.
    setMouseTracking(true);

    // Overlaid directly on the plot (top-right corner of the inset area)
    // rather than reserving dedicated layout space via chrome.cpp -- see
    // native/README.md, "Amplitude scale: from dock section to canvas
    // overlay."
    amplitudeScale_ = new AmplitudeScaleWidget(this);
    amplitudeScale_->setVisible(false);

    // Right-click the legend to change AppState::display.colorScale --
    // applies to this panel only (not Lock-aware like gain/mode), since
    // it's triggered from one specific panel's own overlay widget, not a
    // shared toolbar control, so which panel it means is never ambiguous.
    amplitudeScale_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(amplitudeScale_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(amplitudeScale_);
        struct Entry {
            const char* label;
            segy::ColorScale scale;
        };
        static const Entry kEntries[] = {
            {"Red - White - Blue", segy::ColorScale::RedWhiteBlue},
            {"Red - White - Black", segy::ColorScale::RedWhiteBlack},
            {"Grayscale (White to Black)", segy::ColorScale::GrayscaleWhiteToBlack},
            {"Grayscale (Black to White)", segy::ColorScale::GrayscaleBlackToWhite},
            {"Yellow - Red - White - Black - Cyan", segy::ColorScale::YellowRedWhiteBlackCyan},
        };
        QActionGroup* group = new QActionGroup(&menu);
        for (const Entry& entry : kEntries) {
            QAction* action = menu.addAction(entry.label);
            action->setCheckable(true);
            action->setChecked(app_.display.colorScale == entry.scale);
            action->setActionGroup(group);
            connect(action, &QAction::triggered, this, [this, scale = entry.scale]() {
                app_.display.colorScale = scale;
                amplitudeScale_->setColorScale(scale);
                update();
            });
        }

        menu.addSeparator();
        QAction* clipAction = menu.addAction("Clip...");
        connect(clipAction, &QAction::triggered, this, [this]() {
            if (!clipDialog_) {
                clipDialog_ = new ClipDialog(app_, this);
                clipDialog_->setChangedCallback([this]() { notifyStateChanged(); });
            }
            clipDialog_->show();
            clipDialog_->raise();
            clipDialog_->activateWindow();
        });

        QAction* lockScaleAction = menu.addAction("Lock Scale");
        lockScaleAction->setCheckable(true);
        lockScaleAction->setChecked(app_.display.clipLocked);
        connect(lockScaleAction, &QAction::triggered, this,
                [this](bool checked) { app_.display.clipLocked = checked; });

        menu.exec(amplitudeScale_->mapToGlobal(pos));
    });

    // Foreground/Background dataset boxes -- see native/README.md,
    // "Dataset pool: Foreground/Background." Labels ("Foreground 1" vs
    // "Foreground 2" etc.) are set by MainWindow right after construction
    // (setDatasetSlotLabels), since only it knows which panel this is.
    foregroundSlot_ = new DatasetSlotWidget(QString(), this);
    foregroundSlot_->setPickCallback([this]() { if (onDatasetSlotPick_) onDatasetSlotPick_(true); });
    backgroundSlot_ = new DatasetSlotWidget(QString(), this);
    backgroundSlot_->setPickCallback([this]() { if (onDatasetSlotPick_) onDatasetSlotPick_(false); });
}

void SegyCanvas::setAmplitudeScale(float posClip, float negClip, bool visible, segy::ColorScale colorScale) {
    amplitudeScale_->setClipRange(posClip, negClip);
    amplitudeScale_->setColorScale(colorScale);
    amplitudeScale_->setVisible(visible);
}

void SegyCanvas::setDatasetSlotLabels(const QString& foregroundLabel, const QString& backgroundLabel) {
    foregroundSlot_->setLabel(foregroundLabel);
    backgroundSlot_->setLabel(backgroundLabel);
}

void SegyCanvas::setDatasetSlotPickCallback(std::function<void(bool)> callback) {
    onDatasetSlotPick_ = std::move(callback);
}

void SegyCanvas::refreshDatasetSlotNames() {
    foregroundSlot_->setDatasetName(QString::fromStdString(app_.foreground->name));
    backgroundSlot_->setDatasetName(QString::fromStdString(app_.background->name));
}

int SegyCanvas::plotAreaWidth() const {
    int reserved = app_.display.showColorBar ? kAmplitudeScaleColumnWidth : 0;
    return std::max(0, this->width() - 2 * kCanvasMargin - reserved);
}

SegyCanvas::IdentInsets SegyCanvas::identInsets() const {
    int topRows = 0, bottomRows = 0;
    for (int i = 0; i < kIdentFieldCount; ++i) {
        if (app_.idents.topEnabled[i]) ++topRows;
        if (app_.idents.bottomEnabled[i]) ++bottomRows;
    }
    IdentInsets r;
    if (app_.idents.plotField >= 0) r.top += kIdentPlotHeight + kIdentRowGap;
    if (topRows > 0) r.top += topRows * kIdentRowHeight + kIdentRowGap;
    if (bottomRows > 0) r.bottom += bottomRows * kIdentRowHeight + kIdentRowGap;
    return r;
}

void SegyCanvas::repositionAmplitudeScale() {
    constexpr int kOverlayWidth = 46;
    IdentInsets insets = identInsets();
    int topInset = kCanvasMargin + insets.top;
    int insetHeight = std::max(0, height() - topInset - (kCanvasMargin + insets.bottom));
    int overlayHeight = int(insetHeight * 0.7);
    // Its own reserved column, immediately right of the plot/chrome's own
    // right axis -- never overlapping either, unlike the first version of
    // this overlay (see native/README.md).
    int x = kCanvasMargin + plotAreaWidth() + (kAmplitudeScaleColumnWidth - kOverlayWidth);
    int y = topInset + (insetHeight - overlayHeight) / 2;
    amplitudeScale_->setGeometry(x, y, kOverlayWidth, overlayHeight);
}

void SegyCanvas::repositionDatasetSlots() {
    // Flush with the plot's own left axis (lastPlotX_), not the widget's
    // own left edge -- see native/README.md: in split view this is what
    // makes panel 2's boxes line up near the screen's middle (where its
    // own plot begins) rather than at the window's actual left edge.
    // lastPlotX_ is only meaningful after the first paint; it defaults to
    // 0, which still places the boxes at a reasonable spot before then.
    // y tracks the image's actual top edge (kCanvasMargin, plus whatever
    // idents' top rows/plot currently reserve) rather than a fixed 2px, so
    // the boxes stay pinned the same 14px above the image as idents grow
    // the inset -- 14 = kCanvasMargin(16) - the original fixed y(2), i.e.
    // this reduces to the exact original position when no idents are
    // enabled (identInsets().top == 0) -- see native/README.md, "Idents."
    int x = kCanvasMargin + lastPlotX_;
    int y = (kCanvasMargin + identInsets().top) - (kCanvasMargin - 2);
    foregroundSlot_->move(x, y);
    backgroundSlot_->move(x + foregroundSlot_->width() + 4, y);
}

void SegyCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    repositionAmplitudeScale();
    repositionDatasetSlots();
}

void SegyCanvas::refreshIdentLayout() {
    repositionAmplitudeScale();
    repositionDatasetSlots();
    update();
}

std::string SegyCanvas::statusLineText() const {
    char buf[512];
    if (!app_.loaded && !app_.loading) {
        std::snprintf(buf, sizeof(buf), "File > Open (or Ctrl+O) to open a SEG-Y file");
        return buf;
    }
    if (app_.loading) {
        std::snprintf(buf, sizeof(buf), "Building pyramid... %lld / %lld blocks",
                       (long long)app_.progressDone.load(), (long long)app_.progressTotal.load());
        return buf;
    }
    if (app_.tool == ToolMode::BoxSelect) {
        if (app_.draggingBoxIndex >= 0) {
            std::snprintf(buf, sizeof(buf), "Drag corner, click to release");
        } else if (!app_.selectionBoxes.empty() && !app_.selectionBoxes.back().complete) {
            std::snprintf(buf, sizeof(buf), "Click to place the second corner");
        } else if (const SelectionBox* box = activeBox(app_)) {
            std::snprintf(buf, sizeof(buf),
                          "Box %d/%d selected: traces [%.0f, %.0f]  samples [%.0f, %.0f]  -- "
                          "click Zoom in the toolbar, Del to remove, or click empty space for a new box",
                          app_.activeBoxIndex + 1, int(app_.selectionBoxes.size()), std::min(box->traceA, box->traceB),
                          std::max(box->traceA, box->traceB), std::min(box->sampleA, box->sampleB),
                          std::max(box->sampleA, box->sampleB));
        } else if (!app_.selectionBoxes.empty()) {
            std::snprintf(buf, sizeof(buf), "%d box(es) drawn -- right-click one to select it, or click empty space for a new box",
                          int(app_.selectionBoxes.size()));
        } else {
            std::snprintf(buf, sizeof(buf), "Click to place the first corner");
        }
        return buf;
    }
    // Hover info (trace/time/amplitude) moved to the dock's Trace Info
    // section -- see currentHoverInfo()/notifyHoverChanged() -- so the
    // status bar always shows the summary line now, in every tool mode.
    const char* hint = (app_.tool == ToolMode::Pan) ? "drag=pan, wheel=zoom, R=reset" : "wheel=zoom, R=reset";
    std::snprintf(buf, sizeof(buf), "traces [%.0f, %.0f)  samples [%.0f, %.0f)  format=%d  render=%.2fms  (%s)",
                  app_.view.traceStart, app_.view.traceEnd, app_.view.sampleStart, app_.view.sampleEnd,
                  int(app_.foreground->binHeader.formatCode), app_.lastRenderMs, hint);
    return buf;
}

void SegyCanvas::updateStatusBar() {
    if (statusBar_ && !statusOverrideActive_) statusBar_->showMessage(QString::fromStdString(statusLineText()));
}

void SegyCanvas::refreshStatusBar() { updateStatusBar(); }

void SegyCanvas::setStatusOverrideActive(bool active) { statusOverrideActive_ = active; }

void SegyCanvas::notifyStateChanged() {
    if (notifyStateChanged_) notifyStateChanged_();
}

void SegyCanvas::setStatusWidgets(QStatusBar* statusBar, std::function<void()> onStateChanged) {
    statusBar_ = statusBar;
    notifyStateChanged_ = std::move(onStateChanged);
    updateStatusBar();
}

void SegyCanvas::setHoverInfoCallback(std::function<void(const HoverInfo&)> callback) {
    notifyHoverChanged_ = std::move(callback);
}

void SegyCanvas::setViewChangedCallback(std::function<void()> callback) { notifyViewChanged_ = std::move(callback); }

void SegyCanvas::setActivatedCallback(std::function<void()> callback) { notifyActivated_ = std::move(callback); }

void SegyCanvas::setDatasetLoadedCallback(std::function<void(std::shared_ptr<Dataset>)> callback) {
    onDatasetLoaded_ = std::move(callback);
}

// Exact (not bilinearly-interpolated) sample at the hovered position, plus
// that trace's header -- moved here from statusLineText() so it can feed
// the dock's Trace Info section instead.
HoverInfo SegyCanvas::currentHoverInfo() const {
    HoverInfo info;
    if (!app_.loaded || app_.tool != ToolMode::Arrow || !hovering_) return info;
    double traceCoord = 0, sampleCoord = 0;
    if (!dataCoordAt(hoverPixelX_, hoverPixelY_, traceCoord, sampleCoord)) return info;

    int64_t traceIdx = std::clamp<int64_t>(int64_t(traceCoord), 0, std::max<int64_t>(0, app_.foreground->traceCount - 1));
    int sampleIdx = std::clamp(int(sampleCoord), 0, std::max(0, int(app_.foreground->binHeader.samplesPerTrace) - 1));
    const uint8_t* fileBase = app_.foreground->file.data();
    const uint8_t* traceBase = fileBase + segy::kHeaderTotalSize + size_t(traceIdx) * app_.foreground->traceStrideBytes;
    segy::TraceHeader th = segy::parseTraceHeader(traceBase);
    int sampleSize = segy::sampleFormatSizeBytes(app_.foreground->binHeader.formatCode);
    const uint8_t* sampleBase = traceBase + segy::kTraceHeaderSize + size_t(sampleIdx) * size_t(sampleSize);
    float value = 0.0f;
    segy::decodeSamples(sampleBase, &value, 1, app_.foreground->binHeader.formatCode);

    info.valid = true;
    info.traceIdx = traceIdx;
    info.seqNum = th.traceSequenceLine;
    info.timeMs = double(sampleIdx) * (app_.foreground->binHeader.sampleIntervalUs / 1000.0);
    info.amplitude = value;
    return info;
}

void SegyCanvas::notifyHoverChanged() {
    if (notifyHoverChanged_) notifyHoverChanged_(currentHoverInfo());
}

bool SegyCanvas::dataCoordAt(int pixelX, int pixelY, double& traceCoord, double& sampleCoord) const {
    // pixelX/pixelY are widget-local (include the dark canvas margin, plus
    // whatever extra top/bottom inset idents currently reserve -- see
    // identInsets()); lastPlotX_/lastPlotWidth_ and the height used below
    // are relative to the *inset* plot area's own origin, matching what
    // paintEvent passes to chrome.
    IdentInsets insets = identInsets();
    int topInset = kCanvasMargin + insets.top;
    int insetHeight = this->height() - topInset - (kCanvasMargin + insets.bottom);
    int localX = pixelX - kCanvasMargin;
    int localY = pixelY - topInset;
    if (lastPlotWidth_ <= 0 || insetHeight <= 0) return false;
    if (localX < lastPlotX_ || localX >= lastPlotX_ + lastPlotWidth_) return false;
    if (localY < 0 || localY >= insetHeight) return false;
    double traceSpan = app_.view.traceEnd - app_.view.traceStart;
    double sampleSpan = app_.view.sampleEnd - app_.view.sampleStart;
    int xInPlot = localX - lastPlotX_;
    // Flip mirrors the finished plot left-right (see DisplaySettings::
    // flipHorizontal) without touching how `view` maps to pixels anywhere
    // else -- un-mirroring the pixel coordinate here, before the normal
    // mapping below, is what keeps hover/box-select/zoom-at-cursor reading
    // the correct real trace under the cursor.
    if (app_.display.flipHorizontal) xInPlot = lastPlotWidth_ - 1 - xInPlot;
    traceCoord = app_.view.traceStart + (double(xInPlot) + 0.5) / lastPlotWidth_ * traceSpan;
    sampleCoord = app_.view.sampleStart + (double(localY) + 0.5) / insetHeight * sampleSpan;
    return true;
}

void SegyCanvas::pixelForData(double traceCoord, double sampleCoord, double& pixelX, double& pixelY) const {
    IdentInsets insets = identInsets();
    int topInset = kCanvasMargin + insets.top;
    int insetHeight = this->height() - topInset - (kCanvasMargin + insets.bottom);
    double traceSpan = std::max(1e-9, app_.view.traceEnd - app_.view.traceStart);
    double sampleSpan = std::max(1e-9, app_.view.sampleEnd - app_.view.sampleStart);
    double xInPlot = (traceCoord - app_.view.traceStart) / traceSpan * lastPlotWidth_;
    // Inverse of dataCoordAt's un-mirroring, so box corners/handles land on
    // the same mirrored pixel the (also mirrored) plot itself draws at.
    if (app_.display.flipHorizontal) xInPlot = lastPlotWidth_ - xInPlot;
    pixelX = kCanvasMargin + lastPlotX_ + xInPlot;
    pixelY = topInset + (sampleCoord - app_.view.sampleStart) / sampleSpan * insetHeight;
}

int SegyCanvas::hitTestCorner(int pixelX, int pixelY, int* outBoxIndex) const {
    constexpr double kHitRadius = 8.0;
    int bestBox = -1, bestCorner = -1;
    double bestDist = kHitRadius;
    for (size_t bi = 0; bi < app_.selectionBoxes.size(); ++bi) {
        const SelectionBox& box = app_.selectionBoxes[bi];
        if (!box.complete) continue;
        double traces[2] = {box.traceA, box.traceB};
        double samples[2] = {box.sampleA, box.sampleB};
        for (int i = 0; i < 4; ++i) {
            double px, py;
            pixelForData(traces[i / 2], samples[i % 2], px, py);
            double dx = px - pixelX, dy = py - pixelY;
            double dist = std::sqrt(dx * dx + dy * dy);
            if (dist < bestDist) {
                bestDist = dist;
                bestBox = int(bi);
                bestCorner = i;
            }
        }
    }
    if (outBoxIndex) *outBoxIndex = bestBox;
    return bestCorner;
}

void SegyCanvas::handleBoxSelectPress(QMouseEvent* event) {
    int px = event->pos().x(), py = event->pos().y();

    if (event->button() == Qt::RightButton) {
        // Selects whichever complete box's rect contains the click (topmost
        // -- i.e. most recently drawn -- wins on overlap) as the "active"
        // one Delete/Zoom/Histogram/Spectrum act on.
        for (int bi = int(app_.selectionBoxes.size()) - 1; bi >= 0; --bi) {
            const SelectionBox& box = app_.selectionBoxes[size_t(bi)];
            if (!box.complete) continue;
            double x0, y0, x1, y1;
            pixelForData(std::min(box.traceA, box.traceB), std::min(box.sampleA, box.sampleB), x0, y0);
            pixelForData(std::max(box.traceA, box.traceB), std::max(box.sampleA, box.sampleB), x1, y1);
            if (px >= x0 - 4 && px <= x1 + 4 && py >= y0 - 4 && py <= y1 + 4) {
                app_.activeBoxIndex = bi;
                notifyStateChanged();
                update();
                return;
            }
        }
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        // Deletes the active box (the one a previous right-click selected)
        // -- replaces an earlier status-bar "Del" button that turned out to
        // be an easy-to-miss, easy-to-mis-click extra control; middle-click
        // is a common "delete/close this thing" gesture (e.g. closing a
        // browser tab) and needs no on-screen affordance.
        SelectionBox* box = activeBoxMut(app_);
        if (box) {
            app_.selectionBoxes.erase(app_.selectionBoxes.begin() + app_.activeBoxIndex);
            app_.activeBoxIndex = -1;
            notifyStateChanged();
            update();
        }
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    if (app_.draggingBoxIndex >= 0) {
        app_.draggingBoxIndex = -1;
        app_.draggingCorner = -1;
        notifyStateChanged();
        update();
        return;
    }

    // An in-progress box (first corner already placed) always takes the
    // next click as its second corner, wherever that click lands -- only a
    // *complete* box's corner handles are hit-testable for dragging, below.
    if (!app_.selectionBoxes.empty() && !app_.selectionBoxes.back().complete) {
        SelectionBox& box = app_.selectionBoxes.back();
        double traceCoord, sampleCoord;
        if (!dataCoordAt(px, py, traceCoord, sampleCoord)) return;
        box.traceB = traceCoord;
        box.sampleB = sampleCoord;
        box.complete = true;
        // Auto-select the box just finished -- it's what Zoom/Histogram/
        // Spectrum/Delete should act on next, without a separate right-click.
        app_.activeBoxIndex = int(app_.selectionBoxes.size()) - 1;
        notifyStateChanged();
        update();
        return;
    }

    int hitBoxIndex = -1;
    int corner = hitTestCorner(px, py, &hitBoxIndex);
    if (corner >= 0) {
        app_.draggingBoxIndex = hitBoxIndex;
        app_.draggingCorner = corner;
        update();
        return;
    }

    // Empty space, and no box mid-placement: start a brand-new one. Any
    // number can coexist -- e.g. a large box left from an earlier zoom,
    // plus a smaller new one drawn inside it.
    double traceCoord, sampleCoord;
    if (!dataCoordAt(px, py, traceCoord, sampleCoord)) return;
    SelectionBox newBox;
    newBox.traceA = newBox.traceB = traceCoord;
    newBox.sampleA = newBox.sampleB = sampleCoord;
    newBox.hasFirstCorner = true;
    newBox.colorIndex = app_.nextBoxColorIndex++;
    app_.selectionBoxes.push_back(newBox);
    notifyStateChanged();
    update();
}

segy::RenderContext SegyCanvas::makeRenderContext() const { return makeRenderContextFor(app_); }

// Idents (see native/README.md): top rows + the ident plot, then bottom
// rows -- pure QPainter text/line drawing, disjoint from the image region
// (kCanvasMargin..topInset above it, topInset+insetHeight..height-margin
// below), so no clipping is needed here (unlike the overlay lines below,
// which paint over the image itself).
void SegyCanvas::paintIdentText(QPainter& painter, const segy::ChromeLayout& chrome, int topInset, int bottomInset,
                                 int insetHeight) const {
    (void)bottomInset;
    const IdentSettings& idents = app_.idents;
    int topRows = 0, bottomRows = 0;
    for (int i = 0; i < kIdentFieldCount; ++i) {
        if (idents.topEnabled[i]) ++topRows;
        if (idents.bottomEnabled[i]) ++bottomRows;
    }
    if (idents.plotField < 0 && topRows == 0 && bottomRows == 0) return;

    const uint8_t* fileBase = app_.foreground->file.data();
    size_t stride = app_.foreground->traceStrideBytes;
    int64_t traceCount = app_.foreground->traceCount;
    auto headerAt = [&](int64_t idx) {
        const uint8_t* traceBase = fileBase + segy::kHeaderTotalSize + size_t(idx) * stride;
        return segy::parseTraceHeader(traceBase);
    };

    QFont font = axisFont();
    QFontMetrics fm(font);
    painter.save();
    painter.setFont(font);
    painter.setPen(Qt::white);

    auto drawRow = [&](int y, IdentField field, const std::vector<int64_t>& ticks) {
        for (int64_t idx : ticks) {
            double px, py;
            pixelForData(double(idx), 0.0, px, py);
            QString text = QString::number(identFieldValue(headerAt(idx), field));
            int tw = fm.horizontalAdvance(text);
            painter.drawText(QPoint(int(std::lround(px)) - tw / 2, y + fm.ascent()), text);
        }
    };

    std::vector<int64_t> textTicks = identTracePositions(app_.view, traceCount, 12);
    int y = kCanvasMargin;

    if (idents.plotField >= 0) {
        IdentField field = IdentField(idents.plotField);
        std::vector<int64_t> plotTicks = identTracePositions(app_.view, traceCount, std::max(1, chrome.plotWidth));
        std::vector<QPointF> points;
        std::vector<int32_t> values;
        points.reserve(plotTicks.size());
        values.reserve(plotTicks.size());
        double minVal = 0, maxVal = 0;
        for (size_t i = 0; i < plotTicks.size(); ++i) {
            double px, py;
            pixelForData(double(plotTicks[i]), 0.0, px, py);
            int32_t v = identFieldValue(headerAt(plotTicks[i]), field);
            if (i == 0) { minVal = maxVal = v; } else { minVal = std::min(minVal, double(v)); maxVal = std::max(maxVal, double(v)); }
            points.emplace_back(px, 0.0);
            values.push_back(v);
        }
        double range = std::max(1e-9, maxVal - minVal);
        int labelH = fm.height();
        int plotTop = y + labelH;           // leave room for the max label above the line
        int plotBottom = y + kIdentPlotHeight - labelH; // and the min label below it
        for (size_t i = 0; i < points.size(); ++i) {
            double t = (double(values[i]) - minVal) / range;
            points[i].setY(plotBottom - t * (plotBottom - plotTop));
        }
        if (points.size() >= 2) {
            QPainterPath path;
            path.moveTo(points[0]);
            for (size_t i = 1; i < points.size(); ++i) path.lineTo(points[i]);
            QPen linePen(QColor(90, 170, 255));
            linePen.setWidth(0);
            painter.setPen(linePen);
            painter.drawPath(path);
            painter.setPen(Qt::white);
        }
        int labelX = kCanvasMargin + chrome.plotX + 2;
        painter.drawText(QPoint(labelX, y + fm.ascent()), QString::number(maxVal, 'f', 1));
        painter.drawText(QPoint(labelX, y + kIdentPlotHeight - labelH + fm.ascent()), QString::number(minVal, 'f', 1));
        y += kIdentPlotHeight + kIdentRowGap;
    }

    for (int i = 0; i < kIdentFieldCount; ++i) {
        if (!idents.topEnabled[i]) continue;
        drawRow(y, IdentField(i), textTicks);
        y += kIdentRowHeight;
    }

    if (bottomRows > 0) {
        y = topInset + insetHeight + kIdentRowGap;
        for (int i = 0; i < kIdentFieldCount; ++i) {
            if (!idents.bottomEnabled[i]) continue;
            drawRow(y, IdentField(i), textTicks);
            y += kIdentRowHeight;
        }
    }

    painter.restore();
}

// Idents' "Display on Seismic" overlay lines: a field's raw value
// reinterpreted as a time in ms, converted to a Y pixel via the same
// pixelForData() selection-box corners already use. A gap (new subpath)
// is started whenever a sampled point's reinterpreted time falls outside
// the currently visible sample range, rather than clipping it to the
// plot's edge -- confirmed with the user: makes "this part is out of
// range" visually obvious instead of reading as a real flat value.
void SegyCanvas::paintIdentOverlayLines(QPainter& painter, const segy::ChromeLayout& chrome, int topInset,
                                         int insetHeight) const {
    const IdentSettings& idents = app_.idents;
    if (idents.overlayCyanField < 0 && idents.overlayRedField < 0) return;
    if (chrome.plotWidth <= 0) return;

    const uint8_t* fileBase = app_.foreground->file.data();
    size_t stride = app_.foreground->traceStrideBytes;
    int64_t traceCount = app_.foreground->traceCount;
    double sampleIntervalMs = app_.foreground->binHeader.sampleIntervalUs / 1000.0;
    if (sampleIntervalMs <= 0.0) return;

    std::vector<int64_t> ticks = identTracePositions(app_.view, traceCount, chrome.plotWidth);

    auto drawOverlay = [&](int fieldIdx, const QColor& color) {
        if (fieldIdx < 0) return;
        IdentField field = IdentField(fieldIdx);
        painter.save();
        painter.setClipRect(QRect(kCanvasMargin + chrome.plotX, topInset, chrome.plotWidth, insetHeight));
        QPen pen(color);
        pen.setWidth(0);
        painter.setPen(pen);
        QPainterPath path;
        bool havePoint = false;
        for (int64_t idx : ticks) {
            const uint8_t* traceBase = fileBase + segy::kHeaderTotalSize + size_t(idx) * stride;
            segy::TraceHeader th = segy::parseTraceHeader(traceBase);
            double timeMs = double(identFieldValue(th, field));
            double sampleCoord = timeMs / sampleIntervalMs;
            bool inRange = sampleCoord >= app_.view.sampleStart && sampleCoord <= app_.view.sampleEnd;
            if (!inRange) {
                havePoint = false;
                continue;
            }
            double px, py;
            pixelForData(double(idx), sampleCoord, px, py);
            if (!havePoint) {
                path.moveTo(px, py);
                havePoint = true;
            } else {
                path.lineTo(px, py);
            }
        }
        painter.drawPath(path);
        painter.restore();
    };

    drawOverlay(idents.overlayCyanField, QColor(0, 220, 220));
    drawOverlay(idents.overlayRedField, QColor(230, 60, 60));
}

void SegyCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    int width = this->width();
    int height = this->height();

    // Dark surround (see native/README.md, "Dark Pro"): fill the whole
    // widget first, then everything else below is inset by kCanvasMargin
    // so the plot reads as a panel within the canvas instead of full-bleed.
    // pixelForData()/dataCoordAt() already bake this same margin into their
    // widget-local coordinates, so mouse hit-testing and this drawing stay
    // consistent without a painter-wide translate (which would conflict
    // with pixelForData's margin-inclusive contract for the box below).
    painter.fillRect(0, 0, width, height, kCanvasSurroundColor);

    // Narrower than the full inset whenever the amplitude-scale legend's
    // column is reserved (see plotAreaWidth()) -- constant across
    // Variable Density/Wiggle switches, so the plot itself never resizes
    // just from toggling display mode.
    int insetWidth = plotAreaWidth();
    IdentInsets identInsetsNow = identInsets();
    int topInset = kCanvasMargin + identInsetsNow.top;
    int bottomInset = kCanvasMargin + identInsetsNow.bottom;
    int insetHeight = height - topInset - bottomInset;

    if (app_.loaded && insetWidth > 0 && insetHeight > 0) {
        if (int(app_.pixels.size()) != insetWidth * insetHeight) {
            app_.pixels.assign(size_t(insetWidth) * size_t(insetHeight), 0);
        }

        QFont font = axisFont();
        QFontMetrics fm(font);
        segy::TextMetrics metrics;
        metrics.fontHeightPx = fm.height();
        metrics.measureWidth = [fm](const std::string& s) -> int {
            return s.empty() ? 0 : fm.horizontalAdvance(QString::fromStdString(s));
        };

        // Color bar relocated to the dock's Amplitude Scale widget (see
        // native/README.md) -- forced off here regardless of the user's
        // showColorBar setting, which now controls the dock section's
        // visibility instead. A local copy, so chrome.cpp itself (and its
        // own tests, which exercise showColorBar directly) stay untouched.
        segy::DisplaySettings canvasDisplay = app_.display;
        canvasDisplay.showColorBar = false;

        segy::ChromeLayout chrome = segy::renderFrameWithChrome(
            makeRenderContext(), app_.view, canvasDisplay, metrics, app_.pixels.data(), insetWidth, insetHeight);
        app_.lastRenderMs = chrome.lastRenderMs;
        bool plotXChanged = lastPlotX_ != chrome.plotX;
        lastPlotX_ = chrome.plotX;
        lastPlotWidth_ = chrome.plotWidth;
        // The time scale/color bar toggling (not just a resize) can move
        // the plot's left edge -- keep the dataset boxes flush with it
        // either way. Calling this mid-paintEvent is safe: it only moves
        // child widgets, it doesn't trigger a synchronous repaint of them.
        if (plotXChanged) repositionDatasetSlots();

        if (app_.display.flipHorizontal && chrome.plotWidth > 0) {
            // Mirrors the already-rendered plot columns left-right in place
            // -- renderFrameWithChrome/chrome.cpp render the normal,
            // unmirrored raster (kept out of the tested shared code
            // entirely, see DisplaySettings::flipHorizontal), the Qt shell
            // just reverses each row's plot-region pixels afterwards. Grid
            // lines are unaffected (each is blended uniformly across the
            // whole row, so reversing a row of identical values is a
            // no-op); the color bar column is separately always 0 width
            // here (relocated to the dock, see above).
            uint32_t* pixels = app_.pixels.data();
            for (int y = 0; y < insetHeight; ++y) {
                uint32_t* row = pixels + size_t(y) * size_t(insetWidth) + size_t(chrome.plotX);
                std::reverse(row, row + chrome.plotWidth);
            }
        }

        // QImage::Format_RGB32 is bit-for-bit the 0x00RRGGBB packing
        // renderFrameWithChrome() already produces, so this wraps the
        // buffer directly -- no copy, no per-platform blit call needed.
        QImage image(reinterpret_cast<uchar*>(app_.pixels.data()), insetWidth, insetHeight,
                     int(insetWidth * sizeof(uint32_t)), QImage::Format_RGB32);
        painter.drawImage(QPoint(kCanvasMargin, topInset), image);

        // Idents (see native/README.md): rows + plot above the image,
        // rows below it -- disjoint from the image region, so no clip
        // needed. Drawn right after the image blit, before the time-scale
        // labels, so it reads as "chrome" rather than sitting over the
        // axis text.
        paintIdentText(painter, chrome, topInset, bottomInset, insetHeight);

        if (app_.display.showTimeScale) {
            int leftNumbersX = kCanvasMargin + chrome.leftScaleX + chrome.titleColumnWidth + chrome.columnGap;
            int rightNumbersX = kCanvasMargin + chrome.rightScaleX + chrome.columnGap;
            int leftTitleX = kCanvasMargin + chrome.leftScaleX;
            int rightTitleX =
                kCanvasMargin + chrome.rightScaleX + chrome.columnGap + chrome.numbersColumnWidth + chrome.columnGap;

            painter.setPen(Qt::white);
            for (const segy::TimeTick& tick : chrome.ticks) {
                // Labels are centered on their tick's Y position, so one
                // sitting right at the top/bottom edge (e.g. sample 0, or
                // the last visible sample) would have half its rotated
                // extent fall outside the plot and get clipped. Nudge it
                // inward just enough to stay fully visible, same as the
                // reference example this feature was built from.
                int halfSpan = fm.horizontalAdvance(QString::fromStdString(tick.label)) / 2;
                int y = topInset + std::clamp(tick.pixelY, halfSpan, insetHeight - halfSpan);
                drawRotatedLabel(painter, font, leftNumbersX, y, tick.label);
                drawRotatedLabel(painter, font, rightNumbersX, y, tick.label);
            }
            int titleCenterY = topInset + insetHeight / 2;
            drawRotatedLabel(painter, font, leftTitleX, titleCenterY, chrome.axisTitle);
            drawRotatedLabel(painter, font, rightTitleX, titleCenterY, chrome.axisTitle);
        }

        if (app_.display.seismicMode == segy::SeismicDisplayMode::Wiggle && chrome.plotWidth > 0) {
            // chrome.cpp skips the raster render entirely in wiggle mode
            // (see chrome.cpp), so chrome.lastRenderMs is always 0 here --
            // measure the actual per-frame cost (decode + decimation, the
            // wiggle-mode equivalent of renderFrame()'s own work) instead,
            // so the status line's render time stays meaningful in both
            // modes.
            auto wiggleStart = std::chrono::high_resolution_clock::now();
            segy::WiggleLayout wiggle = segy::computeWiggleLayout(
                makeRenderContext(), app_.view, chrome.plotWidth, insetHeight,
                app_.display.gainDb + kWiggleGainBoostDb, app_.display.reversePolarity);
            app_.lastRenderMs =
                std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - wiggleStart)
                    .count();
            painter.save();
            // Wiggle is deliberately crisp/non-smoothed -- see native/README.md.
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.translate(kCanvasMargin + chrome.plotX, topInset);
            QPen linePen(Qt::black);
            linePen.setWidth(0); // cosmetic pen: always exactly 1 device pixel, cheapest to draw
            painter.setPen(linePen);
            painter.setBrush(Qt::black);
            const WigglePresentationSettings& wp = app_.wigglePresentation;
            bool fillPos = wp.positiveFill;
            bool fillNeg = wp.negativeFill;
            for (const segy::WiggleTrace& tr : wiggle.traces) {
                if (tr.linePoints.size() < 2) continue;
                std::vector<QPointF> qpoints;
                qpoints.reserve(tr.linePoints.size());
                for (const segy::Point& p : tr.linePoints) qpoints.emplace_back(p.x, p.y);
                // computeWiggleLayout knows nothing about flipHorizontal (it
                // lives in the untouched shared core) -- mirror its geometry
                // here instead, same approach as the raster path above.
                double baselineX = tr.baselineX;
                if (app_.display.flipHorizontal) {
                    baselineX = chrome.plotWidth - baselineX;
                    for (QPointF& pt : qpoints) pt.setX(chrome.plotWidth - pt.x());
                }

                if (app_.display.wiggleInfill && (fillPos || fillNeg)) {
                    // "Variable area" fill, built per-segment as quads/
                    // triangles clipped to the baseline rather than one
                    // merged polygon -- adjacent pieces share only an edge,
                    // never overlapping area, so the default odd-even fill
                    // rule still comes out correct.
                    //
                    // A segment entirely on one side of the baseline is a
                    // plain quad. A segment that crosses the baseline is
                    // split at its exact linearly-interpolated zero-crossing
                    // -- an earlier version instead clamped each endpoint's
                    // X to the baseline while keeping its own Y, which
                    // silently assumed the crossing sat at whichever
                    // endpoint was already closest to zero; on a segment
                    // that actually crosses partway through, that made the
                    // filled lobe's tip bulge past the point where the
                    // wiggle line itself reaches zero.
                    QPainterPath fillPath;
                    for (size_t i = 0; i + 1 < qpoints.size(); ++i) {
                        double a0 = qpoints[i].x() - baselineX;
                        double a1 = qpoints[i + 1].x() - baselineX;
                        double y0 = qpoints[i].y();
                        double y1 = qpoints[i + 1].y();

                        if ((a0 >= 0.0) == (a1 >= 0.0)) {
                            bool positiveSide = a0 >= 0.0;
                            if (positiveSide ? !fillPos : !fillNeg) continue;
                            fillPath.moveTo(baselineX, y0);
                            fillPath.lineTo(qpoints[i].x(), y0);
                            fillPath.lineTo(qpoints[i + 1].x(), y1);
                            fillPath.lineTo(baselineX, y1);
                            fillPath.closeSubpath();
                            continue;
                        }

                        double t = a0 / (a0 - a1);
                        double yCross = y0 + t * (y1 - y0);
                        bool startsPositive = a0 > 0.0;
                        if ((startsPositive && fillPos) || (!startsPositive && fillNeg)) {
                            fillPath.moveTo(baselineX, y0);
                            fillPath.lineTo(qpoints[i].x(), y0);
                            fillPath.lineTo(baselineX, yCross);
                            fillPath.closeSubpath();
                        }
                        if ((startsPositive && fillNeg) || (!startsPositive && fillPos)) {
                            fillPath.moveTo(baselineX, yCross);
                            fillPath.lineTo(qpoints[i + 1].x(), y1);
                            fillPath.lineTo(baselineX, y1);
                            fillPath.closeSubpath();
                        }
                    }
                    painter.fillPath(fillPath, Qt::black);
                }
                if (wp.centerLine) {
                    // Zero-amplitude reference line for this trace, full
                    // plot height -- drawn before the wiggle line itself so
                    // the (thicker-looking, solid) wiggle trace stays the
                    // most visually prominent line at the crossing points.
                    QPen centerPen(QColor(160, 160, 160));
                    centerPen.setWidth(0);
                    painter.setPen(centerPen);
                    painter.drawLine(QPointF(baselineX, 0), QPointF(baselineX, insetHeight));
                    painter.setPen(linePen);
                }
                painter.drawPolyline(qpoints.data(), int(qpoints.size()));
            }
            painter.restore();
        }

        // Idents' "Display on Seismic" overlay lines (see
        // native/README.md) -- drawn before selection boxes so the box
        // the user is actively dragging/editing stays the topmost element.
        paintIdentOverlayLines(painter, chrome, topInset, insetHeight);

        // Selection boxes: derived fresh from their data-space corners every
        // frame (same reasoning as the density plot itself), so they stay
        // correctly anchored to the data across pan/zoom. Clipped to the
        // plot area so a box drawn before a zoom/pan that moved it outside
        // the current view doesn't visibly spill into the axis columns or
        // dark surround.
        painter.save();
        painter.setClipRect(QRect(kCanvasMargin + chrome.plotX, topInset, chrome.plotWidth, insetHeight));
        for (size_t bi = 0; bi < app_.selectionBoxes.size(); ++bi) {
            const SelectionBox& box = app_.selectionBoxes[bi];
            if (!box.hasFirstCorner) continue;
            double x0, y0, x1, y1;
            pixelForData(std::min(box.traceA, box.traceB), std::min(box.sampleA, box.sampleB), x0, y0);
            pixelForData(std::max(box.traceA, box.traceB), std::max(box.sampleA, box.sampleB), x1, y1);
            bool isActive = int(bi) == app_.activeBoxIndex;
            QColor edgeColor = boxColorForIndex(box.colorIndex);
            if (!isActive) edgeColor.setAlpha(170); // dims inactive boxes so the active one visibly pops
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, false);
            QPen edgePen(edgeColor);
            edgePen.setWidth(isActive ? 2 : 1);
            painter.setPen(edgePen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(QPointF(x0, y0), QPointF(x1, y1)));
            if (box.complete) {
                constexpr double kHandle = 6.0;
                painter.setBrush(edgeColor);
                double corners[4][2] = {{x0, y0}, {x0, y1}, {x1, y0}, {x1, y1}};
                for (auto& c : corners) {
                    painter.drawRect(QRectF(c[0] - kHandle / 2, c[1] - kHandle / 2, kHandle, kHandle));
                }
            }
            painter.restore();
        }
        painter.restore();
    }
    // No file loaded: the dark surround fill above already covers the
    // whole widget, so there's nothing more to draw here.

    // Keeps the status bar in sync with whatever was just rendered (render
    // time, view range) without forcing this on every hover move -- a
    // repaint just happened, so one more cheap text update here is
    // negligible next to the render itself. mouseMoveEvent's own
    // updateStatusBar() call handles the hover-text-only case that doesn't
    // trigger a repaint at all.
    updateStatusBar();
}

void SegyCanvas::mousePressEvent(QMouseEvent* event) {
    if (notifyActivated_) notifyActivated_();
    if (!app_.loaded) return;
    if (app_.tool == ToolMode::BoxSelect) {
        handleBoxSelectPress(event);
    } else if (app_.tool == ToolMode::Pan && event->button() == Qt::LeftButton) {
        app_.dragging = true;
        app_.dragAnchorX = event->pos().x();
        app_.dragAnchorY = event->pos().y();
        app_.dragAnchorView = app_.view;
    }
    updateStatusBar();
}

void SegyCanvas::mouseMoveEvent(QMouseEvent* event) {
    hoverPixelX_ = event->pos().x();
    hoverPixelY_ = event->pos().y();
    hovering_ = true;
    bool needsRepaint = false;

    if (app_.tool == ToolMode::Pan && app_.dragging) {
        int insetWidth = std::max(1, plotAreaWidth());
        IdentInsets insets = identInsets();
        int insetHeight = std::max(1, this->height() - (kCanvasMargin + insets.top) - (kCanvasMargin + insets.bottom));
        int dx = event->pos().x() - app_.dragAnchorX;
        int dy = event->pos().y() - app_.dragAnchorY;
        // panFromAnchor (renderer.cpp) always shifts `view` in the direction
        // that makes on-screen content follow an *unmirrored* drag; negate
        // dx first so a mirrored display still follows the mouse instead of
        // panning backwards.
        if (app_.display.flipHorizontal) dx = -dx;
        segy::panFromAnchor(app_.view, app_.dragAnchorView, app_.foreground->traceCount, app_.foreground->binHeader.samplesPerTrace,
                             insetWidth, insetHeight, dx, dy);
        needsRepaint = true;
        if (notifyViewChanged_) notifyViewChanged_();
    } else if (app_.tool == ToolMode::BoxSelect) {
        bool draggingCorner = app_.draggingBoxIndex >= 0;
        bool placingSecondCorner = !app_.selectionBoxes.empty() && !app_.selectionBoxes.back().complete;
        if (draggingCorner || placingSecondCorner) {
            double traceCoord, sampleCoord;
            if (dataCoordAt(hoverPixelX_, hoverPixelY_, traceCoord, sampleCoord)) {
                SelectionBox& box =
                    draggingCorner ? app_.selectionBoxes[size_t(app_.draggingBoxIndex)] : app_.selectionBoxes.back();
                if (draggingCorner) {
                    int i = app_.draggingCorner;
                    if (i / 2 == 0) box.traceA = traceCoord; else box.traceB = traceCoord;
                    if (i % 2 == 0) box.sampleA = sampleCoord; else box.sampleB = sampleCoord;
                } else {
                    // Live rubber-band preview while placing the second corner.
                    box.traceB = traceCoord;
                    box.sampleB = sampleCoord;
                }
                needsRepaint = true;
            }
        }
    } else if (app_.tool == ToolMode::Arrow) {
        notifyHoverChanged();
    }

    updateStatusBar();
    if (needsRepaint) update();
}

void SegyCanvas::mouseReleaseEvent(QMouseEvent*) { app_.dragging = false; }

void SegyCanvas::wheelEvent(QWheelEvent* event) {
    if (!app_.loaded) return;
    int insetWidth = std::max(1, plotAreaWidth());
    IdentInsets insets = identInsets();
    int topInset = kCanvasMargin + insets.top;
    int insetHeight = std::max(1, this->height() - topInset - (kCanvasMargin + insets.bottom));
    QPoint pos = event->position().toPoint();
    int px = pos.x() - kCanvasMargin;
    int py = pos.y() - topInset;
    // zoomAt (renderer.cpp) has no notion of the mirrored display -- give it
    // the un-mirrored pixel position so it still zooms on the real trace
    // under the cursor (same reasoning as dataCoordAt).
    if (app_.display.flipHorizontal) px = insetWidth - 1 - px;
    // Forward/up notches zoom in, matching the removed shells' convention.
    int notches = event->angleDelta().y() / 120;
    double factor = std::pow(1.15, double(-notches));
    segy::zoomAt(app_.view, app_.foreground->traceCount, app_.foreground->binHeader.samplesPerTrace, insetWidth, insetHeight, px, py, factor);
    if (notifyViewChanged_) notifyViewChanged_();
    updateStatusBar();
    update();
}

void SegyCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_R && app_.loaded) {
        segy::resetView(app_.view, app_.foreground->traceCount, app_.foreground->binHeader.samplesPerTrace);
        if (notifyViewChanged_) notifyViewChanged_();
        updateStatusBar();
        update();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SegyCanvas::leaveEvent(QEvent*) {
    hovering_ = false;
    updateStatusBar();
    notifyHoverChanged(); // clears the dock's Trace Info section
}

void SegyCanvas::setForegroundDataset(std::shared_ptr<Dataset> dataset) {
    // Never mutate a Dataset's fields in place -- it may be shared with
    // the pool or another panel's Background slot, so this only ever
    // reassigns the pointer (an O(1) swap, whether the dataset is brand
    // new from startLoading or already sitting in the pool).
    app_.foreground = std::move(dataset);
    app_.loaded = true;
    segy::resetView(app_.view, app_.foreground->traceCount, app_.foreground->binHeader.samplesPerTrace);
    // A newly-active dataset invalidates any boxes drawn against the
    // previous one's trace/sample space.
    app_.selectionBoxes.clear();
    app_.activeBoxIndex = -1;
    app_.draggingBoxIndex = -1;
    app_.draggingCorner = -1;
    app_.zoomHistory.clear();
    notifyStateChanged();
    updateStatusBar();
    update();
}

void SegyCanvas::setBackgroundDataset(std::shared_ptr<Dataset> dataset) {
    // Phase 1: purely a held reference (see native/README.md, "Dataset
    // pool") -- nothing reads app_.background for rendering/analysis yet,
    // so this is just the pointer swap plus telling the Background box to
    // repaint its new name.
    app_.background = std::move(dataset);
    notifyStateChanged();
}

void SegyCanvas::startLoading(const std::filesystem::path& path, std::vector<std::string> processingHistory) {
    if (app_.loading) return;
    app_.loaded = false;
    app_.loading = true;
    app_.progressDone = 0;
    app_.progressTotal = 1;

    std::thread([this, path, processingHistory = std::move(processingHistory)]() mutable {
        // Must be a genuinely atomic timestamp -- buildPyramid's progress
        // callback runs concurrently from multiple pool worker threads. A
        // plain std::chrono::time_point mutated across threads without
        // synchronization was a real, confirmed bug in the removed shells
        // (see native/README.md); carrying that fix forward here.
        std::atomic<int64_t> lastPostMs{0};
        segy::LoadResult result = segy::loadSegyFile(path, app_.pool, [this, &lastPostMs](int64_t done, int64_t total) {
            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
            int64_t prevMs = lastPostMs.load(std::memory_order_relaxed);
            if (done == total || nowMs - prevMs > 33) {
                lastPostMs.store(nowMs, std::memory_order_relaxed);
                app_.progressDone = done;
                app_.progressTotal = total;
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        updateStatusBar();
                        update();
                    },
                    Qt::QueuedConnection);
            }
        });

        QMetaObject::invokeMethod(
            this,
            [this, path, result = std::move(result), processingHistory = std::move(processingHistory)]() mutable {
                app_.loading = false;
                if (result.ok) {
                    auto dataset = std::make_shared<Dataset>();
                    dataset->name = path.stem().string();
                    dataset->filePath = path;
                    dataset->file = std::move(result.file);
                    dataset->binHeader = result.binHeader;
                    dataset->traceCount = result.traceCount;
                    dataset->traceStrideBytes = result.traceStrideBytes;
                    dataset->pyramid = std::move(result.pyramid);
                    dataset->processingHistory = std::move(processingHistory);
                    // Foreground first: onDatasetLoaded_ (MainWindow::
                    // registerDataset) ends by refreshing both panels'
                    // Foreground/Background box text, which reads through
                    // app_.foreground -- it must already point at this
                    // dataset by then, or the box shows the *previous*
                    // (possibly empty) one for one frame... or, as a real
                    // bug caught by screenshot-testing this, forever, since
                    // nothing else was re-triggering that refresh.
                    setForegroundDataset(dataset);
                    if (onDatasetLoaded_) onDatasetLoaded_(dataset);
                } else {
                    app_.loaded = false;
                    std::fprintf(stderr, "Failed to open SEG-Y file: %s\n", result.error.c_str());
                    // TODO(UI polish): surface this in-window (e.g. QMessageBox) instead of stderr only.
                }
                updateStatusBar();
                update();
            },
            Qt::QueuedConnection);
    }).detach();

    updateStatusBar();
    update();
}

MainWindow::MainWindow() : QMainWindow() {
    setWindowTitle("SEG-Y Viewer (native)");
    resize(1200, 800);

    // Split view: both panels' canvases always exist, side by side in a
    // splitter; panel B starts hidden (single-panel view) until the
    // toolbar's Split View toggle shows it. app_/canvas_ alias whichever
    // panel was last clicked into (see setActivePanel) so every existing
    // single-panel handler below keeps working unchanged.
    panelA_.canvas = new SegyCanvas(panelA_.app, this);
    panelB_.canvas = new SegyCanvas(panelB_.app, this);
    canvas_ = panelA_.canvas;
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->addWidget(panelA_.canvas);
    splitter_->addWidget(panelB_.canvas);
    panelB_.canvas->hide();
    setCentralWidget(splitter_);
    panelA_.canvas->setActivatedCallback([this]() { setActivePanel(&panelA_); });
    panelB_.canvas->setActivatedCallback([this]() { setActivePanel(&panelB_); });

    // Same dummy-vs-real split as the removed shells: only File > Open and
    // File > Exit do anything. The rest are deliberately inert placeholders
    // (an addAction() with no connected slot literally does nothing when
    // clicked) so the app has the conventional File/Edit/Selection/View/
    // Help menu bar of a normal desktop app without pretending to implement
    // features that don't exist yet.
    QMenu* fileMenu = menuBar()->addMenu("&File");
    QAction* openAction = fileMenu->addAction("&Open...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);
    QAction* saveSegyAction = fileMenu->addAction("&Save SEG-Y...");
    saveSegyAction->setShortcut(QKeySequence::Save);
    connect(saveSegyAction, &QAction::triggered, this, &MainWindow::openSaveSegyDialog);
    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction("E&xit");
    connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);

    // Undo/Redo removed for now (per the original ask) -- nothing in this
    // app is undoable yet; Calculator/Processing below produce a *new*
    // dataset each time rather than modifying one in place.
    QMenu* editMenu = menuBar()->addMenu("&Edit");
    QAction* calculatorAction = editMenu->addAction("&Calculator...");
    connect(calculatorAction, &QAction::triggered, this, &MainWindow::openCalculatorDialog);
    QMenu* processingMenu = editMenu->addMenu("&Processing");
    QAction* bandpassAction = processingMenu->addAction("&Bandpass...");
    connect(bandpassAction, &QAction::triggered, this, &MainWindow::openBandpassDialog);
    editMenu->addSeparator();
    editMenu->addAction("Cu&t");
    editMenu->addAction("&Copy");
    editMenu->addAction("&Paste");
    editMenu->addSeparator();
    QAction* preferencesAction = editMenu->addAction("&Preferences...");
    connect(preferencesAction, &QAction::triggered, this, [this]() {
        if (!preferencesDialog_) {
            preferencesDialog_ = new PreferencesDialog(prefs_, this);
            preferencesDialog_->setAppliedCallback([this](const Preferences& newPrefs) {
                prefs_ = newPrefs;
                applyToolbarPosition(prefs_.toolbarPosition); // also applies prefs_.drawerPosition
                savePreferences();
            });
        }
        preferencesDialog_->show();
        preferencesDialog_->raise();
        preferencesDialog_->activateWindow();
    });

    QMenu* selectionMenu = menuBar()->addMenu("&Selection");
    selectionMenu->addAction("Select &All");
    selectionMenu->addAction("&Clear Selection");

    QMenu* viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction("Zoom &In");
    viewMenu->addAction("Zoom &Out");
    viewMenu->addAction("&Reset View");
    viewMenu->addSeparator();
    viewMenu->addAction("&Fit to Window");
    viewMenu->addSeparator();

    // Seismic Display: real, working toggles (unlike the placeholders
    // above) -- DisplaySettings is read fresh every frame, so flipping
    // these and repainting is all that's needed, same pattern the existing
    // time-scale/color-bar/grid-line settings already follow. Also
    // reachable from the toolbar's single toggle button now (below); both
    // drive the same app_.display.seismicMode and stay in sync with each
    // other.
    QMenu* seismicDisplayMenu = viewMenu->addMenu("&Seismic Display");
    QActionGroup* seismicDisplayMenuGroup = new QActionGroup(this);
    variableDensityMenuAction_ = seismicDisplayMenu->addAction("&Variable Density");
    variableDensityMenuAction_->setCheckable(true);
    variableDensityMenuAction_->setChecked(true);
    seismicDisplayMenuGroup->addAction(variableDensityMenuAction_);
    connect(variableDensityMenuAction_, &QAction::triggered, this,
            [this]() { setSeismicMode(segy::SeismicDisplayMode::VariableDensity); });
    wiggleMenuAction_ = seismicDisplayMenu->addAction("&Wiggle");
    wiggleMenuAction_->setCheckable(true);
    seismicDisplayMenuGroup->addAction(wiggleMenuAction_);
    connect(wiggleMenuAction_, &QAction::triggered, this,
            [this]() { setSeismicMode(segy::SeismicDisplayMode::Wiggle); });

    QAction* wiggleInfillAction = viewMenu->addAction("Wiggle &Infill");
    wiggleInfillAction->setCheckable(true);
    wiggleInfillAction->setChecked(true);
    connect(wiggleInfillAction, &QAction::triggered, this, [this](bool checked) {
        for (Panel* p : targetPanels()) {
            p->app.display.wiggleInfill = checked;
            p->canvas->update();
        }
    });

    QMenu* helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&About");

    // Toolbar: docks below the menu bar automatically, though Preferences
    // can move it to any of the 4 sides (see applyToolbarPosition, called
    // once loadPreferences() runs at the end of this constructor). Zoom/
    // Mooz are one-shot actions (not modes); Pan/Box Select are mutually-
    // exclusive modes with ExclusiveOptional so clicking the active one
    // again deselects back to the implicit Arrow tool (no visible third
    // button needed for it).
    toolbar_ = addToolBar("Tools");
    toolbar_->setMovable(false);
    // 85% of the 24px icons' nominal size.
    toolbar_->setIconSize(QSize(20, 20));
    QToolBar* toolbar = toolbar_; // short alias for the many toolbar->... calls below

    zoomAction_ = toolbar->addAction(iconMagnifier(true), "Zoom");
    zoomAction_->setToolTip("Zoom to the selection box");
    zoomAction_->setEnabled(false);
    connect(zoomAction_, &QAction::triggered, this, &MainWindow::zoomToBox);

    moozAction_ = toolbar->addAction(iconMagnifier(false), "Mooz");
    moozAction_->setToolTip("Undo the last box zoom");
    moozAction_->setEnabled(false);
    connect(moozAction_, &QAction::triggered, this, &MainWindow::moozBack);

    toolbar->addSeparator();

    QActionGroup* toolGroup = new QActionGroup(this);
    toolGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);

    panAction_ = toolbar->addAction(iconHand(), "Pan");
    panAction_->setCheckable(true);
    panAction_->setToolTip("Drag to pan");
    toolGroup->addAction(panAction_);
    connect(panAction_, &QAction::triggered, this, [this](bool checked) {
        for (Panel* p : targetPanels()) {
            p->app.tool = checked ? ToolMode::Pan : ToolMode::Arrow;
            p->canvas->setCursor(checked ? QCursor(Qt::OpenHandCursor) : QCursor(Qt::ArrowCursor));
        }
        refreshToolChrome();
    });

    boxSelectAction_ = toolbar->addAction(iconRectangle(), "Box Select");
    boxSelectAction_->setCheckable(true);
    boxSelectAction_->setToolTip("Draw a box to zoom into, or edit/delete the existing one");
    toolGroup->addAction(boxSelectAction_);
    connect(boxSelectAction_, &QAction::triggered, this, [this](bool checked) {
        for (Panel* p : targetPanels()) {
            p->app.tool = checked ? ToolMode::BoxSelect : ToolMode::Arrow;
            p->canvas->setCursor(checked ? makeBoxSelectCursor() : QCursor(Qt::ArrowCursor));
        }
        refreshToolChrome();
    });

    toolbar->addSeparator();

    // Seismic Display: a single standard-size toolbar button rather than a
    // segmented pair -- its icon always shows the *other* mode (the one
    // clicking it switches to), same convention as a play/pause button.
    seismicModeToolbarAction_ = toolbar->addAction(QIcon(), "Seismic Display");
    connect(seismicModeToolbarAction_, &QAction::triggered, this, [this]() {
        bool nowWiggle = app_->display.seismicMode != segy::SeismicDisplayMode::Wiggle;
        setSeismicMode(nowWiggle ? segy::SeismicDisplayMode::Wiggle : segy::SeismicDisplayMode::VariableDensity);
    });

    // Mirrors the display left-right -- see DisplaySettings::flipHorizontal
    // (chrome.h) for why this is a pure Qt-shell concern (mirroring/
    // un-mirroring around the untouched shared render paths) rather than
    // something renderer.cpp/wiggle.cpp know about.
    flipHorizontalAction_ = toolbar->addAction(iconArrowsHorizontal(), "Flip Horizontal");
    flipHorizontalAction_->setCheckable(true);
    flipHorizontalAction_->setToolTip("Mirror the display left-right");
    connect(flipHorizontalAction_, &QAction::triggered, this, [this](bool checked) {
        for (Panel* p : targetPanels()) p->app.display.flipHorizontal = checked;
        refreshToolChrome();
    });

    // Idents: trace-header reference rows/plot above and below the plot,
    // plus up to two colored lines drawn directly on the seismic -- see
    // native/README.md. Menu rebuilt fresh from the active panel's
    // app_->idents every time it's opened (same approach as
    // toolbarMoreMenu_), not persisted, not kept live-in-sync.
    identsAction_ = toolbar->addAction(iconIdents(), "Idents");
    identsAction_->setToolTip("Trace-header rows/plot, and lines on the seismic");
    connect(identsAction_, &QAction::triggered, this, [this]() {
        QMenu menu(this);

        // Display at Top / Display at Bottom: independent checkable
        // toggles, capped at 4/2 -- refusing an over-cap check (rather
        // than evicting the oldest) is simplest: the real QAction/menu is
        // thrown away the instant exec() returns, so there's no state to
        // revert -- the next open just rebuilds from the untouched
        // app_->idents.
        auto buildCappedSubmenu = [this](QMenu& parent, const QString& title, bool (IdentSettings::*array)[kIdentFieldCount],
                                          int cap) {
            QMenu* sub = parent.addMenu(title);
            for (int i = 0; i < kIdentFieldCount; ++i) {
                QAction* a = sub->addAction(identFieldLabel(IdentField(i)));
                a->setCheckable(true);
                a->setChecked((app_->idents.*array)[i]);
                connect(a, &QAction::triggered, this, [this, array, i, cap](bool checked) {
                    if (checked) {
                        int count = 0;
                        for (int k = 0; k < kIdentFieldCount; ++k) {
                            if ((app_->idents.*array)[k]) ++count;
                        }
                        if (count >= cap) {
                            flashStatusMessage(QString("Limited to %1 idents here").arg(cap));
                            return;
                        }
                    }
                    for (Panel* p : targetPanels()) {
                        (p->app.idents.*array)[i] = checked;
                        p->canvas->refreshIdentLayout();
                    }
                });
            }
        };
        buildCappedSubmenu(menu, "Display at Top", &IdentSettings::topEnabled, 4);
        buildCappedSubmenu(menu, "Display at Bottom", &IdentSettings::bottomEnabled, 2);

        // Ident Plot / each overlay-line color: single-select-or-none --
        // an ExclusiveOptional QActionGroup (same policy already used for
        // Pan/Box Select) makes clicking the checked entry again clear it.
        auto buildPickerSubmenu = [this, &menu](QMenu& parent, const QString& title, int IdentSettings::*field) {
            QMenu* sub = parent.addMenu(title);
            QActionGroup* group = new QActionGroup(&menu);
            group->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
            for (int i = 0; i < kIdentFieldCount; ++i) {
                QAction* a = sub->addAction(identFieldLabel(IdentField(i)));
                a->setCheckable(true);
                a->setChecked(app_->idents.*field == i);
                a->setActionGroup(group);
                connect(a, &QAction::triggered, this, [this, field, i](bool checked) {
                    for (Panel* p : targetPanels()) {
                        p->app.idents.*field = checked ? i : -1;
                        p->canvas->refreshIdentLayout();
                    }
                });
            }
        };
        buildPickerSubmenu(menu, "Ident Plot", &IdentSettings::plotField);
        QMenu* seismicMenu = menu.addMenu("Display on Seismic");
        buildPickerSubmenu(*seismicMenu, "Line Cyan", &IdentSettings::overlayCyanField);
        buildPickerSubmenu(*seismicMenu, "Line Red", &IdentSettings::overlayRedField);

        QWidget* button = toolbar_->widgetForAction(identsAction_);
        if (button) menu.exec(button->mapToGlobal(QPoint(0, button->height())));
    });

    toolbar->addSeparator();

    // Gain: dB gain applied on top of the pyramid's auto-computed clip
    // (see DisplaySettings::gainDb) -- auto-scaling stays the baseline,
    // this just biases sensitivity around it. A slider (matching the
    // approved mockup) paired with a spin box for direct typing --
    // "select the value and type a number, auto-clipped" from the
    // original ask -- both bound to the same value and kept in sync. The
    // "Gain" label was replaced with a disclosure button (caret-up-down)
    // that opens the fuller DisplayParametersDialog.
    gainDetailAction_ = toolbar->addAction(iconAdjustments(), "Display Parameters");
    gainDetailAction_->setToolTip("Display parameters");
    connect(gainDetailAction_, &QAction::triggered, this, [this]() {
        // Binds to whichever panel is active the *first* time this opens
        // (a C++ reference can't be rebound to the other panel later if
        // you switch) -- a known, documented limitation for this
        // placeholder-heavy dialog, not something split view's core
        // Zoom/Mooz/Gain/Tool-mode/Pan handling depends on.
        if (!displayParametersDialog_) {
            displayParametersDialog_ = new DisplayParametersDialog(*app_, this);
            displayParametersDialog_->setWindowTitle(QString("Display Parameters") + datasetTitleSuffix(*app_));
            displayParametersDialog_->setGainChangedCallback([this](double value) { setGainDb(value); });
            displayParametersDialog_->setModeChangedCallback(
                [this](segy::SeismicDisplayMode mode) { setSeismicMode(mode); });
        }
        displayParametersDialog_->refreshFromAppState();
        displayParametersDialog_->show();
        displayParametersDialog_->raise();
        displayParametersDialog_->activateWindow();
    });
    gainSlider_ = new QSlider(Qt::Horizontal, this);
    gainSlider_->setRange(-6, 24);
    gainSlider_->setSingleStep(3);
    gainSlider_->setPageStep(3);
    gainSlider_->setFixedWidth(110);
    gainSliderAction_ = toolbar->addWidget(gainSlider_);

    connect(gainSlider_, &QSlider::valueChanged, this, [this](int value) {
        // Snap drags to 3 dB steps even though QSlider itself allows any
        // integer in range -- setSingleStep only affects arrow-key/wheel
        // stepping, not dragging.
        int snapped = int(std::lround(value / 3.0)) * 3;
        if (snapped != value) {
            gainSlider_->blockSignals(true);
            gainSlider_->setValue(snapped);
            gainSlider_->blockSignals(false);
        }
        setGainDb(snapped);
    });

    // The slider has no room to lay out sideways when the toolbar is docked
    // Left/Right -- a vertical QSlider would read as a totally different
    // control, not the same one rotated, and its orange-handle styling is
    // specifically a horizontal-track look, not meant to survive rotation.
    // For a vertical toolbar it's swapped for a plain +/- step pair
    // instead. gainValueLabel_ (the actual dB readout) is the one part
    // that's always visible in both orientations -- it used to be paired
    // with a separate QSpinBox for horizontal mode, but that was dropped
    // as redundant once the label itself gained the same "type an exact
    // value" ability via right-click, below. MainWindow::applyToolbarPosition
    // -- driven directly by the ToolbarPosition enum, not by QToolBar's own
    // orientation() property, which turned out not to reliably flip to
    // Qt::Vertical purely from addToolBar(area, ...) on every platform --
    // toggles gainSlider_ vs. gainUpAction_/gainDownAction_.
    gainUpAction_ = toolbar->addAction(iconChevronUp(), "Gain +3 dB");
    gainUpAction_->setToolTip("Increase gain by 3 dB");
    connect(gainUpAction_, &QAction::triggered, this,
            [this]() { setGainDb(std::clamp(app_->display.gainDb + 3.0, -6.0, 24.0)); });

    gainValueLabel_ = new QLabel(this);
    gainValueLabel_->setAlignment(Qt::AlignCenter);
    gainValueLabel_->setContentsMargins(0, 0, 0, 0);
    gainValueLabel_->setStyleSheet("color: #d4d4d8; font-size: 10px;");
    // Its fixed width (matching a toolbar icon button's actual rendered
    // width, so it lines up flush instead of leaving dead space beside it)
    // is set later, in applyToolbarPosition() -- querying any button's
    // sizeHint() this early, right after adding it, runs before the
    // toolbar's layout has settled and undersizes it.
    // The up/down arrows only step in fixed 3 dB increments -- right-click
    // the value between them for an exact, arbitrary value instead.
    gainValueLabel_->setToolTip("Right-click to enter an exact gain value");
    gainValueLabel_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(gainValueLabel_, &QWidget::customContextMenuRequested, this, [this](const QPoint&) {
        bool ok = false;
        double value = QInputDialog::getDouble(this, "Set Gain", "Gain (dB):", app_->display.gainDb, -6.0, 24.0, 1,
                                                &ok);
        if (ok) setGainDb(value);
    });
    toolbar->addWidget(gainValueLabel_);

    gainDownAction_ = toolbar->addAction(iconChevronDown(), "Gain -3 dB");
    gainDownAction_->setToolTip("Decrease gain by 3 dB");
    connect(gainDownAction_, &QAction::triggered, this,
            [this]() { setGainDb(std::clamp(app_->display.gainDb - 3.0, -6.0, 24.0)); });

    toolbar->addSeparator();

    // Spectrum/Histogram both open real parameters dialogs (see
    // native/README.md). Lock is a real two-state toggle -- see
    // targetPanels() -- that makes split view's toolbar/menu actions
    // (tool mode, gain, seismic mode, zoom/mooz, wiggle infill, box
    // delete) apply to both panels at once instead of just the active
    // one, icon swaps lock-open/lock-closed accordingly.
    spectrumAction_ = toolbar->addAction(iconChartArea(), "Spectrum");
    spectrumAction_->setToolTip("Spectrum parameters");
    connect(spectrumAction_, &QAction::triggered, this, [this]() { openSpectrumDialog(); });

    histogramAction_ = toolbar->addAction(iconChartBar(), "Histogram");
    histogramAction_->setToolTip("Histogram parameters");
    connect(histogramAction_, &QAction::triggered, this, [this]() { openHistogramDialog(); });

    // Octave Band Display: bandpass-filters the active panel's *visible*
    // data into a row of 2^N Hz bands in a new window -- see
    // computeAndShowOctaveBands. Opens straight into a result with the
    // default 2-128 Hz range (the window's own Source/Min/Max controls
    // change it afterward), rather than a parameters dialog first.
    octaveBandAction_ = toolbar->addAction(iconColumns3(), "Octave Bands");
    octaveBandAction_->setToolTip("Octave band display of the visible data");
    connect(octaveBandAction_, &QAction::triggered, this, [this]() {
        if (!activePanel_->app.loaded) {
            flashStatusMessage("No data selected");
            return;
        }
        openOctaveBandDialog();
        octaveBandDialog_->setSourcePanel(activePanel_ == &panelA_ ? 0 : 1);
        computeAndShowOctaveBands(activePanel_, 2, 128);
    });

    // Text/Binary header viewers: no settings to keep sticky, so these just
    // re-populate from whichever panel is active on every click, unlike
    // Histogram/Spectrum/Display Parameters above.
    ebcdicHeaderAction_ = toolbar->addAction(iconFileDescription(), "Text Header");
    ebcdicHeaderAction_->setToolTip("EBCDIC text header (80x40, fixed)");
    connect(ebcdicHeaderAction_, &QAction::triggered, this, [this]() {
        if (!app_->loaded) return;
        if (!ebcdicHeaderWidget_) ebcdicHeaderWidget_ = new EbcdicHeaderWidget();
        std::string text = segy::decodeEbcdicText(app_->foreground->file.data());
        // NOT fromStdString/fromUtf8: decodeEbcdicText's lookup table maps
        // each EBCDIC byte to a Latin-1 code point (0-255, one byte in,
        // one character out) -- treating that as UTF-8 instead corrupts
        // everything from the first byte >=128 onward (an invalid/partial
        // UTF-8 sequence), which is exactly backwards from "convert EBCDIC
        // to modern text": the bytes already *are* the target character
        // codes, they just need the matching one-byte-per-char encoding.
        ebcdicHeaderWidget_->setHeaderText(QString::fromLatin1(text.data(), int(text.size())));
        ebcdicHeaderWidget_->setWindowTitle(QString("Text Header (EBCDIC)") + datasetTitleSuffix(*app_));
        ebcdicHeaderWidget_->show();
        ebcdicHeaderWidget_->raise();
        ebcdicHeaderWidget_->activateWindow();
    });

    binaryHeaderAction_ = toolbar->addAction(iconFileDigit(), "Binary Header");
    binaryHeaderAction_->setToolTip("Binary header fields");
    connect(binaryHeaderAction_, &QAction::triggered, this, [this]() {
        if (!app_->loaded) return;
        if (!binaryHeaderWidget_) binaryHeaderWidget_ = new BinaryHeaderWidget();
        binaryHeaderWidget_->setHeader(app_->foreground->file.data() + segy::kTextHeaderSize);
        binaryHeaderWidget_->setWindowTitle(QString("Binary Header") + datasetTitleSuffix(*app_));
        binaryHeaderWidget_->show();
        binaryHeaderWidget_->raise();
        binaryHeaderWidget_->activateWindow();
    });

    // Split view: panel B starts hidden (see the splitter setup earlier in
    // this constructor); this just shows/hides it. Independent datasets,
    // independent pan/zoom/tool state -- see the Panel struct.
    splitViewAction_ = toolbar->addAction(iconLayoutColumns(), "Split View");
    splitViewAction_->setCheckable(true);
    splitViewAction_->setToolTip("Split into two panels (right-click for split options)");
    connect(splitViewAction_, &QAction::triggered, this, [this](bool checked) {
        panelB_.canvas->setVisible(checked);
        // A QSplitter remembers the hidden widget's old handle position
        // rather than defaulting to an even split when it's shown again --
        // without this, panel B could reappear pinned to a sliver of its
        // last size instead of taking half the space.
        if (checked) resetSplitToHalf();
    });

    // Right-click the Split View button for the options that don't need
    // their own permanent toolbar button: force an even 50/50 split (handy
    // after dragging it lopsided), or flip between panels side by side
    // (the default) and panels stacked top/bottom.
    if (QWidget* splitButton = toolbar_->widgetForAction(splitViewAction_)) {
        splitButton->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(splitButton, &QWidget::customContextMenuRequested, this, [this, splitButton](const QPoint& pos) {
            QMenu menu(this);
            QAction* set50 = menu.addAction("Set Split 50%");
            QAction* toggleOrientation =
                menu.addAction(splitter_->orientation() == Qt::Horizontal ? "Vertical Split" : "Horizontal Split");
            QAction* chosen = menu.exec(splitButton->mapToGlobal(pos));
            if (chosen == set50) {
                resetSplitToHalf();
            } else if (chosen == toggleOrientation) {
                splitter_->setOrientation(splitter_->orientation() == Qt::Horizontal ? Qt::Vertical
                                                                                      : Qt::Horizontal);
                resetSplitToHalf();
                // Depict the actual divider orientation -- a vertical
                // (top/bottom) split shows a horizontal-divider icon, not
                // the vertical-divider default.
                splitViewAction_->setIcon(splitter_->orientation() == Qt::Horizontal ? iconLayoutColumns()
                                                                                      : iconLayoutRows());
            }
        });
    }

    lockAction_ = toolbar->addAction(iconLock(false), "Lock");
    lockAction_->setCheckable(true);
    lockAction_->setToolTip("Lock: apply toolbar/menu actions to both panels at once");
    connect(lockAction_, &QAction::triggered, this, [this](bool checked) {
        lockAction_->setIcon(iconLock(checked));
    });

    // Pushes everything after it to the toolbar's far end (right when
    // horizontal, bottom when vertical -- Qt lays a QToolBar's widgets out
    // along whichever axis matches its current orientation) -- just the
    // sidebar toggle, so it reads as a standalone "view" control rather
    // than grouped with the tool buttons to its left.
    QWidget* toolbarSpacer = new QWidget(this);
    toolbarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    toolbarSpacerAction_ = toolbar->addWidget(toolbarSpacer);

    // Right dock visibility -- the dock has no close button of its own
    // (NoDockWidgetFeatures, set below), so this is the only way to hide
    // or restore it.
    sidebarToggleAction_ = toolbar->addAction(iconSidebarCollapse(), "Sidebar");
    sidebarToggleAction_->setCheckable(true);
    sidebarToggleAction_->setChecked(true);
    sidebarToggleAction_->setToolTip("Show/hide the sidebar");

    // Toolbar overflow (see native/README.md) -- every plain icon button
    // above is a candidate; the gain slider/value-label/spacer are
    // deliberately excluded (the slider's only ever visible horizontally
    // to begin with, and a QWidgetAction's widget can't be safely shown
    // in both the toolbar and a menu at once -- it would just move).
    toolbarOverflowCandidates_ = {
        zoomAction_,          moozAction_,         panAction_,         boxSelectAction_,
        seismicModeToolbarAction_, flipHorizontalAction_, identsAction_,  gainDetailAction_, gainUpAction_,
        gainDownAction_,      spectrumAction_,     histogramAction_,   octaveBandAction_,
        ebcdicHeaderAction_,  binaryHeaderAction_, splitViewAction_,   lockAction_,
        sidebarToggleAction_,
    };
    // A QToolButton parented to toolbar_ but deliberately NOT added via
    // toolbar->addWidget() -- it's positioned manually in
    // updateToolbarOverflow() instead, fused to the bottom/right edge of
    // whatever's currently visible (the "Option A" mockup the user
    // picked, chevron + a count badge -- see its own member comment for
    // why it isn't managed by QToolBarLayout like every other toolbar
    // widget here).
    toolbarMoreButton_ = new QToolButton(toolbar);
    toolbarMoreButton_->setIcon(renderOverflowIcon(0));
    toolbarMoreButton_->setToolTip("More toolbar options");
    toolbarMoreButton_->setAutoRaise(true);
    toolbarMoreButton_->setVisible(false);
    toolbarMoreMenu_ = new QMenu(this);
    connect(toolbarMoreButton_, &QToolButton::clicked, this, [this]() {
        toolbarMoreMenu_->clear();
        for (QAction* action : toolbarOverflowCandidates_) {
            if (action->isVisible()) continue; // only the ones overflow actually hid
            // A fresh proxy per item, not the real action reused directly
            // -- see the member comment above for why, and rebuilt every
            // time rather than kept in sync live, since the menu is only
            // ever open for a moment.
            QAction* proxy = toolbarMoreMenu_->addAction(action->icon(), action->text());
            proxy->setCheckable(action->isCheckable());
            proxy->setChecked(action->isChecked());
            connect(proxy, &QAction::triggered, action, &QAction::trigger);
        }
        toolbarMoreMenu_->exec(toolbarMoreButton_->mapToGlobal(QPoint(0, toolbarMoreButton_->height())));
    });
    // The overflow check needs every candidate's real widget laid out at
    // least once, which only exists once the toolbar has actually been
    // shown -- same reasoning as gainValueLabel_'s width computation a
    // little further down.
    QTimer::singleShot(0, this, [this]() { updateToolbarOverflow(); });

    // Right dock: relocated color bar (Amplitude Scale) and hover readout
    // (Trace Info), plus a QC Notes placeholder -- see native/README.md
    // for why these moved off the canvas/status bar for the "Dark Pro" UI.
    dock_ = new QDockWidget("", this);
    dock_->setFeatures(QDockWidget::NoDockWidgetFeatures); // not closable/floatable/movable
    dock_->setTitleBarWidget(new QWidget(dock_));          // hides the (otherwise empty) title bar
    QWidget* dockContent = new QWidget(dock_);
    QVBoxLayout* dockLayout = new QVBoxLayout(dockContent);
    dockLayout->setContentsMargins(16, 16, 16, 16);
    dockLayout->setSpacing(20);

    auto sectionLabel = [](const QString& text) {
        QLabel* label = new QLabel(text);
        label->setStyleSheet("color: #7a7c82; font-size: 11px; letter-spacing: 1px; font-weight: 600;");
        return label;
    };
    auto valueRow = [&](const QString& caption, QLabel*& valueOut) {
        QWidget* row = new QWidget;
        QHBoxLayout* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        QLabel* captionLabel = new QLabel(caption);
        captionLabel->setStyleSheet("color: #9a9ca3;");
        valueOut = new QLabel("--");
        valueOut->setStyleSheet("color: #d4d4d8; font-family: monospace;");
        rowLayout->addWidget(captionLabel);
        rowLayout->addStretch();
        rowLayout->addWidget(valueOut);
        return row;
    };

    dockLayout->addWidget(sectionLabel("TRACE INFO"));
    dockLayout->addWidget(valueRow("Trace", traceValueLabel_));
    dockLayout->addWidget(valueRow("Seq #", seqValueLabel_));
    dockLayout->addWidget(valueRow("Time", timeValueLabel_));
    dockLayout->addWidget(valueRow("Amplitude", amplitudeValueLabel_));

    // Amplitude Scale used to be a section here; it's now an overlay
    // legend on the canvas itself (see AmplitudeScaleWidget / setAmplitudeScale).

    dockLayout->addWidget(sectionLabel("QC NOTES"));
    QLabel* qcPlaceholder = new QLabel("No issues flagged on this line yet.");
    qcPlaceholder->setWordWrap(true);
    qcPlaceholder->setStyleSheet("color: #68696e; font-style: italic;");
    dockLayout->addWidget(qcPlaceholder);

    dockLayout->addStretch();
    dock_->setWidget(dockContent);
    dock_->setFixedWidth(260);
    addDockWidget(Qt::RightDockWidgetArea, dock_);
    connect(sidebarToggleAction_, &QAction::triggered, dock_, &QDockWidget::setVisible);

    // Status bar: native, docked at the bottom by QMainWindow. Deleting the
    // active selection box is a middle-click on the canvas itself (see
    // SegyCanvas::handleBoxSelectPress) rather than a status-bar button --
    // both panels share the one status bar and dock -- whichever panel
    // most recently changed/hovered "wins" the shared display, same
    // reasoning as the single QMainWindow status bar/dock predating split
    // view at all.
    for (Panel* p : {&panelA_, &panelB_}) {
        p->canvas->setStatusWidgets(statusBar(), [this]() { refreshToolChrome(); });
        p->canvas->setHoverInfoCallback([this](const HoverInfo& info) { updateHoverInfo(info); });
        p->canvas->setDatasetLoadedCallback([this](std::shared_ptr<Dataset> dataset) {
            registerDataset(std::move(dataset));
        });
        p->canvas->setViewChangedCallback([this, p]() { syncLockedView(p); });
    }
    // "Foreground 1"/"Background 1" (panelA_) vs "...2" (panelB_) -- see
    // native/README.md, "Dataset pool: Foreground/Background."
    panelA_.canvas->setDatasetSlotLabels("Foreground 1", "Background 1");
    panelB_.canvas->setDatasetSlotLabels("Foreground 2", "Background 2");
    panelA_.canvas->setDatasetSlotPickCallback([this](bool isForeground) { openDatasetPicker(&panelA_, isForeground); });
    panelB_.canvas->setDatasetSlotPickCallback([this](bool isForeground) { openDatasetPicker(&panelB_, isForeground); });

    // Background-task progress (Calculator/Bandpass/Save SEG-Y/Octave
    // Bands) -- see runBackgroundTask/native/README.md. Hidden until a
    // task actually starts.
    backgroundProgressBar_ = new QProgressBar();
    backgroundProgressBar_->setFixedWidth(160);
    backgroundProgressBar_->setTextVisible(false);
    backgroundProgressBar_->setVisible(false);
    statusBar()->addPermanentWidget(backgroundProgressBar_);
    backgroundTaskTimer_ = new QTimer(this);
    backgroundTaskTimer_->setInterval(33); // matches startLoading's own progress-post throttle
    connect(backgroundTaskTimer_, &QTimer::timeout, this, [this]() { updateBackgroundTaskStatusBar(); });

    refreshSeismicModeToolbarIcon();
    setGainDb(0);

    loadPreferences();
    applyToolbarPosition(prefs_.toolbarPosition); // also applies prefs_.drawerPosition -- see its own comment
}

// $HOME/.segyread_settings, an explicit path rather than QSettings'
// platform-default location (registry on Windows, ~/.config/<org>/<app>.conf
// on Linux) -- the user asked for this specific, visible-on-disk file.
namespace {
QString preferencesFilePath() { return QDir::homePath() + "/.segyread_settings"; }
} // namespace

void MainWindow::loadPreferences() {
    QSettings settings(preferencesFilePath(), QSettings::IniFormat);
    prefs_.toolbarPosition =
        ToolbarPosition(settings.value("toolbarPosition", int(ToolbarPosition::Left)).toInt());
    prefs_.drawerPosition = DrawerPosition(settings.value("drawerPosition", int(DrawerPosition::Right)).toInt());
    prefs_.uiScalePercent = settings.value("uiScalePercent", 100.0).toDouble();
}

void MainWindow::savePreferences() {
    QSettings settings(preferencesFilePath(), QSettings::IniFormat);
    settings.setValue("toolbarPosition", int(prefs_.toolbarPosition));
    settings.setValue("drawerPosition", int(prefs_.drawerPosition));
    settings.setValue("uiScalePercent", prefs_.uiScalePercent);
}

void MainWindow::applyToolbarPosition(ToolbarPosition position) {
    Qt::ToolBarArea area = Qt::TopToolBarArea;
    switch (position) {
        case ToolbarPosition::Left: area = Qt::LeftToolBarArea; break;
        case ToolbarPosition::Top: area = Qt::TopToolBarArea; break;
        case ToolbarPosition::Bottom: area = Qt::BottomToolBarArea; break;
        case ToolbarPosition::Right: area = Qt::RightToolBarArea; break;
    }
    // A toolbar already added to the window just moves when re-added to a
    // different area -- no need to remove it first.
    addToolBar(area, toolbar_);
    // Swap the Gain controls for whichever pair fits this position -- driven
    // directly by the enum rather than QToolBar::orientation()/
    // orientationChanged, which don't reliably flip to Qt::Vertical purely
    // from this addToolBar(area, ...) call (observed: both pairs staying
    // visible at once on Windows).
    bool vertical = position == ToolbarPosition::Left || position == ToolbarPosition::Right;
    // Toggling the *action*'s visibility, not just the embedded widget's --
    // QToolBar re-shows a widget added via addWidget() on its next layout
    // pass as long as the QWidgetAction wrapping it is still visible, which
    // silently undid a plain gainSlider_->setVisible(false) here (the
    // slider kept reappearing after addToolBar() above triggered exactly
    // that relayout).
    gainSliderAction_->setVisible(!vertical);
    gainUpAction_->setVisible(vertical);
    gainDownAction_->setVisible(vertical);
    // gainValueLabel_ (the dB readout) is the one gain control shown in
    // both orientations -- see its construction, above. Its fixed width is
    // (re)computed here, not at construction time, against
    // sidebarToggleAction_'s actual rendered width -- every toolbar icon
    // button is meant to be the same width, but querying one's sizeHint()
    // immediately after adding it (as this used to do, against
    // gainUpAction_) ran before the toolbar's layout had settled and
    // undersized the label; by the time applyToolbarPosition() runs, every
    // action in the toolbar exists and has been laid out at least once.
    if (QWidget* sidebarButton = toolbar_->widgetForAction(sidebarToggleAction_)) {
        gainValueLabel_->setFixedWidth(sidebarButton->sizeHint().width());
    }
    // Left/Right forces the drawer to the other side, overriding whatever
    // was stored (same rule PreferencesDialog::updateDrawerControlState
    // enforces in the UI); Top/Bottom leaves prefs_.drawerPosition as
    // whatever the user independently chose. Updating prefs_ itself here
    // (not just the live dock placement below) means a later
    // savePreferences() can't persist a stale/inconsistent combination.
    if (position == ToolbarPosition::Left) {
        prefs_.drawerPosition = DrawerPosition::Right;
    } else if (position == ToolbarPosition::Right) {
        prefs_.drawerPosition = DrawerPosition::Left;
    }
    applyDrawerPosition(prefs_.drawerPosition);
    updateToolbarOverflow();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    // Deferred, not called directly here: toolbar_->height() queried
    // synchronously inside MainWindow's own resizeEvent can still reflect
    // the *previous* layout pass (confirmed by screenshot-testing this --
    // shrinking the window well past the point everything should have
    // overflowed left every button visible, with no "More" button at
    // all, because "available" was being measured before the toolbar's
    // own geometry had actually caught up). Queuing it instead runs
    // after the current layout pass has fully settled.
    QTimer::singleShot(0, this, [this]() { updateToolbarOverflow(); });
}

void MainWindow::updateToolbarOverflow() {
    if (toolbarOverflowCandidates_.empty() || !toolbarMoreButton_) return;
    // toolbar_->orientation() is not reliable right after addToolBar()
    // (see applyToolbarPosition()'s own comment on this same quirk) --
    // the ToolbarPosition enum this app already tracks is the source of
    // truth instead, same as that function uses.
    bool vertical = prefs_.toolbarPosition == ToolbarPosition::Left || prefs_.toolbarPosition == ToolbarPosition::Right;

    auto setActionVisible = [this](QAction* action, bool v) {
        // QAction::setVisible() alone left the underlying QToolButton's
        // own visibility lagging behind the action's (confirmed by
        // logging both: the action reported visible=true while its
        // widget still reported visible=false) -- setting the widget
        // directly too forces the actual rendering to match immediately.
        action->setVisible(v);
        if (QWidget* w = toolbar_->widgetForAction(action)) w->setVisible(v);
    };

    // Rather than predicting from sizeHint() sums whether N candidates
    // fit (tried first -- repeatedly wrong by a margin that tracked
    // inter-item spacing/separators QToolBarLayout adds but sizeHint()
    // doesn't report, confirmed via geometry-level debug logging: Qt
    // marked a button "visible" with a plausible-looking cached geometry
    // that put its bottom edge past the toolbar's own real height, and
    // simply never painted it -- no chevron, no error), show everything
    // and let Qt actually lay it out once, then read back each
    // candidate's REAL resulting position and hide whichever ones
    // overflow the toolbar's own rect. Positions are monotonic along the
    // toolbar's axis, so the first overflowing candidate and everything
    // after it all get hidden together.
    // The expanding spacer (see its own construction comment) must stay
    // hidden through this measurement pass, not just whenever overflow
    // is already known -- confirmed by geometry-level debug logging: with
    // it shown here, its Expanding size policy claimed all of the
    // toolbar's unused height during measurement (there being plenty,
    // since every candidate is visible and the toolbar is genuinely
    // tall), inflating the sidebar-toggle button's *measured* Y position
    // (pushed to the spacer's far end, exactly its intended cosmetic
    // effect when truly idle) well past the real budget, so it measured
    // as "doesn't fit" even on a window tall enough to fit every
    // candidate comfortably without the spacer in the way.
    for (QAction* action : toolbarOverflowCandidates_) setActionVisible(action, true);
    if (toolbarSpacerAction_) setActionVisible(toolbarSpacerAction_, false);
    if (toolbar_->layout()) toolbar_->layout()->activate();

    int limit = vertical ? toolbar_->height() : toolbar_->width();
    // zoomAction_ is always the very first candidate and never hidden, so
    // its widget's geometry is always real/laid-out -- used as a
    // same-style reference size/offset for the manually-positioned More
    // button below, rather than guessing fixed pixel values.
    QWidget* refWidget = toolbar_->widgetForAction(zoomAction_);
    QRect refGeom = refWidget ? refWidget->geometry() : QRect(4, 4, 45, 37);
    int moreLength = vertical ? refGeom.height() : refGeom.width();
    int budget = std::max(0, limit - moreLength);

    bool anyHidden = false;
    int hiddenCount = 0;
    for (QAction* action : toolbarOverflowCandidates_) {
        QWidget* w = toolbar_->widgetForAction(action);
        if (!w) continue;
        QRect g = w->geometry();
        bool fits = !anyHidden && (vertical ? g.bottom() <= budget : g.right() <= budget);
        if (!fits) {
            anyHidden = true;
            ++hiddenCount;
        }
        setActionVisible(action, fits);
    }
    // Only re-shown once it's known nothing overflowed -- see the
    // measurement-phase comment above for why it has to stay hidden
    // through the loop itself, not just whenever overflow was already
    // known from a previous call. Re-activating afterward resolves its
    // (cosmetic, intended) push to the toolbar's far end synchronously,
    // rather than leaving it for Qt's own lazy relayout to apply on some
    // later, uncontrolled paint pass -- confirmed by screenshot-testing
    // this: without it, the sidebar-toggle button it pushes landed
    // somewhere past the toolbar's visible area instead of at its
    // intended resting place right at the bottom.
    if (toolbarSpacerAction_) setActionVisible(toolbarSpacerAction_, !anyHidden);
    if (toolbar_->layout()) toolbar_->layout()->activate();

    toolbarMoreButton_->setVisible(anyHidden);
    toolbarMoreButton_->setIcon(renderOverflowIcon(hiddenCount));
    if (anyHidden) {
        // Manually positioned (not toolbar->addWidget()'d into
        // QToolBarLayout) -- see its own member comment for why: the
        // layout's own last one or two children reliably come back with
        // stale, never-laid-out geometry, independent of overflow/space,
        // confirmed by logging every candidate's geometry alongside
        // this button's (Lock/Sidebar -- always the last two candidates
        // in toolbarOverflowCandidates_ -- showed the exact same stale
        // (0,0,100,30) rect whenever they were hidden, i.e. whenever
        // *they* were the layout's actual last children; it had nothing
        // to do with available space). Floating on top of the toolbar
        // instead sidesteps that entirely, fused to the bottom/right
        // edge of whatever's currently visible -- same technique this
        // file already uses for DatasetSlotWidget/AmplitudeScaleWidget.
        int crossOffset = vertical ? refGeom.x() : refGeom.y();
        int crossSize = vertical ? refGeom.width() : refGeom.height();
        if (vertical) {
            toolbarMoreButton_->setGeometry(crossOffset, budget, crossSize, moreLength);
        } else {
            toolbarMoreButton_->setGeometry(budget, crossOffset, moreLength, crossSize);
        }
        toolbarMoreButton_->raise();
    }
}

void MainWindow::applyDrawerPosition(DrawerPosition position) {
    addDockWidget(position == DrawerPosition::Left ? Qt::LeftDockWidgetArea : Qt::RightDockWidgetArea, dock_);
}

void MainWindow::resetSplitToHalf() {
    int total = splitter_->orientation() == Qt::Horizontal ? splitter_->width() : splitter_->height();
    splitter_->setSizes({total / 2, total - total / 2});
}

void MainWindow::flashStatusMessage(const QString& text) {
    // QStatusBar's message label renders showMessage()'s argument as plain
    // text, not rich text (an inline-styled <span> just showed up literally
    // as text) -- so the color override has to go on the status bar's own
    // QSS instead, which does take precedence over the app-wide
    // "QStatusBar { color: ... }" rule for as long as it's set.
    statusBar()->setStyleSheet("QStatusBar { color: #e8905a; }");
    statusBar()->showMessage(text, 2000);
    QTimer::singleShot(2000, this, [this]() {
        statusBar()->setStyleSheet(QString());
        if (canvas_) canvas_->refreshStatusBar();
    });
}

std::vector<Panel*> MainWindow::targetPanels() {
    if (lockAction_->isChecked()) return {&panelA_, &panelB_};
    return {activePanel_};
}

void MainWindow::syncLockedView(Panel* source) {
    if (!lockAction_->isChecked()) return;
    Panel* other = source == &panelA_ ? &panelB_ : &panelA_;
    if (!other->app.loaded) return; // nothing sensible to sync into
    other->app.view = source->app.view;
    // The two panels' datasets can have different trace/sample extents --
    // a view valid for `source` isn't necessarily valid for `other`.
    segy::clampView(other->app.view, other->app.foreground->traceCount,
                     other->app.foreground->binHeader.samplesPerTrace);
    other->canvas->refreshStatusBar();
    other->canvas->update();
}

void MainWindow::setActivePanel(Panel* panel) {
    activePanel_ = panel;
    app_ = &panel->app;
    canvas_ = panel->canvas;
    // Re-sync every toolbar/menu control to reflect the newly active
    // panel's own state -- setChecked()/setValue() don't emit the
    // triggered signals these are normally driven by (only user
    // interaction does), so this can't loop back into itself.
    panAction_->setChecked(app_->tool == ToolMode::Pan);
    boxSelectAction_->setChecked(app_->tool == ToolMode::BoxSelect);
    gainSlider_->blockSignals(true);
    gainSlider_->setValue(int(std::lround(app_->display.gainDb)));
    gainSlider_->blockSignals(false);
    gainValueLabel_->setText(formatGainDb(app_->display.gainDb));
    bool isWiggle = app_->display.seismicMode == segy::SeismicDisplayMode::Wiggle;
    variableDensityMenuAction_->setChecked(!isWiggle);
    wiggleMenuAction_->setChecked(isWiggle);
    refreshSeismicModeToolbarIcon();
    refreshToolChrome();
}

void MainWindow::zoomToBox() {
    // Locked: zoom each target panel to its *own* active, completed box (if
    // it has one) -- not a shared box, since the two panels can hold
    // different datasets with entirely different trace/sample ranges.
    for (Panel* p : targetPanels()) {
        const SelectionBox* box = activeBox(p->app);
        if (!box || !box->complete) continue;
        p->app.zoomHistory.push_back(p->app.view);
        p->app.view.traceStart = std::min(box->traceA, box->traceB);
        p->app.view.traceEnd = std::max(box->traceA, box->traceB);
        p->app.view.sampleStart = std::min(box->sampleA, box->sampleB);
        p->app.view.sampleEnd = std::max(box->sampleA, box->sampleB);
        segy::clampView(p->app.view, p->app.foreground->traceCount, p->app.foreground->binHeader.samplesPerTrace);
    }
    refreshToolChrome();
}

void MainWindow::moozBack() {
    for (Panel* p : targetPanels()) {
        if (p->app.zoomHistory.empty()) continue;
        p->app.view = p->app.zoomHistory.back();
        p->app.zoomHistory.pop_back();
    }
    refreshToolChrome();
}

void MainWindow::refreshToolChrome() {
    const SelectionBox* box = activeBox(*app_);
    zoomAction_->setEnabled(box && box->complete);
    moozAction_->setEnabled(!app_->zoomHistory.empty());
    for (Panel* p : {&panelA_, &panelB_}) p->canvas->update();
    canvas_->refreshStatusBar();
    refreshAmplitudeScale();
    refreshWindowTitle();
}

void MainWindow::refreshWindowTitle() { setWindowTitle(QString("SEG-Y Viewer (native)") + datasetTitleSuffix(*app_)); }

void MainWindow::setSeismicMode(segy::SeismicDisplayMode mode) {
    for (Panel* p : targetPanels()) p->app.display.seismicMode = mode;
    bool isWiggle = mode == segy::SeismicDisplayMode::Wiggle;
    variableDensityMenuAction_->setChecked(!isWiggle);
    wiggleMenuAction_->setChecked(isWiggle);
    refreshSeismicModeToolbarIcon();
    refreshAmplitudeScale();
    for (Panel* p : {&panelA_, &panelB_}) p->canvas->update();
}

void MainWindow::refreshSeismicModeToolbarIcon() {
    bool isWiggle = app_->display.seismicMode == segy::SeismicDisplayMode::Wiggle;
    // Icon/tooltip always show the *other* mode -- what clicking the
    // button switches to, not the current one.
    seismicModeToolbarAction_->setIcon(isWiggle ? iconVariableDensityGlyph() : iconWiggleGlyph());
    seismicModeToolbarAction_->setToolTip(isWiggle ? "Switch to Variable Density display"
                                                     : "Switch to Wiggle display");
    // Stays live-synced while DisplayParametersDialog is open (non-modal),
    // not just when it's (re)shown.
    if (displayParametersDialog_ && displayParametersDialog_->isVisible()) {
        displayParametersDialog_->refreshFromAppState();
    }
}

void MainWindow::setGainDb(double value) {
    for (Panel* p : targetPanels()) p->app.display.gainDb = value;
    gainSlider_->blockSignals(true);
    gainSlider_->setValue(int(std::lround(value)));
    gainSlider_->blockSignals(false);
    gainValueLabel_->setText(formatGainDb(value));
    refreshAmplitudeScale();
    for (Panel* p : {&panelA_, &panelB_}) p->canvas->update();
    if (displayParametersDialog_ && displayParametersDialog_->isVisible()) {
        displayParametersDialog_->refreshFromAppState();
    }
}

void MainWindow::refreshAmplitudeScale() {
    // Mirrors chrome.cpp's effectiveShowColorBar: a color bar for a wiggle
    // plot (no colormap in use) is meaningless, so it stays hidden in that
    // mode regardless of the showColorBar setting -- same as before this
    // moved from a dock section to a canvas overlay. Refreshed for both
    // panels (not just the active one) since Lock can keep both in the
    // same display mode/gain at once, each with its own data range.
    for (Panel* p : {&panelA_, &panelB_}) {
        bool visible = p->app.loaded && p->app.display.showColorBar &&
                        p->app.display.seismicMode == segy::SeismicDisplayMode::VariableDensity;
        // Same effectiveClipPosNeg/clipForGain math as renderer.cpp -- small
        // enough to duplicate here rather than export from the shared
        // renderer for this one caller. Respects the manual clip override
        // (ClipDialog) the same way the actual render does, so the legend
        // never shows a range the seismic itself isn't using.
        float rawClip = std::max(std::fabs(p->app.foreground->pyramid.globalMin), std::fabs(p->app.foreground->pyramid.globalMax));
        float basePos = p->app.display.manualClip.enabled ? p->app.display.manualClip.posMagnitude : rawClip;
        float baseNeg = p->app.display.manualClip.enabled ? p->app.display.manualClip.negMagnitude : rawClip;
        double linearGain = std::pow(10.0, p->app.display.gainDb / 20.0);
        float posClip = float(double(basePos) / linearGain);
        float negClip = float(double(baseNeg) / linearGain);
        if (posClip <= 0.0f) posClip = 1.0f;
        if (negClip <= 0.0f) negClip = 1.0f;
        p->canvas->setAmplitudeScale(posClip, negClip, visible, p->app.display.colorScale);
    }
}

void MainWindow::updateHoverInfo(const HoverInfo& info) {
    if (!info.valid) {
        traceValueLabel_->setText("--");
        seqValueLabel_->setText("--");
        timeValueLabel_->setText("--");
        amplitudeValueLabel_->setText("--");
        return;
    }
    traceValueLabel_->setText(QString::number(info.traceIdx));
    seqValueLabel_->setText(QString::number(info.seqNum));
    timeValueLabel_->setText(QString("%1 ms").arg(info.timeMs, 0, 'f', 1));
    amplitudeValueLabel_->setText(QString::number(info.amplitude, 'f', 2));
}

void MainWindow::openHistogramDialog() {
    if (!app_->loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    if (!histogramDialog_) {
        // Captures the specific panel bound to app_ right now -- the
        // dialog's own AppState& reference can't be rebound later, so this
        // callback must keep computing against that same panel even if you
        // switch the active one afterward.
        Panel* boundPanel = activePanel_;
        histogramDialog_ = new HistogramDialog(*app_, this);
        histogramDialog_->setWindowTitle(QString("Histogram Parameters") + datasetTitleSuffix(*app_));
        histogramDialog_->setAppliedCallback([this, boundPanel]() { computeAndShowHistogram(boundPanel); });
    }
    histogramDialog_->show();
    histogramDialog_->raise();
    histogramDialog_->activateWindow();
}

void MainWindow::openSpectrumDialog() {
    if (!app_->loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    if (!spectrumDialog_) {
        Panel* boundPanel = activePanel_;
        spectrumDialog_ = new SpectrumDialog(*app_, this);
        spectrumDialog_->setWindowTitle(QString("Spectrum Parameters") + datasetTitleSuffix(*app_));
        spectrumDialog_->setAppliedCallback([this, boundPanel]() { computeAndShowSpectrum(boundPanel); });
    }
    spectrumDialog_->show();
    spectrumDialog_->raise();
    spectrumDialog_->activateWindow();
}

void MainWindow::computeAndShowHistogram(Panel* panel) {
    if (!panel->app.loaded) return;
    AppState& app = panel->app;
    AnalysisSource src = resolveAnalysisSource(app, app.histogram.useSelection);
    float rangeMin, rangeMax;
    if (app.histogram.useUserClip) {
        rangeMin = float(app.histogram.userMin);
        rangeMax = float(app.histogram.userMax);
    } else {
        rangeMin = app.foreground->pyramid.globalMin;
        rangeMax = app.foreground->pyramid.globalMax;
    }
    segy::HistogramResult result =
        segy::computeHistogram(makeRenderContextFor(app), src.traceStart, src.traceEnd, src.sampleStart,
                                src.sampleEnd, app.histogram.numBins, rangeMin, rangeMax);
    if (!histogramPlot_) {
        histogramPlot_ = new HistogramPlotWidget();
        histogramPlot_->setParametersCallback([this]() { openHistogramDialog(); });
    }
    histogramPlot_->setResult(result, src.color);
    histogramPlot_->setWindowTitle(QString("Histogram") + datasetTitleSuffix(app));
    histogramPlot_->show();
    histogramPlot_->raise();
    histogramPlot_->activateWindow();
}

void MainWindow::computeAndShowSpectrum(Panel* panel) {
    // Normalize mode/smooth-points UI is wired for real now, but always
    // normalizes to the averaged spectrum's own peak regardless of which
    // Normalize radio is picked (segy::computeSpectrum's one, fixed
    // convention) -- distinguishing "Absolute Max from Selected Curve" vs.
    // "Relative Max to Each Curve" vs. "...from User" is a real follow-up,
    // not wired to the computation yet (see native/README.md).
    if (!panel->app.loaded) return;
    AppState& app = panel->app;
    AnalysisSource src = resolveAnalysisSource(app, app.spectrum.useSelection);
    segy::SpectrumResult result =
        segy::computeSpectrum(makeRenderContextFor(app), src.traceStart, src.traceEnd, src.sampleStart,
                               src.sampleEnd, app.foreground->binHeader.sampleIntervalUs, app.spectrum.smoothPoints);
    if (!spectrumPlot_) {
        spectrumPlot_ = new SpectrumPlotWidget();
        spectrumPlot_->setParametersCallback([this]() { openSpectrumDialog(); });
    }
    spectrumPlot_->setResult(result, src.color);
    spectrumPlot_->setWindowTitle(QString("Spectrum") + datasetTitleSuffix(app));
    spectrumPlot_->show();
    spectrumPlot_->raise();
    spectrumPlot_->activateWindow();
}

void MainWindow::runCalculator(AppState& a, AppState& b, bool subtract, const QString& outputName) {
    if (!a.loaded || !b.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    // Clipped to the overlap when the two datasets differ in size, rather
    // than refusing -- combining a full line with a sub-window of it is a
    // reasonable thing to want.
    int64_t traceCount = std::min(a.foreground->traceCount, b.foreground->traceCount);
    int samplesPerTrace = std::min<int>(a.foreground->binHeader.samplesPerTrace, b.foreground->binHeader.samplesPerTrace);
    if (traceCount <= 0 || samplesPerTrace <= 0) {
        flashStatusMessage("No data selected");
        return;
    }

    // Snapshot the two source Datasets by shared_ptr *now*, on the UI
    // thread -- the background closure below reads through these, never
    // through `a`/`b` again, so it stays correct (and the underlying
    // mmap'd files/pyramids stay alive) even if the user switches a
    // panel's Foreground or deletes one of these from the pool while the
    // computation is still running.
    std::shared_ptr<Dataset> dsA = a.foreground;
    std::shared_ptr<Dataset> dsB = b.foreground;

    segy::BinaryHeader outHeader = dsA->binHeader;
    outHeader.samplesPerTrace = int16_t(samplesPerTrace);

    QString nameA = QString::fromStdString(dsA->filePath.stem().string());
    QString nameB = QString::fromStdString(dsB->filePath.stem().string());
    QStringList inputBlocks;
    inputBlocks << QString("FILE: %1  TRACES: %2  SAMPLES/TRACE: %3  SAMPLE INTERVAL: %4 US")
                       .arg(nameA)
                       .arg(dsA->traceCount)
                       .arg(dsA->binHeader.samplesPerTrace)
                       .arg(dsA->binHeader.sampleIntervalUs);
    inputBlocks << QString("FILE: %1  TRACES: %2  SAMPLES/TRACE: %3  SAMPLE INTERVAL: %4 US")
                       .arg(nameB)
                       .arg(dsB->traceCount)
                       .arg(dsB->binHeader.samplesPerTrace)
                       .arg(dsB->binHeader.sampleIntervalUs);

    // Dataset A's own history carries forward (it's the primary input); B's
    // is only named in the input block, not merged in.
    std::vector<std::string> history = dsA->processingHistory;
    history.push_back(QString("CALCULATOR: %1 %2 %3").arg(nameA, subtract ? "MINUS" : "PLUS", nameB).toStdString());

    QString textHeader = buildSegyTextHeader(inputBlocks, history, outputName, traceCount, samplesPerTrace,
                                              outHeader.sampleIntervalUs);
    std::filesystem::path outPath = scratchSegyPath(outputName);

    auto writeOk = std::make_shared<bool>(false);
    auto writeError = std::make_shared<std::string>();

    runBackgroundTask(
        QString("Calculator: %1 %2 %3").arg(nameA, subtract ? "-" : "+", nameB),
        [this, dsA, dsB, traceCount, samplesPerTrace, subtract, outHeader, textHeader, outPath, writeOk,
         writeError]() {
            backgroundTaskTotal_ = traceCount;
            std::vector<segy::WriteTrace> traces(static_cast<size_t>(traceCount));
            std::vector<float> samplesA(static_cast<size_t>(samplesPerTrace));
            std::vector<float> samplesB(static_cast<size_t>(samplesPerTrace));
            for (int64_t t = 0; t < traceCount; ++t) {
                const uint8_t* traceA = dsA->file.data() + segy::kHeaderTotalSize + size_t(t) * dsA->traceStrideBytes;
                const uint8_t* traceB = dsB->file.data() + segy::kHeaderTotalSize + size_t(t) * dsB->traceStrideBytes;
                segy::decodeSamples(traceA + segy::kTraceHeaderSize, samplesA.data(), size_t(samplesPerTrace),
                                    dsA->binHeader.formatCode);
                segy::decodeSamples(traceB + segy::kTraceHeaderSize, samplesB.data(), size_t(samplesPerTrace),
                                    dsB->binHeader.formatCode);
                segy::WriteTrace& out = traces[size_t(t)];
                out.headerBytes.assign(traceA, traceA + segy::kTraceHeaderSize); // dataset A's own trace headers carry over
                out.samples.resize(size_t(samplesPerTrace));
                for (int s = 0; s < samplesPerTrace; ++s) {
                    out.samples[size_t(s)] =
                        subtract ? samplesA[size_t(s)] - samplesB[size_t(s)] : samplesA[size_t(s)] + samplesB[size_t(s)];
                }
                backgroundTaskDone_ = t + 1;
            }
            *writeOk = segy::writeSegyFile(outPath, textHeader.toStdString(), outHeader, traces, writeError.get());
        },
        [this, writeOk, writeError, outPath, history]() {
            if (!*writeOk) {
                QMessageBox::warning(this, "Calculator", QString::fromStdString(*writeError));
                return;
            }
            panelA_.canvas->startLoading(outPath, history);
        });
}

void MainWindow::runBandpass(AppState& source, const segy::BandpassParams& params, const QString& outputName) {
    if (!source.loaded || source.foreground->traceCount <= 0 || source.foreground->binHeader.samplesPerTrace <= 0) {
        flashStatusMessage("No data selected");
        return;
    }
    // Snapshot by shared_ptr now -- see runCalculator's comment for why.
    std::shared_ptr<Dataset> ds = source.foreground;
    int64_t traceCount = ds->traceCount;
    int samplesPerTrace = ds->binHeader.samplesPerTrace;

    QString sourceName = QString::fromStdString(ds->filePath.stem().string());
    QStringList inputBlocks;
    inputBlocks << QString("FILE: %1  TRACES: %2  SAMPLES/TRACE: %3  SAMPLE INTERVAL: %4 US")
                       .arg(sourceName)
                       .arg(ds->traceCount)
                       .arg(ds->binHeader.samplesPerTrace)
                       .arg(ds->binHeader.sampleIntervalUs);
    std::vector<std::string> history = ds->processingHistory;
    history.push_back(QString("BANDPASS FILTER (HANNING TAPER) %1-%2-%3-%4 HZ")
                          .arg(params.lowCut, 0, 'g', 4)
                          .arg(params.lowPass, 0, 'g', 4)
                          .arg(params.highPass, 0, 'g', 4)
                          .arg(params.highCut, 0, 'g', 4)
                          .toStdString());

    QString textHeader = buildSegyTextHeader(inputBlocks, history, outputName, traceCount, samplesPerTrace,
                                              ds->binHeader.sampleIntervalUs);
    std::filesystem::path outPath = scratchSegyPath(outputName);
    segy::BinaryHeader outHeader = ds->binHeader;

    auto writeOk = std::make_shared<bool>(false);
    auto writeError = std::make_shared<std::string>();

    runBackgroundTask(
        QString("Bandpass: %1").arg(sourceName),
        [this, ds, traceCount, samplesPerTrace, params, outHeader, textHeader, outPath, writeOk, writeError]() {
            backgroundTaskTotal_ = traceCount;
            std::vector<segy::WriteTrace> traces(static_cast<size_t>(traceCount));
            std::vector<float> samples(static_cast<size_t>(samplesPerTrace));
            for (int64_t t = 0; t < traceCount; ++t) {
                const uint8_t* traceBase = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                segy::decodeSamples(traceBase + segy::kTraceHeaderSize, samples.data(), size_t(samplesPerTrace),
                                    ds->binHeader.formatCode);
                segy::applyBandpassFilter(samples, ds->binHeader.sampleIntervalUs, params);
                segy::WriteTrace& out = traces[size_t(t)];
                out.headerBytes.assign(traceBase, traceBase + segy::kTraceHeaderSize);
                out.samples = samples;
                backgroundTaskDone_ = t + 1;
            }
            *writeOk = segy::writeSegyFile(outPath, textHeader.toStdString(), outHeader, traces, writeError.get());
        },
        [this, writeOk, writeError, outPath, history]() {
            if (!*writeOk) {
                QMessageBox::warning(this, "Bandpass Filter", QString::fromStdString(*writeError));
                return;
            }
            panelA_.canvas->startLoading(outPath, history);
        });
}

void MainWindow::runSaveSegy(AppState& source, const QString& filePath) {
    if (!source.loaded || source.foreground->traceCount <= 0 || source.foreground->binHeader.samplesPerTrace <= 0) {
        flashStatusMessage("No data selected");
        return;
    }
    // Snapshot by shared_ptr now -- see runCalculator's comment for why.
    std::shared_ptr<Dataset> ds = source.foreground;
    int64_t traceCount = ds->traceCount;
    int samplesPerTrace = ds->binHeader.samplesPerTrace;

    std::filesystem::path outPath(filePath.toStdString());
    QString datasetName = QString::fromStdString(ds->filePath.stem().string());
    QString outputName = QString::fromStdString(outPath.stem().string());
    QStringList inputBlocks;
    inputBlocks << QString("FILE: %1  TRACES: %2  SAMPLES/TRACE: %3  SAMPLE INTERVAL: %4 US")
                       .arg(datasetName)
                       .arg(ds->traceCount)
                       .arg(ds->binHeader.samplesPerTrace)
                       .arg(ds->binHeader.sampleIntervalUs);
    QString textHeader = buildSegyTextHeader(inputBlocks, ds->processingHistory, outputName, traceCount,
                                              samplesPerTrace, ds->binHeader.sampleIntervalUs);
    segy::BinaryHeader outHeader = ds->binHeader;

    auto writeOk = std::make_shared<bool>(false);
    auto writeError = std::make_shared<std::string>();

    runBackgroundTask(
        QString("Save SEG-Y: %1").arg(QString::fromStdString(outPath.filename().string())),
        [this, ds, traceCount, samplesPerTrace, outHeader, textHeader, outPath, writeOk, writeError]() {
            backgroundTaskTotal_ = traceCount;
            std::vector<segy::WriteTrace> traces(static_cast<size_t>(traceCount));
            for (int64_t t = 0; t < traceCount; ++t) {
                const uint8_t* traceBase = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                segy::WriteTrace& out = traces[size_t(t)];
                out.headerBytes.assign(traceBase, traceBase + segy::kTraceHeaderSize);
                out.samples.resize(size_t(samplesPerTrace));
                segy::decodeSamples(traceBase + segy::kTraceHeaderSize, out.samples.data(), size_t(samplesPerTrace),
                                    ds->binHeader.formatCode);
                backgroundTaskDone_ = t + 1;
            }
            *writeOk = segy::writeSegyFile(outPath, textHeader.toStdString(), outHeader, traces, writeError.get());
        },
        [this, writeOk, writeError, outPath]() {
            if (!*writeOk) {
                QMessageBox::warning(this, "Save SEG-Y", QString::fromStdString(*writeError));
                return;
            }
            flashStatusMessage(QString("Saved %1").arg(QString::fromStdString(outPath.filename().string())));
        });
}

void MainWindow::computeAndShowOctaveBands(Panel* sourcePanel, int minFreqHz, int maxFreqHz) {
    AppState& app = sourcePanel->app;
    if (!app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    // "Visible data in that window" -- the panel's current view, not the
    // whole file, per the original ask.
    int64_t t0 = std::max<int64_t>(0, int64_t(std::floor(app.view.traceStart)));
    int64_t t1 = std::min<int64_t>(app.foreground->traceCount, int64_t(std::ceil(app.view.traceEnd)));
    int s0 = std::max(0, int(std::floor(app.view.sampleStart)));
    int s1 = std::min<int>(app.foreground->binHeader.samplesPerTrace, int(std::ceil(app.view.sampleEnd)));
    int64_t visibleTraces = t1 - t0;
    int visibleSamples = s1 - s0;
    if (visibleTraces <= 0 || visibleSamples < 2 || app.foreground->binHeader.sampleIntervalUs <= 0) return;
    if (maxFreqHz <= minFreqHz) {
        flashStatusMessage("Max frequency must be above min");
        return;
    }

    // Snapshot by shared_ptr now -- see runCalculator's comment for why.
    std::shared_ptr<Dataset> ds = app.foreground;
    segy::ColorScale colorScale = app.display.colorScale;

    // A "look" tool over a handful of evenly-spaced traces, not an
    // exhaustive filter of every visible one -- filtering every trace of a
    // huge view eight-plus times over would freeze the UI for no visible
    // benefit at this panel width.
    constexpr int kMaxTracesPerBand = 80;
    constexpr int kPanelWidth = 100;
    constexpr int kPanelHeight = 480;
    int decodeCount = int(std::min<int64_t>(visibleTraces, kMaxTracesPerBand));

    auto result = std::make_shared<OctaveBandResult>();

    runBackgroundTask(
        QString("Octave Bands: %1-%2 Hz").arg(minFreqHz).arg(maxFreqHz),
        [this, ds, t0, t1, s0, visibleSamples, decodeCount, minFreqHz, maxFreqHz, colorScale, result]() {
            int bandCount = 1;
            for (int lo = minFreqHz; lo < maxFreqHz; lo *= 2) ++bandCount;
            backgroundTaskTotal_ = bandCount;

            double step = double(t1 - t0) / double(decodeCount);
            std::vector<std::vector<float>> rawTraces(static_cast<size_t>(decodeCount));
            int sampleSize = segy::sampleFormatSizeBytes(ds->binHeader.formatCode);
            for (int i = 0; i < decodeCount; ++i) {
                int64_t traceIdx = std::min<int64_t>(t0 + int64_t((double(i) + 0.5) * step), t1 - 1);
                const uint8_t* traceBase = ds->file.data() + segy::kHeaderTotalSize + size_t(traceIdx) * ds->traceStrideBytes;
                rawTraces[size_t(i)].resize(size_t(visibleSamples));
                segy::decodeSamples(traceBase + segy::kTraceHeaderSize + size_t(s0) * size_t(sampleSize),
                                    rawTraces[size_t(i)].data(), size_t(visibleSamples), ds->binHeader.formatCode);
            }

            result->bands.push_back({"Unfiltered", rasterizeOctaveBand(rawTraces, kPanelWidth, kPanelHeight, colorScale)});
            backgroundTaskDone_ = 1;
            int bandsDone = 1;
            for (int lo = minFreqHz; lo < maxFreqHz; lo *= 2) {
                int hi = std::min(lo * 2, maxFreqHz);
                // A small taper past each edge (15% of the band's own
                // width) instead of a hard wall at exactly lo/hi, so
                // adjacent bands overlap smoothly rather than ringing --
                // same reasoning as Bandpass's Hanning taper.
                double taper = double(hi - lo) * 0.15;
                segy::BandpassParams params;
                params.lowCut = std::max(0.0, double(lo) - taper);
                params.lowPass = double(lo);
                params.highPass = double(hi);
                params.highCut = double(hi) + taper;
                std::vector<std::vector<float>> filtered = rawTraces;
                for (std::vector<float>& tr : filtered) {
                    segy::applyBandpassFilter(tr, ds->binHeader.sampleIntervalUs, params);
                }
                result->bands.push_back({QString("%1 - %2 Hz").arg(lo).arg(hi),
                                         rasterizeOctaveBand(filtered, kPanelWidth, kPanelHeight, colorScale)});
                backgroundTaskDone_ = ++bandsDone;
            }
        },
        [this, sourcePanel, result]() {
            if (!octaveBandDialog_) openOctaveBandDialog(); // creates it, wires the callback, refreshes panel list
            octaveBandDialog_->setResult(*result);
            octaveBandDialog_->setWindowTitle(QString("Octave Band Display") + datasetTitleSuffix(sourcePanel->app));
            octaveBandDialog_->show();
            octaveBandDialog_->raise();
            octaveBandDialog_->activateWindow();
        });
}

void MainWindow::openCalculatorDialog() {
    if (!calculatorDialog_) {
        calculatorDialog_ = new CalculatorDialog(this);
        calculatorDialog_->setApplyCallback([this](int idxA, int idxB, bool subtract, QString outputName) {
            Panel* a = idxA == 0 ? &panelA_ : &panelB_;
            Panel* b = idxB == 0 ? &panelA_ : &panelB_;
            runCalculator(a->app, b->app, subtract, outputName);
        });
    }
    calculatorDialog_->refreshPanels(QString::fromStdString(panelA_.app.foreground->filePath.stem().string()), panelA_.app.loaded,
                                      QString::fromStdString(panelB_.app.foreground->filePath.stem().string()), panelB_.app.loaded);
    calculatorDialog_->show();
    calculatorDialog_->raise();
    calculatorDialog_->activateWindow();
}

void MainWindow::openBandpassDialog() {
    if (!bandpassDialog_) {
        bandpassDialog_ = new BandpassDialog(this);
        bandpassDialog_->setApplyCallback([this](segy::BandpassParams params, QString outputName) {
            // Filters whichever panel is active when Apply is pressed, not
            // whichever was active when the dialog first opened -- it's a
            // non-modal dialog, so the user may have clicked the other
            // panel since.
            runBandpass(activePanel_->app, params, outputName);
        });
    }
    bandpassDialog_->refreshSource(QString::fromStdString(activePanel_->app.foreground->filePath.stem().string()),
                                    activePanel_->app.loaded);
    bandpassDialog_->show();
    bandpassDialog_->raise();
    bandpassDialog_->activateWindow();
}

void MainWindow::openSaveSegyDialog() {
    if (!activePanel_->app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    AppState& app = activePanel_->app;
    if (!saveSegyDialog_) {
        saveSegyDialog_ = new SaveSegyDialog(this);
        saveSegyDialog_->setSaveCallback([this](QString filePath) { runSaveSegy(activePanel_->app, filePath); });
    }
    QString datasetName = QString::fromStdString(app.foreground->filePath.stem().string());
    QString summary = QString("%1 -- %2 traces x %3 samples, %4 us")
                          .arg(datasetName)
                          .arg(app.foreground->traceCount)
                          .arg(app.foreground->binHeader.samplesPerTrace)
                          .arg(app.foreground->binHeader.sampleIntervalUs);
    QStringList inputBlocks;
    inputBlocks << QString("FILE: %1  TRACES: %2  SAMPLES/TRACE: %3  SAMPLE INTERVAL: %4 US")
                       .arg(datasetName)
                       .arg(app.foreground->traceCount)
                       .arg(app.foreground->binHeader.samplesPerTrace)
                       .arg(app.foreground->binHeader.sampleIntervalUs);
    QString header = buildSegyTextHeader(inputBlocks, app.foreground->processingHistory, datasetName, app.foreground->traceCount,
                                          app.foreground->binHeader.samplesPerTrace, app.foreground->binHeader.sampleIntervalUs);
    // Displayed one 80-column line per row -- the real header has no
    // newlines (fixed-width lines packed end to end), this is just for
    // reading.
    QStringList displayLines;
    for (int i = 0; i + 80 <= header.size(); i += 80) displayLines << header.mid(i, 80);
    saveSegyDialog_->setPreview(datasetName + ".sgy", summary, displayLines.join('\n'));
    saveSegyDialog_->show();
    saveSegyDialog_->raise();
    saveSegyDialog_->activateWindow();
}

void MainWindow::openOctaveBandDialog() {
    if (!octaveBandDialog_) {
        octaveBandDialog_ = new OctaveBandDialog(this);
        octaveBandDialog_->setApplyCallback([this](int sourceIdx, int minHz, int maxHz) {
            computeAndShowOctaveBands(sourceIdx == 0 ? &panelA_ : &panelB_, minHz, maxHz);
        });
    }
    octaveBandDialog_->refreshPanels(QString::fromStdString(panelA_.app.foreground->filePath.stem().string()), panelA_.app.loaded,
                                      QString::fromStdString(panelB_.app.foreground->filePath.stem().string()), panelB_.app.loaded);
}

void MainWindow::registerDataset(std::shared_ptr<Dataset> dataset) {
    // Renaming here (not in SegyCanvas::startLoading) is safe: at this
    // point nothing else holds this shared_ptr yet (it's about to become
    // this panel's Foreground, right after this callback returns), so
    // there's no one else to see the name change mid-flight.
    std::string baseName = dataset->name;
    int suffix = 2;
    while (std::any_of(datasetPool_.begin(), datasetPool_.end(),
                        [&](const std::shared_ptr<Dataset>& d) { return d->name == dataset->name; })) {
        dataset->name = baseName + " (" + std::to_string(suffix++) + ")";
    }
    datasetPool_.push_back(std::move(dataset));
    refreshAllDatasetSlotNames();
}

bool MainWindow::isDatasetInUse(const std::shared_ptr<Dataset>& dataset) const {
    for (const Panel* p : {&panelA_, &panelB_}) {
        if (p->app.foreground == dataset || p->app.background == dataset) return true;
    }
    return false;
}

void MainWindow::openDatasetPicker(Panel* panel, bool isForeground) {
    if (!datasetPickerDialog_) {
        datasetPickerDialog_ = new DatasetPickerDialog(this);
    }
    std::shared_ptr<Dataset> current = isForeground ? panel->app.foreground : panel->app.background;
    datasetPickerDialog_->setWindowTitle(isForeground ? "Select Foreground Dataset" : "Select Background Dataset");
    datasetPickerDialog_->setDatasets(datasetPool_, current, /*allowNone=*/!isForeground,
                                       [this](const std::shared_ptr<Dataset>& d) { return isDatasetInUse(d); });
    datasetPickerDialog_->setCallbacks(
        [this, panel, isForeground](std::shared_ptr<Dataset> picked) {
            if (isForeground) {
                panel->canvas->setForegroundDataset(std::move(picked));
            } else {
                panel->canvas->setBackgroundDataset(std::move(picked));
            }
            refreshAllDatasetSlotNames();
            refreshToolChrome();
        },
        [this](std::shared_ptr<Dataset> removed) {
            datasetPool_.erase(std::remove(datasetPool_.begin(), datasetPool_.end(), removed), datasetPool_.end());
        });
    datasetPickerDialog_->show();
    datasetPickerDialog_->raise();
    datasetPickerDialog_->activateWindow();
}

void MainWindow::refreshAllDatasetSlotNames() {
    panelA_.canvas->refreshDatasetSlotNames();
    panelB_.canvas->refreshDatasetSlotNames();
}

void MainWindow::runBackgroundTask(const QString& label, std::function<void()> backgroundWork,
                                    std::function<void()> uiCompletion) {
    if (backgroundTaskRunning_) {
        flashStatusMessage("Another background operation is already running");
        return;
    }
    backgroundTaskRunning_ = true;
    backgroundTaskLabel_ = label;
    backgroundTaskDone_ = 0;
    backgroundTaskTotal_ = 1;
    for (Panel* p : {&panelA_, &panelB_}) p->canvas->setStatusOverrideActive(true);
    updateBackgroundTaskStatusBar();
    backgroundTaskTimer_->start();

    std::thread([this, backgroundWork = std::move(backgroundWork), uiCompletion = std::move(uiCompletion)]() mutable {
        backgroundWork();
        QMetaObject::invokeMethod(
            this,
            [this, uiCompletion = std::move(uiCompletion)]() mutable {
                backgroundTaskTimer_->stop();
                backgroundTaskRunning_ = false;
                for (Panel* p : {&panelA_, &panelB_}) p->canvas->setStatusOverrideActive(false);
                updateBackgroundTaskStatusBar();
                uiCompletion();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::updateBackgroundTaskStatusBar() {
    if (!backgroundTaskRunning_) {
        backgroundProgressBar_->setVisible(false);
        for (Panel* p : {&panelA_, &panelB_}) p->canvas->refreshStatusBar();
        return;
    }
    int64_t done = backgroundTaskDone_.load();
    int64_t total = std::max<int64_t>(1, backgroundTaskTotal_.load());
    backgroundProgressBar_->setVisible(true);
    backgroundProgressBar_->setRange(0, int(std::min<int64_t>(total, 1'000'000)));
    backgroundProgressBar_->setValue(int(std::min<int64_t>(done, total) * backgroundProgressBar_->maximum() / total));
    statusBar()->showMessage(QString("%1 (%2 / %3)").arg(backgroundTaskLabel_).arg(done).arg(total));
}

void MainWindow::openInitialFile(const std::filesystem::path& path) { canvas_->startLoading(path); }

void MainWindow::openFile() {
    // Deferred one event-loop turn rather than opened directly here: this
    // slot runs synchronously inside the "Open..." QAction's triggered
    // signal, i.e. while the menu that was just clicked is still in the
    // middle of closing. Constructing and exec()'ing a brand-new modal
    // dialog in that same call stack raced the menu's own closing X11
    // events under (at least) WSLg's xcb compositor -- confirmed
    // reproducing as exactly this: the dialog appeared mapped but
    // unpainted, and stayed that way until some later window-manager event
    // (moving the main window) forced a redraw. QTimer::singleShot(0, ...)
    // is the standard, imperceptible-delay fix for this class of bug.
    QTimer::singleShot(0, this, [this]() {
        SegyOpenDialog dialog(this);
        if (dialog.exec() != QDialog::Accepted) return;
        std::filesystem::path path = dialog.selectedPath();
        if (path.empty()) return;
        // Loading itself never touches gainDb (see startLoading()), so gain
        // already persists across files by default -- unless the user left
        // "Use clipping from previously loaded window" unchecked, in which
        // case this resets it before the new file's own render picks it up.
        if (!dialog.keepPreviousClip()) setGainDb(0.0);
        canvas_->startLoading(path);
    });
}

} // namespace segyqt

namespace {

// "Dark Pro" app-wide QSS, applied once in main() -- the seismic canvas
// itself paints its own pixels directly and is unaffected by this.
QString darkStyleSheet() {
    return QStringLiteral(R"(
        QMainWindow, QDialog { background: #1e1f22; }
        QMenuBar { background: #2b2d30; color: #d4d4d8; border-bottom: 1px solid #17181a; }
        QMenuBar::item { background: transparent; padding: 4px 10px; }
        QMenuBar::item:selected { background: #3c3f41; }
        QMenu { background: #2b2d30; color: #d4d4d8; border: 1px solid #17181a; }
        QMenu::item { padding: 4px 20px; }
        QMenu::item:selected { background: #3c3f41; }
        QToolBar { background: #2b2d30; border-bottom: 1px solid #17181a; spacing: 4px; padding: 4px; }
        QToolBar QLabel { color: #9a9ca3; }
        QToolButton { background: transparent; border-radius: 6px; padding: 6px 10px; color: #d4d4d8; border: 1px solid transparent; }
        QToolButton:hover { background: #3c3f41; }
        QToolButton:checked { background: #4a3626; color: #e8905a; border: 1px solid #e8905a; }
        QStatusBar { background: #17181a; color: #c7c9cc; }
        QStatusBar::item { border: none; }
        QDockWidget { background: #2b2d30; color: #d4d4d8; }
        QLabel { color: #d4d4d8; }
        QPushButton { background: #3c3f41; color: #d4d4d8; border: 1px solid #4a4c50; border-radius: 4px; padding: 4px 12px; }
        QPushButton:hover { background: #4a4c50; }
        QSlider::groove:horizontal { background: #4a4c50; height: 4px; border-radius: 2px; }
        QSlider::handle:horizontal { background: #e8905a; width: 14px; margin: -6px 0; border-radius: 7px; }
        QSlider::handle:horizontal:hover { background: #f0a06c; }
        QSpinBox, QDoubleSpinBox { background: #17181a; color: #d4d4d8; border: 1px solid #3c3f41; border-radius: 4px; padding: 2px 4px; }
        QSpinBox:disabled, QDoubleSpinBox:disabled { color: #68696e; }
        QGroupBox { color: #d4d4d8; border: 1px solid #3c3f41; border-radius: 4px; margin-top: 10px; padding-top: 6px; }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #9a9ca3; }
        QRadioButton, QCheckBox { color: #d4d4d8; spacing: 6px; }
        QRadioButton:disabled, QCheckBox:disabled { color: #68696e; }
        QRadioButton::indicator, QCheckBox::indicator { width: 14px; height: 14px; }
        QRadioButton::indicator { border-radius: 7px; border: 1px solid #6b6d72; background: #17181a; }
        QRadioButton::indicator:checked { background: #e8905a; border: 1px solid #e8905a; }
        QRadioButton::indicator:disabled { border: 1px solid #3c3f41; }
        QCheckBox::indicator { border-radius: 3px; border: 1px solid #6b6d72; background: #17181a; }
        QCheckBox::indicator:checked { background: #e8905a; border: 1px solid #e8905a; }
        QCheckBox::indicator:disabled { border: 1px solid #3c3f41; }
        QComboBox { background: #17181a; color: #d4d4d8; border: 1px solid #3c3f41; border-radius: 4px; padding: 2px 8px; }
        QComboBox QAbstractItemView { background: #201f23; color: #d4d4d8; selection-background-color: #4a3626; selection-color: #e8905a; }
        QTableWidget { background: #17181a; color: #d4d4d8; border: 1px solid #3c3f41; gridline-color: #3c3f41; }
        QTableWidget::item { padding: 4px 6px; }
        QHeaderView::section { background: #2b2d30; color: #9a9ca3; border: none; border-bottom: 1px solid #3c3f41; border-right: 1px solid #3c3f41; padding: 4px 6px; }
    )");
}

} // namespace

int main(int argc, char** argv) {
    // WSLg defaults Qt to its native-Wayland platform plugin (WAYLAND_DISPLAY
    // is set), and that path has confirmed bugs for this app under WSLg
    // specifically: window transparency (background bleeding through
    // unpainted regions) and wheel/scroll events being delivered to whatever
    // window is behind ours instead of to us. Xwayland (QT_QPA_PLATFORM=xcb)
    // doesn't have either problem. Forcing xcb only under WSL (never on a
    // real native-Wayland Linux desktop, and never overriding an explicit
    // QT_QPA_PLATFORM the user already set) is a targeted workaround for a
    // known environment limitation, not a general anti-Wayland stance.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM") && qEnvironmentVariableIsSet("WSL_DISTRO_NAME")) {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
    // Qt6's default high-DPI rounding policy snaps a fractional monitor
    // scale factor (e.g. Windows' 150%) to the nearest *integer* factor
    // before applying it to fonts/layout -- at 150% that rounds away from
    // 1.5 entirely, which is why menu/toolbar text was appearing at 100%
    // scale despite the display being set to 150%. PassThrough uses the
    // real factor as reported by the OS instead. Must be set before
    // QApplication is constructed. Applies equally on Linux (Ubuntu's
    // fractional-scaling support reports the same kind of factor via
    // xcb/XSETTINGS), not just Windows.
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Qt5 leaves high-DPI scaling off unless asked; Qt6 always has it on.
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#ifndef Q_OS_WIN
    // Preferences' UI Scaling override (Linux only -- Windows already scales
    // itself, see PreferencesDialog) has to be applied via QT_SCALE_FACTOR,
    // which Qt only reads while constructing QApplication -- too late to
    // apply from MainWindow once the preference is loaded normally, so read
    // the same settings file directly, here, before that construction.
    {
        QSettings settings(QDir::homePath() + "/.segyread_settings", QSettings::IniFormat);
        double uiScalePercent = settings.value("uiScalePercent", 100.0).toDouble();
        if (uiScalePercent > 0.0 && uiScalePercent != 100.0) {
            qputenv("QT_SCALE_FACTOR", QByteArray::number(uiScalePercent / 100.0));
        }
    }
#endif
    QApplication app(argc, argv);
    // Window/taskbar icon on both platforms -- compiled in via app_icon.qrc
    // (native/assets/icons/LEESMIJ.md), so this doesn't depend on the built
    // executable's location at runtime. The Windows exe's own icon
    // (Explorer, taskbar before the window paints) is separate, embedded by
    // app.rc.
    app.setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));
    app.setStyleSheet(darkStyleSheet());
    segyqt::MainWindow window;
    window.show();
    if (argc > 1) window.openInitialFile(std::filesystem::path(argv[1]));
    return app.exec();
}
