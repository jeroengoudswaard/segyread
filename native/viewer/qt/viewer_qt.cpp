#include "viewer_qt.h"
#include "theme.h"

#include "axis_ticks.h"
#include "segy_decode.h"
#include "viewer_qt_icons.h"
#include "dark_titlebar.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDate>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFontMetrics>
#include <QGridLayout>
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
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTableWidget>
#include <QSignalBlocker>
#include <QFile>
#include <QTextStream>
#include <QColorDialog>
#include <map>
#include <complex>
#include "fft.h"
#include "depth_conversion.h"
#include <atomic>
#include <QRegularExpression>
#include <QToolButton>
#include <QWidgetAction>
#include <QTimer>
#include <QtGlobal>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <QElapsedTimer>
#include <QDesktopServices>
#include <QProcess>
#include <QUrl>
#include <QUrlQuery>
#include <QSysInfo>

namespace segyqt {

namespace {
QFile* perfFile() {
    static QFile* file = []() -> QFile* {
        if (!qEnvironmentVariableIsSet("SEGYREAD_PROFILE")) return nullptr;
        auto* f = new QFile(qEnvironmentVariable("SEGYREAD_PROFILE"));
        if (!f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            delete f;
            return nullptr;
        }
        return f;
    }();
    return file;
}

std::mutex& perfMutex() {
    static std::mutex m;
    return m;
}

qint64 perfClockMs() {
    static QElapsedTimer clock = []() {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

// Logs `what` when the scope took at least `thresholdMs`.
struct PerfScope {
    QString what;
    qint64 thresholdMs;
    QElapsedTimer timer;
    PerfScope(QString w, qint64 threshold) : what(std::move(w)), thresholdMs(threshold) {
        if (perfEnabled()) timer.start();
    }
    ~PerfScope() {
        if (timer.isValid() && timer.elapsed() >= thresholdMs) perfLog(QString("%1: %2 ms").arg(what).arg(timer.elapsed()));
    }
};
} // namespace

bool perfEnabled() { return perfFile() != nullptr; }

void perfLog(const QString& line) {
    QFile* f = perfFile();
    if (!f) return;
    std::lock_guard<std::mutex> lock(perfMutex());
    f->write(QString("%1  %2\n").arg(perfClockMs() / 1000.0, 8, 'f', 3).arg(line).toUtf8());
    f->flush();
}

namespace {

// "Dark Pro" palette (see the approved UI mockup) -- kept as named
// constants rather than scattered hex literals since several places
// (icons, canvas surround, amplitude scale widget) need to agree on them.
constexpr int kCanvasMargin = 16; // dark surround around the inset plot

// A palette colour that follows the theme on every use (see theme.h).
struct ThemedColor {
    QColor dark;
    operator QColor() const { return tc(dark); }
    QString name() const { return tc(dark).name(); }
    QRgb rgb() const { return tc(dark).rgb(); }
};
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
const ThemedColor kCanvasSurroundColor{QColor(0x14, 0x14, 0x16)};
const ThemedColor kAccentColor{QColor(0xe8, 0x90, 0x5a)};
const ThemedColor kSecondaryTextColor{QColor(0x9a, 0x9c, 0xa3)};

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
    AnalysisSource src{0, app.foreground->traceCount, 0, app.foreground->binHeader.samplesPerTrace,
                       tc(QColor(Qt::white))};
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
// accessor for each of the 8 fields segy::TraceHeader already decodes --
// kept together so a new IdentField enumerator is a compile error in both
// places (a switch with no default) until handled, not a silent gap.
const char* identFieldLabel(IdentField f) {
    switch (f) {
        case IdentField::TraceSequenceLine: return "Seq (Line)";
        case IdentField::TraceSequenceFile: return "Seq (File)";
        case IdentField::FieldRecord: return "Field Record";
        case IdentField::TraceNumber: return "Trace Number";
        case IdentField::Cdp: return "CDP";
        case IdentField::Offset: return "Offset";
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
        case IdentField::Offset: return th.offset;
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
        p.setBrush(tc(QColor(0xe8, 0x90, 0x5a))); // Dark Pro accent
        p.drawEllipse(badgeRect);
        QFont font = p.font();
        font.setPixelSize(9);
        font.setBold(true);
        p.setFont(font);
        p.setPen(tc(QColor(0x1e, 0x1f, 0x22))); // dark text, readable on the accent orange
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
    if (app.flatten.active && app.flatten.original) {
        return QString::fromUtf8(" \xe2\x80\x94 ") +
               QString::fromStdString(app.flatten.original->filePath.filename().string()) + " (flattened)";
    }
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
// Streams `traceCount` output traces to `path` (segy::writeSegyFileChunked):
// chunk by chunk, each chunk's traces made in parallel by `makeTrace(t, out)`
// (any pool thread; `out` may hold the previous chunk's buffers, so assign,
// don't append). Only two chunks are ever in memory, whatever the file size.
// `progress` gets the number of traces done.
bool streamTraces(const std::filesystem::path& path, const std::string& textHeader, const segy::BinaryHeader& header,
                  int64_t traceCount, const std::function<void(int64_t, segy::WriteTrace&)>& makeTrace,
                  std::atomic<int64_t>& progress, std::string* error) {
    segy::ThreadPool pool;
    return segy::writeSegyFileChunked(
        path, textHeader, header, traceCount,
        [&](int64_t begin, int64_t end, std::vector<segy::WriteTrace>& chunk) {
            pool.parallelFor(size_t(end - begin), 8, [&](size_t b, size_t e) {
                for (size_t i = b; i < e; ++i) makeTrace(begin + int64_t(i), chunk[i]);
            });
            progress = end;
        },
        error, 4096);
}

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

// Builds one octave band's display data: the traces plus a robust clip --
// the 99.5th percentile of |amplitude| rather than the single peak, so one
// spike can't push the rest of the band toward white (amplitude varies
// wildly band-to-band, hence a clip per band at all).
std::shared_ptr<const OctaveBandData> makeOctaveBandData(std::vector<std::vector<float>> traces,
                                                         segy::ColorScale colorScale, bool colorScaleFlipped,
                                                         bool reversePolarity = false) {
    auto data = std::make_shared<OctaveBandData>();
    std::vector<float> magnitudes;
    size_t total = 0;
    for (const std::vector<float>& tr : traces) total += tr.size();
    magnitudes.reserve(total);
    double sumSquares = 0.0;
    for (const std::vector<float>& tr : traces) {
        for (float v : tr) {
            magnitudes.push_back(std::fabs(v));
            sumSquares += double(v) * double(v);
        }
    }
    if (!magnitudes.empty()) {
        data->rms = float(std::sqrt(sumSquares / double(magnitudes.size())));
        size_t k = std::min(magnitudes.size() - 1, size_t(double(magnitudes.size()) * 0.995));
        std::nth_element(magnitudes.begin(), magnitudes.begin() + std::ptrdiff_t(k), magnitudes.end());
        data->clip = std::max(magnitudes[k], 1e-12f);
    }
    data->traces = std::move(traces);
    data->colorScale = colorScale;
    data->colorScaleFlipped = colorScaleFlipped;
    data->reversePolarity = reversePolarity;
    return data;
}

// Rasterizes one octave band at exactly width x height pixels:
//   - time axis: linear interpolation between samples when stretching,
//     box-averaging the covered samples when shrinking (no dropped rows);
//   - trace axis: linear interpolation between neighboring traces when
//     there are more columns than traces (even, smooth trace widths instead
//     of a mix of 1- and 2-pixel columns), box-averaging when fewer.
// The traces here are a small, evenly spaced sample of the view (not the
// main canvas's decimated full-res traces), so blending neighbors is the
// right call for this overview, unlike renderer.cpp's nearest-trace rule.
// `clip` is the |amplitude| mapped to the color scale's ends -- the band's
// own (per-band scaling) or the Unfiltered band's (common scaling).
QImage rasterizeOctaveBand(const OctaveBandData& data, int width, int height, float clipPos, float clipNeg) {
    QImage img(std::max(1, width), std::max(1, height), QImage::Format_RGB32);
    const auto& traces = data.traces;
    if (traces.empty() || traces[0].empty() || width <= 0 || height <= 0) {
        img.fill(Qt::white);
        return img;
    }
    const int nTraces = int(traces.size());
    const int nSamples = int(traces[0].size());

    // Resample every trace to `height` rows first (one pass per trace), then
    // resample across traces per pixel -- O(traces*height + width*height).
    std::vector<float> columns(size_t(nTraces) * size_t(height));
    const double samplesPerRow = double(nSamples) / double(height);
    for (int t = 0; t < nTraces; ++t) {
        const std::vector<float>& tr = traces[size_t(t)];
        float* out = columns.data() + size_t(t) * size_t(height);
        for (int y = 0; y < height; ++y) {
            if (samplesPerRow > 1.0) {
                int a = std::clamp(int(std::floor(y * samplesPerRow)), 0, nSamples - 1);
                int b = std::clamp(int(std::ceil((y + 1) * samplesPerRow)), a + 1, nSamples);
                double sum = 0.0;
                for (int i = a; i < b; ++i) sum += tr[size_t(i)];
                out[y] = float(sum / double(b - a));
            } else {
                // Samples sit at their integer positions; row centers map onto that axis.
                double coord = (y + 0.5) * samplesPerRow - 0.5;
                int i0 = std::clamp(int(std::floor(coord)), 0, nSamples - 1);
                int i1 = std::min(i0 + 1, nSamples - 1);
                float f = float(std::clamp(coord - std::floor(coord), 0.0, 1.0));
                if (coord < 0.0) f = 0.0f;
                out[y] = tr[size_t(i0)] + (tr[size_t(i1)] - tr[size_t(i0)]) * f;
            }
        }
    }

    // Same mapping as the seismic renderer: value / clip, with separate
    // positive/negative clips, reverse polarity, then the color scale.
    const float direction = data.colorScaleFlipped ? -1.0f : 1.0f;
    const float scalePos = direction / std::max(clipPos, 1e-12f);
    const float scaleNeg = direction / std::max(clipNeg, 1e-12f);
    const float polarity = data.reversePolarity ? -1.0f : 1.0f;
    const double tracesPerCol = double(nTraces) / double(width);
    for (int x = 0; x < width; ++x) {
        int a = 0, b = 0, i0 = 0, i1 = 0;
        float f = 0.0f;
        const bool average = tracesPerCol > 1.0;
        if (average) {
            a = std::clamp(int(std::floor(x * tracesPerCol)), 0, nTraces - 1);
            b = std::clamp(int(std::ceil((x + 1) * tracesPerCol)), a + 1, nTraces);
        } else {
            double coord = (x + 0.5) * tracesPerCol - 0.5;
            i0 = std::clamp(int(std::floor(coord)), 0, nTraces - 1);
            i1 = std::min(i0 + 1, nTraces - 1);
            f = coord < 0.0 ? 0.0f : float(std::clamp(coord - std::floor(coord), 0.0, 1.0));
        }
        for (int y = 0; y < height; ++y) {
            float value;
            if (average) {
                float sum = 0.0f;
                for (int t = a; t < b; ++t) sum += columns[size_t(t) * size_t(height) + size_t(y)];
                value = sum / float(b - a);
            } else {
                float v0 = columns[size_t(i0) * size_t(height) + size_t(y)];
                float v1 = columns[size_t(i1) * size_t(height) + size_t(y)];
                value = v0 + (v1 - v0) * f;
            }
            value *= polarity;
            uint8_t r, g, bl;
            segy::applyColorScale(data.colorScale, value * (value >= 0.0f ? scalePos : scaleNeg), &bl, &g, &r);
            reinterpret_cast<uint32_t*>(img.scanLine(y))[x] = (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(bl);
        }
    }
    return img;
}

QIcon colorScalePreviewIcon(segy::ColorScale scale, bool flipped) {
    constexpr int kWidth = 96;
    constexpr int kHeight = 16;
    QPixmap pixmap(kWidth, kHeight);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    const float direction = flipped ? -1.0f : 1.0f;
    for (int x = 0; x < kWidth; ++x) {
        float t = (2.0f * float(x) / float(kWidth - 1) - 1.0f) * direction;
        uint8_t r, g, b;
        segy::applyColorScale(scale, t, &b, &g, &r);
        painter.setPen(QColor(r, g, b));
        painter.drawLine(x, 0, x, kHeight - 1);
    }
    painter.setPen(tc(QColor(0x66, 0x68, 0x6d)));
    painter.drawRect(0, 0, kWidth - 1, kHeight - 1);
    return QIcon(pixmap);
}

class OctaveBandCrosshairOverlay : public QWidget {
public:
    explicit OctaveBandCrosshairOverlay(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
    }

    void setCrosshair(double x, double y, bool visible) {
        crosshairX_ = x;
        crosshairY_ = y;
        crosshairVisible_ = visible;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!crosshairVisible_) return;
        QPainter painter(this);
        const int x = int(crosshairX_ * width());
        const int y = int(crosshairY_ * height());
        painter.setPen(QPen(QColor(0, 0, 0, 190), 3));
        painter.drawLine(x, 0, x, height() - 1);
        painter.drawLine(0, y, width() - 1, y);
        painter.setPen(QPen(Qt::white, 1, Qt::DashLine));
        painter.drawLine(x, 0, x, height() - 1);
        painter.drawLine(0, y, width() - 1, y);
    }

private:
    double crosshairX_ = 0.5;
    double crosshairY_ = 0.5;
    bool crosshairVisible_ = false;
};

// Vertical time/lag axis for the octave-band and autocorrelation plots: the
// same "nice" 1-2-5 intervals (at most 11 lines, coarsened until labels
// don't overlap) as the seismic canvas's Time axis, via computeNiceAxisTicks.
// Mapped exactly the way rasterizeOctaveBand places samples -- sample s's row
// centre at (s - sampleStart + 0.5) / n of the height -- so ticks and grid
// lines sit on the data, not half a sample off. Both plot widgets call this
// with the same 8 px axis font and plot height, so their lines coincide.
QFont plotAxisFont(const QWidget* widget) {
    QFont f = widget->font();
    f.setPixelSize(10); // 8 px was too small to read
    return f;
}

std::vector<segy::AxisTick> plotTimeTicks(int sampleStart, int sampleEnd, double sampleIntervalUs, int height,
                                          const QFont& axisFont) {
    if (height <= 0 || sampleIntervalUs <= 0.0 || sampleEnd < sampleStart) return {};
    const double dtMs = sampleIntervalUs / 1000.0;
    QFontMetrics metrics(axisFont);
    return segy::computeNiceAxisTicks((sampleStart - 0.5) * dtMs, (sampleEnd + 0.5) * dtMs, height, 11,
                                      [&metrics](const std::string& label) {
                                          // Rotated labels: their width runs along this axis; a few px gap.
                                          return metrics.horizontalAdvance(QString::fromStdString(label)) + 6;
                                      });
}

// Horizontal grid line, matching the seismic canvas's (chrome.cpp blends
// each grid row 50% with #202020).
const QColor kPlotGridColor(0x20, 0x20, 0x20, 128);

class OctaveBandPlotWidget : public QWidget {
public:
    explicit OctaveBandPlotWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(150, 200);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        crosshairOverlay_ = new OctaveBandCrosshairOverlay(this);
        // Exact re-rasterize once a resize settles; until then a cheap
        // stretch of the last exact raster keeps live resizing responsive.
        rasterTimer_ = new QTimer(this);
        rasterTimer_->setSingleShot(true);
        rasterTimer_->setInterval(80);
        connect(rasterTimer_, &QTimer::timeout, this, [this]() {
            rasterizeExact();
            update();
        });
    }

    void setBandData(std::shared_ptr<const OctaveBandData> bandData) {
        data_ = std::move(bandData);
        rasterizeExact();
        update();
    }

    // "Time (ms)" by default; the autocorrelogram uses "Lag (ms)".
    void setVerticalTitle(const QString& title) {
        verticalTitle_ = title;
        update();
    }

    // <= 0 means "the band's own clip". `clipNeg` <= 0: same as `clip`.
    void setClip(float clip, float clipNeg = 0.0f) {
        if (clipNeg <= 0.0f) clipNeg = clip;
        if (clip == clip_ && clipNeg == clipNeg_) return;
        clip_ = clip;
        clipNeg_ = clipNeg;
        rasterizeExact();
        update();
    }

    void setAxisRange(double traceStart, double traceEnd, int sampleStart, int sampleEnd, double sampleIntervalUs) {
        traceStart_ = traceStart;
        traceEnd_ = traceEnd;
        sampleStart_ = sampleStart;
        sampleEnd_ = sampleEnd;
        sampleIntervalUs_ = sampleIntervalUs;
        update();
    }

    void setCrosshair(double x, double y, bool visible) { crosshairOverlay_->setCrosshair(x, y, visible); }

    void setMouseCallbacks(std::function<void(double, double)> onMoved, std::function<void()> onLeft) {
        onMoved_ = std::move(onMoved);
        onLeft_ = std::move(onLeft);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), tc(QColor(0x1e, 0x1f, 0x22)));
        QRect plot = plotRect();
        if (plot.width() <= 0 || plot.height() <= 0) return;
        if (!raster_.isNull()) {
            // Exact-size raster: a 1:1 blit. A stale one mid-resize gets a
            // smooth stretch until rasterTimer_ fires.
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(plot, raster_);
        }

        const QFont axisFont = plotAxisFont(this);
        const std::vector<segy::AxisTick> timeTicks =
            plotTimeTicks(sampleStart_, sampleEnd_, sampleIntervalUs_, plot.height(), axisFont);
        // Grid lines across the data, like the seismic canvas.
        painter.setPen(QPen(kPlotGridColor, 1));
        for (const segy::AxisTick& tick : timeTicks) {
            int y = plot.top() + tick.pixelPos;
            if (y > plot.top() && y < plot.bottom()) painter.drawLine(plot.left(), y, plot.right(), y);
        }

        painter.setPen(tc(QColor(0x9a, 0x9c, 0xa3)));
        painter.drawLine(plot.topLeft(), plot.bottomLeft());
        painter.drawLine(plot.bottomLeft(), plot.bottomRight());
        painter.setPen(tc(QColor(0xe0, 0xe2, 0xe7)));
        painter.setFont(axisFont);
        for (const segy::AxisTick& tick : timeTicks) {
            int y = plot.top() + tick.pixelPos;
            painter.drawLine(plot.left() - 4, y, plot.left(), y);
            painter.save();
            painter.translate(plot.left() - 12, y);
            painter.rotate(-90);
            painter.drawText(QRect(-30, -8, 60, 16), Qt::AlignCenter, QString::fromStdString(tick.label));
            painter.restore();
        }
        // Trace axis: first, middle, last (labels at the two ends).
        for (int i = 0; i <= 2; ++i) {
            double fraction = double(i) / 2.0;
            int x = plot.left() + int(std::round(fraction * (plot.width() - 1)));
            double trace = traceStart_ + fraction * (traceEnd_ - traceStart_);
            painter.drawLine(x, plot.bottom(), x, plot.bottom() + 4);
            // Whole trace numbers (not 1.508e+04), as wide as they need.
            const QString text = QString::number(qlonglong(std::llround(trace)));
            const int tw = QFontMetrics(axisFont).horizontalAdvance(text) + 2;
            if (i == 0) {
                painter.drawText(QRect(plot.left(), plot.bottom() + 3, tw, 15), Qt::AlignLeft | Qt::AlignVCenter, text);
            } else if (i == 2 && plot.width() > 2 * tw + 8) {
                // Kept inside the widget: the plot can reach its right edge.
                const int left = std::min(plot.right() - tw + 1, width() - tw - 6);
                painter.drawText(QRect(left, plot.bottom() + 3, tw, 15), Qt::AlignRight | Qt::AlignVCenter, text);
            }
        }
        painter.setFont(font());
        painter.drawText(QRect(plot.left(), plot.bottom() + 18, plot.width(), 16), Qt::AlignCenter, "Trace");
        painter.save();
        painter.translate(10, plot.center().y());
        painter.rotate(-90);
        painter.drawText(QRect(-plot.height() / 2, -9, plot.height(), 18), Qt::AlignCenter, verticalTitle_);
        painter.restore();
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (raster_.isNull()) {
            rasterizeExact(); // first layout: nothing to stretch yet
        } else {
            rasterTimer_->start();
        }
        crosshairOverlay_->setGeometry(plotRect());
        crosshairOverlay_->raise();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (onMoved_) {
            QRect plot = plotRect();
            double x = std::clamp(double(event->pos().x() - plot.left()) / std::max(1, plot.width()), 0.0, 1.0);
            double y = std::clamp(double(event->pos().y() - plot.top()) / std::max(1, plot.height()), 0.0, 1.0);
            onMoved_(x, y);
        }
    }

    void leaveEvent(QEvent* event) override {
        if (onLeft_) onLeft_();
        QWidget::leaveEvent(event);
    }

private:
    std::shared_ptr<const OctaveBandData> data_;
    QString verticalTitle_ = "Time (ms)";
    float clip_ = 0.0f;
    float clipNeg_ = 0.0f;
    QImage raster_; // rasterized at plotRect() size x devicePixelRatio
    QTimer* rasterTimer_ = nullptr;
    OctaveBandCrosshairOverlay* crosshairOverlay_ = nullptr;
    double traceStart_ = 0.0;
    double traceEnd_ = 0.0;
    int sampleStart_ = 0;
    int sampleEnd_ = 0;
    double sampleIntervalUs_ = 0.0;
    std::function<void(double, double)> onMoved_;
    std::function<void()> onLeft_;

    QRect plotRect() const { return rect().adjusted(42, 24, -6, -38); }

    // Device pixels, not logical ones, so HiDPI screens get a sharp image
    // rather than a 2x-upscaled one.
    void rasterizeExact() {
        QSize size = plotRect().size();
        if (!data_ || size.width() <= 0 || size.height() <= 0) {
            raster_ = QImage();
            return;
        }
        const qreal dpr = devicePixelRatioF();
        raster_ = rasterizeOctaveBand(*data_, int(std::lround(size.width() * dpr)),
                                      int(std::lround(size.height() * dpr)), clip_ > 0.0f ? clip_ : data_->clip,
                                      clip_ > 0.0f ? clipNeg_ : data_->clip);
        raster_.setDevicePixelRatio(dpr);
    }
};

} // namespace

AmplitudeScaleWidget::AmplitudeScaleWidget(QWidget* parent) : QWidget(parent) {}

void AmplitudeScaleWidget::setClipRange(float posClip, float negClip) {
    posClip_ = posClip > 0.0f ? posClip : 1.0f;
    negClip_ = negClip > 0.0f ? negClip : 1.0f;
    update();
}

void AmplitudeScaleWidget::setCenter(float center, const QString& unit) {
    center_ = center;
    unit_ = unit;
    update();
}

void AmplitudeScaleWidget::setColorScale(segy::ColorScale scale, bool flipped) {
    colorScale_ = scale;
    colorScaleFlipped_ = flipped;
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
    painter.fillRect(rect(), tc(QColor(0x14, 0x14, 0x16, 200)));

    QFontMetrics fm(painter.font());
    int textH = fm.height();
    constexpr int kPad = 6;
    int barW = 8;
    int barX = kPad;
    // A unit (x-f: "dB") goes on its own line above the scale -- the
    // labels beside the bar have room for a number only.
    const int headerH = unit_.isEmpty() ? 0 : textH;
    int barY = kPad + textH / 2 + headerH;
    int barH = h - 2 * (kPad + textH / 2) - headerH;

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
    const float colorScaleDirection = colorScaleFlipped_ ? -1.0f : 1.0f;
    QLinearGradient gradient(0, barY, 0, barY + barH);
    constexpr int kStopsPerHalf = 8;
    for (int i = 0; i <= kStopsPerHalf; ++i) {
        float frac = float(i) / float(kStopsPerHalf);  // 0 (top of this half) .. 1 (zero line)
        float t = 1.0f - frac;                          // +1 (top) .. 0 (zero line), for the positive half
        uint8_t r, g, b;
        segy::applyColorScale(colorScale_, t * colorScaleDirection, &b, &g, &r);
        gradient.setColorAt(double(frac) * zeroFrac, QColor(r, g, b));
    }
    for (int i = 0; i <= kStopsPerHalf; ++i) {
        float frac = float(i) / float(kStopsPerHalf);  // 0 (zero line) .. 1 (bottom of this half)
        float t = -frac;                                // 0 (zero line) .. -1 (bottom), for the negative half
        uint8_t r, g, b;
        segy::applyColorScale(colorScale_, t * colorScaleDirection, &b, &g, &r);
        gradient.setColorAt(double(zeroFrac) + double(frac) * (1.0 - zeroFrac), QColor(r, g, b));
    }
    painter.fillRect(QRectF(barX, barY, barW, barH), gradient);

    painter.setPen(kSecondaryTextColor);
    int textX = barX + barW + kPad;
    int textW = w - textX - kPad;
    // Signed labels with a real minus sign (U+2212); "+" only around a zero centre.
    // Whole numbers from 10 up, three significant digits below (small
    // amplitudes, e.g. after integration, would otherwise all read "0").
    auto signedLabel = [](float value, bool plusSign) {
        const double magnitude = std::fabs(double(value));
        QString digits = magnitude >= 10.0 || magnitude == 0.0 ? QString::number(std::lround(magnitude))
                                                               : QString::number(magnitude, 'g', 3);
        if (digits == "0") return QString("0");
        if (value < 0.0f) return QString(QChar(0x2212)) + digits;
        return (plusSign ? QString("+") : QString()) + digits;
    };
    const bool zeroCentre = center_ == 0.0f;
    QString topLabel = signedLabel(center_ + posClip_, zeroCentre);
    QString midLabel = signedLabel(center_, false);
    QString bottomLabel = signedLabel(center_ - negClip_, zeroCentre);
    int zeroY = barY + int(zeroFrac * barH);
    if (headerH > 0) painter.drawText(QRect(kPad, 0, w - 2 * kPad, textH), Qt::AlignLeft | Qt::AlignTop, unit_);
    painter.drawText(QRect(textX, headerH, textW, textH), Qt::AlignLeft | Qt::AlignTop, topLabel);
    painter.drawText(QRect(textX, zeroY - textH / 2, textW, textH), Qt::AlignLeft | Qt::AlignVCenter, midLabel);
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

    painter.setPen(tc(QColor(0x3c, 0x3f, 0x41)));
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
        QPen pen(active ? QColor(Qt::white) : QColor(kAccentColor));
        pen.setWidth(active ? 2 : 1);
        painter.setPen(pen);
        painter.drawLine(x, chart.top(), x, chart.bottom());
        constexpr int kHandle = 6;
        painter.setBrush(active ? QColor(Qt::white) : QColor(kAccentColor));
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
    hint->setStyleSheet(themed("color: #9a9ca3; font-size: 11px;"));
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

AmplitudeProcessingDialog::AmplitudeProcessingDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Amplitude Processing");
    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* hint = new QLabel("Display only: the data itself, Spectrum, Histogram and Save SEG-Y are unaffected.");
    hint->setWordWrap(true);
    hint->setStyleSheet(themed("color: #9a9ca3;"));
    layout->addWidget(hint);

    // --- Computed gain: time-gain pairs ---
    gainGroup_ = new QGroupBox("Computed Gain (time-gain pairs)");
    gainGroup_->setCheckable(true);
    gainGroup_->setChecked(false);
    QVBoxLayout* gainLayout = new QVBoxLayout(gainGroup_);
    QLabel* gainHint = new QLabel("Gain in dB, interpolated linearly in dB between pairs and held "
                                  "constant before the first and after the last.");
    gainHint->setWordWrap(true);
    gainLayout->addWidget(gainHint);
    gainTable_ = new QTableWidget(0, 2);
    gainTable_->setHorizontalHeaderLabels({"Time (ms)", "Gain (dB)"});
    gainTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    gainTable_->verticalHeader()->setVisible(false);
    gainTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    gainTable_->setMinimumHeight(160);
    gainLayout->addWidget(gainTable_);
    QHBoxLayout* rowButtons = new QHBoxLayout;
    QPushButton* addButton = new QPushButton("Add Pair");
    QPushButton* removeButton = new QPushButton("Remove Pair");
    rowButtons->addWidget(addButton);
    rowButtons->addWidget(removeButton);
    rowButtons->addStretch();
    gainLayout->addLayout(rowButtons);
    layout->addWidget(gainGroup_);

    connect(addButton, &QPushButton::clicked, this, [this]() {
        // Continue the curve: 500 ms past the last pair, same gain.
        double t = 0.0, db = 0.0;
        if (const segy::AmplitudeProcessing p = collect(); !p.gainCurve.empty()) {
            t = p.gainCurve.back().timeMs + 500.0;
            db = p.gainCurve.back().gainDb;
        }
        addGainRow(t, db);
        emitChanged();
    });
    connect(removeButton, &QPushButton::clicked, this, [this]() {
        int row = gainTable_->currentRow();
        if (row < 0) row = gainTable_->rowCount() - 1;
        if (row < 0) return;
        gainTable_->removeRow(row);
        emitChanged();
    });
    connect(gainGroup_, &QGroupBox::toggled, this, [this](bool on) {
        // A sensible starting curve the first time it's switched on:
        // flat at the top, +12 dB by the end of the record.
        if (on && gainTable_->rowCount() == 0) {
            addGainRow(0.0, 0.0);
            addGainRow(recordLengthMs_, 12.0);
        }
        emitChanged();
    });

    // --- AGC ---
    agcGroup_ = new QGroupBox("Automatic Gain Control (AGC)");
    agcGroup_->setCheckable(true);
    agcGroup_->setChecked(false);
    QFormLayout* agcLayout = new QFormLayout(agcGroup_);
    agcWindowSpin_ = new QDoubleSpinBox;
    agcWindowSpin_->setRange(4.0, 20000.0);
    agcWindowSpin_->setDecimals(0);
    agcWindowSpin_->setSingleStep(50.0);
    agcWindowSpin_->setSuffix(" ms");
    agcWindowSpin_->setValue(500.0);
    agcWindowSpin_->setKeyboardTracking(false);
    agcWindowSpin_->setToolTip("Length of the RMS window centered on each sample");
    agcLayout->addRow("Window:", agcWindowSpin_);
    agcLevelSpin_ = new QDoubleSpinBox;
    agcLevelSpin_->setRange(1.0, 100.0);
    agcLevelSpin_->setDecimals(0);
    agcLevelSpin_->setSingleStep(5.0);
    agcLevelSpin_->setSuffix(" %");
    agcLevelSpin_->setValue(25.0);
    agcLevelSpin_->setKeyboardTracking(false);
    agcLevelSpin_->setToolTip("Where each window's RMS lands on the color scale / wiggle excursion "
                              "(the toolbar gain still applies on top)");
    agcLayout->addRow("Level (of full scale):", agcLevelSpin_);
    layout->addWidget(agcGroup_);

    connect(agcGroup_, &QGroupBox::toggled, this, [this]() { emitChanged(); });
    connect(agcWindowSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { emitChanged(); });
    connect(agcLevelSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { emitChanged(); });

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

void AmplitudeProcessingDialog::addGainRow(double timeMs, double gainDb) {
    int row = gainTable_->rowCount();
    gainTable_->insertRow(row);
    auto makeSpin = [this](double min, double max, double step, double value) {
        QDoubleSpinBox* spin = new QDoubleSpinBox;
        spin->setRange(min, max);
        spin->setDecimals(1);
        spin->setSingleStep(step);
        spin->setValue(value);
        spin->setKeyboardTracking(false); // apply on Enter/focus-out, not every keystroke
        spin->setFrame(false);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() { emitChanged(); });
        return spin;
    };
    gainTable_->setCellWidget(row, 0, makeSpin(0.0, 1e6, 100.0, timeMs));
    gainTable_->setCellWidget(row, 1, makeSpin(-60.0, 60.0, 1.0, gainDb));
}

segy::AmplitudeProcessing AmplitudeProcessingDialog::collect() const {
    segy::AmplitudeProcessing p;
    p.gainCurveEnabled = gainGroup_->isChecked();
    for (int row = 0; row < gainTable_->rowCount(); ++row) {
        auto* t = qobject_cast<QDoubleSpinBox*>(gainTable_->cellWidget(row, 0));
        auto* g = qobject_cast<QDoubleSpinBox*>(gainTable_->cellWidget(row, 1));
        if (t && g) p.gainCurve.push_back({t->value(), g->value()});
    }
    p.agcEnabled = agcGroup_->isChecked();
    p.agcWindowMs = agcWindowSpin_->value();
    p.agcLevelPercent = agcLevelSpin_->value();
    return p;
}

void AmplitudeProcessingDialog::emitChanged() {
    if (loading_ || !onChanged_) return;
    onChanged_(collect());
}

void AmplitudeProcessingDialog::setProcessing(const segy::AmplitudeProcessing& processing, double recordLengthMs) {
    loading_ = true;
    if (recordLengthMs > 0.0) recordLengthMs_ = recordLengthMs;
    gainTable_->setRowCount(0);
    for (const segy::GainPoint& point : processing.gainCurve) addGainRow(point.timeMs, point.gainDb);
    gainGroup_->setChecked(processing.gainCurveEnabled);
    agcGroup_->setChecked(processing.agcEnabled);
    agcWindowSpin_->setValue(processing.agcWindowMs);
    agcLevelSpin_->setValue(processing.agcLevelPercent);
    loading_ = false;
}

void AmplitudeProcessingDialog::setChangedCallback(std::function<void(const segy::AmplitudeProcessing&)> callback) {
    onChanged_ = std::move(callback);
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
    crossRadio_ = new QRadioButton("Cross-correlate");
    crossRadio_->setToolTip("A x B trace by trace (normalized), shown like the autocorrelation; no new dataset");
    addRadio_->setChecked(true);
    QButtonGroup* opGroup = new QButtonGroup(this);
    opGroup->addButton(addRadio_);
    opGroup->addButton(subtractRadio_);
    opGroup->addButton(crossRadio_);
    opRow->addWidget(addRadio_);
    opRow->addWidget(subtractRadio_);
    opRow->addWidget(crossRadio_);
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
    connect(crossRadio_, &QRadioButton::toggled, this, [this](bool on) { outputNameEdit_->setEnabled(!on); });
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

void CalculatorDialog::refreshDatasets(const QStringList& names, int defaultA, int defaultB) {
    datasetACombo_->clear();
    datasetBCombo_->clear();
    for (int i = 0; i < names.size(); ++i) {
        datasetACombo_->addItem(names[i], i);
        datasetBCombo_->addItem(names[i], i);
    }
    if (defaultA >= 0 && defaultA < names.size()) datasetACombo_->setCurrentIndex(defaultA);
    if (defaultB >= 0 && defaultB < names.size()) datasetBCombo_->setCurrentIndex(defaultB);
    outputNameEdited_ = false;
    updateDefaultOutputName();
}

void CalculatorDialog::setApplyCallback(std::function<void(int, int, int, QString)> callback) {
    onApply_ = std::move(callback);
}

void CalculatorDialog::updateDefaultOutputName() {
    if (outputNameEdited_) return;
    QString a = datasetACombo_->currentText().section("   [", 0, 0);
    QString b = datasetBCombo_->currentText().section("   [", 0, 0);
    if (a.isEmpty() || b.isEmpty()) return;
    QString op = addRadio_->isChecked() ? "Plus" : "Minus";
    outputNameEdit_->setText(QString("%1_%2_%3").arg(a, op, b));
}

void CalculatorDialog::emitApply() {
    if (!onApply_ || datasetACombo_->count() == 0 || datasetBCombo_->count() == 0) return;
    const int op = crossRadio_->isChecked() ? 2 : subtractRadio_->isChecked() ? 1 : 0;
    onApply_(datasetACombo_->currentData().toInt(), datasetBCombo_->currentData().toInt(), op, outputNameEdit_->text());
}

BandpassDialog::BandpassDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Bandpass Filter");
    setModal(false);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* cornerRow = new QHBoxLayout();
    auto column = [&](const QString& label, QWidget* field) {
        QVBoxLayout* col = new QVBoxLayout();
        col->addWidget(new QLabel(label));
        col->addWidget(field);
        cornerRow->addLayout(col);
    };
    lowSpin_ = new QDoubleSpinBox();
    lowSpin_->setRange(0.0, 100000.0);
    lowSpin_->setSuffix(" Hz");
    lowSpin_->setValue(10.0);
    lowSpin_->setToolTip("Low corner (-3 dB per pass). 0 Hz: no low cut, i.e. a lowpass.");
    highSpin_ = new QDoubleSpinBox();
    highSpin_->setRange(0.1, 100000.0);
    highSpin_->setSuffix(" Hz");
    highSpin_->setValue(50.0);
    highSpin_->setToolTip("High corner (-3 dB per pass). At or above Nyquist: no high cut, i.e. a highpass.");
    orderSpin_ = new QSpinBox();
    orderSpin_->setRange(1, 10);
    orderSpin_->setValue(4);
    orderSpin_->setToolTip("Butterworth order per pass; filtfilt runs it twice, doubling the roll-off.");
    edgesCombo_ = new QComboBox();
    edgesCombo_->addItem("Mirror (even)", int(segy::EdgePadding::Even));
    edgesCombo_->addItem("Point mirror (odd, scipy default)", int(segy::EdgePadding::Odd));
    edgesCombo_->addItem("Repeat end value (constant)", int(segy::EdgePadding::Constant));
    edgesCombo_->setToolTip("How each trace is extended past its ends before filtering (scipy's padtype).\n"
                            "Mirror gave the smallest edge effects on real data, especially in low bands.");
    column("Low Corner", lowSpin_);
    column("High Corner", highSpin_);
    column("Order", orderSpin_);
    column("Edges", edgesCombo_);
    layout->addLayout(cornerRow);

    QLabel* hint = new QLabel("Butterworth, applied zero-phase with scipy.signal.sosfiltfilt's method "
                              "(trace ends padded, steady-state start). Corners end up at -6 dB.");
    hint->setWordWrap(true);
    hint->setStyleSheet(themed("color: #9a9ca3; font-size: 11px;"));
    layout->addWidget(hint);

    QHBoxLayout* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel("Output Name:"));
    outputNameEdit_ = new QLineEdit();
    nameRow->addWidget(outputNameEdit_, 1);
    layout->addLayout(nameRow);

    for (QDoubleSpinBox* spin : {lowSpin_, highSpin_}) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, spin](double) {
            enforceOrdering(spin);
            updateDefaultOutputName();
        });
    }
    connect(orderSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { updateDefaultOutputName(); });
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

void BandpassDialog::setApplyCallback(std::function<void(segy::ButterworthBandParams, QString)> callback) {
    onApply_ = std::move(callback);
}

void BandpassDialog::enforceOrdering(QDoubleSpinBox* edited) {
    // Low 0 means "no low cut", so only a positive low corner constrains high.
    const double low = lowSpin_->value();
    const double high = highSpin_->value();
    if (low <= 0.0 || low < high) return;
    QDoubleSpinBox* other = edited == lowSpin_ ? highSpin_ : lowSpin_;
    const double value = edited == lowSpin_ ? low + 1.0 : std::max(0.0, high - 1.0);
    other->blockSignals(true);
    other->setValue(value);
    other->blockSignals(false);
}

void BandpassDialog::updateDefaultOutputName() {
    if (outputNameEdited_ || sourceName_.isEmpty()) return;
    outputNameEdit_->setText(QString("%1_Bandpass_%2_%3_N%4")
                                  .arg(sourceName_)
                                  .arg(lowSpin_->value(), 0, 'g', 4)
                                  .arg(highSpin_->value(), 0, 'g', 4)
                                  .arg(orderSpin_->value()));
}

void BandpassDialog::emitApply() {
    if (!onApply_) return;
    segy::ButterworthBandParams params;
    params.lowHz = lowSpin_->value();
    params.highHz = highSpin_->value();
    params.order = orderSpin_->value();
    params.padding = segy::EdgePadding(edgesCombo_->currentData().toInt());
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
    datasetSummaryLabel_->setStyleSheet(themed("color: #9a9ca3;"));
    layout->addWidget(datasetSummaryLabel_);

    layout->addWidget(new QLabel("EBCDIC Text Header Preview:"));
    ebcdicPreview_ = new QPlainTextEdit();
    ebcdicPreview_->setReadOnly(true);
    ebcdicPreview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(9);
    ebcdicPreview_->setFont(mono);
    ebcdicPreview_->setStyleSheet(themed("background: #f4f2ec; color: #1b1b1b;"));
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
    toolbarRow->addWidget(new QLabel("Source"));
    sourceCombo_ = new QComboBox();
    toolbarRow->addWidget(sourceCombo_);
    toolbarRow->addWidget(new QLabel("From"));
    minFreqCombo_ = new QComboBox();
    toolbarRow->addWidget(minFreqCombo_);
    toolbarRow->addWidget(new QLabel("to"));
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
    QPushButton* applyButton = new QPushButton("Recompute");
    connect(applyButton, &QPushButton::clicked, this, [this]() { emitApply(); });
    toolbarRow->addWidget(applyButton);
    toolbarRow->addSpacing(16);
    toolbarRow->addWidget(new QLabel("Scale"));
    scaleCombo_ = new QComboBox();
    scaleCombo_->addItem("Same as main display", 0);
    scaleCombo_->addItem("Per band", 1);
    scaleCombo_->setToolTip("Common: all bands share the unfiltered clip, so weak bands look weak.\n"
                            "Per band: each band scaled to itself, to see structure in weak bands.");
    connect(scaleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { applyScaling(); });
    toolbarRow->addWidget(scaleCombo_);
    toolbarRow->addStretch();
    layout->addLayout(toolbarRow);

    scroll_ = new QScrollArea();
    scroll_->setWidgetResizable(true);
    bandsRow_ = new QWidget();
    QHBoxLayout* bandsLayout = new QHBoxLayout(bandsRow_);
    bandsLayout->setSpacing(0);
    bandsLayout->setContentsMargins(0, 0, 0, 0);
    scroll_->setWidget(bandsRow_);
    layout->addWidget(scroll_, 1);

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
    bandCrosshairUpdaters_.clear();
    bandClipSetters_.clear();
    bandOwnClips_.clear();
    // bands.front() is always "Unfiltered" (see computeAndShowOctaveBands).
    const OctaveBandData* reference = !result.bands.empty() ? result.bands.front().data.get() : nullptr;
    mainClipPos_ = result.mainClipPos;
    mainClipNeg_ = result.mainClipNeg;
    const float referenceRms = reference ? reference->rms : 0.0f;
    if (oldLayout) {
        while (QLayoutItem* item = oldLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        delete oldLayout;
    }
    QHBoxLayout* bandsLayout = new QHBoxLayout();
    bandsLayout->setSpacing(0);
    bandsLayout->setContentsMargins(0, 0, 0, 0);
    for (const OctaveBandResult::Band& band : result.bands) {
        QWidget* col = new QWidget();
        col->setMinimumWidth(140);
        col->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        col->setStyleSheet(themed("border-right: 1px solid #3c3f41;"));
        QVBoxLayout* colLayout = new QVBoxLayout(col);
        colLayout->setContentsMargins(4, 4, 4, 4);
        // Each filtered band's RMS level relative to the unfiltered data --
        // the number behind "how weak is this band", independent of the
        // Scale choice.
        QString labelText = band.label;
        if (band.data.get() != reference && referenceRms > 0.0f) {
            // On its own line, so the dB text never widens the column (it
            // used to make the band row wider than the window). QChar, not
            // a "\u221e" literal: the build has no /utf-8, so MSVC would
            // encode that in the local code page, not UTF-8.
            labelText += band.data->rms > 0.0f
                             ? QString("\n%1 dB").arg(20.0 * std::log10(double(band.data->rms) / referenceRms), 0, 'f', 0)
                             : QString("\n-%1 dB").arg(QString(QChar(0x221E)));
        } else if (band.data.get() == reference) {
            // The reference itself: a second line too, so every header is
            // the same height and all plots line up.
            labelText += "\n0 dB (reference)";
        }
        QLabel* labelWidget = new QLabel(labelText);
        labelWidget->setAlignment(Qt::AlignCenter);
        labelWidget->setStyleSheet(themed(
            "QLabel { color: #f0f2f5; background-color: #303238; font-size: 12px; font-weight: bold; padding: 2px; "
            "border: none; }"));
        colLayout->addWidget(labelWidget);
        OctaveBandPlotWidget* plotWidget = new OctaveBandPlotWidget();
        plotWidget->setBandData(band.data);
        plotWidget->setAxisRange(result.traceStart, result.traceEnd, result.sampleStart, result.sampleEnd,
                                result.sampleIntervalUs);
        bandCrosshairUpdaters_.push_back([plotWidget](double x, double y, bool visible) {
            plotWidget->setCrosshair(x, y, visible);
        });
        bandClipSetters_.push_back([plotWidget](float clipPos, float clipNeg) { plotWidget->setClip(clipPos, clipNeg); });
        bandOwnClips_.push_back(band.data->clip);
        plotWidget->setMouseCallbacks(
            [this](double x, double y) {
                for (const auto& updateCrosshair : bandCrosshairUpdaters_) updateCrosshair(x, y, true);
            },
            [this]() {
                for (const auto& updateCrosshair : bandCrosshairUpdaters_) updateCrosshair(0.0, 0.0, false);
            });
        colLayout->addWidget(plotWidget, 1);
        bandsLayout->addWidget(col);
    }
    bandsRow_->setLayout(bandsLayout);
    applyScaling();
    fitWidthToBands();
}

void OctaveBandDialog::fitWidthToBands() {
    // Content width the band row needs, plus everything around the scroll
    // area's viewport: the dialog's layout margins and the scroll frame.
    const int needed = bandsRow_->minimumSizeHint().width();
    const QMargins margins = layout()->contentsMargins();
    const int chrome = margins.left() + margins.right() + 2 * scroll_->frameWidth() + 2;
    int wanted = needed + chrome;
    if (wanted <= width()) return; // never shrink what the user chose
    const QScreen* screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, wanted, height());
    // The frame (title bar/borders) is outside width(); leave room for it.
    const int frameExtra = frameGeometry().width() - width();
    wanted = std::min(wanted, available.width() - std::max(0, frameExtra) - 16);
    resize(wanted, height());
    // Keep a visible window fully on screen after growing.
    if (isVisible()) {
        QRect frame = frameGeometry();
        if (frame.right() > available.right()) frame.moveRight(available.right());
        if (frame.left() < available.left()) frame.moveLeft(available.left());
        move(frame.topLeft());
    }
}

void OctaveBandDialog::applyScaling() {
    const bool sameAsMain = scaleCombo_->currentData().toInt() == 0;
    for (size_t i = 0; i < bandClipSetters_.size(); ++i) {
        if (sameAsMain) {
            bandClipSetters_[i](mainClipPos_, mainClipNeg_);
        } else {
            bandClipSetters_[i](bandOwnClips_[i], bandOwnClips_[i]);
        }
    }
}

void OctaveBandDialog::setMainClip(float clipPos, float clipNeg) {
    if (clipPos == mainClipPos_ && clipNeg == mainClipNeg_) return;
    mainClipPos_ = clipPos;
    mainClipNeg_ = clipNeg;
    applyScaling();
}

void OctaveBandDialog::setApplyCallback(std::function<void(int, int, int)> callback) { onApply_ = std::move(callback); }

namespace {

// The average autocorrelation as a curve, lag on the vertical axis so it
// lines up row-for-row with the autocorrelogram beside it (same top/bottom
// plot margins as OctaveBandPlotWidget::plotRect()). Horizontal: -1..+1,
// with the zero line and +/-0.5 guides.
class AutocorrelationAverageWidget : public QWidget {
public:
    explicit AutocorrelationAverageWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumWidth(170);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        setMouseTracking(true);
    }
    // `sampleStart`..`sampleEnd` (lags in samples) and the interval give it
    // the same tick/grid positions as the correlogram beside it.
    void setAverage(std::vector<float> average, int sampleStart, int sampleEnd, double sampleIntervalUs) {
        average_ = std::move(average);
        sampleStart_ = sampleStart;
        sampleEnd_ = sampleEnd;
        sampleIntervalUs_ = sampleIntervalUs;
        measureCount_ = 0;
        update();
    }
    void setPeak(double lagSamples) {
        peakLag_ = lagSamples;
        update();
    }
    // Box mode: a vertical drag selects a lag range (ms) for Zoom.
    void setBoxMode(bool on) {
        boxMode_ = on;
        setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    }
    std::function<void(double fromMs, double toMs)> onLagBox;
    std::function<void(const QString&)> onMeasure; // delta-t text, empty when cleared
    std::function<void()> onDoubleClick;

protected:
    QRect plotRect() const { return rect().adjusted(10, 24, -10, -38); }
    double lagMsAt(double y) const {
        const QRect plot = plotRect();
        const int n = sampleEnd_ - sampleStart_ + 1;
        const double sample = sampleStart_ + (y - plot.top()) / std::max(1, plot.height()) * n - 0.5;
        return sample * sampleIntervalUs_ / 1000.0;
    }
    double yForLagMs(double ms) const {
        const QRect plot = plotRect();
        const int n = sampleEnd_ - sampleStart_ + 1;
        const double sample = ms * 1000.0 / std::max(1e-9, sampleIntervalUs_);
        return plot.top() + (sample - sampleStart_ + 0.5) / std::max(1, n) * plot.height();
    }
    // The app's zoom gestures: drag a lag range to zoom to it, double-click
    // to go back. A plain click measures delta-t instead.
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || average_.size() < 2) return;
        dragging_ = true;
        dragA_ = dragB_ = lagMsAt(event->position().y());
        pressY_ = event->position().y();
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && onDoubleClick) onDoubleClick();
    }
    void measureAt(double ms) {
        // Delta-t measuring: first click, second click, third click clears.
        if (measureCount_ >= 2) measureCount_ = 0;
        measure_[measureCount_++] = ms;
        if (onMeasure) {
            onMeasure(measureCount_ == 2 ? QString("%1t = %2 ms (%3 to %4 ms)")
                                               .arg(QChar(0x0394))
                                               .arg(std::fabs(measure_[1] - measure_[0]), 0, 'f', 1)
                                               .arg(measure_[0], 0, 'f', 1)
                                               .arg(measure_[1], 0, 'f', 1)
                                         : QString("Click a second lag to measure %1t").arg(QChar(0x0394)));
        }
        update();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!dragging_) return;
        dragB_ = lagMsAt(event->position().y());
        update();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!dragging_ || event->button() != Qt::LeftButton) return;
        dragging_ = false;
        if (std::fabs(event->position().y() - pressY_) < 5.0) {
            measureAt(dragA_);
        } else if (onLagBox) {
            onLagBox(std::min(dragA_, dragB_), std::max(dragA_, dragB_));
        }
        update();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), tc(QColor(0x1e, 0x1f, 0x22)));
        QRect plot = plotRect();
        if (plot.width() <= 0 || plot.height() <= 0) return;
        painter.fillRect(plot, Qt::white);
        painter.setPen(QPen(kPlotGridColor, 1));
        for (const segy::AxisTick& tick :
             plotTimeTicks(sampleStart_, sampleEnd_, sampleIntervalUs_, plot.height(), plotAxisFont(this))) {
            int y = plot.top() + tick.pixelPos;
            if (y > plot.top() && y < plot.bottom()) painter.drawLine(plot.left(), y, plot.right(), y);
        }
        auto xFor = [&](double v) { return plot.left() + (v + 1.0) * 0.5 * (plot.width() - 1); };
        painter.setPen(QPen(tc(QColor(0xd0, 0xd2, 0xd6)), 1, Qt::DashLine));
        for (double guide : {-0.5, 0.5}) painter.drawLine(QPointF(xFor(guide), plot.top()), QPointF(xFor(guide), plot.bottom()));
        painter.setPen(QPen(tc(QColor(0x9a, 0x9c, 0xa3)), 1));
        painter.drawLine(QPointF(xFor(0.0), plot.top()), QPointF(xFor(0.0), plot.bottom()));
        if (sampleStart_ <= 0 && sampleEnd_ >= 0) {
            const double y0 = yForLagMs(0.0);
            painter.drawLine(QPointF(plot.left(), y0), QPointF(plot.right(), y0));
        }
        painter.save();
        painter.setClipRect(plot);
        if (average_.size() >= 2) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            QPolygonF line;
            const int n = int(average_.size());
            for (int i = 0; i < n; ++i) {
                // Same row mapping as rasterizeOctaveBand's stretched case:
                // sample i sits at row (i + 0.5) / n of the plot height.
                double y = plot.top() + (double(i) + 0.5) / n * plot.height();
                line << QPointF(xFor(std::clamp(double(average_[size_t(i)]), -1.0, 1.0)), y);
            }
            painter.setPen(QPen(QColor(0x1f, 0x4e, 0xd8), 1.6));
            painter.drawPolyline(line);
        }
        // Side peak (peak-to-peak distance), mirrored to both sides of lag 0.
        if (!std::isnan(peakLag_)) {
            const double ms = std::fabs(peakLag_) * sampleIntervalUs_ / 1000.0;
            painter.setPen(QPen(tc(QColor(0xe8, 0x90, 0x5a)), 1.5, Qt::DashLine));
            for (double s : {-1.0, 1.0}) {
                const double y = yForLagMs(s * ms);
                painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            }
        }
        // Delta-t measurement points.
        painter.setPen(QPen(QColor(0x19, 0x9e, 0x70), 1.5));
        for (int i = 0; i < measureCount_; ++i) {
            const double y = yForLagMs(measure_[i]);
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        if (dragging_) {
            const double ya = yForLagMs(dragA_), yb = yForLagMs(dragB_);
            painter.setPen(QPen(tc(QColor(0xe8, 0x90, 0x5a)), 1.5, Qt::DashLine));
            painter.setBrush(tc(QColor(0xe8, 0x90, 0x5a, 40)));
            painter.drawRect(QRectF(QPointF(plot.left(), ya), QPointF(plot.right(), yb)).normalized());
        }
        painter.restore();
        painter.setPen(tc(QColor(0xe0, 0xe2, 0xe7)));
        QFont axisFont = painter.font();
        axisFont.setPixelSize(10);
        painter.setFont(axisFont);
        painter.drawText(QRect(plot.left() - 10, plot.bottom() + 3, 20, 14), Qt::AlignCenter, "-1");
        painter.drawText(QRect(int(xFor(0.0)) - 10, plot.bottom() + 3, 20, 14), Qt::AlignCenter, "0");
        painter.drawText(QRect(plot.right() - 10, plot.bottom() + 3, 20, 14), Qt::AlignCenter, "1");
        painter.setFont(font());
        painter.drawText(QRect(plot.left(), plot.bottom() + 18, plot.width(), 16), Qt::AlignCenter, "Average");
    }

private:
    std::vector<float> average_;
    int sampleStart_ = 0;
    int sampleEnd_ = 0;
    double sampleIntervalUs_ = 0.0;
    double pressY_ = 0.0;
    double peakLag_ = std::nan("");
    bool boxMode_ = false;
    bool dragging_ = false;
    double dragA_ = 0.0, dragB_ = 0.0;
    double measure_[2] = {0.0, 0.0};
    int measureCount_ = 0;
};

} // namespace

AutocorrelationDialog::AutocorrelationDialog(QWidget* parent, bool crossCorrelation) : QDialog(parent) {
    setWindowTitle(crossCorrelation ? "Cross-correlation" : "Autocorrelation");
    setModal(false);
    QVBoxLayout* layout = new QVBoxLayout(this);

    QHBoxLayout* toolbarRow = new QHBoxLayout();
    auto lagSpin = [](const QString& tip) {
        QDoubleSpinBox* spin = new QDoubleSpinBox();
        spin->setRange(0.0, 10000.0);
        spin->setDecimals(0);
        spin->setSingleStep(20.0);
        spin->setSuffix(" ms");
        spin->setValue(200.0);
        spin->setToolTip(tip);
        return spin;
    };
    minLagSpin_ = lagSpin("Lags shown before 0 (the negative side)");
    maxLagSpin_ = lagSpin("Lags shown after 0 (the positive side)");
    toolbarRow->addWidget(new QLabel(QString("%1lag").arg(QChar(0x2212))));
    toolbarRow->addWidget(minLagSpin_);
    toolbarRow->addWidget(new QLabel("+lag"));
    toolbarRow->addWidget(maxLagSpin_);
    QPushButton* applyButton = new QPushButton("Recompute");
    toolbarRow->addWidget(applyButton);
    toolbarRow->addSpacing(12);
    QToolBar* zoomBar = new QToolBar(this);
    zoomBar->setVisible(false); // zoom is a gesture now (drag / double-click)
    zoomBar->setIconSize(QSize(18, 18));
    zoomAction_ = zoomBar->addAction(iconMagnifier(true), "Zoom");
    zoomAction_->setToolTip("Zoom the lag axis to the box (without one: drag one on the average curve)");
    unzoomAction_ = zoomBar->addAction(iconMagnifier(false), "Unzoom");
    unzoomAction_->setToolTip("Back to the previous lag window");
    boxAction_ = zoomBar->addAction(iconRectangle(), "Box Select");
    boxAction_->setCheckable(true);
    boxAction_->setToolTip("Drag on the average curve to select a lag range");
    toolbarRow->addWidget(zoomBar);
    spectrumCheck_ = new QCheckBox("Spectrum");
    spectrumCheck_->setToolTip("Power spectrum of the average, below the plots");
    toolbarRow->addWidget(spectrumCheck_);
    toolbarRow->addStretch();
    layout->addLayout(toolbarRow);

    QHBoxLayout* infoRow = new QHBoxLayout();
    peakLabel_ = new QLabel();
    peakLabel_->setStyleSheet(themed("color: #f0f2f5; font-weight: 600;"));
    infoRow->addWidget(peakLabel_);
    infoRow->addSpacing(16);
    infoLabel_ = new QLabel();
    infoLabel_->setStyleSheet(themed("color: #9a9ca3;"));
    infoRow->addWidget(infoLabel_, 1);
    layout->addLayout(infoRow);

    connect(applyButton, &QPushButton::clicked, this, [this]() { applyWindow(minLagSpin_->value(), maxLagSpin_->value()); });
    // Enter in a lag field recomputes too -- but only for a real edit
    // while the window is open. editingFinished also fires when the field
    // loses focus because the window is closing; recomputing then re-opened
    // the window (the compute's completion shows it), so it couldn't be
    // closed.
    for (QDoubleSpinBox* spin : {minLagSpin_, maxLagSpin_}) {
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this]() {
            if (!isVisible()) return;
            if (minLagSpin_->value() == lastAppliedMin_ && maxLagSpin_->value() == lastAppliedMax_) return;
            applyWindow(minLagSpin_->value(), maxLagSpin_->value());
        });
    }

    QHBoxLayout* plots = new QHBoxLayout();
    plots->setSpacing(0);
    auto* panelPlot = new OctaveBandPlotWidget();
    panelPlot->setVerticalTitle("Lag (ms)");
    panelPlot_ = panelPlot;
    auto* averagePlot = new AutocorrelationAverageWidget();
    averagePlot_ = averagePlot;
    plots->addWidget(panelPlot, 3);
    plots->addWidget(averagePlot, 1);
    layout->addLayout(plots, 3);
    auto* spectrum = new DbCurveChart();
    spectrum->setTitle("Power spectrum of the average (dB) vs. Frequency (Hz)");
    spectrum->setEmptyText("Apply to compute");
    spectrum->setMinimumHeight(200);
    spectrum->setVisible(false);
    spectrumChart_ = spectrum;
    layout->addWidget(spectrum, 2);
    connect(spectrumCheck_, &QCheckBox::toggled, this, [spectrum](bool on) { spectrum->setVisible(on); });

    averagePlot->onLagBox = [this](double fromMs, double toMs) {
        hasLagBox_ = true;
        lagBoxA_ = fromMs;
        lagBoxB_ = toMs;
        zoomToLagBox();
    };
    averagePlot->onDoubleClick = [this]() { unzoomAction_->trigger(); };
    averagePlot->onMeasure = [this](const QString& text) { infoLabel_->setText(text); };
    connect(boxAction_, &QAction::toggled, this, [averagePlot](bool on) { averagePlot->setBoxMode(on); });
    connect(zoomAction_, &QAction::triggered, this, [this]() {
        if (hasLagBox_) {
            zoomToLagBox();
            return;
        }
        // No box yet: draw one, and the window zooms as soon as it's there.
        zoomWhenBoxDrawn_ = true;
        if (!boxAction_->isChecked()) boxAction_->setChecked(true);
        infoLabel_->setText("Drag on the average curve to choose the lags");
    });
    connect(unzoomAction_, &QAction::triggered, this, [this]() {
        if (zoomHistory_.empty()) return;
        const auto previous = zoomHistory_.back();
        zoomHistory_.pop_back();
        minLagSpin_->setValue(previous.first);
        maxLagSpin_->setValue(previous.second);
        if (onApply_) onApply_(previous.first, previous.second);
        refreshZoomActions();
    });
    refreshZoomActions();
    resize(860, 720);
}

void AutocorrelationDialog::applyWindow(double minLagMs, double maxLagMs) {
    if (onApply_) onApply_(minLagMs, maxLagMs);
}

void AutocorrelationDialog::zoomToLagBox() {
    if (!hasLagBox_) return;
    zoomHistory_.push_back({minLagSpin_->value(), maxLagSpin_->value()});
    // The box's lags, signed: the negative side becomes -lag, the positive
    // side +lag (a box entirely on one side keeps a little of the other).
    const double from = lagBoxA_, to = lagBoxB_;
    minLagSpin_->setValue(std::max(0.0, -from));
    maxLagSpin_->setValue(std::max(0.0, to));
    hasLagBox_ = false;
    if (boxAction_->isChecked()) boxAction_->setChecked(false);
    applyWindow(minLagSpin_->value(), maxLagSpin_->value());
    refreshZoomActions();
}

void AutocorrelationDialog::refreshZoomActions() {
    zoomAction_->setEnabled(true);
    unzoomAction_->setEnabled(!zoomHistory_.empty());
}

double AutocorrelationDialog::minLagMs() const { return minLagSpin_->value(); }

double AutocorrelationDialog::maxLagMs() const { return maxLagSpin_->value(); }

void AutocorrelationDialog::setResult(const AutocorrelationResult& result) {
    auto* panelPlot = static_cast<OctaveBandPlotWidget*>(panelPlot_);
    panelPlot->setBandData(result.panel);
    panelPlot->setClip(1.0f); // normalized: an autocorrelation is exactly 1 at lag 0
    panelPlot->setAxisRange(result.traceStart, result.traceEnd, result.minLagSamples, result.maxLagSamples,
                            result.sampleIntervalUs);
    auto* averagePlot = static_cast<AutocorrelationAverageWidget*>(averagePlot_);
    averagePlot->setAverage(result.average, result.minLagSamples, result.maxLagSamples, result.sampleIntervalUs);
    averagePlot->setPeak(result.peakLagSamples);
    const double dtMs = result.sampleIntervalUs / 1000.0;
    lastAppliedMin_ = minLagSpin_->value();
    lastAppliedMax_ = maxLagSpin_->value();
    peakLabel_->setText(std::isnan(result.peakLagSamples)
                            ? QString("Peak-to-peak: no side peak %1 0.5").arg(QChar(0x2265))
                            : QString("Peak-to-peak: %1 ms (r = %2)")
                                  .arg(std::fabs(result.peakLagSamples) * dtMs, 0, 'f', 1)
                                  .arg(double(result.peakValue), 0, 'f', 2));
    infoLabel_->setText(QString("%1%2 traces, lags %3 to +%4 ms%5")
                            .arg(result.description.isEmpty() ? QString() : result.description + " | ")
                            .arg(result.traceCount)
                            .arg(result.minLagSamples * dtMs, 0, 'f', 0)
                            .arg(result.maxLagSamples * dtMs, 0, 'f', 0)
                            .arg(result.description.isEmpty() ? QString(", 1 at lag 0") : QString()));
    DbCurve curve;
    curve.name = "Power";
    curve.color = QColor(0x39, 0x87, 0xe5);
    curve.db = result.spectrumDb;
    static_cast<DbCurveChart*>(spectrumChart_)->setCurves(result.spectrumHz, {curve});
}

void AutocorrelationDialog::setApplyCallback(std::function<void(double, double)> callback) { onApply_ = std::move(callback); }

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
                                    std::function<void()> onParameters,
                                    std::function<void(bool)> onCrosshairToggled = nullptr) {
    // One menu: what the window shows, and its parameters. (File > Close
    // and an empty Edit menu only repeated the window's own close button.)
    QMenuBar* menuBar = new QMenuBar(window);
    QMenu* viewMenu = menuBar->addMenu("&View");
    QAction* gridAction = viewMenu->addAction("&Grid Lines");
    gridAction->setCheckable(true);
    gridAction->setChecked(true);
    QObject::connect(gridAction, &QAction::toggled, window, std::move(onGridLinesToggled));
    if (onCrosshairToggled) {
        QAction* crosshairAction = viewMenu->addAction("&Crosshair");
        crosshairAction->setCheckable(true);
        crosshairAction->setChecked(true);
        QObject::connect(crosshairAction, &QAction::toggled, window, std::move(onCrosshairToggled));
    }
    if (onParameters) {
        viewMenu->addSeparator();
        QAction* paramsAction = viewMenu->addAction("&Parameters...");
        QObject::connect(paramsAction, &QAction::triggered, window, std::move(onParameters));
    }
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
    painter.fillRect(bottomChart, tc(QColor(Qt::black)));
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

namespace {
// "Axis Ranges" dialog shared by the dB charts and the seismic display:
// min/max for both axes. Returns false on Cancel. Values are swapped into
// order on OK; equal ends are refused by keeping the dialog open.
bool editAxisRanges(QWidget* parent, const QString& title, const QString& xName, const QString& yName, double& x0,
                    double& x1, double& y0, double& y1, int xDecimals, int yDecimals) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QGridLayout* grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    auto spin = [](double value, int decimals) {
        QDoubleSpinBox* box = new QDoubleSpinBox();
        box->setRange(-1e9, 1e9);
        box->setDecimals(decimals);
        box->setValue(value);
        box->setMinimumWidth(110);
        return box;
    };
    QDoubleSpinBox* x0Spin = spin(x0, xDecimals);
    QDoubleSpinBox* x1Spin = spin(x1, xDecimals);
    QDoubleSpinBox* y0Spin = spin(y0, yDecimals);
    QDoubleSpinBox* y1Spin = spin(y1, yDecimals);
    QLabel* minHeading = new QLabel("Min");
    QLabel* maxHeading = new QLabel("Max");
    for (QLabel* heading : {minHeading, maxHeading}) heading->setStyleSheet(themed("color: #7a7c82;"));
    grid->addWidget(minHeading, 0, 1);
    grid->addWidget(maxHeading, 0, 2);
    grid->addWidget(new QLabel(xName), 1, 0);
    grid->addWidget(x0Spin, 1, 1);
    grid->addWidget(x1Spin, 1, 2);
    grid->addWidget(new QLabel(yName), 2, 0);
    grid->addWidget(y0Spin, 2, 1);
    grid->addWidget(y1Spin, 2, 2);
    layout->addLayout(grid);
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();
    QPushButton* ok = new QPushButton("OK");
    QPushButton* cancel = new QPushButton("Cancel");
    ok->setDefault(true);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    QObject::connect(ok, &QPushButton::clicked, &dialog, [&]() {
        if (x0Spin->value() == x1Spin->value() || y0Spin->value() == y1Spin->value()) return; // empty range
        dialog.accept();
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    x0 = std::min(x0Spin->value(), x1Spin->value());
    x1 = std::max(x0Spin->value(), x1Spin->value());
    y0 = std::min(y0Spin->value(), y1Spin->value());
    y1 = std::max(y0Spin->value(), y1Spin->value());
    return true;
}
} // namespace

ZoomableChart::ZoomableChart(QWidget* parent) : QWidget(parent) {}

void ZoomableChart::linkGroup(const std::vector<ZoomableChart*>& group) { group_ = group; }

std::vector<ZoomableChart*> ZoomableChart::peers() const {
    if (group_.empty()) return {const_cast<ZoomableChart*>(this)};
    return group_;
}

void ZoomableChart::setZoomStateCallback(std::function<void()> callback) { onZoomState_ = std::move(callback); }

void ZoomableChart::notifyZoomState() {
    for (ZoomableChart* chart : peers()) {
        if (chart->onZoomState_) chart->onZoomState_();
    }
}

bool ZoomableChart::canZoom() const {
    for (ZoomableChart* chart : peers()) {
        if (chart->hasBox_) return true;
    }
    return false;
}

void ZoomableChart::setBoxMode(bool on) {
    for (ZoomableChart* chart : peers()) chart->applyBoxMode(on);
}

void ZoomableChart::applyBoxMode(bool on) {
    boxMode_ = on;
    draggingBox_ = false;
    setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void ZoomableChart::setAutoRange(const Range& range) {
    auto_ = range;
    if (!manualRange_) view_ = auto_;
}

void ZoomableChart::zoomToBox() {
    for (ZoomableChart* chart : peers()) {
        if (!chart->hasBox_) continue;
        const Range box{std::min(chart->boxA_.x(), chart->boxB_.x()), std::max(chart->boxA_.x(), chart->boxB_.x()),
                        std::min(chart->boxA_.y(), chart->boxB_.y()), std::max(chart->boxA_.y(), chart->boxB_.y())};
        for (ZoomableChart* peer : peers()) peer->hasBox_ = false;
        setAxisRanges(box.x0, box.x1, box.y0, box.y1);
        return;
    }
}

void ZoomableChart::setAxisRanges(double x0, double x1, double y0, double y1) {
    if (!(x1 > x0) || !(y1 > y0)) return;
    for (ZoomableChart* chart : peers()) chart->applyAxisRanges(Range{x0, x1, y0, y1});
    notifyZoomState();
}

void ZoomableChart::applyAxisRanges(const Range& range) {
    if (manualRange_) history_.push_back(view_);
    manualRange_ = true;
    view_ = range;
    viewChanged();
}

void ZoomableChart::unzoom() {
    for (ZoomableChart* chart : peers()) chart->applyUnzoom();
    notifyZoomState();
}

void ZoomableChart::applyUnzoom() {
    if (!manualRange_) return;
    if (history_.empty()) {
        manualRange_ = false;
        view_ = auto_;
    } else {
        view_ = history_.back();
        history_.pop_back();
    }
    viewChanged();
}

void ZoomableChart::resetAxes() {
    for (ZoomableChart* chart : peers()) chart->applyResetAxes();
    notifyZoomState();
}

void ZoomableChart::applyResetAxes() {
    history_.clear();
    manualRange_ = false;
    view_ = auto_;
    hasBox_ = false;
    viewChanged();
}

QPointF ZoomableChart::dataAt(const QPointF& pos) const {
    const QRect chart = chartRect();
    const double fx = (pos.x() - chart.left()) / std::max(1, chart.width());
    const double fy = (chart.bottom() - pos.y()) / std::max(1, chart.height());
    return QPointF(view_.x0 + fx * (view_.x1 - view_.x0), view_.y0 + fy * (view_.y1 - view_.y0));
}

QPointF ZoomableChart::pixelAt(double x, double y) const {
    const QRect chart = chartRect();
    return QPointF(chart.left() + (x - view_.x0) / (view_.x1 - view_.x0) * chart.width(),
                   chart.bottom() - (y - view_.y0) / (view_.y1 - view_.y0) * chart.height());
}

bool ZoomableChart::handleBoxPress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !boxMode_ || !chartRect().contains(event->position().toPoint())) return false;
    for (ZoomableChart* chart : peers()) {
        if (chart != this && chart->hasBox_) {
            chart->hasBox_ = false;
            chart->update();
        }
    }
    boxA_ = boxB_ = dataAt(event->position());
    draggingBox_ = true;
    hasBox_ = false;
    update();
    notifyZoomState();
    return true;
}

bool ZoomableChart::handleBoxMove(QMouseEvent* event) {
    if (!draggingBox_) return false;
    const QRect chart = chartRect();
    const QPointF pos = event->position();
    boxB_ = dataAt(QPointF(std::clamp(pos.x(), double(chart.left()), double(chart.right())),
                           std::clamp(pos.y(), double(chart.top()), double(chart.bottom()))));
    update();
    return true;
}

bool ZoomableChart::handleBoxRelease(QMouseEvent* event) {
    if (!draggingBox_ || event->button() != Qt::LeftButton) return false;
    draggingBox_ = false;
    // A click without a drag is no box.
    const QPointF a = pixelAt(boxA_.x(), boxA_.y()), b = pixelAt(boxB_.x(), boxB_.y());
    hasBox_ = std::fabs(a.x() - b.x()) >= 4.0 && std::fabs(a.y() - b.y()) >= 4.0;
    if (hasBox_) {
        zoomToBox(); // the box is the zoom: no separate Zoom step
        return true;
    }
    update();
    notifyZoomState();
    return true;
}

void ZoomableChart::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && chartRect().contains(event->position().toPoint())) unzoom();
}

void ZoomableChart::wheelEvent(QWheelEvent* event) {
    const QPointF pos = event->position();
    if (!chartRect().contains(pos.toPoint())) return;
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0) return;
    const double factor = std::pow(1.15, double(-notches));
    const QPointF at = dataAt(pos);
    const Range range{at.x() + (view_.x0 - at.x()) * factor, at.x() + (view_.x1 - at.x()) * factor,
                      at.y() + (view_.y0 - at.y()) * factor, at.y() + (view_.y1 - at.y()) * factor};
    // One wheel gesture is one step back for Previous Zoom, not one per notch.
    static QElapsedTimer clock;
    if (!clock.isValid()) clock.start();
    const bool newGesture = clock.elapsed() - lastWheelMs_ > 600;
    lastWheelMs_ = clock.elapsed();
    for (ZoomableChart* chart : peers()) chart->applyWheelRange(range, newGesture);
    notifyZoomState();
}

void ZoomableChart::applyWheelRange(const Range& range, bool newGesture) {
    if (newGesture && manualRange_) history_.push_back(view_);
    manualRange_ = true;
    view_ = range;
    viewChanged();
}

void ZoomableChart::paintBox(QPainter& painter) const {
    if (!hasBox_ && !draggingBox_) return;
    painter.save();
    painter.setClipRect(chartRect());
    painter.setPen(QPen(tc(QColor(0xe8, 0x90, 0x5a)), 1.5, Qt::DashLine));
    painter.setBrush(tc(QColor(0xe8, 0x90, 0x5a, 40)));
    painter.drawRect(QRectF(pixelAt(boxA_.x(), boxA_.y()), pixelAt(boxB_.x(), boxB_.y())).normalized());
    painter.restore();
}

void ZoomableChart::showAxisMenu(const QPoint& globalPos, const QString& xName, const QString& yName, int xDecimals,
                                 int yDecimals) {
    // The same menu as the seismic display's.
    QMenu menu(this);
    QAction* previous = menu.addAction("Previous Zoom");
    previous->setEnabled(manualRange_);
    QAction* reset = menu.addAction("Full View");
    reset->setEnabled(manualRange_);
    menu.addSeparator();
    QAction* ranges = menu.addAction("Axis Ranges...");
    QAction* chosen = menu.exec(globalPos);
    if (chosen == previous) {
        unzoom();
    } else if (chosen == reset) {
        resetAxes();
    } else if (chosen == ranges) {
        double x0 = view_.x0, x1 = view_.x1, y0 = view_.y0, y1 = view_.y1;
        if (editAxisRanges(this, "Axis Ranges", xName, yName, x0, x1, y0, y1, xDecimals, yDecimals)) {
            setAxisRanges(x0, x1, y0, y1);
        }
    }
}

DbCurveChart::DbCurveChart(QWidget* parent) : ZoomableChart(parent) {
    setMouseTracking(true);
    setMinimumSize(360, 260);
    auto_ = view_ = Range{0.0, 1.0, -60.0, 0.0};
}

void DbCurveChart::setCurves(std::vector<float> frequencyHz, std::vector<DbCurve> curves) {
    frequencyHz_ = std::move(frequencyHz);
    curves_ = std::move(curves);
    hoverIndex_ = -1;
    updateRange(); // a manual zoom/axis range survives a recompute
    update();
    if (onHover_) onHover_(-1, 0.0);
}

void DbCurveChart::setCurveVisible(int index, bool visible) {
    if (index < 0 || index >= int(curves_.size())) return;
    curves_[size_t(index)].visible = visible;
    updateRange();
    update();
}

void DbCurveChart::setTitle(const QString& title) {
    title_ = title;
    update();
}

void DbCurveChart::setEmptyText(const QString& text) {
    emptyText_ = text;
    update();
}

void DbCurveChart::setGridLinesVisible(bool visible) {
    gridLinesVisible_ = visible;
    update();
}

void DbCurveChart::setCrosshairEnabled(bool enabled) {
    crosshairEnabled_ = enabled;
    if (!enabled && hoverIndex_ >= 0) {
        hoverIndex_ = -1;
        if (onHover_) onHover_(-1, 0.0);
    }
    update();
}

void DbCurveChart::setHoverCallback(std::function<void(int, double)> callback) { onHover_ = std::move(callback); }

void DbCurveChart::viewChanged() {
    hoverIndex_ = -1;
    update();
}

QRect DbCurveChart::chartRect() const {
    const int marginLeft = 60, marginRight = 20, marginTop = 30, marginBottom = 54;
    return QRect(marginLeft, marginTop, width() - marginLeft - marginRight, height() - marginTop - marginBottom);
}

void DbCurveChart::updateRange() {
    float lo = std::numeric_limits<float>::infinity(), hi = -std::numeric_limits<float>::infinity();
    for (const DbCurve& curve : curves_) {
        if (!curve.visible) continue;
        for (float v : curve.db) {
            if (!std::isfinite(v)) continue;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (!std::isfinite(lo) || !std::isfinite(hi)) {
        lo = -60.0f;
        hi = 0.0f;
    }
    // A near-empty bin (e.g. DC after DC removal) shouldn't flatten everything else.
    lo = std::max(lo, hi - 120.0f);
    Range range;
    range.y1 = std::ceil(double(hi) / 5.0) * 5.0 + 5.0;
    range.y0 = std::floor(double(lo) / 10.0) * 10.0;
    if (range.y1 - range.y0 < 10.0) range.y0 = range.y1 - 10.0;
    range.x0 = 0.0;
    range.x1 = frequencyHz_.empty() || frequencyHz_.back() <= 0.0f ? 1.0 : double(frequencyHz_.back());
    setAutoRange(range);
}

void DbCurveChart::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kCanvasSurroundColor);
    const QColor gridColor(0x3c, 0x3f, 0x41);

    if (frequencyHz_.size() < 2 || frequencyHz_.back() <= 0.0f || curves_.empty()) {
        painter.setPen(kSecondaryTextColor);
        painter.drawText(rect().adjusted(20, 0, -20, 0), Qt::AlignCenter | Qt::TextWordWrap, emptyText_);
        return;
    }

    QFont font = axisFont();
    painter.setFont(font);
    QFontMetrics fm(font);
    auto measureWidth = [&fm](const std::string& text) {
        return text.empty() ? 0 : fm.horizontalAdvance(QString::fromStdString(text));
    };

    const QRect chart = chartRect();
    if (chart.width() <= 0 || chart.height() <= 0) return;
    const Range v = view_;
    auto xFor = [&](double f) { return chart.left() + (f - v.x0) / (v.x1 - v.x0) * chart.width(); };
    auto yFor = [&](double db) { return chart.bottom() - (db - v.y0) / (v.y1 - v.y0) * chart.height(); };

    constexpr int kMaxLines = 11;
    std::vector<segy::AxisTick> yTicks = segy::computeNiceAxisTicks(v.y0, v.y1, chart.height(), kMaxLines, measureWidth);
    std::vector<segy::AxisTick> xTicks = segy::computeNiceAxisTicks(v.x0, v.x1, chart.width(), kMaxLines, measureWidth);

    painter.setPen(kSecondaryTextColor);
    painter.drawText(QRect(0, chart.top() - 22, width(), 20), Qt::AlignHCenter, title_);
    drawHorizontalGridlinesAndLabels(painter, chart, yTicks, gridColor, kSecondaryTextColor, chart.left() - 6,
                                      gridLinesVisible_);
    drawVerticalGridlines(painter, chart, xTicks, gridColor, gridLinesVisible_);
    painter.setPen(gridColor);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(chart);

    painter.save();
    painter.setClipRect(chart.adjusted(1, 1, -1, -1));
    for (const DbCurve& curve : curves_) {
        if (!curve.visible) continue;
        QPen pen(curve.color, 2.0);
        if (curve.dashed) pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        QPolygonF run;
        const size_t count = std::min(curve.db.size(), frequencyHz_.size());
        for (size_t i = 0; i <= count; ++i) {
            bool valid = i < count && std::isfinite(curve.db[i]);
            if (valid) {
                run << QPointF(xFor(frequencyHz_[i]), yFor(curve.db[i]));
            } else if (!run.isEmpty()) {
                // NaN breaks the line: draw what we have, start a new run.
                if (run.size() > 1) painter.drawPolyline(run);
                run.clear();
            }
        }
    }
    painter.restore();
    paintBox(painter);

    drawXAxisTicksAndLabels(painter, chart, xTicks, kSecondaryTextColor, measureWidth);
    painter.setPen(kSecondaryTextColor);
    painter.drawText(QRect(chart.left(), height() - 20, chart.width(), 20), Qt::AlignHCenter, "Frequency (Hz)");

    // Crosshair: same black-underlay + white-dash look as the Octave Band
    // Display's, snapped to the hovered frequency bin, with a marker on
    // every visible curve there.
    if (crosshairEnabled_ && hoverIndex_ >= 0 && hoverIndex_ < int(frequencyHz_.size()) && !draggingBox_) {
        const double x = xFor(frequencyHz_[size_t(hoverIndex_)]);
        const double y = std::clamp(hoverY_, double(chart.top()), double(chart.bottom()));
        painter.save();
        painter.setClipRect(chart);
        for (const QPen& pen : {QPen(QColor(0, 0, 0, 190), 3), QPen(Qt::white, 1, Qt::DashLine)}) {
            painter.setPen(pen);
            painter.drawLine(QPointF(x, chart.top()), QPointF(x, chart.bottom()));
            painter.drawLine(QPointF(chart.left(), y), QPointF(chart.right(), y));
        }
        for (const DbCurve& curve : curves_) {
            if (!curve.visible || size_t(hoverIndex_) >= curve.db.size()) continue;
            const float val = curve.db[size_t(hoverIndex_)];
            if (!std::isfinite(val) || val < v.y0 || val > v.y1) continue;
            painter.setPen(QPen(QColor(kCanvasSurroundColor), 2)); // surface ring
            painter.setBrush(curve.color);
            painter.drawEllipse(QPointF(x, yFor(val)), 4.5, 4.5);
        }
        painter.restore();
    }
}

void DbCurveChart::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        if (frequencyHz_.size() >= 2) {
            showAxisMenu(event->globalPosition().toPoint(), "Frequency (Hz)", "Level (dB)", 1, 1);
        }
        return;
    }
    if (frequencyHz_.size() >= 2) handleBoxPress(event);
}

void DbCurveChart::mouseReleaseEvent(QMouseEvent* event) { handleBoxRelease(event); }

void DbCurveChart::mouseMoveEvent(QMouseEvent* event) {
    if (handleBoxMove(event)) return;
    const QRect chart = chartRect();
    const QPointF pos = event->position();
    if (!crosshairEnabled_ || frequencyHz_.size() < 2 || frequencyHz_.back() <= 0.0f || !chart.contains(pos.toPoint())) {
        if (hoverIndex_ >= 0) {
            hoverIndex_ = -1;
            update();
            if (onHover_) onHover_(-1, 0.0);
        }
        return;
    }
    // Frequencies are evenly spaced (FFT bins): nearest bin directly.
    const QPointF point = dataAt(pos);
    const double df = frequencyHz_[1] - frequencyHz_[0];
    hoverIndex_ = std::clamp(int(std::lround((point.x() - frequencyHz_[0]) / df)), 0, int(frequencyHz_.size()) - 1);
    hoverY_ = pos.y();
    update();
    if (onHover_) onHover_(hoverIndex_, point.y());
}

void DbCurveChart::leaveEvent(QEvent*) {
    if (hoverIndex_ < 0) return;
    hoverIndex_ = -1;
    update();
    if (onHover_) onHover_(-1, 0.0);
}

// Zoom / Unzoom / Box Select for a chart, the same three buttons (and
// icons) as the seismic toolbar's, as a slim strip above the chart.
QToolBar* buildChartZoomToolbar(ZoomableChart* chart, QWidget* parent) {
    QToolBar* bar = new QToolBar(parent);
    bar->setVisible(false); // zoom is a gesture now: drag, double-click, wheel, right-click
    bar->setIconSize(QSize(18, 18));
    bar->setMovable(false);
    bar->setStyleSheet(themed("QToolBar { background: #141416; border: none; padding: 2px 8px; spacing: 4px; }"));
    QAction* zoom = bar->addAction(iconMagnifier(true), "Zoom");
    zoom->setToolTip("Zoom to the box");
    QAction* unzoom = bar->addAction(iconMagnifier(false), "Unzoom");
    unzoom->setToolTip("Undo the last zoom (back to the full range at the end)");
    QAction* box = bar->addAction(iconRectangle(), "Box Select");
    box->setCheckable(true);
    box->setToolTip("Drag a box on the chart to zoom into");
    QObject::connect(zoom, &QAction::triggered, chart, [chart]() { chart->zoomToBox(); });
    QObject::connect(unzoom, &QAction::triggered, chart, [chart]() { chart->unzoom(); });
    QObject::connect(box, &QAction::toggled, chart, [chart](bool on) { chart->setBoxMode(on); });
    auto refresh = [chart, zoom, unzoom]() {
        zoom->setEnabled(chart->canZoom());
        unzoom->setEnabled(chart->canUnzoom());
    };
    chart->setZoomStateCallback(refresh);
    refresh();
    return bar;
}

namespace {
// Trace Info dock look (see MainWindow's dock): small caps section heading,
// caption left, monospace value right.
QLabel* readoutSectionLabel(const QString& text) {
    QLabel* label = new QLabel(text);
    label->setStyleSheet(themed("color: #7a7c82; font-size: 11px; letter-spacing: 1px; font-weight: 600;"));
    return label;
}

QWidget* readoutValueRow(QWidget* swatch, const QString& caption, QLabel*& valueOut) {
    QWidget* row = new QWidget;
    QHBoxLayout* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);
    if (swatch) rowLayout->addWidget(swatch);
    QLabel* captionLabel = new QLabel(caption);
    captionLabel->setStyleSheet(themed("color: #9a9ca3;"));
    rowLayout->addWidget(captionLabel);
    rowLayout->addStretch();
    valueOut = new QLabel("--");
    valueOut->setStyleSheet(themed("color: #d4d4d8; font-family: monospace;"));
    rowLayout->addWidget(valueOut);
    return row;
}

// A short line sample in the curve's color (dashed for dashed curves) --
// identity for the readout rows and the Signal/Noise toggles.
QPixmap curveSwatch(const QColor& color, bool dashed) {
    QPixmap pixmap(18, 10);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 2.5);
    if (dashed) pen.setDashPattern({2.0, 1.5});
    painter.setPen(pen);
    painter.drawLine(QPointF(1, 5), QPointF(17, 5));
    return pixmap;
}
} // namespace

CurveReadoutPanel::CurveReadoutPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("curveReadout");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(themed("#curveReadout { background-color: #1a1b1e; border-left: 1px solid #2b2d31; }"));
    setFixedWidth(210);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);
    layout->addWidget(readoutSectionLabel("CURSOR"));
    layout->addWidget(readoutValueRow(nullptr, "Frequency", frequencyValue_));
    layout->addWidget(readoutValueRow(nullptr, "Level", cursorValue_));
    layout->addSpacing(12);
    layout->addWidget(readoutSectionLabel("AT FREQUENCY"));
    curveRows_ = new QWidget;
    QVBoxLayout* rowsLayout = new QVBoxLayout(curveRows_);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(8);
    layout->addWidget(curveRows_);
    layout->addStretch();
    QLabel* hint = new QLabel("Hover over the chart to read values.");
    hint->setWordWrap(true);
    hint->setStyleSheet(themed("color: #68696e; font-style: italic; font-size: 11px;"));
    layout->addWidget(hint);
}

void CurveReadoutPanel::syncRows(const DbCurveChart& chart) {
    const std::vector<DbCurve>& curves = chart.curves();
    bool sameShape = rows_.size() == curves.size();
    if (!sameShape) {
        for (auto& row : rows_) delete row.first;
        rows_.clear();
        for (const DbCurve& curve : curves) {
            QLabel* swatch = new QLabel;
            swatch->setPixmap(curveSwatch(curve.color, curve.dashed));
            QLabel* value = nullptr;
            QWidget* row = readoutValueRow(swatch, curve.name, value);
            curveRows_->layout()->addWidget(row);
            rows_.push_back({row, value});
        }
    }
    for (size_t i = 0; i < curves.size(); ++i) rows_[i].first->setVisible(curves[i].visible);
    showValues(chart, -1, 0.0);
}

void CurveReadoutPanel::showValues(const DbCurveChart& chart, int frequencyIndex, double cursorDb) {
    const std::vector<float>& freqs = chart.frequencies();
    const std::vector<DbCurve>& curves = chart.curves();
    const bool valid = frequencyIndex >= 0 && frequencyIndex < int(freqs.size());
    frequencyValue_->setText(valid ? QString("%1 Hz").arg(freqs[size_t(frequencyIndex)], 0, 'f', 1) : "--");
    cursorValue_->setText(valid ? QString("%1 dB").arg(cursorDb, 0, 'f', 1) : "--");
    for (size_t i = 0; i < rows_.size() && i < curves.size(); ++i) {
        QString text = "--";
        if (valid && size_t(frequencyIndex) < curves[i].db.size()) {
            float v = curves[i].db[size_t(frequencyIndex)];
            if (std::isfinite(v)) text = QString("%1 dB").arg(double(v), 0, 'f', 1);
        }
        rows_[i].second->setText(text);
    }
}

SpectrumPlotWidget::SpectrumPlotWidget(QWidget* parent) : QWidget(parent) {
    setWindowTitle("Spectrum");
    resize(900, 480);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    chart_ = new DbCurveChart(this);
    chart_->setEmptyText("No data yet -- open a file, then click Apply or OK in Spectrum Parameters.");
    readout_ = new CurveReadoutPanel(this);
    chart_->setHoverCallback([this](int index, double cursorDb) { readout_->showValues(*chart_, index, cursorDb); });
    layout->addWidget(buildResultWindowMenuBar(
        this, [this](bool checked) { chart_->setGridLinesVisible(checked); },
        [this]() {
            if (onParameters_) onParameters_();
        },
        [this](bool checked) {
            chart_->setCrosshairEnabled(checked);
            readout_->setVisible(checked);
        }));
    layout->addWidget(buildChartZoomToolbar(chart_, this));
    QHBoxLayout* body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(chart_, 1);
    body->addWidget(readout_);
    layout->addLayout(body, 1);
}

void SpectrumPlotWidget::setResult(const segy::SpectrumResult& result, QColor sourceColor) {
    DbCurve curve;
    curve.name = "Amplitude";
    curve.color = sourceColor;
    curve.db = result.amplitudeDb;
    chart_->setCurves(result.frequencyHz, {curve});
    readout_->syncRows(*chart_);
}

void SpectrumPlotWidget::setParametersCallback(std::function<void()> callback) { onParameters_ = std::move(callback); }

namespace {
// Dark-surface categorical steps (validated: lightness, chroma, CVD and
// contrast against the dark chart surface). Trace is the neutral context
// line; S/N is also dashed so it never relies on hue alone.
const QColor kSnTraceColor(0xc3, 0xc2, 0xb7);
const QColor kSnSignalColor(0x39, 0x87, 0xe5);
const QColor kSnNoiseColor(0xd9, 0x59, 0x26);
const QColor kSnRatioColor(0x19, 0x9e, 0x70);
} // namespace

SignalNoiseWindow::SignalNoiseWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setWindowTitle("Signal/Noise");
    resize(1000, 620);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    chart_ = new DbCurveChart(this);
    chart_->setTitle("Level (dB re trace peak) and S/N (dB) vs. Frequency (Hz)");
    chart_->setEmptyText("Computing...");
    readout_ = new CurveReadoutPanel(this);
    chart_->setHoverCallback([this](int index, double cursorDb) { readout_->showValues(*chart_, index, cursorDb); });
    layout->addWidget(buildResultWindowMenuBar(
        this, [this](bool checked) { chart_->setGridLinesVisible(checked); }, nullptr,
        [this](bool checked) {
            chart_->setCrosshairEnabled(checked);
            readout_->setVisible(checked);
        }));

    QWidget* header = new QWidget(this);
    header->setStyleSheet(themed("background-color: #141416;"));
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 10, 14, 10);
    headerLayout->setSpacing(16);

    QVBoxLayout* left = new QVBoxLayout();
    left->setSpacing(8);
    // Curve toggles: a centred row along the bottom of the window, like a
    // legend under the chart (built here, placed below).
    QWidget* legendBar = new QWidget(this);
    legendBar->setStyleSheet(themed("background-color: #141416;"));
    QHBoxLayout* toggles = new QHBoxLayout(legendBar);
    toggles->setContentsMargins(14, 8, 14, 8);
    toggles->setSpacing(24);
    toggles->addStretch();
    const char* names[4] = {"Trace", "Signal", "Noise", "S/N"};
    const QColor colors[4] = {kSnTraceColor, kSnSignalColor, kSnNoiseColor, kSnRatioColor};
    for (int i = 0; i < 4; ++i) {
        QCheckBox* check = new QCheckBox(names[i]);
        check->setIcon(QIcon(curveSwatch(colors[i], i == 3)));
        check->setIconSize(QSize(18, 10));
        check->setChecked(i == 1 || i == 2); // signal and noise by default
        connect(check, &QCheckBox::toggled, this, [this, i](bool on) {
            chart_->setCurveVisible(i, on);
            readout_->syncRows(*chart_);
        });
        curveChecks_[i] = check;
        toggles->addWidget(check);
    }
    toggles->addStretch();

    QHBoxLayout* params = new QHBoxLayout();
    params->setSpacing(8);
    auto caption = [](const QString& text) {
        QLabel* label = new QLabel(text);
        label->setStyleSheet(themed("color: #9a9ca3;"));
        return label;
    };
    distanceSpin_ = new QSpinBox();
    distanceSpin_->setRange(0, 200);
    distanceSpin_->setSuffix(" traces");
    distanceSpin_->setToolTip("Skipped traces: traces between the two traces of a pair, so the pair distance is "
                              "this + 1 (0 = adjacent traces). (SIGCOR NSKIP)");
    minCorrelationSpin_ = new QDoubleSpinBox();
    minCorrelationSpin_->setRange(0.0, 1.0);
    minCorrelationSpin_->setDecimals(2);
    minCorrelationSpin_->setSingleStep(0.05);
    minCorrelationSpin_->setToolTip("Pairs whose normalized correlation peak is below this are left out. (SIGCOR CCMIN)");
    params->addWidget(caption("Skipped traces"));
    params->addWidget(distanceSpin_);
    params->addSpacing(16);
    params->addWidget(caption("Min correlation"));
    params->addWidget(minCorrelationSpin_);
    params->addStretch();
    left->addLayout(params);
    headerLayout->addLayout(left, 1);
    auto edited = [this]() {
        if (onParamsEdited_) onParamsEdited_();
    };
    connect(distanceSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, edited);
    connect(minCorrelationSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, edited);

    // Time-shift statistics (opens the details table) above Recompute, same size.
    QVBoxLayout* right = new QVBoxLayout();
    right->setSpacing(8);
    shiftButton_ = new QPushButton();
    shiftButton_->setToolTip("Mean and 1 sigma of the time shifts between the trace pairs -- click for details");
    QHBoxLayout* shiftButtonLayout = new QHBoxLayout(shiftButton_);
    shiftButtonLayout->setContentsMargins(12, 0, 12, 0);
    shiftLabel_ = new QLabel();
    shiftLabel_->setTextFormat(Qt::RichText);
    shiftLabel_->setAlignment(Qt::AlignCenter);
    shiftLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    shiftLabel_->setStyleSheet(themed("background: transparent; color: #d4d4d8;"));
    shiftButtonLayout->addWidget(shiftLabel_);
    connect(shiftButton_, &QPushButton::clicked, this, [this]() { showDetails(); });
    recomputeButton_ = new QPushButton("Recompute");
    recomputeButton_->setToolTip("Recompute from the active panel's selection box, or its visible data without one");
    connect(recomputeButton_, &QPushButton::clicked, this, [this]() {
        if (onRecompute_) onRecompute_();
    });
    right->addWidget(shiftButton_);
    right->addWidget(recomputeButton_);
    headerLayout->addLayout(right);
    layout->addWidget(header);
    layout->addWidget(buildChartZoomToolbar(chart_, this));

    QHBoxLayout* body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    // The toggles sit under the chart only (centred on it), the readout
    // panel runs the full height beside both.
    QVBoxLayout* chartColumn = new QVBoxLayout();
    chartColumn->setContentsMargins(0, 0, 0, 0);
    chartColumn->setSpacing(0);
    chartColumn->addWidget(chart_, 1);
    chartColumn->addWidget(legendBar);
    body->addLayout(chartColumn, 1);
    body->addWidget(readout_);
    layout->addLayout(body, 1);

    setWindowParams(segy::SigcorParams{});
    setResult(SignalNoiseDisplay{});
}

void SignalNoiseWindow::setResult(const SignalNoiseDisplay& display) {
    if (display.frequencyHz.empty()) {
        chart_->setEmptyText(display.emptyReason.isEmpty() ? QString("Computing...") : display.emptyReason);
        chart_->setCurves({}, {});
    } else {
        std::vector<DbCurve> curves(4);
        curves[0] = {"Trace", kSnTraceColor, display.traceDb, curveChecks_[0]->isChecked(), false};
        curves[1] = {"Signal", kSnSignalColor, display.signalDb, curveChecks_[1]->isChecked(), false};
        curves[2] = {"Noise", kSnNoiseColor, display.noiseDb, curveChecks_[2]->isChecked(), false};
        curves[3] = {"S/N", kSnRatioColor, display.snDb, curveChecks_[3]->isChecked(), true};
        chart_->setCurves(display.frequencyHz, std::move(curves));
    }
    // Delta t = mean +/- 1 sigma, typeset like the LaTeX $\Delta t = -1.14 \pm 2.33\,\text{ms}$.
    auto signedMs = [](double v) {
        QString text = QString::number(std::fabs(v), 'f', 2);
        return v < 0.0 && text != "0.00" ? "&minus;" + text : text;
    };
    if (display.hasShift) {
        shiftLabel_->setText(QString("<i>&Delta;t</i> = %1 &plusmn; %2 ms")
                                 .arg(signedMs(display.shiftMeanMs))
                                 .arg(QString::number(display.shiftSigmaMs, 'f', 2)));
    } else {
        shiftLabel_->setText("<i>&Delta;t</i> = &ndash;");
    }
    details_ = display.details;
    shiftButton_->setEnabled(!details_.empty());
    if (detailsDialog_ && detailsDialog_->isVisible()) showDetails();
    matchButtonSizes();
    readout_->syncRows(*chart_);
}

void SignalNoiseWindow::matchButtonSizes() {
    const QSize label = shiftLabel_->sizeHint();
    const QSize recompute = recomputeButton_->sizeHint();
    const int w = std::max({label.width() + 28, recompute.width(), 150});
    const int h = std::max(label.height() + 12, recompute.height());
    shiftButton_->setFixedSize(w, h);
    recomputeButton_->setFixedSize(w, h);
}

void SignalNoiseWindow::showDetails() {
    if (details_.empty()) return;
    if (!detailsDialog_) {
        detailsDialog_ = new QDialog(this);
        detailsDialog_->setWindowTitle("Signal/Noise details");
        QVBoxLayout* layout = new QVBoxLayout(detailsDialog_);
        layout->setContentsMargins(12, 12, 12, 12);
        detailsTable_ = new QTableWidget(detailsDialog_);
        detailsTable_->setColumnCount(2);
        detailsTable_->horizontalHeader()->hide();
        detailsTable_->verticalHeader()->hide();
        detailsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        detailsTable_->setSelectionMode(QAbstractItemView::NoSelection);
        detailsTable_->setShowGrid(false);
        detailsTable_->setFocusPolicy(Qt::NoFocus);
        detailsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        detailsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        detailsTable_->setStyleSheet(themed("QTableWidget { background: #1a1b1e; border: none; }"));
        layout->addWidget(detailsTable_);
    }
    detailsTable_->clearSpans();
    detailsTable_->setRowCount(int(details_.size()));
    for (int row = 0; row < int(details_.size()); ++row) {
        const auto& entry = details_[size_t(row)];
        QTableWidgetItem* label = new QTableWidgetItem(entry.first);
        if (entry.second.isEmpty()) {
            // Section heading, Trace Info dock style.
            label->setText(entry.first.toUpper());
            QFont font = label->font();
            font.setBold(true);
            font.setPointSizeF(font.pointSizeF() * 0.85);
            font.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
            label->setFont(font);
            label->setForeground(tc(QColor(0x7a, 0x7c, 0x82)));
            detailsTable_->setItem(row, 0, label);
            detailsTable_->setItem(row, 1, new QTableWidgetItem());
            detailsTable_->setSpan(row, 0, 1, 2);
            continue;
        }
        label->setForeground(tc(QColor(0x9a, 0x9c, 0xa3)));
        QTableWidgetItem* value = new QTableWidgetItem(entry.second);
        value->setForeground(tc(QColor(0xd4, 0xd4, 0xd8)));
        value->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        detailsTable_->setItem(row, 0, label);
        detailsTable_->setItem(row, 1, value);
    }
    detailsTable_->resizeRowsToContents();
    int height = 0;
    for (int row = 0; row < detailsTable_->rowCount(); ++row) height += detailsTable_->rowHeight(row);
    detailsTable_->setMinimumHeight(height + 4);
    detailsDialog_->resize(380, height + 32);
    detailsDialog_->show();
    detailsDialog_->raise();
    detailsDialog_->activateWindow();
}

void SignalNoiseWindow::setRecomputeCallback(std::function<void()> callback) { onRecompute_ = std::move(callback); }

void SignalNoiseWindow::setParamsEditedCallback(std::function<void()> callback) { onParamsEdited_ = std::move(callback); }

void SignalNoiseWindow::setRecomputeUseful(bool useful) {
    recomputeButton_->setStyleSheet(themed(useful ? "QPushButton { background: #e8905a; color: #1e1f22; border: 1px solid #e8905a; }"
                                             "QPushButton:hover { background: #f0a06c; }"
                                           : QString()));
    recomputeButton_->setToolTip(useful ? "The data or the parameters changed: recompute"
                                        : "Up to date with the data and parameters");
}

void SignalNoiseWindow::setWindowParams(const segy::SigcorParams& params) {
    QSignalBlocker blockDistance(distanceSpin_);
    QSignalBlocker blockCorrelation(minCorrelationSpin_);
    distanceSpin_->setValue(params.nskip);
    minCorrelationSpin_->setValue(params.ccmin);
}

void SignalNoiseWindow::applyWindowParams(segy::SigcorParams& params) const {
    params.nskip = distanceSpin_->value();
    params.ccmin = minCorrelationSpin_->value();
}

FkChart::FkChart(QWidget* parent) : ZoomableChart(parent) {
    setMouseTracking(true);
    setMinimumSize(420, 320);
}

void FkChart::setData(const FkDisplay& display) {
    data_ = display;
    Range range;
    range.x0 = data_.wavenumberMin;
    range.x1 = -data_.wavenumberMin; // symmetric about k = 0
    range.y0 = 0.0;
    range.y1 = data_.frequencies > 1 ? data_.frequencyStepHz * (data_.frequencies - 1) : 1.0;
    if (!(range.x1 > range.x0)) range.x1 = range.x0 + 1.0;
    // A zoom made in other units (cycles/trace vs cycles/km) means nothing now.
    if (manualRange_ && (view_.x1 > range.x1 * 1.5 || view_.x0 < range.x0 * 1.5)) resetAxes();
    setAutoRange(range);
    raster_ = QImage();
    hovering_ = false;
    update();
    if (onHover_) onHover_(false, 0.0, 0.0, 0.0f);
}

void FkChart::setVelocityLinesVisible(bool visible) {
    velocityLines_ = visible;
    update();
}

void FkChart::setCrosshairEnabled(bool enabled) {
    crosshairEnabled_ = enabled;
    update();
}

void FkChart::setGridLinesVisible(bool visible) {
    gridLinesVisible_ = visible;
    update();
}

void FkChart::setShowLegend(bool show) {
    showLegend_ = show;
    raster_ = QImage();
    update();
}

void FkChart::setHoverCallback(std::function<void(bool, double, double, float)> callback) { onHover_ = std::move(callback); }

QRect FkChart::chartRect() const {
    const int marginLeft = 64, marginRight = showLegend_ ? 66 : 14, marginTop = 34, marginBottom = 54;
    return QRect(marginLeft, marginTop, width() - marginLeft - marginRight, height() - marginTop - marginBottom);
}

void FkChart::viewChanged() {
    raster_ = QImage();
    update();
}

float FkChart::valueAt(double frequencyHz, double wavenumber, bool* inside) const {
    *inside = false;
    if (data_.frequencies < 2 || data_.wavenumbers < 2 || data_.frequencyStepHz <= 0.0 || data_.wavenumberStep <= 0.0) {
        return -120.0f;
    }
    const double row = frequencyHz / data_.frequencyStepHz;
    const double col = (wavenumber - data_.wavenumberMin) / data_.wavenumberStep;
    if (row < 0.0 || row > data_.frequencies - 1 || col < -0.5 || col > data_.wavenumbers - 0.5) return -120.0f;
    *inside = true;
    // Bilinear in f; in k the last column wraps to the first (-k_Nyquist = +k_Nyquist).
    const int r0 = std::min(int(row), data_.frequencies - 2);
    const double fr = row - r0;
    const double cc = std::max(0.0, col);
    const int c0 = std::min(int(cc), data_.wavenumbers - 1);
    const int c1 = (c0 + 1) % data_.wavenumbers;
    const double fc = std::clamp(cc - c0, 0.0, 1.0);
    auto at = [this](int r, int c) { return double(data_.db[size_t(r) * size_t(data_.wavenumbers) + size_t(c)]); };
    const double top = at(r0, c0) * (1.0 - fc) + at(r0, c1) * fc;
    const double bottom = at(r0 + 1, c0) * (1.0 - fc) + at(r0 + 1, c1) * fc;
    return float(top * (1.0 - fr) + bottom * fr);
}

void FkChart::rebuildRaster(const QRect& chart) {
    const qreal dpr = devicePixelRatioF();
    const int w = std::max(1, int(std::lround(chart.width() * dpr)));
    const int h = std::max(1, int(std::lround(chart.height() * dpr)));
    raster_ = QImage(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        const double f = view_.y1 - (y + 0.5) / h * (view_.y1 - view_.y0);
        uint32_t* line = reinterpret_cast<uint32_t*>(raster_.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double k = view_.x0 + (x + 0.5) / w * (view_.x1 - view_.x0);
            bool inside = false;
            const float db = valueAt(f, k, &inside);
            if (!inside) {
                line[x] = kCanvasSurroundColor.rgb();
                continue;
            }
            // -40 dB green .. 0 dB red, as on the x-f display.
            const float t = std::clamp((db + 20.0f) / 20.0f, -1.0f, 1.0f);
            uint8_t r, g, b;
            segy::applyColorScale(segy::ColorScale::RedYellowGreen, -t, &b, &g, &r);
            line[x] = 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        }
    }
    raster_.setDevicePixelRatio(dpr);
}

void FkChart::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kCanvasSurroundColor);
    if (data_.frequencies < 2 || data_.wavenumbers < 2) {
        painter.setPen(kSecondaryTextColor);
        painter.drawText(rect().adjusted(20, 0, -20, 0), Qt::AlignCenter | Qt::TextWordWrap,
                         data_.emptyReason.isEmpty() ? QString("Computing...") : data_.emptyReason);
        return;
    }
    const QRect chart = chartRect();
    if (chart.width() <= 0 || chart.height() <= 0) return;
    if (raster_.isNull() || raster_.deviceIndependentSize().toSize() != chart.size()) rebuildRaster(chart);
    painter.drawImage(chart.topLeft(), raster_);

    QFont font = axisFont();
    painter.setFont(font);
    // What the panels and the k axis are, e.g. "Panel: CDP = 543" and
    // "Range: offset = 0-3000 (scalar 1, dx 25 m)".
    // The range keeps its full text; the panel text gets what's left.
    painter.setPen(tc(QColor(0xd4, 0xd4, 0xd8)));
    const QFontMetrics topMetrics(font);
    const int rangeWidth = data_.rangeText.isEmpty() ? 0 : std::min(chart.width(), topMetrics.horizontalAdvance(data_.rangeText) + 4);
    const int panelWidth = std::max(0, chart.width() - rangeWidth - (rangeWidth > 0 ? 16 : 0));
    painter.drawText(QRect(chart.left(), 6, panelWidth, 22), Qt::AlignLeft | Qt::AlignVCenter,
                     topMetrics.elidedText(data_.panelText, Qt::ElideRight, panelWidth));
    painter.drawText(QRect(chart.right() - rangeWidth, 6, rangeWidth, 22), Qt::AlignRight | Qt::AlignVCenter,
                     topMetrics.elidedText(data_.rangeText, Qt::ElideLeft, rangeWidth));
    QFontMetrics fm(font);
    auto measureWidth = [&fm](const std::string& text) {
        return text.empty() ? 0 : fm.horizontalAdvance(QString::fromStdString(text));
    };
    const QColor gridColor(0, 0, 0, 90);
    constexpr int kMaxLines = 11;
    std::vector<segy::AxisTick> yTicks =
        segy::computeNiceAxisTicks(view_.y0, view_.y1, chart.height(), kMaxLines, measureWidth);
    std::vector<segy::AxisTick> xTicks =
        segy::computeNiceAxisTicks(view_.x0, view_.x1, chart.width(), kMaxLines, measureWidth);
    drawHorizontalGridlinesAndLabels(painter, chart, yTicks, gridColor, kSecondaryTextColor, chart.left() - 6,
                                      gridLinesVisible_);
    drawVerticalGridlines(painter, chart, xTicks, gridColor, gridLinesVisible_);
    drawXAxisTicksAndLabels(painter, chart, xTicks, kSecondaryTextColor, measureWidth);
    painter.setPen(kSecondaryTextColor);
    painter.drawText(QRect(chart.left(), height() - 20, chart.width(), 20), Qt::AlignHCenter,
                     data_.spacingKnown ? "Wavenumber (cycles/km)" : "Wavenumber (cycles/trace)");
    painter.save();
    painter.translate(12, chart.center().y());
    painter.rotate(-90);
    painter.drawText(QRect(-chart.height() / 2, -9, chart.height(), 18), Qt::AlignCenter, "Frequency (Hz)");
    painter.restore();

    painter.save();
    painter.setClipRect(chart);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // k = 0.
    painter.setPen(QPen(QColor(0, 0, 0, 120), 1, Qt::DashLine));
    painter.drawLine(pixelAt(0.0, view_.y0), pixelAt(0.0, view_.y1));
    // Apparent-velocity fan (f = v k), both dip directions, labelled where
    // each line leaves the plot.
    if (velocityLines_ && data_.spacingKnown) {
        QFont small = font;
        small.setPixelSize(11);
        painter.setFont(small);
        std::vector<QRectF> placedLabels;
        std::vector<std::pair<QRectF, QString>> labels; // drawn after every line, so no line crosses a label
        for (double velocity : {300.0, 500.0, 1000.0, 1500.0, 2000.0, 3000.0, 5000.0}) {
            for (double sign : {-1.0, 1.0}) {
                const double kTop = sign * view_.y1 * 1000.0 / velocity; // k (cycles/km) at f = top
                double kEnd = kTop, fEnd = view_.y1;
                if (kEnd > view_.x1 || kEnd < view_.x0) {
                    kEnd = sign > 0 ? view_.x1 : view_.x0;
                    fEnd = std::fabs(kEnd) * velocity / 1000.0;
                }
                const QPointF from = pixelAt(0.0, 0.0), to = pixelAt(kEnd, fEnd);
                painter.setPen(QPen(QColor(0, 0, 0, 150), 2.5));
                painter.drawLine(from, to);
                painter.setPen(QPen(QColor(255, 255, 255, 200), 1, Qt::DashLine));
                painter.drawLine(from, to);
                const QString label = QString::number(int(velocity));
                const int lw = fm.horizontalAdvance(label) + 8;
                QPointF anchor = to + QPointF(sign > 0 ? -lw - 2 : 4, 4);
                if (fEnd < view_.y1) anchor = to + QPointF(sign > 0 ? -lw - 2 : 4, -16);
                QRectF box(anchor, QSizeF(lw, 14));
                bool overlaps = false;
                for (const QRectF& placed : placedLabels) overlaps = overlaps || placed.adjusted(-2, -2, 2, 2).intersects(box);
                if (overlaps) continue; // the line stays, its label would collide
                placedLabels.push_back(box);
                labels.push_back({box, label});
            }
        }
        for (const auto& [box, label] : labels) {
            painter.fillRect(box, tc(QColor(0x14, 0x14, 0x16))); // opaque: readable over the raster
            painter.setPen(tc(QColor(0xf0, 0xf2, 0xf5)));
            painter.drawText(box, Qt::AlignCenter, label);
        }
        painter.setFont(font);
    }
    painter.restore();
    paintBox(painter);
    painter.setPen(tc(QColor(0x3c, 0x3f, 0x41)));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(chart);

    // Legend: dB scale to the right of the plot.
    if (showLegend_) {
    const int barX = chart.right() + 16, barW = 10;
    const int barTop = chart.top() + 18, barH = std::max(10, chart.height() - 18);
    QLinearGradient gradient(0, barTop, 0, barTop + barH);
    for (int i = 0; i <= 8; ++i) {
        const float t = 1.0f - 2.0f * float(i) / 8.0f; // 0 dB (top) .. -40 dB (bottom)
        uint8_t r, g, b;
        segy::applyColorScale(segy::ColorScale::RedYellowGreen, -t, &b, &g, &r);
        gradient.setColorAt(double(i) / 8.0, QColor(r, g, b));
    }
    painter.fillRect(QRect(barX, barTop, barW, barH), gradient);
    painter.setPen(kSecondaryTextColor);
    painter.drawText(QRect(barX - 4, chart.top(), 40, 16), Qt::AlignLeft | Qt::AlignVCenter, "dB");
    const QString minus(QChar(0x2212));
    painter.drawText(QRect(barX + barW + 4, barTop - 8, 40, 16), Qt::AlignLeft | Qt::AlignVCenter, "0");
    painter.drawText(QRect(barX + barW + 4, barTop + barH / 2 - 8, 40, 16), Qt::AlignLeft | Qt::AlignVCenter, minus + "20");
    painter.drawText(QRect(barX + barW + 4, barTop + barH - 8, 40, 16), Qt::AlignLeft | Qt::AlignVCenter, minus + "40");
    }

    if (crosshairEnabled_ && hovering_ && !draggingBox_) {
        painter.save();
        painter.setClipRect(chart);
        for (const QPen& pen : {QPen(QColor(0, 0, 0, 190), 3), QPen(Qt::white, 1, Qt::DashLine)}) {
            painter.setPen(pen);
            painter.drawLine(QPointF(hoverPos_.x(), chart.top()), QPointF(hoverPos_.x(), chart.bottom()));
            painter.drawLine(QPointF(chart.left(), hoverPos_.y()), QPointF(chart.right(), hoverPos_.y()));
        }
        painter.restore();
    }
}

void FkChart::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        if (data_.frequencies >= 2) {
            showAxisMenu(event->globalPosition().toPoint(),
                         data_.spacingKnown ? "Wavenumber (cycles/km)" : "Wavenumber (cycles/trace)", "Frequency (Hz)",
                         data_.spacingKnown ? 2 : 4, 1);
        }
        return;
    }
    if (data_.frequencies >= 2) handleBoxPress(event);
}

void FkChart::mouseReleaseEvent(QMouseEvent* event) { handleBoxRelease(event); }

void FkChart::mouseMoveEvent(QMouseEvent* event) {
    if (handleBoxMove(event)) return;
    const QPointF pos = event->position();
    const bool inside = data_.frequencies >= 2 && chartRect().contains(pos.toPoint());
    if (!inside) {
        if (hovering_) {
            hovering_ = false;
            update();
            if (onHover_) onHover_(false, 0.0, 0.0, 0.0f);
        }
        return;
    }
    hovering_ = true;
    hoverPos_ = pos;
    const QPointF point = dataAt(pos);
    bool onData = false;
    const float db = valueAt(point.y(), point.x(), &onData);
    update();
    if (onHover_) onHover_(onData, point.y(), point.x(), db);
}

void FkChart::leaveEvent(QEvent*) {
    if (!hovering_) return;
    hovering_ = false;
    update();
    if (onHover_) onHover_(false, 0.0, 0.0, 0.0f);
}

FkWindow::FkWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setWindowTitle("f-k display");
    resize(1100, 720);
    mainLayout_ = new QVBoxLayout(this);
    mainLayout_->setContentsMargins(0, 0, 0, 0);
    mainLayout_->setSpacing(0);
    mainLayout_->addWidget(buildResultWindowMenuBar(
        this,
        [this](bool checked) {
            gridLinesVisible_ = checked;
            for (FkChart* chart : charts_) chart->setGridLinesVisible(checked);
        },
        nullptr,
        [this](bool checked) {
            crosshairEnabled_ = checked;
            for (FkChart* chart : charts_) chart->setCrosshairEnabled(checked);
            readout_->setVisible(checked);
        }));

    QWidget* header = new QWidget(this);
    header->setStyleSheet(themed("background-color: #141416;"));
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 10, 14, 10);
    headerLayout->setSpacing(10);
    auto caption = [](const QString& text) {
        QLabel* label = new QLabel(text);
        label->setStyleSheet(themed("color: #9a9ca3;"));
        return label;
    };
    gatherCombo_ = new QComboBox();
    gatherCombo_->addItem("Auto", 0);
    gatherCombo_->addItem("FFID", 1);
    gatherCombo_->addItem("CDP", 2);
    gatherCombo_->addItem("None (stack)", 3);
    gatherCombo_->setToolTip(
        "Panel ident: what one f-k panel is. FFID/CDP: runs of traces with the same value (gathers), one f-k plot "
        "per gather in view. None: sliding panels of 256 traces (50% overlap), averaged, for stacked sections. "
        "Auto picks FFID, then CDP, then None, from the trace headers.");
    distanceCombo_ = new QComboBox();
    distanceCombo_->addItem("Auto", 0);
    distanceCombo_->addItem("Offset", 1);
    distanceCombo_->addItem("CDP", 2);
    distanceCombo_->setToolTip(
        "Distance ident for the wavenumber axis. Gathers are sorted on it; the trace spacing is its range per "
        "panel over (traces - 1), times the scalar. Auto: offset in gathers, CDP otherwise.");
    scalarCaption_ = caption("Offset scalar");
    scalarSpin_ = new QDoubleSpinBox();
    scalarSpin_->setRange(0.0001, 100000.0);
    scalarSpin_->setDecimals(4);
    averageCheck_ = new QCheckBox("Average gathers");
    averageCheck_->setToolTip("One f-k plot averaged over all gathers in view, instead of one plot per gather");
    velocityCheck_ = new QCheckBox("Velocity lines");
    velocityCheck_->setChecked(true);
    velocityCheck_->setToolTip("Apparent-velocity fan f = v k (m/s), needs the trace spacing");
    headerLayout->addWidget(caption("Panel"));
    headerLayout->addWidget(gatherCombo_);
    headerLayout->addSpacing(12);
    headerLayout->addWidget(caption("Distance"));
    headerLayout->addWidget(distanceCombo_);
    headerLayout->addWidget(scalarCaption_);
    headerLayout->addWidget(scalarSpin_);
    headerLayout->addSpacing(12);
    headerLayout->addWidget(averageCheck_);
    headerLayout->addWidget(velocityCheck_);
    headerLayout->addStretch();
    recomputeButton_ = new QPushButton("Recompute");
    recomputeButton_->setMinimumWidth(150);
    connect(recomputeButton_, &QPushButton::clicked, this, [this]() {
        if (onRecompute_) onRecompute_();
    });
    headerLayout->addWidget(recomputeButton_);
    mainLayout_->addWidget(header);
    auto edited = [this]() {
        if (onParamsEdited_) onParamsEdited_();
    };
    connect(gatherCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, edited](int) {
        syncScalarField();
        edited();
    });
    connect(distanceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, edited](int) {
        syncScalarField();
        edited();
    });
    connect(scalarSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, edited](double value) {
        if (showingCdp_) {
            cdpDistance_ = value;
            cdpDistanceTyped_ = true;
        } else {
            offsetScalar_ = value;
        }
        edited();
    });
    connect(averageCheck_, &QCheckBox::toggled, this, edited);
    connect(velocityCheck_, &QCheckBox::toggled, this, [this](bool on) {
        for (FkChart* chart : charts_) chart->setVelocityLinesVisible(on);
    });
    syncScalarField();

    // The k-axis line (ident, range, scalar, dx), then counts and gate.
    rangeLabel_ = new QLabel();
    rangeLabel_->setStyleSheet(themed("background-color: #141416; color: #d4d4d8; padding: 0 14px 2px 14px;"));
    mainLayout_->addWidget(rangeLabel_);
    infoLabel_ = new QLabel();
    infoLabel_->setStyleSheet(themed("background-color: #141416; color: #9a9ca3; padding: 0 14px 8px 14px;"));
    infoLabel_->setWordWrap(true);
    mainLayout_->addWidget(infoLabel_);
    zoomBar_ = new QToolBar(this); // replaced by a real one once there are charts
    mainLayout_->addWidget(zoomBar_);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chartsHost_ = new QWidget();
    chartsHost_->setStyleSheet(themed("background-color: #141416;"));
    chartsLayout_ = new QHBoxLayout(chartsHost_);
    chartsLayout_->setContentsMargins(0, 0, 0, 0);
    chartsLayout_->setSpacing(0);
    emptyLabel_ = new QLabel("Computing...");
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setWordWrap(true);
    emptyLabel_->setStyleSheet(themed("color: #9a9ca3;"));
    chartsLayout_->addWidget(emptyLabel_);
    scroll_->setWidget(chartsHost_);

    readout_ = new QWidget(this);
    readout_->setObjectName("fkReadout");
    readout_->setAttribute(Qt::WA_StyledBackground, true);
    readout_->setStyleSheet(themed("#fkReadout { background-color: #1a1b1e; border-left: 1px solid #2b2d31; }"));
    readout_->setFixedWidth(210);
    QVBoxLayout* readoutLayout = new QVBoxLayout(readout_);
    readoutLayout->setContentsMargins(16, 16, 16, 16);
    readoutLayout->setSpacing(8);
    readoutLayout->addWidget(readoutSectionLabel("CURSOR"));
    readoutLayout->addWidget(readoutValueRow(nullptr, "Panel", readPanel_));
    readoutLayout->addWidget(readoutValueRow(nullptr, "Frequency", readFrequency_));
    readoutLayout->addWidget(readoutValueRow(nullptr, "Wavenumber", readWavenumber_));
    readoutLayout->addWidget(readoutValueRow(nullptr, "Velocity", readVelocity_));
    readoutLayout->addWidget(readoutValueRow(nullptr, "Level", readLevel_));
    readoutLayout->addStretch();
    QLabel* hint = new QLabel("Positive k: events that get later on later traces.");
    hint->setWordWrap(true);
    hint->setStyleSheet(themed("color: #68696e; font-style: italic; font-size: 11px;"));
    readoutLayout->addWidget(hint);

    QHBoxLayout* body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(scroll_, 1);
    body->addWidget(readout_);
    mainLayout_->addLayout(body, 1);
    for (QLabel* label : {readPanel_, readFrequency_, readWavenumber_, readVelocity_, readLevel_}) label->setText("--");
}

void FkWindow::setResult(const std::vector<FkDisplay>& panels, const QString& rangeText, const QString& info,
                         const QString& emptyReason) {
    for (FkChart* chart : charts_) delete chart;
    charts_.clear();
    rangeLabel_->setText(rangeText);
    rangeLabel_->setVisible(!rangeText.isEmpty());
    infoLabel_->setText(info);
    infoLabel_->setVisible(!info.isEmpty());
    emptyLabel_->setVisible(panels.empty());
    emptyLabel_->setText(emptyReason.isEmpty() ? QString("Not enough data.") : emptyReason);

    // One chart per panel; several side by side get a minimum width and
    // scroll, and only the last draws the dB legend.
    for (size_t i = 0; i < panels.size(); ++i) {
        FkChart* chart = new FkChart(chartsHost_);
        chart->setShowLegend(i + 1 == panels.size());
        chart->setGridLinesVisible(gridLinesVisible_);
        chart->setCrosshairEnabled(crosshairEnabled_);
        chart->setVelocityLinesVisible(velocityCheck_->isChecked());
        if (panels.size() > 1) chart->setMinimumWidth(i + 1 == panels.size() ? 330 : 280);
        chart->setData(panels[i]);
        const QString name = panels[i].panelText;
        chart->setHoverCallback([this, chart, name](bool valid, double f, double k, float db) {
            if (!valid) {
                for (QLabel* label : {readPanel_, readFrequency_, readWavenumber_, readVelocity_, readLevel_}) {
                    label->setText("--");
                }
                return;
            }
            const bool km = chart->display().spacingKnown;
            readPanel_->setText(name);
            readFrequency_->setText(QString("%1 Hz").arg(f, 0, 'f', 1));
            readWavenumber_->setText(km ? QString("%1 /km").arg(k, 0, 'f', 2) : QString("%1 /tr").arg(k, 0, 'f', 3));
            // Apparent velocity v = f / k: m/s with a spacing, traces/s without.
            if (std::fabs(k) < 1e-9) {
                readVelocity_->setText(QString(QChar(0x221E)));
            } else {
                readVelocity_->setText(km ? QString("%1 m/s").arg(std::fabs(1000.0 * f / k), 0, 'f', 0)
                                          : QString("%1 tr/s").arg(std::fabs(f / k), 0, 'f', 0));
            }
            readLevel_->setText(QString("%1 dB").arg(double(db), 0, 'f', 1));
        });
        chartsLayout_->addWidget(chart, 1);
        charts_.push_back(chart);
    }
    std::vector<ZoomableChart*> group(charts_.begin(), charts_.end());
    for (FkChart* chart : charts_) chart->linkGroup(group);

    // A zoom strip for the group (bound to the first chart; the group does the rest).
    const int barIndex = mainLayout_->indexOf(zoomBar_);
    delete zoomBar_;
    zoomBar_ = charts_.empty() ? new QToolBar(this) : buildChartZoomToolbar(charts_.front(), this);
    mainLayout_->insertWidget(barIndex, zoomBar_);
}

int FkWindow::gatherMode() const { return gatherCombo_->currentData().toInt(); }

bool FkWindow::averageGathers() const { return averageCheck_->isChecked(); }

int FkWindow::distanceMode() const { return distanceCombo_->currentData().toInt(); }

double FkWindow::offsetScalar() const { return offsetScalar_; }

double FkWindow::cdpDistance() const { return cdpDistance_; }

void FkWindow::proposeCdpDistance(double metres) {
    if (cdpDistanceTyped_ || !(metres > 0.0)) return;
    cdpDistance_ = metres;
    syncScalarField();
}

// The one number field shows the scalar of the distance ident in use: the
// offset scalar, or the CDP distance (Auto resolves like the computation:
// offset with gathers, CDP for None).
void FkWindow::syncScalarField() {
    const int mode = distanceMode();
    showingCdp_ = mode == 2 || (mode == 0 && gatherMode() == 3);
    QSignalBlocker block(scalarSpin_);
    if (showingCdp_) {
        scalarCaption_->setText("CDP distance");
        scalarSpin_->setSuffix(" m");
        scalarSpin_->setValue(cdpDistance_);
        scalarSpin_->setToolTip("Metres per CDP number. Proposed from the trace coordinates when they allow it, "
                                "until you type a value.");
    } else {
        scalarCaption_->setText("Offset scalar");
        scalarSpin_->setSuffix(QString());
        scalarSpin_->setValue(offsetScalar_);
        scalarSpin_->setToolTip("Metres per offset unit in the headers: 1 for offsets in metres.");
    }
}

void FkWindow::setRecomputeCallback(std::function<void()> callback) { onRecompute_ = std::move(callback); }

void FkWindow::setParamsEditedCallback(std::function<void()> callback) { onParamsEdited_ = std::move(callback); }

void FkWindow::setRecomputeUseful(bool useful) {
    recomputeButton_->setStyleSheet(themed(useful ? "QPushButton { background: #e8905a; color: #1e1f22; border: 1px solid #e8905a; }"
                                             "QPushButton:hover { background: #f0a06c; }"
                                           : QString()));
    recomputeButton_->setToolTip(useful ? "The data or the settings changed: recompute"
                                        : "Up to date with the data and settings");
}

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
        QString label = QString::fromStdString(ds->name);
        if (!ds->attribute.empty()) label += QString("   [%1]").arg(QString::fromStdString(ds->attribute));
        list_->addItem(label);
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
    pickButton_->setStyleSheet(themed(
        "QPushButton { background: transparent; border: none; } "
        "QPushButton:hover { background: #3c3f41; border-radius: 2px; }"));
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

void DatasetSlotWidget::setSlotWidth(int width) {
    setFixedWidth(std::max(24, width));
    pickButton_->move(this->width() - 22, 14);
    update();
}

void DatasetSlotWidget::setPickCallback(std::function<void()> callback) { onPick_ = std::move(callback); }

void DatasetSlotWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QRect r = rect().adjusted(0, 0, -1, -1);
    painter.fillRect(r, tc(QColor(0x2b, 0x2d, 0x30)));
    painter.setPen(tc(QColor(0x3c, 0x3f, 0x41)));
    painter.drawRect(r);

    QFont captionFont = font();
    captionFont.setPixelSize(9);
    painter.setFont(captionFont);
    painter.setPen(tc(QColor(0x9a, 0x9c, 0xa3)));
    painter.drawText(QRect(4, 1, width() - 8, 12), Qt::AlignLeft | Qt::AlignVCenter, label_);

    QFont nameFont = font();
    nameFont.setPixelSize(11);
    painter.setFont(nameFont);
    painter.setPen(tc(QColor(0xd4, 0xd4, 0xd8)));
    QString shown = datasetName_.isEmpty() ? QString(QChar(0x2014)) /* em dash; QChar keeps it independent of source encoding */ : datasetName_;
    QRect nameRect(4, 15, width() - 8 - 20, 16);
    painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter.fontMetrics().elidedText(shown, Qt::ElideRight, nameRect.width()));
}

SegyOpenDialog::SegyOpenDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Open SEG-Y File");
    setModal(true);

    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* loadOptionsLabel = new QLabel("Load Options:");
    loadOptionsLabel->setStyleSheet(themed("font-style: italic; color: #9a9ca3;"));
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
    browseLabel->setStyleSheet(themed("font-style: italic; color: #9a9ca3; margin-top: 8px;"));
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

PreferencesDialog::PreferencesDialog(const Preferences& initial, const std::vector<ToolbarItemInfo>& toolbarItems,
                                     QWidget* parent)
    : QDialog(parent) {
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

    // Which buttons the toolbar shows -- the one place this is edited.
    // Every function stays reachable from the menus either way.
    QGroupBox* buttonsGroup = new QGroupBox("Toolbar Buttons");
    QVBoxLayout* buttonsLayout = new QVBoxLayout(buttonsGroup);
    // One block per category (consecutive items sharing it): its name on
    // the left, its buttons in two columns on the right, a thin line
    // between blocks.
    QGridLayout* grid = new QGridLayout();
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(4);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    int row = 0;
    for (size_t i = 0; i < toolbarItems.size();) {
        size_t end = i;
        while (end < toolbarItems.size() && toolbarItems[end].category == toolbarItems[i].category) ++end;
        if (row > 0) {
            // A plain 1 px strip: a styled QFrame::HLine rendered ~3 px and too bright.
            QWidget* line = new QWidget();
            line->setFixedHeight(1);
            line->setStyleSheet(themed("background-color: #34363b;"));
            grid->addWidget(line, row++, 0, 1, 3);
        }
        QLabel* heading = new QLabel(toolbarItems[i].category.toUpper());
        heading->setStyleSheet(themed("color: #7a7c82; font-size: 10px; letter-spacing: 1px; font-weight: 600;"));
        const int blockRows = int((end - i + 1) / 2);
        grid->addWidget(heading, row, 0, blockRows, 1, Qt::AlignTop | Qt::AlignLeft);
        for (size_t k = i; k < end; ++k) {
            QCheckBox* check = new QCheckBox(toolbarItems[k].label);
            check->setChecked(!initial.hiddenToolbarItems.contains(toolbarItems[k].id));
            const int offset = int(k - i);
            grid->addWidget(check, row + offset / 2, 1 + offset % 2); // left to right, then down
            toolbarItemChecks_.push_back({toolbarItems[k].id, check});
        }
        row += blockRows;
        i = end;
    }
    buttonsLayout->addLayout(grid);
    QHBoxLayout* buttonsRow = new QHBoxLayout();
    QPushButton* showAll = new QPushButton("Show All");
    connect(showAll, &QPushButton::clicked, this, [this]() {
        for (auto& entry : toolbarItemChecks_) entry.second->setChecked(true);
    });
    buttonsRow->addWidget(showAll);
    buttonsRow->addStretch();
    QLabel* buttonsHint = new QLabel("Hidden buttons stay available from the menus.");
    buttonsHint->setStyleSheet(themed("color: #9a9ca3; font-size: 11px;"));
    buttonsRow->addWidget(buttonsHint);
    buttonsLayout->addLayout(buttonsRow);
    layout->addWidget(buttonsGroup);

    // Signal/Noise settings that don't need to be at hand in its window
    // (Skipped traces and Min correlation are there). SIGCOR card names in
    // the tooltips.
    initialSignalNoise_ = initial.signalNoise;
    QGroupBox* snGroup = new QGroupBox("Signal/Noise");
    QGridLayout* snGrid = new QGridLayout(snGroup);
    snGrid->setHorizontalSpacing(10);
    snGrid->setVerticalSpacing(6);
    snGrid->setColumnStretch(1, 1);
    snGrid->setColumnStretch(3, 1);
    snGrid->setColumnMinimumWidth(2, 90);
    auto addSnField = [snGrid](int row, int col, const QString& label, QWidget* field, const QString& tip) {
        QLabel* caption = new QLabel(label);
        caption->setToolTip(tip);
        field->setToolTip(tip);
        snGrid->addWidget(caption, row, col * 2);
        snGrid->addWidget(field, row, col * 2 + 1);
    };
    const segy::SigcorParams& sn = initial.signalNoise;
    snMethodCombo_ = new QComboBox();
    snMethodCombo_->addItem("Spectrum", int(segy::SigcorOption::Spectr));
    snMethodCombo_->addItem("Autocorrelation", int(segy::SigcorOption::Autcor));
    snMethodCombo_->setCurrentIndex(sn.option == segy::SigcorOption::Autcor ? 1 : 0);
    addSnField(0, 0, "Method", snMethodCombo_,
               "Spectrum: average the power spectra of the gates (the reference SPECTR). Autocorrelation: average "
               "the auto- and crosscorrelations, transform once at the end; the same spectra on a twice as fine "
               "frequency grid. (SIGCOR OPTION SPECTR/AUTCOR)");
    snBetaSpin_ = new QDoubleSpinBox();
    snBetaSpin_->setRange(0.0, 50.0);
    snBetaSpin_->setDecimals(1);
    snBetaSpin_->setSingleStep(0.5);
    snBetaSpin_->setValue(sn.kaiserBeta);
    addSnField(0, 1, "Taper beta", snBetaSpin_,
               "Kaiser-Bessel taper over the gate: 0 is no taper, larger values taper harder. (SIGCOR taper, "
               "beta 5)");
    snRandomSpin_ = new QSpinBox();
    snRandomSpin_->setRange(0, 200);
    snRandomSpin_->setPrefix("\xc2\xb1");
    snRandomSpin_->setSuffix(" traces");
    snRandomSpin_->setValue(sn.iransk);
    addSnField(1, 0, "Random variation", snRandomSpin_,
               "Per pair, the skipped traces vary randomly by up to this many either way, against a constant "
               "dip. At most Skipped traces. (SIGCOR IRANSK)");
    snMaxShiftSpin_ = new QDoubleSpinBox();
    snMaxShiftSpin_->setRange(0.1, 1000.0);
    snMaxShiftSpin_->setDecimals(1);
    snMaxShiftSpin_->setSuffix(" ms");
    snMaxShiftSpin_->setValue(sn.shmaxMs);
    addSnField(1, 1, "Max shift", snMaxShiftSpin_,
               "Largest time shift searched between the two traces, rounded to whole samples (at least one). A "
               "best match at the limit rejects the pair. The gate must be at least 2 x this + 3 samples long. "
               "(SIGCOR SHMAX)");
    QHBoxLayout* snChecks = new QHBoxLayout();
    snEqualizeCheck_ = new QCheckBox("Equalize energy");
    snEqualizeCheck_->setChecked(sn.normalise);
    snEqualizeCheck_->setToolTip("After alignment, scale both gates of a pair to unit RMS. (SIGCOR IGNORM)");
    snRemoveDcCheck_ = new QCheckBox("Remove DC");
    snRemoveDcCheck_->setChecked(sn.removeDc);
    snRemoveDcCheck_->setToolTip("After alignment, remove each gate's mean. (SIGCOR IGNORM)");
    snChecks->addWidget(snEqualizeCheck_);
    snChecks->addWidget(snRemoveDcCheck_);
    snChecks->addStretch();
    snGrid->addLayout(snChecks, 2, 0, 1, 4);
    layout->addWidget(snGroup);

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
    scaleHint->setStyleSheet(themed("color: #9a9ca3; font-size: 11px;"));
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
    for (const auto& entry : toolbarItemChecks_) {
        if (!entry.second->isChecked()) prefs.hiddenToolbarItems.append(entry.first);
    }
    segy::SigcorParams& sn = prefs.signalNoise;
    sn = initialSignalNoise_; // Skipped traces/Min correlation: MainWindow keeps the window's
    sn.option = segy::SigcorOption(snMethodCombo_->currentData().toInt());
    sn.iransk = snRandomSpin_->value();
    sn.shmaxMs = snMaxShiftSpin_->value();
    sn.kaiserBeta = snBetaSpin_->value();
    sn.normalise = snEqualizeCheck_->isChecked();
    sn.removeDc = snRemoveDcCheck_->isChecked();
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
    setStyleSheet(themed("background-color: #1e1f22;"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    textEdit_ = new QPlainTextEdit(this);
    textEdit_->setReadOnly(true);
    textEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    textEdit_->setStyleSheet(themed("background-color: black; color: white; border: none;"));
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
    setStyleSheet(themed("background-color: #1e1f22;")); // matches the app's dark chrome, same as EbcdicHeaderWidget
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
    // Opened by mousePressEvent for a right-click anywhere in the color
    // bar's column (the overlay itself passes mouse presses on to the
    // canvas), so that column belongs to this menu and never to the axis
    // menu.
    colorScaleMenu_ = [this](const QPoint& globalPos) {
        QMenu menu(amplitudeScale_);
        menu.setToolTipsVisible(true);
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
            {"Rainbow", segy::ColorScale::Rainbow},
            {"Red - Yellow - Green", segy::ColorScale::RedYellowGreen},
        };
        QActionGroup* group = new QActionGroup(&menu);
        for (const Entry& entry : kEntries) {
            QWidgetAction* action = new QWidgetAction(&menu);
            action->setText(entry.label);
            action->setToolTip(entry.label);
            action->setIcon(colorScalePreviewIcon(entry.scale, app_.display.colorScaleFlipped));
            action->setCheckable(true);
            action->setChecked(app_.display.colorScale == entry.scale);
            action->setActionGroup(group);
            QToolButton* preview = new QToolButton(&menu);
            preview->setToolButtonStyle(Qt::ToolButtonIconOnly);
            preview->setIconSize(QSize(96, 16));
            action->setDefaultWidget(preview);
            preview->setDefaultAction(action);
            menu.addAction(action);
            connect(action, &QAction::triggered, this, [this, scale = entry.scale]() {
                app_.display.colorScale = scale;
                amplitudeScale_->setColorScale(scale, app_.display.colorScaleFlipped);
                update();
            });
        }

        menu.addSeparator();
        QAction* flipColorMapAction = menu.addAction("Flip Color Map");
        flipColorMapAction->setCheckable(true);
        flipColorMapAction->setChecked(app_.display.colorScaleFlipped);
        connect(flipColorMapAction, &QAction::triggered, this, [this](bool flipped) {
            app_.display.colorScaleFlipped = flipped;
            amplitudeScale_->setColorScale(app_.display.colorScale, flipped);
            update();
        });

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

        menu.exec(globalPos);
    };

    // Foreground/Background dataset boxes -- see native/README.md,
    // "Dataset pool: Foreground/Background." Labels ("Foreground 1" vs
    // "Foreground 2" etc.) are set by MainWindow right after construction
    // (setDatasetSlotLabels), since only it knows which panel this is.
    foregroundSlot_ = new DatasetSlotWidget(QString(), this);
    foregroundSlot_->setPickCallback([this]() { if (onDatasetSlotPick_) onDatasetSlotPick_(true); });
    backgroundSlot_ = new DatasetSlotWidget(QString(), this);
    backgroundSlot_->setPickCallback([this]() { if (onDatasetSlotPick_) onDatasetSlotPick_(false); });
}

void SegyCanvas::setAmplitudeScale(float posClip, float negClip, bool visible, segy::ColorScale colorScale,
                                    bool colorScaleFlipped) {
    amplitudeScale_->setClipRange(posClip, negClip);
    amplitudeScale_->setColorScale(colorScale, colorScaleFlipped);
    amplitudeScale_->setCenter(app_.display.manualClip.center, app_.display.frequencyAxis ? "dB" : QString());
    amplitudeScale_->setVisible(visible);
}

void SegyCanvas::setDatasetSlotLabels(const QString& foregroundLabel, const QString& backgroundLabel) {
    foregroundSlot_->setLabel(foregroundLabel);
    backgroundSlot_->setLabel(backgroundLabel);
    repositionDatasetSlots();
}

void SegyCanvas::setDatasetLabelPosition(DatasetLabelPosition position) {
    datasetLabelPosition_ = position;
    repositionDatasetSlots();
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
    if (datasetLabelPosition_ == DatasetLabelPosition::Off) {
        foregroundSlot_->hide();
        backgroundSlot_->hide();
        return;
    }

    int plotLeft = kCanvasMargin + lastPlotX_;
    int plotRight = plotLeft + lastPlotWidth_;
    int plotTop = kCanvasMargin + identInsets().top;
    int plotBottom = height() - kCanvasMargin - identInsets().bottom;
    int availableWidth = std::max(0, width() - 2 * kCanvasMargin - 4);
    int slotWidth = std::min(160, availableWidth / 2);
    foregroundSlot_->setSlotWidth(slotWidth);
    backgroundSlot_->setSlotWidth(slotWidth);
    int groupWidth = foregroundSlot_->width() + 4 + backgroundSlot_->width();
    int groupHeight = std::max(foregroundSlot_->height(), backgroundSlot_->height());
    bool right = datasetLabelPosition_ == DatasetLabelPosition::TopRight ||
                 datasetLabelPosition_ == DatasetLabelPosition::BottomRight;
    bool bottom = datasetLabelPosition_ == DatasetLabelPosition::BottomLeft ||
                  datasetLabelPosition_ == DatasetLabelPosition::BottomRight;
    int x = right ? plotRight - groupWidth - 4 : plotLeft + 4;
    int y = bottom ? plotBottom - groupHeight - 4 : plotTop + 4;
    x = std::clamp(x, kCanvasMargin, std::max(kCanvasMargin, width() - groupWidth - kCanvasMargin));
    y = std::clamp(y, kCanvasMargin, std::max(kCanvasMargin, height() - groupHeight - kCanvasMargin));
    foregroundSlot_->move(x, y);
    backgroundSlot_->move(x + foregroundSlot_->width() + 4, y);
    foregroundSlot_->show();
    backgroundSlot_->show();
    foregroundSlot_->raise();
    backgroundSlot_->raise();
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
                          "View > Zoom to Box zooms to it, middle-click deletes it, click empty space for a new box",
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
    // Plain words: what is in view, the sample format, and the draw time.
    const segy::BinaryHeader& bh = app_.foreground->binHeader;
    const double step = bh.sampleIntervalUs / 1000.0;
    const char* unit = app_.display.frequencyAxis ? "Hz" : app_.display.depthAxis ? "m" : "ms";
    const int format = bh.formatCode;
    const char* formatName = format == 1 ? "IBM float" : format == 5 ? "IEEE float" : format == 2 ? "32-bit integer"
                           : format == 3 ? "16-bit integer" : format == 8 ? "8-bit integer" : "format ?";
    const int64_t firstTrace = int64_t(std::max(0.0, std::floor(app_.view.traceStart))) + 1;
    const int64_t lastTrace = std::min<int64_t>(app_.foreground->traceCount, int64_t(std::ceil(app_.view.traceEnd)));
    std::snprintf(buf, sizeof(buf),
                  "Traces %lld\xe2\x80\x93%lld of %lld   \xc2\xb7   %.0f\xe2\x80\x93%.0f %s   \xc2\xb7   %s, %g %s   \xc2\xb7   drawn in %.0f ms",
                  (long long)firstTrace, (long long)lastTrace, (long long)app_.foreground->traceCount,
                  app_.view.sampleStart * step, app_.view.sampleEnd * step, unit, formatName, step, unit,
                  app_.lastRenderMs);
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
    info.traceIdx = app_.xf.active ? app_.xf.traceOffset + traceIdx * app_.xf.traceStride : traceIdx;
    info.seqNum = th.traceSequenceLine;
    info.timeMs = double(sampleIdx) * (app_.foreground->binHeader.sampleIntervalUs / 1000.0);
    info.amplitude = value;
    info.frequencyAxis = app_.display.frequencyAxis;
    info.depthAxis = app_.display.depthAxis;
    info.dataset = app_.foreground.get();
    info.sourceCanvas = this;
    info.traceCoord = traceCoord;
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
        if (onBoxCompleted_) onBoxCompleted_();
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
    painter.setPen(tc(QColor(Qt::white)));

    // A label that would touch the previous one in its row is skipped, so a
    // narrow window or long values (e.g. 163004113) never pile up.
    auto drawRow = [&](int y, IdentField field, const std::vector<int64_t>& ticks) {
        int lastRight = std::numeric_limits<int>::min();
        const int plotLeft = kCanvasMargin + chrome.plotX, plotRight = plotLeft + chrome.plotWidth;
        for (int64_t idx : ticks) {
            double px, py;
            pixelForData(double(idx), 0.0, px, py);
            QString text = QString::number(identFieldValue(headerAt(idx), field));
            int tw = fm.horizontalAdvance(text);
            int left = int(std::lround(px)) - tw / 2;
            left = std::clamp(left, plotLeft - tw / 2, std::max(plotLeft - tw / 2, plotRight - tw / 2));
            if (left < lastRight + 10) continue;
            painter.drawText(QPoint(left, y + fm.ascent()), text);
            lastRight = left + tw;
        }
    };

    // As many label positions as fit (about one per 90 px, at most 12).
    const int labelSlots = std::clamp(chrome.plotWidth / 90, 1, 12);
    std::vector<int64_t> textTicks = identTracePositions(app_.view, traceCount, labelSlots);
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
            painter.setPen(tc(QColor(Qt::white)));
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
    PerfScope perf("paint canvas", 20);
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
        // Profiling: time per paint stage (logged from 5 ms).
        QElapsedTimer lapTimer;
        if (perfEnabled()) lapTimer.start();
        auto lap = [&lapTimer](const char* stage) {
            if (!lapTimer.isValid()) return;
            const qint64 ms = lapTimer.restart();
            if (ms >= 5) perfLog(QString("  paint stage %1: %2 ms").arg(stage).arg(ms));
        };
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
        lap("render (raster)");
        bool plotGeometryChanged = lastPlotX_ != chrome.plotX || lastPlotWidth_ != chrome.plotWidth;
        lastPlotX_ = chrome.plotX;
        lastPlotWidth_ = chrome.plotWidth;
        // The time scale/color bar toggling (not just a resize) can move
        // the plot's left edge -- keep the dataset boxes flush with it
        // either way. Calling this mid-paintEvent is safe: it only moves
        // child widgets, it doesn't trigger a synchronous repaint of them.
        if (plotGeometryChanged) repositionDatasetSlots();

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
        if (lightTheme()) {
            // The raster leaves the axis columns black; light ones here.
            const QColor axisBackground = tc(QColor(Qt::black));
            painter.fillRect(QRect(kCanvasMargin + chrome.leftScaleX, topInset, chrome.leftScaleWidth, insetHeight),
                             axisBackground);
            painter.fillRect(QRect(kCanvasMargin + chrome.rightScaleX, topInset, chrome.rightScaleWidth, insetHeight),
                             axisBackground);
        }

        // Idents (see native/README.md): rows + plot above the image,
        // rows below it -- disjoint from the image region, so no clip
        // needed. Drawn right after the image blit, before the time-scale
        // labels, so it reads as "chrome" rather than sitting over the
        // axis text.
        lap("blit");
        paintIdentText(painter, chrome, topInset, bottomInset, insetHeight);
        lap("ident rows");

        if (app_.display.showTimeScale) {
            int leftNumbersX = kCanvasMargin + chrome.leftScaleX + chrome.titleColumnWidth + chrome.columnGap;
            int rightNumbersX = kCanvasMargin + chrome.rightScaleX + chrome.columnGap;
            int leftTitleX = kCanvasMargin + chrome.leftScaleX;
            int rightTitleX =
                kCanvasMargin + chrome.rightScaleX + chrome.columnGap + chrome.numbersColumnWidth + chrome.columnGap;

            painter.setPen(tc(QColor(Qt::white)));
            int lastLabelEnd = std::numeric_limits<int>::min();
            for (const segy::TimeTick& tick : chrome.ticks) {
                // Labels are centered on their tick's Y position, so one
                // sitting right at the top/bottom edge (e.g. sample 0, or
                // the last visible sample) would have half its rotated
                // extent fall outside the plot and get clipped. Nudge it
                // inward just enough to stay fully visible, same as the
                // reference example this feature was built from.
                int halfSpan = fm.horizontalAdvance(QString::fromStdString(tick.label)) / 2;
                int y = topInset + std::clamp(tick.pixelY, halfSpan, insetHeight - halfSpan);
                if (y - halfSpan < lastLabelEnd + 6) continue; // would overlap the previous label
                lastLabelEnd = y + halfSpan;
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
                app_.display.gainDb + kWiggleGainBoostDb, app_.display.reversePolarity, &app_.display.processing);
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
        lap("time axis + wiggle");
        paintIdentOverlayLines(painter, chrome, topInset, insetHeight);
        lap("ident overlay lines");

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

        lap("selection boxes");
        // Horizons of this dataset: a line through the picks (gaps where
        // there are none), anchors as squares, the active one thicker.
        if (!app_.foreground->horizons.empty() && !app_.display.frequencyAxis) {
            const double sampleMs = app_.foreground->binHeader.sampleIntervalUs / 1000.0;
            const QRect plot(kCanvasMargin + chrome.plotX, topInset, chrome.plotWidth, insetHeight);
            const int64_t first = std::max<int64_t>(0, int64_t(std::floor(app_.view.traceStart)));
            const int64_t last = std::min<int64_t>(app_.foreground->traceCount - 1, int64_t(std::ceil(app_.view.traceEnd)));
            // At most ~2 points per pixel column.
            const int64_t step = std::max<int64_t>(1, (last - first + 1) / std::max(1, 2 * chrome.plotWidth));
            painter.save();
            painter.setClipRect(plot);
            // Smooth lines only when zoomed in; decimated lines draw faster without.
            painter.setRenderHint(QPainter::Antialiasing, step == 1);
            for (int hi = 0; hi < int(app_.foreground->horizons.size()); ++hi) {
                const Horizon& hz = app_.foreground->horizons[size_t(hi)];
                const bool active = hi == app_.foreground->activeHorizon;
                QPolygonF run;
                auto flush = [&]() {
                    if (run.size() > 1) {
                        painter.setPen(QPen(QColor(0, 0, 0, 140), active ? 4.5 : 3.0));
                        painter.drawPolyline(run);
                        painter.setPen(QPen(hz.color, active ? 2.5 : 1.5));
                        painter.drawPolyline(run);
                    }
                    run.clear();
                };
                for (int64_t t = first; t <= last && sampleMs > 0.0; t += step) {
                    const float ms = t < int64_t(hz.timeMs.size()) ? hz.timeMs[size_t(t)] : NAN;
                    if (!std::isfinite(ms)) {
                        flush();
                        continue;
                    }
                    double x = 0.0, y = 0.0;
                    pixelForData(double(t) + 0.5, ms / sampleMs, x, y);
                    run << QPointF(x, y);
                }
                flush();
                painter.setPen(QPen(Qt::black, 1));
                painter.setBrush(hz.color);
                for (int64_t a : hz.anchors) {
                    if (a < first || a > last || a >= int64_t(hz.timeMs.size()) || !std::isfinite(hz.timeMs[size_t(a)])) continue;
                    double x = 0.0, y = 0.0;
                    pixelForData(double(a) + 0.5, hz.timeMs[size_t(a)] / sampleMs, x, y);
                    painter.drawRect(QRectF(x - 3.5, y - 3.5, 7.0, 7.0));
                }
                painter.setBrush(Qt::NoBrush);
            }
            painter.restore();
        }

        if (zoomBandActive_) {
            painter.save();
            painter.setPen(QPen(QColor(kAccentColor), 1.5, Qt::DashLine));
            QColor fill = kAccentColor;
            fill.setAlpha(40);
            painter.setBrush(fill);
            painter.drawRect(QRect(zoomBandStart_, zoomBandEnd_).normalized());
            painter.restore();
        }
        lap("horizons");
        // Linked crosshair: where the cursor is on the other panel (same
        // trace index and time), black underlay + white dashes like the
        // chart crosshairs. Not on an x-f display (frequency, not time).
        if (linkedCrosshairValid_ && !app_.display.frequencyAxis) {
            const double sampleMs = app_.foreground->binHeader.sampleIntervalUs / 1000.0;
            if (sampleMs > 0.0) {
                double x = 0.0, y = 0.0;
                pixelForData(linkedTraceCoord_, linkedTimeMs_ / sampleMs, x, y);
                const QRect plot(kCanvasMargin + chrome.plotX, topInset, chrome.plotWidth, insetHeight);
                painter.save();
                painter.setClipRect(plot);
                for (const QPen& pen : {QPen(QColor(0, 0, 0, 170), 3), QPen(Qt::white, 1, Qt::DashLine)}) {
                    painter.setPen(pen);
                    painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
                    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
                }
                painter.restore();
            }
        }
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

bool SegyCanvas::refuseIfViewLocked() {
    if (!app_.viewLocked) return false;
    if (statusBar_) statusBar_->showMessage("View is locked (Lock View)", 2000);
    return true;
}

void SegyCanvas::setLinkedCrosshair(bool valid, double traceCoord, double timeMs) {
    if (!valid && !linkedCrosshairValid_) return;
    linkedCrosshairValid_ = valid;
    linkedTraceCoord_ = traceCoord;
    linkedTimeMs_ = timeMs;
    update();
}

void SegyCanvas::setBoxCompletedCallback(std::function<void()> callback) { onBoxCompleted_ = std::move(callback); }

void SegyCanvas::setPickCallback(std::function<void(double, double, bool)> callback) { onPick_ = std::move(callback); }

void SegyCanvas::mousePressEvent(QMouseEvent* event) {
    if (notifyActivated_) notifyActivated_();
    if (!app_.loaded) return;
    if (event->button() == Qt::RightButton) {
        // The color bar's column: color scale menu only.
        if (app_.display.showColorBar && amplitudeScale_->isVisible() && event->pos().x() >= kCanvasMargin + plotAreaWidth()) {
            if (colorScaleMenu_) colorScaleMenu_(event->globalPosition().toPoint());
            return;
        }
        double traceCoord = 0.0, sampleCoord = 0.0;
        if (!dataCoordAt(event->pos().x(), event->pos().y(), traceCoord, sampleCoord)) {
            showAxisRangeMenu(event->globalPosition().toPoint());
            return;
        }
    }
    // Middle-drag pans in every tool but Box Select (where middle-click
    // deletes the box).
    if (event->button() == Qt::MiddleButton && app_.tool != ToolMode::BoxSelect) {
        if (refuseIfViewLocked()) return;
        middlePanning_ = true;
        app_.dragAnchorX = event->pos().x();
        app_.dragAnchorY = event->pos().y();
        app_.dragAnchorView = app_.view;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() == Qt::RightButton && (app_.tool == ToolMode::Arrow || app_.tool == ToolMode::Pan)) {
        showAxisRangeMenu(event->globalPosition().toPoint());
        return;
    }
    if (app_.tool == ToolMode::Arrow && event->button() == Qt::LeftButton) {
        double traceCoord = 0.0, sampleCoord = 0.0;
        if (dataCoordAt(event->pos().x(), event->pos().y(), traceCoord, sampleCoord) && !app_.viewLocked) {
            zoomBandActive_ = true;
            zoomBandStart_ = zoomBandEnd_ = event->pos();
        }
        return;
    }
    if (app_.tool == ToolMode::Pick) {
        double traceCoord = 0.0, sampleCoord = 0.0;
        if ((event->button() == Qt::LeftButton || event->button() == Qt::RightButton) && onPick_ &&
            dataCoordAt(event->pos().x(), event->pos().y(), traceCoord, sampleCoord)) {
            onPick_(traceCoord, sampleCoord, event->button() == Qt::RightButton);
        }
        return;
    }
    if (app_.tool == ToolMode::BoxSelect) {
        handleBoxSelectPress(event);
    } else if (app_.tool == ToolMode::Pan && event->button() == Qt::LeftButton) {
        if (refuseIfViewLocked()) return;
        app_.dragging = true;
        setCursor(Qt::ClosedHandCursor);
        app_.dragAnchorX = event->pos().x();
        app_.dragAnchorY = event->pos().y();
        app_.dragAnchorView = app_.view;
    }
    updateStatusBar();
}

void SegyCanvas::showAxisRangeMenu(const QPoint& globalPos) {
    if (!app_.loaded) return;
    if (refuseIfViewLocked()) return;
    // The same menu as every chart's.
    QMenu menu(this);
    QAction* previous = menu.addAction("Previous Zoom");
    previous->setEnabled(!app_.zoomHistory.empty());
    QAction* reset = menu.addAction("Full View");
    menu.addSeparator();
    QAction* ranges = menu.addAction("Axis Ranges...");
    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;
    if (chosen == previous) {
        if (onPreviousZoom_) onPreviousZoom_();
        return;
    }
    const int64_t traceCount = app_.foreground->traceCount;
    const int samplesPerTrace = app_.foreground->binHeader.samplesPerTrace;
    const double sampleMs = app_.foreground->binHeader.sampleIntervalUs / 1000.0; // Hz per bin on x-f
    segy::ViewRange next = app_.view;
    if (chosen == reset) {
        segy::resetView(next, traceCount, samplesPerTrace);
    } else if (chosen == ranges) {
        // Traces as the hover readout numbers them (x-f: the seismic's).
        const double offset = app_.xf.active ? double(app_.xf.traceOffset) : 0.0;
        const double stride = app_.xf.active ? double(app_.xf.traceStride) : 1.0;
        double x0 = app_.view.traceStart * stride + offset, x1 = app_.view.traceEnd * stride + offset;
        double y0 = app_.view.sampleStart * sampleMs, y1 = app_.view.sampleEnd * sampleMs;
        const QString yName = app_.display.frequencyAxis ? "Frequency (Hz)"
                              : app_.display.depthAxis   ? "Depth (m)"
                                                         : "Time (ms)";
        if (!editAxisRanges(this, "Axis Ranges", "Trace", yName, x0, x1, y0, y1, 0, 2)) return;
        next.traceStart = (x0 - offset) / stride;
        next.traceEnd = (x1 - offset) / stride;
        next.sampleStart = sampleMs > 0.0 ? y0 / sampleMs : next.sampleStart;
        next.sampleEnd = sampleMs > 0.0 ? y1 / sampleMs : next.sampleEnd;
        segy::clampView(next, traceCount, samplesPerTrace);
    } else {
        return;
    }
    app_.zoomHistory.push_back(app_.view);
    app_.view = next;
    if (notifyViewChanged_) notifyViewChanged_();
    notifyStateChanged();
    update();
}

void SegyCanvas::mouseMoveEvent(QMouseEvent* event) {
    hoverPixelX_ = event->pos().x();
    hoverPixelY_ = event->pos().y();
    hovering_ = true;
    bool needsRepaint = false;

    if (zoomBandActive_) {
        zoomBandEnd_ = event->pos();
        update();
        return;
    }
    if ((app_.tool == ToolMode::Pan && app_.dragging) || middlePanning_) {
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

void SegyCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (zoomBandActive_ && event->button() == Qt::LeftButton) finishZoomBand();
    if (middlePanning_ && event->button() == Qt::MiddleButton) middlePanning_ = false;
    if (app_.dragging) app_.dragging = false;
    unsetCursorForTool();
}

void SegyCanvas::finishZoomBand() {
    zoomBandActive_ = false;
    update();
    const QRect band = QRect(zoomBandStart_, zoomBandEnd_).normalized();
    if (band.width() < 8 || band.height() < 8) return; // a click, not a zoom
    double t0, s0, t1, s1;
    if (!dataCoordAt(band.left(), band.top(), t0, s0) || !dataCoordAt(band.right(), band.bottom(), t1, s1)) {
        // Clamp to the plot: use the band corners that fall inside it.
        dataCoordAt(std::max(band.left(), 0), std::max(band.top(), 0), t0, s0);
        dataCoordAt(band.right(), band.bottom(), t1, s1);
    }
    if (refuseIfViewLocked()) return;
    app_.zoomHistory.push_back(app_.view);
    app_.view.traceStart = std::min(t0, t1);
    app_.view.traceEnd = std::max(t0, t1);
    app_.view.sampleStart = std::min(s0, s1);
    app_.view.sampleEnd = std::max(s0, s1);
    segy::clampView(app_.view, app_.foreground->traceCount, app_.foreground->binHeader.samplesPerTrace);
    if (notifyViewChanged_) notifyViewChanged_();
    notifyStateChanged();
    update();
}

void SegyCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (app_.tool != ToolMode::Arrow && app_.tool != ToolMode::Pan) return;
    zoomBandActive_ = false;
    if (onPreviousZoom_) onPreviousZoom_();
}

void SegyCanvas::showFullView() {
    if (!app_.loaded || refuseIfViewLocked()) return;
    app_.zoomHistory.push_back(app_.view);
    segy::resetView(app_.view, app_.foreground->traceCount, app_.foreground->binHeader.samplesPerTrace);
    if (notifyViewChanged_) notifyViewChanged_();
    notifyStateChanged();
    update();
}

void SegyCanvas::zoomByFactor(double factor) {
    if (!app_.loaded || refuseIfViewLocked()) return;
    const int insetWidth = std::max(1, plotAreaWidth());
    IdentInsets insets = identInsets();
    const int insetHeight = std::max(1, height() - (kCanvasMargin + insets.top) - (kCanvasMargin + insets.bottom));
    app_.zoomHistory.push_back(app_.view);
    segy::zoomAt(app_.view, app_.foreground->traceCount, app_.foreground->binHeader.samplesPerTrace, insetWidth,
                 insetHeight, insetWidth / 2, insetHeight / 2, factor);
    if (notifyViewChanged_) notifyViewChanged_();
    notifyStateChanged();
    update();
}

void SegyCanvas::wheelEvent(QWheelEvent* event) {
    if (!app_.loaded) return;
    if (refuseIfViewLocked()) return;
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
    static QElapsedTimer wheelClock;
    if (!wheelClock.isValid()) wheelClock.start();
    if (wheelClock.elapsed() - lastWheelMs_ > 600) {
        app_.zoomHistory.push_back(app_.view);
        notifyStateChanged();
    }
    lastWheelMs_ = wheelClock.elapsed();
    segy::zoomAt(app_.view, app_.foreground->traceCount, app_.foreground->binHeader.samplesPerTrace, insetWidth, insetHeight, px, py, factor);
    if (notifyViewChanged_) notifyViewChanged_();
    updateStatusBar();
    update();
}

void SegyCanvas::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_R || event->key() == Qt::Key_Home) && app_.loaded) {
        if (refuseIfViewLocked()) return;
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

void SegyCanvas::enterXfMode(std::shared_ptr<Dataset> xfDataset, int64_t traceOffset, int64_t traceStride) {
    if (!app_.loaded || !xfDataset) return;
    XfState& xf = app_.xf;
    if (!xf.active) {
        xf.seismic = app_.foreground;
        xf.view = app_.view;
        xf.display = app_.display;
        xf.selectionBoxes = app_.selectionBoxes;
        xf.activeBoxIndex = app_.activeBoxIndex;
        xf.zoomHistory = app_.zoomHistory;
    }
    xf.active = true;
    xf.traceOffset = traceOffset;
    xf.traceStride = std::max<int64_t>(1, traceStride);
    app_.foreground = std::move(xfDataset);
    segy::resetView(app_.view, app_.foreground->traceCount, app_.foreground->binHeader.samplesPerTrace);
    // Variable density, -40 dB (green) .. 0 dB (red) by default: clip 20 dB
    // either side of -20 dB. No gain/AGC -- the values are already dB.
    segy::DisplaySettings& d = app_.display;
    d.seismicMode = segy::SeismicDisplayMode::VariableDensity;
    d.colorScale = xf.colorScale;
    d.colorScaleFlipped = xf.colorScaleFlipped;
    d.gainDb = 0.0;
    d.manualClip = segy::ManualClip{true, 20.0f, 20.0f, -20.0f};
    d.clipLocked = false;
    d.reversePolarity = false;
    d.processing = segy::AmplitudeProcessing{};
    d.frequencyAxis = true;
    app_.selectionBoxes.clear();
    app_.activeBoxIndex = -1;
    app_.draggingBoxIndex = -1;
    app_.draggingCorner = -1;
    app_.zoomHistory.clear();
    notifyStateChanged();
    updateStatusBar();
    update();
}

void SegyCanvas::exitXfMode() {
    XfState& xf = app_.xf;
    if (!xf.active) return;
    // Remembered for the next x-f of this session.
    xf.colorScale = app_.display.colorScale;
    xf.colorScaleFlipped = app_.display.colorScaleFlipped;
    app_.foreground = std::move(xf.seismic);
    app_.view = xf.view;
    app_.display = xf.display;
    app_.selectionBoxes = std::move(xf.selectionBoxes);
    app_.activeBoxIndex = xf.activeBoxIndex;
    app_.draggingBoxIndex = -1;
    app_.draggingCorner = -1;
    app_.zoomHistory = std::move(xf.zoomHistory);
    xf.active = false;
    xf.seismic.reset();
    notifyStateChanged();
    updateStatusBar();
    update();
}

namespace {
// The data's sorting keys, shown on top by default: of FFID, CDP and
// offset, those that change at all over the first traces, slowest-changing
// first (F3 prestack: FFID, CDP, offset; a stack: CDP). Sequence number
// when none of them change.
void applyDefaultSortIdents(AppState& app) {
    std::fill(std::begin(app.idents.topEnabled), std::end(app.idents.topEnabled), false);
    const Dataset& ds = *app.foreground;
    const int64_t n = std::min<int64_t>(ds.traceCount, 5000);
    if (n <= 0) return;
    const IdentField candidates[] = {IdentField::FieldRecord, IdentField::Cdp, IdentField::Offset};
    std::vector<std::pair<int64_t, IdentField>> changing;
    for (IdentField field : candidates) {
        int64_t changes = 0;
        int32_t previous = 0;
        for (int64_t t = 0; t < n; ++t) {
            const uint8_t* base = ds.file.data() + segy::kHeaderTotalSize + size_t(t) * ds.traceStrideBytes;
            const int32_t value = identFieldValue(segy::parseTraceHeader(base), field);
            if (t > 0 && value != previous) ++changes;
            previous = value;
        }
        if (changes > 0) changing.push_back({changes, field});
    }
    std::stable_sort(changing.begin(), changing.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    if (changing.empty()) changing.push_back({0, IdentField::TraceSequenceLine});
    for (const auto& entry : changing) app.idents.topEnabled[int(entry.second)] = true;
}
} // namespace

void SegyCanvas::setForegroundDataset(std::shared_ptr<Dataset> dataset) {
    if (app_.xf.active) {
        // A different Foreground ends x-f: back to the seismic display
        // settings, but the parked dataset and boxes are not restored.
        app_.display = app_.xf.display;
        app_.xf.active = false;
        app_.xf.seismic.reset();
        app_.xf.selectionBoxes.clear();
        app_.xf.zoomHistory.clear();
    }
    // Never mutate a Dataset's fields in place -- it may be shared with
    // the pool or another panel's Background slot, so this only ever
    // reassigns the pointer (an O(1) swap, whether the dataset is brand
    // new from startLoading or already sitting in the pool).
    app_.foreground = std::move(dataset);
    app_.loaded = true;
    app_.flatten = FlattenState{}; // a flattened display ends with any new Foreground
    app_.display.depthAxis = app_.foreground->depth;
    applyDefaultSortIdents(app_);
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

void SegyCanvas::startLoading(const std::filesystem::path& path, std::vector<std::string> processingHistory,
                              std::function<void(std::shared_ptr<Dataset>)> onLoaded) {
    if (app_.loading) return;
    app_.loaded = false;
    app_.loading = true;
    app_.progressDone = 0;
    app_.progressTotal = 1;

    std::thread([this, path, processingHistory = std::move(processingHistory), onLoaded = std::move(onLoaded)]() mutable {
        // Must be a genuinely atomic timestamp -- buildPyramid's progress
        // callback runs concurrently from multiple pool worker threads. A
        // plain std::chrono::time_point mutated across threads without
        // synchronization was a real, confirmed bug in the removed shells
        // (see native/README.md); carrying that fix forward here.
        std::atomic<int64_t> lastPostMs{0};
        QElapsedTimer loadTimer;
        loadTimer.start();
        // Its own pool: the panel's pool must stay free for painting (a
        // flatten display loads while the original is still shown, and a
        // shared pool froze the paint for 1.9 s on a 6 GB file).
        segy::ThreadPool loadPool;
        segy::LoadResult result = segy::loadSegyFile(path, loadPool, [this, &lastPostMs](int64_t done, int64_t total) {
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

        perfLog(QString("load %1: %2 ms (%3 traces, %4 MB)")
                    .arg(QString::fromStdString(path.filename().string()))
                    .arg(loadTimer.elapsed())
                    .arg(result.traceCount)
                    .arg(result.ok ? double(result.file.size()) / 1048576.0 : 0.0, 0, 'f', 0));
        QMetaObject::invokeMethod(
            this,
            [this, path, result = std::move(result), processingHistory = std::move(processingHistory),
             onLoaded = std::move(onLoaded)]() mutable {
                PerfScope perf("load completion on the UI thread", 0);
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
                    // The domain follows the last "DOMAIN: ..." history line
                    // (a dataset derived from depth data is depth too).
                    for (auto it = dataset->processingHistory.rbegin(); it != dataset->processingHistory.rend(); ++it) {
                        if (it->rfind("DOMAIN: ", 0) == 0) {
                            dataset->depth = it->find("DEPTH") != std::string::npos;
                            break;
                        }
                    }
                    if (onLoaded) {
                        app_.loaded = true; // the current Foreground is still there
                        onLoaded(dataset);
                        updateStatusBar();
                        update();
                        return;
                    }
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
                    app_.loaded = bool(onLoaded) && app_.foreground->traceCount > 0;
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

namespace {
// QMenu::clear() deletes the menu's own actions but not child QMenus made
// by addMenu() or QActionGroups parented to it -- a menu repopulated on
// every aboutToShow (View > Idents) would otherwise leak those each time.
void clearMenuCompletely(QMenu* menu) {
    menu->clear();
    const QObjectList children = menu->children();
    for (QObject* child : children) {
        if (qobject_cast<QMenu*>(child) || qobject_cast<QActionGroup*>(child)) delete child;
    }
}
} // namespace

MainWindow::MainWindow() : QMainWindow() {
    setWindowTitle("TraceLab");
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
    for (Panel* p : {&panelA_, &panelB_}) {
        p->canvas->setPickCallback(
            [this, p](double traceCoord, double sampleCoord, bool remove) { handlePick(p, traceCoord, sampleCoord, remove); });
    }
    for (Panel* p : {&panelA_, &panelB_}) {
        p->canvas->setBoxCompletedCallback([this, p]() {
            if (!pendingBoxZoom_ || p != activePanel_) return;
            pendingBoxZoom_ = false;
            zoomToBox();
            if (boxSelectAction_->isChecked()) boxSelectAction_->trigger(); // back to the pointer
        });
    }
    panelB_.canvas->setActivatedCallback([this]() { setActivePanel(&panelB_); });

    // The menu bar, in order (see native/README.md, "Menus"). Everything is
    // filled in below or after the toolbar, once the actions exist.
    QMenu* fileMenu = menuBar()->addMenu("&File");
    QMenu* viewMenu = menuBar()->addMenu("&View");
    QMenu* analysisMenu = menuBar()->addMenu("&Analysis");
    QMenu* processingMenu = menuBar()->addMenu("&Processing");
    QMenu* horizonsMenu = menuBar()->addMenu("Hori&zons");
    QMenu* windowMenu = menuBar()->addMenu("&Window");
    QMenu* helpMenu = menuBar()->addMenu("&Help");

    QAction* openAction = fileMenu->addAction("&Open...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);
    QAction* openAttributeAction = fileMenu->addAction("Open &Velocity or Attribute...");
    openAttributeAction->setToolTip("A velocity model or another attribute SEG-Y, for the dataset list");
    connect(openAttributeAction, &QAction::triggered, this, &MainWindow::openAttributeFile);
    QAction* saveSegyAction = fileMenu->addAction("&Save As SEG-Y...");
    saveSegyAction->setShortcut(QKeySequence::Save);
    connect(saveSegyAction, &QAction::triggered, this, &MainWindow::openSaveSegyDialog);
    fileMenu->addSeparator();
    QMenu* headersMenu = fileMenu->addMenu("&Headers"); // filled after the toolbar
    fileMenu->addSeparator();
    QAction* preferencesAction = fileMenu->addAction("&Preferences...");
    preferencesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::openPreferencesDialog);
    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction("E&xit");
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);

    QMenu* domainMenu = new QMenu("Data &Domain", this);
    {
        QActionGroup* domainGroup = new QActionGroup(this);
        domainTimeAction_ = domainMenu->addAction("&Time");
        domainDepthAction_ = domainMenu->addAction("&Depth");
        for (QAction* a : {domainTimeAction_, domainDepthAction_}) {
            a->setCheckable(true);
            domainGroup->addAction(a);
        }
        domainTimeAction_->setChecked(true);
        domainTimeAction_->setToolTip("The Foreground's samples are two-way time (ms)");
        domainDepthAction_->setToolTip("The Foreground's samples are depth (m)");
        connect(domainTimeAction_, &QAction::triggered, this, [this]() { setDatasetDomain(activePanel_, false); });
        connect(domainDepthAction_, &QAction::triggered, this, [this]() { setDatasetDomain(activePanel_, true); });
    }
    QMenu* datasetLabelsMenu = new QMenu("Dataset &Labels", this);
    QActionGroup* datasetLabelsGroup = new QActionGroup(this);
    const std::pair<const char*, DatasetLabelPosition> labelPositions[] = {
        {"Top Left", DatasetLabelPosition::TopLeft},
        {"Bottom Left", DatasetLabelPosition::BottomLeft},
        {"Top Right", DatasetLabelPosition::TopRight},
        {"Bottom Right", DatasetLabelPosition::BottomRight},
        {"Off", DatasetLabelPosition::Off},
    };
    for (const auto& entry : labelPositions) {
        QAction* action = datasetLabelsMenu->addAction(entry.first);
        action->setCheckable(true);
        action->setData(int(entry.second));
        action->setActionGroup(datasetLabelsGroup);
        connect(action, &QAction::triggered, this, [this, position = entry.second]() {
            applyDatasetLabelPosition(position);
            savePreferences();
        });
    }
    connect(datasetLabelsMenu, &QMenu::aboutToShow, this, [this, datasetLabelsGroup]() {
        for (QAction* action : datasetLabelsGroup->actions()) {
            action->setChecked(action->data().toInt() == int(datasetLabelPosition_));
        }
    });

    // Seismic Display: real, working toggles (unlike the placeholders
    // above) -- DisplaySettings is read fresh every frame, so flipping
    // these and repainting is all that's needed, same pattern the existing
    // time-scale/color-bar/grid-line settings already follow. Also
    // reachable from the toolbar's single toggle button now (below); both
    // drive the same app_.display.seismicMode and stay in sync with each
    // other.
    QActionGroup* seismicDisplayMenuGroup = new QActionGroup(this);
    variableDensityMenuAction_ = new QAction("&Variable Density", this);
    variableDensityMenuAction_->setCheckable(true);
    variableDensityMenuAction_->setChecked(true);
    seismicDisplayMenuGroup->addAction(variableDensityMenuAction_);
    connect(variableDensityMenuAction_, &QAction::triggered, this,
            [this]() { setSeismicMode(segy::SeismicDisplayMode::VariableDensity); });
    wiggleMenuAction_ = new QAction("&Wiggle", this);
    wiggleMenuAction_->setCheckable(true);
    seismicDisplayMenuGroup->addAction(wiggleMenuAction_);
    connect(wiggleMenuAction_, &QAction::triggered, this,
            [this]() { setSeismicMode(segy::SeismicDisplayMode::Wiggle); });

    QAction* wiggleInfillAction = new QAction("Wiggle &Infill", this);
    wiggleInfillAction->setCheckable(true);
    wiggleInfillAction->setChecked(true);
    connect(wiggleInfillAction, &QAction::triggered, this, [this](bool checked) {
        for (Panel* p : targetPanels()) {
            p->app.display.wiggleInfill = checked;
            p->canvas->update();
        }
    });

    // Processing: everything that computes a new dataset, one flat list.
    QAction* calculatorAction = processingMenu->addAction("&Calculator...");
    connect(calculatorAction, &QAction::triggered, this, &MainWindow::openCalculatorDialog);
    processingMenu->addSeparator();
    QAction* bandpassAction = processingMenu->addAction("&Bandpass Filter...");
    connect(bandpassAction, &QAction::triggered, this, &MainWindow::openBandpassDialog);
    QAction* fkFilterAction = processingMenu->addAction("&f-k Filter...");
    connect(fkFilterAction, &QAction::triggered, this, &MainWindow::openFkFilter);
    QAction* whiteningAction = processingMenu->addAction("Spectral &Whitening...");
    connect(whiteningAction, &QAction::triggered, this, &MainWindow::openSpectralWhitening);
    QAction* deconAction = processingMenu->addAction("D&econvolution...");
    connect(deconAction, &QAction::triggered, this, &MainWindow::openDeconvolution);
    processingMenu->addSeparator();
    // Attributes: no questions asked, except the rotation angle.
    QMenu* attributesMenu = processingMenu->addMenu("&Attributes");
    QAction* envelopeAction = attributesMenu->addAction("&Envelope");
    connect(envelopeAction, &QAction::triggered, this, [this]() { runAttribute(0); });
    QAction* phaseAction = attributesMenu->addAction("Instantaneous &Phase");
    connect(phaseAction, &QAction::triggered, this, [this]() { runAttribute(1); });
    QAction* frequencyAction = attributesMenu->addAction("Instantaneous &Frequency");
    connect(frequencyAction, &QAction::triggered, this, [this]() { runAttribute(2); });
    attributesMenu->addSeparator();
    QAction* rotationAction = attributesMenu->addAction("Phase &Rotation...");
    connect(rotationAction, &QAction::triggered, this, [this]() { runAttribute(3); });
    QAction* integrationAction = processingMenu->addAction("&Integration...");
    connect(integrationAction, &QAction::triggered, this, [this]() { openTraceDerivative(true); });
    QAction* differentiationAction = processingMenu->addAction("&Differentiation...");
    connect(differentiationAction, &QAction::triggered, this, [this]() { openTraceDerivative(false); });
    QAction* scaleAction = processingMenu->addAction("&Scale...");
    connect(scaleAction, &QAction::triggered, this, &MainWindow::openTraceScale);
    processingMenu->addSeparator();
    QAction* timeDepthAction = processingMenu->addAction("&Time-Depth Conversion...");
    connect(timeDepthAction, &QAction::triggered, this, &MainWindow::openTimeDepthConversion);

    // Horizons (Pick Horizon goes first, once the toolbar action exists).
    {
        QAction* managerAction = horizonsMenu->addAction("Horizon &Manager...");
        connect(managerAction, &QAction::triggered, this, &MainWindow::openHorizonManager);
        horizonsMenu->addSeparator();
        QAction* seabedAction = horizonsMenu->addAction("Auto-pick &Seabed");
        connect(seabedAction, &QAction::triggered, this, [this]() { autoPickSeabed(activePanel_); });
        flattenAction_ = horizonsMenu->addAction("&Flatten...");
        flattenAction_->setCheckable(true);
        flattenAction_->setToolTip("Flatten on a horizon or a header time; choose again to go back");
        connect(flattenAction_, &QAction::triggered, this, [this](bool on) { toggleFlatten(activePanel_, on); });
        horizonsMenu->addSeparator();
        QAction* importAction = horizonsMenu->addAction("&Import Horizon...");
        connect(importAction, &QAction::triggered, this, [this]() { importHorizon(activePanel_); });
        QAction* exportAction = horizonsMenu->addAction("&Export Horizon...");
        connect(exportAction, &QAction::triggered, this, [this]() { exportHorizon(activePanel_); });
        QAction* toHeaderAction = horizonsMenu->addAction("Write Horizon to &Header...");
        connect(toHeaderAction, &QAction::triggered, this, [this]() { writeHorizonToHeader(activePanel_); });
    }

    // Help.
    QAction* mouseHelpAction = helpMenu->addAction("&Mouse and Keyboard");
    mouseHelpAction->setShortcut(QKeySequence::HelpContents);
    connect(mouseHelpAction, &QAction::triggered, this, &MainWindow::showMouseAndKeyboardHelp);
    QAction* reportBugAction = helpMenu->addAction("&Report a Bug...");
    connect(reportBugAction, &QAction::triggered, this, &MainWindow::reportBug);
    helpMenu->addSeparator();
    QAction* aboutAction = helpMenu->addAction("&About TraceLab");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

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

    // Zooming is done with the mouse (see native/README.md, "Zoom"): the
    // toolbar keeps the two steps you can't do with a gesture alone.
    moozAction_ = toolbar->addAction(iconMagnifier(false), "Previous Zoom");
    moozAction_->setToolTip("Back to the previous zoom (Backspace, or double-click the display)");
    moozAction_->setEnabled(false);
    connect(moozAction_, &QAction::triggered, this, &MainWindow::moozBack);

    fullViewAction_ = toolbar->addAction(iconFullView(), "Full View");
    fullViewAction_->setToolTip("Show all traces and samples (Home)");
    connect(fullViewAction_, &QAction::triggered, this, [this]() {
        for (Panel* p : targetPanels()) p->canvas->showFullView();
    });

    // Zoom to the selection box: in the View menu only.
    zoomAction_ = new QAction(iconMagnifier(true), "Zoom to Box", this);
    zoomAction_->setToolTip("Zoom to the selection box");
    zoomAction_->setEnabled(false);
    connect(zoomAction_, &QAction::triggered, this, &MainWindow::zoomToBox);

    toolbarSeparators_.push_back(toolbar->addSeparator());

    QActionGroup* toolGroup = new QActionGroup(this);
    toolGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);

    panAction_ = toolbar->addAction(iconHand(), "Pan");
    panAction_->setCheckable(true);
    panAction_->setToolTip("Pan: drag to move the view (the middle mouse button pans in any tool)");
    toolGroup->addAction(panAction_);
    connect(panAction_, &QAction::triggered, this, [this](bool checked) {
        for (Panel* p : targetPanels()) {
            p->app.tool = checked ? ToolMode::Pan : ToolMode::Arrow;
            p->canvas->unsetCursorForTool();
        }
        refreshToolChrome();
    });

    boxSelectAction_ = toolbar->addAction(iconRectangle(), "Box Select");
    boxSelectAction_->setCheckable(true);
    boxSelectAction_->setToolTip("Box Select: draw a box for Spectrum, Histogram and the other analyses; "
                                 "drag a corner to change it, middle-click to delete it");
    toolGroup->addAction(boxSelectAction_);
    // Pick: click anchors of the active horizon (Horizons menu, Horizon Manager).
    pickAction_ = toolbar->addAction(iconPick(), "Pick Horizon");
    pickAction_->setCheckable(true);
    pickAction_->setToolTip("Pick Horizon: click points, the traces between are tracked; right-click removes a point");
    toolGroup->addAction(pickAction_);
    connect(pickAction_, &QAction::triggered, this, [this](bool checked) {
        for (Panel* p : targetPanels()) {
            p->app.tool = checked ? ToolMode::Pick : ToolMode::Arrow;
            p->canvas->unsetCursorForTool();
        }
        if (checked && app_->loaded) activeHorizonFor(activePanel_, true);
        refreshHorizonManager();
        refreshToolChrome();
    });
    // The Horizons menu exists already (created with the menu bar, before
    // the toolbar): mirror the tool there, after Horizon Manager.
    addMenuMirror(horizonsMenu, pickAction_, "&Pick Horizon", horizonsMenu->actions().value(0));
    connect(boxSelectAction_, &QAction::triggered, this, [this](bool checked) {
        for (Panel* p : targetPanels()) {
            p->app.tool = checked ? ToolMode::BoxSelect : ToolMode::Arrow;
            p->canvas->unsetCursorForTool();
        }
        refreshToolChrome();
    });

    toolbarSeparators_.push_back(toolbar->addSeparator());

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
    flipHorizontalAction_->setToolTip("Flip Horizontal: mirror the display left to right");
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
    identsAction_->setToolTip("Idents: trace-header values above and below the display");
    connect(identsAction_, &QAction::triggered, this, [this]() {
        QMenu menu(this);
        populateIdentsMenu(menu);
        // Hidden (customized away, or overflowed and reached via the More
        // menu): no button to anchor to, so pop up at the cursor instead.
        QWidget* button = toolbar_->widgetForAction(identsAction_);
        if (button && button->isVisible()) {
            menu.exec(button->mapToGlobal(QPoint(0, button->height())));
        } else {
            menu.exec(QCursor::pos());
        }
    });

    toolbarSeparators_.push_back(toolbar->addSeparator());

    // Gain: dB gain applied on top of the pyramid's auto-computed clip
    // (see DisplaySettings::gainDb) -- auto-scaling stays the baseline,
    // this just biases sensitivity around it. A slider (matching the
    // approved mockup) paired with a spin box for direct typing --
    // "select the value and type a number, auto-clipped" from the
    // original ask -- both bound to the same value and kept in sync. The
    // "Gain" label was replaced with a disclosure button (caret-up-down)
    // that opens the fuller DisplayParametersDialog.
    gainDetailAction_ = toolbar->addAction(iconAdjustments(), "Display Parameters");
    gainDetailAction_->setToolTip("Display Parameters: colours, clip, wiggle and gain");
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
    gainValueLabel_->setStyleSheet(themed("color: #d4d4d8; font-size: 10px;"));
    // Its fixed width (matching a toolbar icon button's actual rendered
    // width, so it lines up flush instead of leaving dead space beside it)
    // is set later, in applyToolbarPosition() -- querying any button's
    // sizeHint() this early, right after adding it, runs before the
    // toolbar's layout has settled and undersizes it.
    // The up/down arrows only step in fixed 3 dB increments -- right-click
    // the value between them for an exact, arbitrary value instead.
    gainValueLabel_->setToolTip("Right-click to enter an exact gain value");
    gainValueLabel_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(gainValueLabel_, &QWidget::customContextMenuRequested, this, [this](const QPoint&) { promptForGainDb(); });
    gainValueLabelAction_ = toolbar->addWidget(gainValueLabel_);

    gainDownAction_ = toolbar->addAction(iconChevronDown(), "Gain -3 dB");
    gainDownAction_->setToolTip("Decrease gain by 3 dB");
    connect(gainDownAction_, &QAction::triggered, this,
            [this]() { setGainDb(std::clamp(app_->display.gainDb - 3.0, -6.0, 24.0)); });

    // AGC quick toggle (window/level and computed gain: View > Amplitude
    // Processing, or right-click this button). Keeps whatever window/level
    // the panel already had -- it only flips agcEnabled.
    agcAction_ = toolbar->addAction(iconAgc(), "AGC");
    agcAction_->setCheckable(true);
    agcAction_->setToolTip("AGC: automatic gain control (right-click for its window and level)");
    connect(agcAction_, &QAction::triggered, this, [this](bool checked) {
        segy::AmplitudeProcessing processing = app_->display.processing;
        processing.agcEnabled = checked;
        setAmplitudeProcessing(processing);
    });

    toolbarSeparators_.push_back(toolbar->addSeparator());

    // Spectrum/Histogram both open real parameters dialogs (see
    // native/README.md). Lock is a real two-state toggle -- see
    // targetPanels() -- that makes split view's toolbar/menu actions
    // (tool mode, gain, seismic mode, zoom/mooz, wiggle infill, box
    // delete) apply to both panels at once instead of just the active
    // one, icon swaps lock-open/lock-closed accordingly.
    spectrumAction_ = toolbar->addAction(iconChartArea(), "Spectrum");
    // Like every other analysis: the result straight away (of the active
    // selection box, or else the whole dataset); its window has Parameters.
    spectrumAction_->setToolTip("Amplitude spectrum of the selection box, or of the whole dataset");
    connect(spectrumAction_, &QAction::triggered, this, [this]() {
        if (!activePanel_->app.loaded) {
            flashStatusMessage("No data selected");
            return;
        }
        const SelectionBox* box = activeBox(activePanel_->app);
        activePanel_->app.spectrum.useSelection = box && box->complete;
        computeAndShowSpectrum(activePanel_);
    });

    histogramAction_ = toolbar->addAction(iconChartBar(), "Histogram");
    histogramAction_->setToolTip("Amplitude histogram of the selection box, or of the whole dataset");
    connect(histogramAction_, &QAction::triggered, this, [this]() {
        if (!activePanel_->app.loaded) {
            flashStatusMessage("No data selected");
            return;
        }
        const SelectionBox* box = activeBox(activePanel_->app);
        activePanel_->app.histogram.useSelection = box && box->complete;
        computeAndShowHistogram(activePanel_);
    });

    // Octave Band Display: bandpass-filters the active panel's *visible*
    // data into a row of 2^N Hz bands in a new window -- see
    // computeAndShowOctaveBands. Opens straight into a result with the
    // default 2-128 Hz range (the window's own Source/Min/Max controls
    // change it afterward), rather than a parameters dialog first.
    octaveBandAction_ = toolbar->addAction(iconColumns3(), "Octave Bands");
    octaveBandAction_->setToolTip("The view split into octave bands (2-128 Hz)");
    connect(octaveBandAction_, &QAction::triggered, this, [this]() {
        if (!activePanel_->app.loaded) {
            flashStatusMessage("No data selected");
            return;
        }
        openOctaveBandDialog();
        octaveBandDialog_->setSourcePanel(activePanel_ == &panelA_ ? 0 : 1);
        computeAndShowOctaveBands(activePanel_, 2, 128);
    });

    // Autocorrelation: opens on the active panel's visible data with the
    // dialog's current length (200 ms the first time).
    autocorrelationAction_ = toolbar->addAction(iconAutocorrelation(), "Autocorrelation");
    autocorrelationAction_->setToolTip("Autocorrelation of the view");
    connect(autocorrelationAction_, &QAction::triggered, this, [this]() {
        if (!activePanel_->app.loaded) {
            flashStatusMessage("No data selected");
            return;
        }
        openAutocorrelationDialog();
        computeAndShowAutocorrelation(activePanel_, autocorrelationDialog_->minLagMs(), autocorrelationDialog_->maxLagMs());
    });

    // x-f display: per-trace amplitude spectra of what's on screen, in dB
    // (see native/README.md). Toggling off restores the seismic display.
    xfAction_ = toolbar->addAction(iconXf(), "x-f Display");
    xfAction_->setCheckable(true);
    xfAction_->setToolTip("x-f Display: the amplitude spectrum (dB) of every trace in view; click again to go back");
    connect(xfAction_, &QAction::triggered, this, [this](bool checked) { toggleXf(activePanel_, checked); });

    // Text/Binary header viewers: no settings to keep sticky, so these just
    // re-populate from whichever panel is active on every click, unlike
    // Histogram/Spectrum/Display Parameters above.
    ebcdicHeaderAction_ = new QAction(iconFileDescription(), "Text Header", this);
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

    binaryHeaderAction_ = new QAction(iconFileDigit(), "Binary Header", this);
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
    splitViewAction_->setToolTip("Two panels (Window menu: layout, lock, linked crosshair)");
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
                setSplitOrientation(splitter_->orientation() == Qt::Horizontal ? Qt::Vertical : Qt::Horizontal);
            }
        });
    }

    lockAction_ = toolbar->addAction(iconLock(false), "Lock Panels");
    lockAction_->setCheckable(true);
    lockAction_->setToolTip("Lock Panels: toolbar and menu actions apply to both panels");
    connect(lockAction_, &QAction::triggered, this, [this](bool checked) {
        lockAction_->setIcon(iconLock(checked));
    });

    // Lock View (per panel): the view can't be zoomed, panned, reset or
    // given new axis ranges until unlocked -- see SegyCanvas's guards.
    lockViewAction_ = toolbar->addAction(iconLockView(), "Lock View");
    lockViewAction_->setCheckable(true);
    lockViewAction_->setToolTip("Lock View: this panel's zoom and position stay fixed");
    connect(lockViewAction_, &QAction::triggered, this, [this](bool checked) {
        app_->viewLocked = checked;
        flashStatusMessage(checked ? "View locked" : "View unlocked");
        refreshToolChrome();
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
    sidebarToggleAction_->setToolTip("Sidebar: the values under the cursor");

    // Toolbar overflow (see native/README.md) -- every plain icon button
    // above is a candidate; the gain slider/value-label/spacer are
    // deliberately excluded (the slider's only ever visible horizontally
    // to begin with, and a QWidgetAction's widget can't be safely shown
    // in both the toolbar and a menu at once -- it would just move).
    toolbarOverflowCandidates_ = {
        moozAction_,          fullViewAction_,     panAction_,         boxSelectAction_, pickAction_,
        seismicModeToolbarAction_, flipHorizontalAction_, identsAction_,  gainDetailAction_, gainUpAction_,
        gainDownAction_,      agcAction_,          spectrumAction_,     histogramAction_,   octaveBandAction_,
        autocorrelationAction_, xfAction_,
        splitViewAction_,   lockAction_, lockViewAction_,
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
            // Only the ones overflow actually hid -- not ones the user
            // removed from the toolbar (those live in the menus instead).
            if (action->isVisible() || !isToolbarActionWanted(action)) continue;
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

    // Customizable toolbar -- see ToolbarItem in viewer_qt.h. Groups match
    // the separators above (toolbarSeparators_[g] follows group g); group 5
    // is the sidebar toggle past the expanding spacer, which has no
    // separator of its own.
    toolbarItems_ = {
        {"mooz", "Previous Zoom", 0, {moozAction_}},
        {"fullView", "Full View", 0, {fullViewAction_}},
        {"pan", "Pan", 1, {panAction_}},
        {"boxSelect", "Box Select", 1, {boxSelectAction_}},
        {"pick", "Pick Horizon", 1, {pickAction_}},
        {"seismicMode", "Seismic Display Toggle", 2, {seismicModeToolbarAction_}},
        {"flipHorizontal", "Flip Horizontal", 2, {flipHorizontalAction_}},
        {"idents", "Idents", 2, {identsAction_}},
        {"displayParameters", "Display Parameters", 3, {gainDetailAction_}},
        {"gain", "Gain", 3, {gainSliderAction_, gainUpAction_, gainValueLabelAction_, gainDownAction_}},
        {"agc", "AGC", 3, {agcAction_}},
        {"spectrum", "Spectrum", 4, {spectrumAction_}},
        {"histogram", "Histogram", 4, {histogramAction_}},
        {"octaveBands", "Octave Bands", 4, {octaveBandAction_}},
        {"autocorrelation", "Autocorrelation", 4, {autocorrelationAction_}},
        {"xf", "x-f Display", 4, {xfAction_}},
        {"splitView", "Split View", 4, {splitViewAction_}},
        {"lock", "Lock Panels", 4, {lockAction_}},
        {"lockView", "Lock View", 4, {lockViewAction_}},
        {"sidebar", "Sidebar Toggle", 5, {sidebarToggleAction_}},
    };

    // Right-clicking the toolbar does nothing by itself: which buttons
    // show is set only in Edit > Preferences (the user's call -- a button's
    // right-click menu should be about that button alone). PreventContextMenu,
    // not NoContextMenu, so the event isn't passed on to QMainWindow, whose
    // default menu would offer to hide the whole toolbar. Buttons with a
    // menu of their own keep it: Split View, the gain value label, and AGC.
    toolbar_->setContextMenuPolicy(Qt::PreventContextMenu);
    if (QWidget* agcButton = toolbar_->widgetForAction(agcAction_)) {
        agcButton->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(agcButton, &QWidget::customContextMenuRequested, this, [this, agcButton](const QPoint& pos) {
            QMenu menu(this);
            QAction* params = menu.addAction("Amplitude Processing...");
            connect(params, &QAction::triggered, this, &MainWindow::openAmplitudeProcessingDialog);
            menu.exec(agcButton->mapToGlobal(pos));
        });
    }

    // Every toolbar function is in a menu too (COPILOT.md item 1), so hiding
    // a button never strands its feature -- mirrors, not the toolbar actions
    // themselves; see addMenuMirror's comment.

    // File > Headers.
    addMenuMirror(headersMenu, ebcdicHeaderAction_, "&Text Header");
    addMenuMirror(headersMenu, binaryHeaderAction_, "&Binary Header");
    headersMenu->addSeparator();
    QAction* rangesAction = headersMenu->addAction("Trace Header &Ranges...");
    connect(rangesAction, &QAction::triggered, this, &MainWindow::openTraceHeaderRanges);
    QAction* inspectorAction = headersMenu->addAction("Trace &Inspector...");
    connect(inspectorAction, &QAction::triggered, this, &MainWindow::openTraceInspector);

    // View: zoom, tools, display, amplitude, annotations, appearance.
    QAction* zoomInAction = viewMenu->addAction("Zoom &In");
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction, &QAction::triggered, this, [this]() {
        for (Panel* p : targetPanels()) p->canvas->zoomByFactor(1.0 / 1.5);
    });
    QAction* zoomOutAction = viewMenu->addAction("Zoom &Out");
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction, &QAction::triggered, this, [this]() {
        for (Panel* p : targetPanels()) p->canvas->zoomByFactor(1.5);
    });
    QAction* previousZoomMenuAction = addMenuMirror(viewMenu, moozAction_, "&Previous Zoom");
    previousZoomMenuAction->setShortcut(Qt::Key_Backspace);
    QAction* fullViewMenuAction = addMenuMirror(viewMenu, fullViewAction_, "&Full View");
    fullViewMenuAction->setShortcut(Qt::Key_Home);
    addMenuMirror(viewMenu, zoomAction_, "Zoom to &Box");
    viewMenu->addSeparator();
    addMenuMirror(viewMenu, panAction_, "P&an");
    addMenuMirror(viewMenu, boxSelectAction_, "Bo&x Select");
    viewMenu->addSeparator();
    viewMenu->addAction(variableDensityMenuAction_);
    viewMenu->addAction(wiggleMenuAction_);
    viewMenu->addAction(wiggleInfillAction);
    addMenuMirror(viewMenu, flipHorizontalAction_, "Flip &Horizontal");
    viewMenu->addSeparator();
    QMenu* gainMenu = viewMenu->addMenu("&Gain");
    QAction* gainUpMenuAction = gainMenu->addAction("&Increase (+3 dB)");
    gainUpMenuAction->setShortcut(QKeySequence(Qt::Key_Plus));
    connect(gainUpMenuAction, &QAction::triggered, this,
            [this]() { setGainDb(std::clamp(app_->display.gainDb + 3.0, -6.0, 24.0)); });
    QAction* gainDownMenuAction = gainMenu->addAction("&Decrease (-3 dB)");
    gainDownMenuAction->setShortcut(QKeySequence(Qt::Key_Minus));
    connect(gainDownMenuAction, &QAction::triggered, this,
            [this]() { setGainDb(std::clamp(app_->display.gainDb - 3.0, -6.0, 24.0)); });
    gainMenu->addSeparator();
    QAction* setGainMenuAction = gainMenu->addAction("&Set Gain...");
    connect(setGainMenuAction, &QAction::triggered, this, &MainWindow::promptForGainDb);
    addMenuMirror(viewMenu, agcAction_, "&AGC");
    QAction* amplitudeProcessingMenuAction = viewMenu->addAction("A&mplitude Processing...");
    connect(amplitudeProcessingMenuAction, &QAction::triggered, this, &MainWindow::openAmplitudeProcessingDialog);
    addMenuMirror(viewMenu, gainDetailAction_, "Display &Parameters...");
    viewMenu->addSeparator();
    // Rebuilt on every open, from whichever panel is active right then.
    QMenu* identsMenu = viewMenu->addMenu("I&dents");
    connect(identsMenu, &QMenu::aboutToShow, this, [this, identsMenu]() {
        clearMenuCompletely(identsMenu);
        populateIdentsMenu(*identsMenu);
    });
    viewMenu->addMenu(datasetLabelsMenu);
    viewMenu->addMenu(domainMenu);
    viewMenu->addSeparator();
    QMenu* themeMenu = viewMenu->addMenu("&Theme");
    {
        QActionGroup* themeGroup = new QActionGroup(this);
        QAction* darkTheme = themeMenu->addAction("&Dark");
        QAction* greyTheme = themeMenu->addAction("&Windows Grey");
        for (QAction* a : {darkTheme, greyTheme}) {
            a->setCheckable(true);
            themeGroup->addAction(a);
        }
        (lightTheme() ? greyTheme : darkTheme)->setChecked(true);
        connect(darkTheme, &QAction::triggered, this, [this]() { switchTheme(UiTheme::Dark); });
        connect(greyTheme, &QAction::triggered, this, [this]() { switchTheme(UiTheme::WindowsGrey); });
    }
    addMenuMirror(viewMenu, sidebarToggleAction_, "Side&bar");

    // Window: the two panels.
    addMenuMirror(windowMenu, splitViewAction_, "&Split View");
    QActionGroup* splitOrientationGroup = new QActionGroup(this);
    QAction* sideBySideAction = windowMenu->addAction("Side by Si&de");
    QAction* stackedAction = windowMenu->addAction("S&tacked");
    for (QAction* a : {sideBySideAction, stackedAction}) {
        a->setCheckable(true);
        a->setActionGroup(splitOrientationGroup);
    }
    connect(sideBySideAction, &QAction::triggered, this, [this]() { setSplitOrientation(Qt::Horizontal); });
    connect(stackedAction, &QAction::triggered, this, [this]() { setSplitOrientation(Qt::Vertical); });
    QAction* split50MenuAction = windowMenu->addAction("&Equal Sizes");
    connect(split50MenuAction, &QAction::triggered, this, &MainWindow::resetSplitToHalf);
    connect(windowMenu, &QMenu::aboutToShow, this, [this, sideBySideAction, stackedAction]() {
        sideBySideAction->setChecked(splitter_->orientation() == Qt::Horizontal);
        stackedAction->setChecked(splitter_->orientation() == Qt::Vertical);
    });
    windowMenu->addSeparator();
    addMenuMirror(windowMenu, lockAction_, "&Lock Panels");
    addMenuMirror(windowMenu, lockViewAction_, "Lock &View");
    linkedCrosshairAction_ = windowMenu->addAction("Linked &Crosshair");
    linkedCrosshairAction_->setCheckable(true);
    linkedCrosshairAction_->setChecked(true);
    linkedCrosshairAction_->setToolTip("Show the cursor position of one panel as a crosshair on the other");
    connect(linkedCrosshairAction_, &QAction::toggled, this, [this](bool on) {
        if (!on) {
            for (Panel* p : {&panelA_, &panelB_}) p->canvas->setLinkedCrosshair(false, 0.0, 0.0);
        }
    });
    windowMenu->addSeparator();
    QAction* sendZoomAction = windowMenu->addAction("Send &Zoom to Other Panel");
    connect(sendZoomAction, &QAction::triggered, this, &MainWindow::sendZoomToOtherPanel);
    QAction* sendDisplayAction = windowMenu->addAction("Send &Display to Other Panel");
    connect(sendDisplayAction, &QAction::triggered, this, &MainWindow::sendDisplayToOtherPanel);

    // Analysis: on what is in view (or the active selection box).
    addMenuMirror(analysisMenu, spectrumAction_, "&Spectrum");
    addMenuMirror(analysisMenu, histogramAction_, "&Histogram");
    addMenuMirror(analysisMenu, autocorrelationAction_, "&Autocorrelation");
    addMenuMirror(analysisMenu, octaveBandAction_, "&Octave Bands");
    analysisMenu->addSeparator();
    signalNoiseAction_ = analysisMenu->addAction("Signal/&Noise");
    connect(signalNoiseAction_, &QAction::triggered, this, [this]() {
        if (!activePanel_->app.loaded) {
            flashStatusMessage("No data selected");
            return;
        }
        computeAndShowSignalNoise(activePanel_);
    });
    analysisMenu->addSeparator();
    addMenuMirror(analysisMenu, xfAction_, "&x-f Display");
    fkAction_ = analysisMenu->addAction("&f-k Display");
    connect(fkAction_, &QAction::triggered, this, [this]() {
        if (!activePanel_->app.loaded) {
            flashStatusMessage("No data selected");
            return;
        }
        computeAndShowFk(activePanel_);
    });

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
        label->setStyleSheet(themed("color: #7a7c82; font-size: 11px; letter-spacing: 1px; font-weight: 600;"));
        return label;
    };
    auto valueRow = [&](const QString& caption, QLabel*& valueOut) {
        QWidget* row = new QWidget;
        QHBoxLayout* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        QLabel* captionLabel = new QLabel(caption);
        captionLabel->setStyleSheet(themed("color: #9a9ca3;"));
        valueOut = new QLabel("--");
        valueOut->setStyleSheet(themed("color: #d4d4d8; font-family: monospace;"));
        rowLayout->addWidget(captionLabel);
        rowLayout->addStretch();
        rowLayout->addWidget(valueOut);
        return row;
    };

    dockLayout->addWidget(sectionLabel("TRACE INFO"));
    dockLayout->addWidget(valueRow("Trace", traceValueLabel_));
    dockLayout->addWidget(valueRow("Seq #", seqValueLabel_));
    QWidget* timeRow = valueRow("Time", timeValueLabel_);
    timeCaptionLabel_ = timeRow->findChildren<QLabel*>().value(0); // the caption, created first
    dockLayout->addWidget(timeRow);
    dockLayout->addWidget(valueRow("Amplitude", amplitudeValueLabel_));

    // Amplitude Scale used to be a section here; it's now an overlay
    // legend on the canvas itself (see AmplitudeScaleWidget / setAmplitudeScale).

    dockLayout->addStretch();
    dock_->setWidget(dockContent);
    dock_->setFixedWidth(220);
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
        p->canvas->setPreviousZoomCallback([this]() { moozBack(); });
        p->canvas->setHoverInfoCallback([this](const HoverInfo& info) { updateHoverInfo(info); });
        p->canvas->setDatasetLoadedCallback([this](std::shared_ptr<Dataset> dataset) {
            registerDataset(std::move(dataset));
        });
        p->canvas->setViewChangedCallback([this, p]() {
            syncLockedView(p);
            refreshRecomputeButtons();
        });
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
    applyDatasetLabelPosition(datasetLabelPosition_);
    applyToolbarPosition(prefs_.toolbarPosition); // also applies prefs_.drawerPosition -- see its own comment
}

// $HOME/.segyread_settings, an explicit path rather than QSettings'
// platform-default location (registry on Windows, ~/.config/<org>/<app>.conf
// on Linux) -- the user asked for this specific, visible-on-disk file.
namespace {
QString preferencesFilePath() { return QDir::homePath() + "/.segyread_settings"; }
} // namespace

QAction* MainWindow::addMenuMirror(QMenu* menu, QAction* source, const QString& text, QAction* before) {
    QAction* mirror = new QAction(text.isEmpty() ? source->text() : text, menu);
    mirror->setCheckable(source->isCheckable());
    menu->insertAction(before, mirror); // before == nullptr appends
    auto sync = [mirror, source]() {
        mirror->setEnabled(source->isEnabled());
        if (mirror->isCheckable()) mirror->setChecked(source->isChecked());
    };
    sync();
    connect(source, &QAction::changed, mirror, sync);
    connect(source, &QAction::toggled, mirror, sync);
    // A checkable mirror has already flipped itself by the time triggered
    // fires; re-syncing after the source handles it keeps the two equal
    // even if the source refused the change. trigger() works on a hidden
    // action (the toolbar's More menu already relies on that).
    connect(mirror, &QAction::triggered, mirror, [source, sync]() {
        source->trigger();
        sync();
    });
    return mirror;
}

bool MainWindow::isToolbarActionWanted(QAction* action) const {
    bool vertical = prefs_.toolbarPosition == ToolbarPosition::Left || prefs_.toolbarPosition == ToolbarPosition::Right;
    if (action == gainSliderAction_ && vertical) return false;
    if ((action == gainUpAction_ || action == gainDownAction_) && !vertical) return false;
    for (const ToolbarItem& item : toolbarItems_) {
        if (std::find(item.actions.begin(), item.actions.end(), action) != item.actions.end()) {
            return !prefs_.hiddenToolbarItems.contains(item.id);
        }
    }
    return true;
}

void MainWindow::applyToolbarCustomization() {
    for (const ToolbarItem& item : toolbarItems_) {
        for (QAction* action : item.actions) {
            bool wanted = isToolbarActionWanted(action);
            // Widget too, not just the action -- same lag
            // updateToolbarOverflow's setActionVisible works around.
            action->setVisible(wanted);
            if (QWidget* w = toolbar_->widgetForAction(action)) w->setVisible(wanted);
        }
    }
    // Groups 0..lastSeparatedGroup are the ones separators sit between;
    // anything after (the sidebar toggle) is set apart by the spacer.
    const int lastSeparatedGroup = int(toolbarSeparators_.size());
    auto groupHasShownItem = [this](int group) {
        for (const ToolbarItem& item : toolbarItems_) {
            if (item.group == group && !prefs_.hiddenToolbarItems.contains(item.id)) return true;
        }
        return false;
    };
    for (int g = 0; g < int(toolbarSeparators_.size()); ++g) {
        bool laterShown = false;
        for (int later = g + 1; later <= lastSeparatedGroup && !laterShown; ++later) laterShown = groupHasShownItem(later);
        toolbarSeparators_[g]->setVisible(groupHasShownItem(g) && laterShown);
    }
    updateToolbarOverflow();
}

// The Idents menu (Display at Top/Bottom, Ident Plot, Display on Seismic),
// built fresh from the active panel's app_->idents -- shared by the Idents
// toolbar button's popup and View > Idents (rebuilt on every aboutToShow).
// Its QActionGroups are parented to `menu`, so a persistent menu must
// delete its child groups/submenus before repopulating -- see
// clearMenuCompletely().
void MainWindow::populateIdentsMenu(QMenu& menu) {
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

}

void MainWindow::loadPreferences() {
    QSettings settings(preferencesFilePath(), QSettings::IniFormat);
    prefs_.toolbarPosition =
        ToolbarPosition(settings.value("toolbarPosition", int(ToolbarPosition::Left)).toInt());
    prefs_.drawerPosition = DrawerPosition(settings.value("drawerPosition", int(DrawerPosition::Right)).toInt());
    prefs_.uiScalePercent = settings.value("uiScalePercent", 100.0).toDouble();
    int labelPosition = settings.value("datasetLabelPosition", int(DatasetLabelPosition::TopLeft)).toInt();
    if (labelPosition >= int(DatasetLabelPosition::TopLeft) && labelPosition <= int(DatasetLabelPosition::Off)) {
        datasetLabelPosition_ = DatasetLabelPosition(labelPosition);
    }
    prefs_.hiddenToolbarItems = settings.value("toolbarHiddenItems").toStringList();
    prefs_.hiddenToolbarItems.removeAll(QString()); // an empty list can read back as {""}
    segy::SigcorParams& sn = prefs_.signalNoise;
    const segy::SigcorParams defaults;
    // sigcor/*: the SPECTR/STNDRD reference conventions. The older
    // signalNoise/* keys (NSKIP as pair distance, one-sided IRANSK, cosine
    // taper) are not carried over; savePreferences removes them.
    sn.option = settings.value("sigcor/option", "SPECTR").toString() == "AUTCOR" ? segy::SigcorOption::Autcor
                                                                                  : segy::SigcorOption::Spectr;
    sn.nskip = std::max(0, settings.value("sigcor/nskip", defaults.nskip).toInt());
    sn.iransk = std::clamp(settings.value("sigcor/iransk", defaults.iransk).toInt(), 0, sn.nskip);
    sn.shmaxMs = settings.value("sigcor/shmaxMs", defaults.shmaxMs).toDouble();
    sn.ccmin = settings.value("sigcor/ccmin", defaults.ccmin).toDouble();
    sn.kaiserBeta = settings.value("sigcor/kaiserBeta", defaults.kaiserBeta).toDouble();
    sn.normalise = settings.value("sigcor/normalise", defaults.normalise).toBool();
    sn.removeDc = settings.value("sigcor/removeDc", defaults.removeDc).toBool();
}

void MainWindow::savePreferences() {
    QSettings settings(preferencesFilePath(), QSettings::IniFormat);
    settings.setValue("toolbarPosition", int(prefs_.toolbarPosition));
    settings.setValue("drawerPosition", int(prefs_.drawerPosition));
    settings.setValue("uiScalePercent", prefs_.uiScalePercent);
    settings.setValue("datasetLabelPosition", int(datasetLabelPosition_));
    settings.setValue("toolbarHiddenItems", prefs_.hiddenToolbarItems);
    const segy::SigcorParams& sn = prefs_.signalNoise;
    settings.remove("signalNoise"); // the pre-reference keys, see loadPreferences
    settings.setValue("sigcor/option", sn.option == segy::SigcorOption::Autcor ? "AUTCOR" : "SPECTR");
    settings.setValue("sigcor/nskip", sn.nskip);
    settings.setValue("sigcor/iransk", sn.iransk);
    settings.setValue("sigcor/shmaxMs", sn.shmaxMs);
    settings.setValue("sigcor/ccmin", sn.ccmin);
    settings.setValue("sigcor/kaiserBeta", sn.kaiserBeta);
    settings.setValue("sigcor/normalise", sn.normalise);
    settings.setValue("sigcor/removeDc", sn.removeDc);
}

void MainWindow::applyDatasetLabelPosition(DatasetLabelPosition position) {
    datasetLabelPosition_ = position;
    panelA_.canvas->setDatasetLabelPosition(position);
    panelB_.canvas->setDatasetLabelPosition(position);
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
    // The Gain controls are swapped for whichever pair fits this position
    // (slider horizontal, +/- arrows vertical) by applyToolbarCustomization()
    // at the end of this function, via isToolbarActionWanted() -- driven
    // directly by the ToolbarPosition enum (prefs_.toolbarPosition, which
    // every caller passes as `position`) rather than QToolBar::orientation()/
    // orientationChanged, which don't reliably flip to Qt::Vertical purely
    // from this addToolBar(area, ...) call (observed: both pairs staying
    // visible at once on Windows). It toggles the *action*'s visibility,
    // not just the embedded widget's -- QToolBar re-shows a widget added via
    // addWidget() on its next layout pass as long as the QWidgetAction
    // wrapping it is still visible.
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
    applyToolbarCustomization(); // also re-runs updateToolbarOverflow()
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
    for (QAction* action : toolbarOverflowCandidates_) setActionVisible(action, isToolbarActionWanted(action));
    if (toolbarSpacerAction_) setActionVisible(toolbarSpacerAction_, false);
    if (toolbar_->layout()) toolbar_->layout()->activate();

    int limit = vertical ? toolbar_->height() : toolbar_->width();
    // zoomAction_ is always the very first candidate and never hidden, so
    // its widget's geometry is always real/laid-out -- used as a
    // same-style reference size/offset for the manually-positioned More
    // button below, rather than guessing fixed pixel values.
    // (Or the first *shown* candidate, now that Zoom itself can be
    // removed from the toolbar.)
    QWidget* refWidget = nullptr;
    for (QAction* action : toolbarOverflowCandidates_) {
        if (!isToolbarActionWanted(action)) continue;
        refWidget = toolbar_->widgetForAction(action);
        if (refWidget) break;
    }
    QRect refGeom = refWidget ? refWidget->geometry() : QRect(4, 4, 45, 37);
    int moreLength = vertical ? refGeom.height() : refGeom.width();
    int budget = std::max(0, limit - moreLength);

    bool anyHidden = false;
    int hiddenCount = 0;
    for (QAction* action : toolbarOverflowCandidates_) {
        if (!isToolbarActionWanted(action)) continue; // already hidden above, not overflow
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

void MainWindow::setSplitOrientation(Qt::Orientation orientation) {
    splitter_->setOrientation(orientation);
    resetSplitToHalf();
    // Depict the actual divider orientation -- a vertical (top/bottom)
    // split shows a horizontal-divider icon, not the vertical-divider
    // default.
    splitViewAction_->setIcon(orientation == Qt::Horizontal ? iconLayoutColumns() : iconLayoutRows());
}

void MainWindow::promptForGainDb() {
    bool ok = false;
    double value = QInputDialog::getDouble(this, "Set Gain", "Gain (dB):", app_->display.gainDb, -6.0, 24.0, 1, &ok);
    if (ok) setGainDb(value);
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
    statusBar()->setStyleSheet(themed("QStatusBar { color: #e8905a; }"));
    statusBar()->showMessage(text, 2000);
    QTimer::singleShot(2000, this, [this]() {
        statusBar()->setStyleSheet(themed(QString()));
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
    if (!other->app.loaded || other->app.viewLocked) return; // nothing sensible (or allowed) to sync into
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
    pickAction_->setChecked(app_->tool == ToolMode::Pick);
    gainSlider_->blockSignals(true);
    gainSlider_->setValue(int(std::lround(app_->display.gainDb)));
    gainSlider_->blockSignals(false);
    gainValueLabel_->setText(formatGainDb(app_->display.gainDb));
    bool isWiggle = app_->display.seismicMode == segy::SeismicDisplayMode::Wiggle;
    variableDensityMenuAction_->setChecked(!isWiggle);
    wiggleMenuAction_->setChecked(isWiggle);
    refreshSeismicModeToolbarIcon();
    refreshAmplitudeProcessingControls();
    xfAction_->setChecked(app_->xf.active);
    if (flattenAction_) flattenAction_->setChecked(app_->flatten.active);
    if (domainDepthAction_) {
        (app_->foreground->depth ? domainDepthAction_ : domainTimeAction_)->setChecked(true);
    }
    lockViewAction_->setChecked(app_->viewLocked);
    // Analyses that assume a time series mean nothing on an x-f display
    // (its samples are frequency bins in dB). Histogram stays: a
    // distribution of dB values is still meaningful.
    const bool timeSeries = !app_->xf.active;
    for (QAction* action : {spectrumAction_, octaveBandAction_, autocorrelationAction_, signalNoiseAction_, fkAction_}) {
        action->setEnabled(timeSeries);
    }
    refreshToolChrome();
}

void MainWindow::setAmplitudeProcessing(const segy::AmplitudeProcessing& processing) {
    for (Panel* p : targetPanels()) p->app.display.processing = processing;
    refreshAmplitudeProcessingControls();
    refreshAmplitudeScale();
    for (Panel* p : {&panelA_, &panelB_}) p->canvas->update();
}

void MainWindow::refreshAmplitudeProcessingControls() {
    agcAction_->setChecked(app_->display.processing.agcEnabled);
    if (amplitudeProcessingDialog_ && amplitudeProcessingDialog_->isVisible()) {
        double recordLengthMs = 0.0;
        if (app_->loaded) {
            recordLengthMs = double(app_->foreground->binHeader.samplesPerTrace) *
                             app_->foreground->binHeader.sampleIntervalUs / 1000.0;
        }
        amplitudeProcessingDialog_->setProcessing(app_->display.processing, recordLengthMs);
        amplitudeProcessingDialog_->setWindowTitle(QString("Amplitude Processing") + datasetTitleSuffix(*app_));
    }
}

void MainWindow::openAmplitudeProcessingDialog() {
    if (!amplitudeProcessingDialog_) {
        amplitudeProcessingDialog_ = new AmplitudeProcessingDialog(this);
        // Only this dialog's own edits come through here; its controls are
        // re-filled by refreshAmplitudeProcessingControls() with the
        // callback suppressed, so there's no feedback loop.
        amplitudeProcessingDialog_->setChangedCallback(
            [this](const segy::AmplitudeProcessing& processing) { setAmplitudeProcessing(processing); });
    }
    amplitudeProcessingDialog_->show();
    refreshAmplitudeProcessingControls(); // after show(): it only refreshes a visible dialog
    amplitudeProcessingDialog_->raise();
    amplitudeProcessingDialog_->activateWindow();
}

void MainWindow::zoomToBox() {
    if (app_->loaded && app_->viewLocked) {
        flashStatusMessage("View is locked (Lock View)");
        return;
    }
    // No box yet: switch to Box Select and zoom as soon as one is drawn.
    const SelectionBox* activeNow = activeBox(*app_);
    if (app_->loaded && (!activeNow || !activeNow->complete)) {
        pendingBoxZoom_ = true;
        if (!boxSelectAction_->isChecked()) boxSelectAction_->trigger();
        flashStatusMessage("Draw a box to zoom");
        return;
    }
    // Locked panels: zoom each target panel to its *own* active, completed
    // box (if it has one) -- not a shared box, since the two panels can hold
    // different datasets with entirely different trace/sample ranges.
    for (Panel* p : targetPanels()) {
        if (p->app.viewLocked) continue;
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

void MainWindow::sendZoomToOtherPanel() {
    Panel* other = activePanel_ == &panelA_ ? &panelB_ : &panelA_;
    if (!app_->loaded || !other->app.loaded) {
        flashStatusMessage("Both panels need data");
        return;
    }
    if (other->app.viewLocked) {
        flashStatusMessage("The other panel's view is locked (Lock View)");
        return;
    }
    if (app_->xf.active != other->app.xf.active) {
        flashStatusMessage("Not between an x-f display and a seismic display");
        return;
    }
    // Same trace index range and the same times (ms), so panels with
    // different sample intervals still line up.
    other->app.zoomHistory.push_back(other->app.view);
    const double fromMs = app_->foreground->binHeader.sampleIntervalUs / 1000.0;
    const double toMs = other->app.foreground->binHeader.sampleIntervalUs / 1000.0;
    segy::ViewRange next = app_->view;
    if (fromMs > 0.0 && toMs > 0.0) {
        next.sampleStart = app_->view.sampleStart * fromMs / toMs;
        next.sampleEnd = app_->view.sampleEnd * fromMs / toMs;
    }
    segy::clampView(next, other->app.foreground->traceCount, other->app.foreground->binHeader.samplesPerTrace);
    other->app.view = next;
    flashStatusMessage("Zoom sent to the other panel");
    refreshToolChrome();
}

void MainWindow::sendDisplayToOtherPanel() {
    Panel* other = activePanel_ == &panelA_ ? &panelB_ : &panelA_;
    if (!app_->loaded || !other->app.loaded) {
        flashStatusMessage("Both panels need data");
        return;
    }
    if (app_->xf.active || other->app.xf.active) {
        flashStatusMessage("Not available with an x-f display");
        return;
    }
    // Everything: mode, colour scale (and flip), gain, AGC/computed gain,
    // manual clip, polarity, grid/scale toggles, flip horizontal.
    other->app.display = app_->display;
    flashStatusMessage("Display settings sent to the other panel");
    refreshAfterDisplaySwap();
}

void MainWindow::moozBack() {
    if (app_->viewLocked) {
        flashStatusMessage("View is locked (Lock View)");
        return;
    }
    for (Panel* p : targetPanels()) {
        if (p->app.viewLocked || p->app.zoomHistory.empty()) continue;
        p->app.view = p->app.zoomHistory.back();
        p->app.zoomHistory.pop_back();
    }
    refreshToolChrome();
}

void MainWindow::refreshToolChrome() {
    // Zoom is always available with data: without a box it starts one.
    zoomAction_->setEnabled(app_->loaded && !app_->viewLocked);
    moozAction_->setEnabled(!app_->zoomHistory.empty() && !app_->viewLocked);
    for (Panel* p : {&panelA_, &panelB_}) p->canvas->update();
    canvas_->refreshStatusBar();
    refreshAmplitudeScale();
    refreshWindowTitle();
    refreshRecomputeButtons();
}

void SegyCanvas::unsetCursorForTool() {
    switch (app_.tool) {
    case ToolMode::Pan: setCursor(Qt::OpenHandCursor); break;
    case ToolMode::BoxSelect: setCursor(makeBoxSelectCursor()); break;
    case ToolMode::Pick: setCursor(Qt::CrossCursor); break;
    default: setCursor(Qt::ArrowCursor); break;
    }
}

void MainWindow::showAbout() {
    QDialog dialog(this);
    dialog.setWindowTitle("About TraceLab");
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(28, 24, 28, 20);
    layout->setSpacing(12);
    QHBoxLayout* head = new QHBoxLayout();
    QLabel* logo = new QLabel();
    logo->setPixmap(QIcon(QStringLiteral(":/app_icon.png")).pixmap(56, 56));
    head->addWidget(logo);
    QLabel* title = new QLabel(QString("<div style='font-size:20px; font-weight:600;'>TraceLab</div>"
                                       "<div style='color:%1;'>2D SEG-Y viewer and QC</div>")
                                   .arg(QColor(kSecondaryTextColor).name()));
    head->addWidget(title, 1);
    layout->addLayout(head);
    const QString text = QString(
        "<p>Built %1 with Qt %2 (running %3), %4.</p>"
        "<p style='margin-top:10px;'><b>Libraries</b></p>"
        "<table cellspacing='0' cellpadding='2'>"
        "<tr><td>Qt %3</td><td>&nbsp;&nbsp;LGPL v3</td><td>&nbsp;&nbsp;user interface, SVG icons</td></tr>"
        "<tr><td>Tabler Icons</td><td>&nbsp;&nbsp;MIT</td><td>&nbsp;&nbsp;toolbar icons</td></tr>"
        "<tr><td>C++17 standard library</td><td></td><td>&nbsp;&nbsp;everything else</td></tr>"
        "</table>"
        "<p style='margin-top:10px;'>The SEG-Y reading, IBM-float decoding (scalar and AVX2), pyramid, renderer, "
        "FFT, Butterworth filters, spectra, f-k, SIGCOR signal/noise, horizon tracking and time-depth "
        "conversion are written for this application, with no third-party code.</p>"
        "<p style='margin-top:10px;'>Header layouts: SEG-Y rev 1.0 and rev 2.0 (SEG), Shell SEGY trace "
        "identing V2.1.</p>")
        .arg(QString::fromLatin1(__DATE__), QString::fromLatin1(QT_VERSION_STR), QString::fromLatin1(qVersion()),
             QSysInfo::buildCpuArchitecture());
    QLabel* body = new QLabel(text);
    body->setWordWrap(true);
    body->setTextFormat(Qt::RichText);
    layout->addWidget(body);
    QHBoxLayout* buttons = new QHBoxLayout();
    QPushButton* bug = new QPushButton("Report a Bug...");
    QPushButton* close = new QPushButton("Close");
    close->setDefault(true);
    buttons->addWidget(bug);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(bug, &QPushButton::clicked, &dialog, [this, &dialog]() {
        dialog.accept();
        reportBug();
    });
    dialog.resize(520, dialog.sizeHint().height());
    dialog.exec();
}

void MainWindow::reportBug() {
    // The developer's address, by the user's own request: reports go to
    // them. Opens the default mail client with what helps reproduce it.
    static const QString kDeveloper = QStringLiteral("jeroengoudswaard@gmail.com");
    QDialog dialog(this);
    dialog.setWindowTitle("Report a Bug");
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 16);
    QLabel* intro = new QLabel("What happened, and what did you expect? Your mail program opens with this text "
                               "and the details below, ready to send.");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    QPlainTextEdit* description = new QPlainTextEdit();
    description->setPlaceholderText("Steps: 1. Open ...  2. ...  What went wrong: ...");
    description->setMinimumHeight(140);
    layout->addWidget(description);
    QString details = QString("TraceLab, built %1\nQt %2 on %3 (%4)\nTheme: %5\n")
                          .arg(QString::fromLatin1(__DATE__), QString::fromLatin1(qVersion()),
                               QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
                               lightTheme() ? "Windows Grey" : "Dark");
    if (app_->loaded) {
        details += QString("Dataset: %1 (%2 traces x %3 samples, %4 us, format %5)\n")
                       .arg(QString::fromStdString(app_->foreground->name))
                       .arg(app_->foreground->traceCount)
                       .arg(app_->foreground->binHeader.samplesPerTrace)
                       .arg(app_->foreground->binHeader.sampleIntervalUs)
                       .arg(app_->foreground->binHeader.formatCode);
    }
    QLabel* detailsLabel = new QLabel(details.trimmed());
    detailsLabel->setStyleSheet(themed("color: #9a9ca3;"));
    layout->addWidget(detailsLabel);
    QHBoxLayout* buttons = new QHBoxLayout();
    QLabel* address = new QLabel(QString("To: %1").arg(kDeveloper));
    address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    address->setStyleSheet(themed("color: #9a9ca3;"));
    buttons->addWidget(address);
    buttons->addStretch();
    QPushButton* send = new QPushButton("Open in Mail");
    QPushButton* cancel = new QPushButton("Cancel");
    send->setDefault(true);
    buttons->addWidget(send);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(send, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.resize(560, 360);
    if (dialog.exec() != QDialog::Accepted) return;
    QUrlQuery query;
    query.addQueryItem("subject", "TraceLab bug report");
    query.addQueryItem("body", description->toPlainText().trimmed() + "\n\n--\n" + details);
    QUrl url(QStringLiteral("mailto:") + kDeveloper);
    url.setQuery(query);
    if (!QDesktopServices::openUrl(url)) {
        QMessageBox::information(this, "Report a Bug",
                                 QString("No mail program is set up. Please send the report to %1.").arg(kDeveloper));
    }
}

void MainWindow::showMouseAndKeyboardHelp() {
    QDialog dialog(this);
    dialog.setWindowTitle("Mouse and Keyboard");
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 16);
    const QString muted = QColor(kSecondaryTextColor).name();
    auto row = [&muted](const QString& key, const QString& what) {
        return QString("<tr><td style='padding:3px 18px 3px 0; white-space:nowrap;'><b>%1</b></td>"
                       "<td style='padding:3px 0; color:%3;'>%2</td></tr>").arg(key, what, muted);
    };
    QString html = "<p><b>Zoom works the same on the seismic and on every chart.</b></p><table>";
    html += row("Wheel", "zoom in or out around the cursor");
    html += row("Drag", "zoom to the rectangle (the pointer tool; on charts always)");
    html += row("Double-click", "back to the previous zoom");
    html += row("Right-click", "Previous Zoom, Full View, Axis Ranges...");
    html += row("Middle-drag", "pan (in any tool but Box Select)");
    html += "</table><p style='margin-top:12px;'><b>Tools</b> (toolbar; click again for the pointer)</p><table>";
    html += row("Pan", "drag to move");
    html += row("Box Select", "a box for the analyses; drag a corner to change it, middle-click to delete");
    html += row("Pick Horizon", "click points, the traces between are tracked; right-click removes a point");
    html += "</table><p style='margin-top:12px;'><b>Keys</b></p><table>";
    html += row("Backspace", "previous zoom");
    html += row("Home or R", "full view");
    html += row("Ctrl + / Ctrl -", "zoom in / out");
    html += row("+ / -", "gain up / down 3 dB");
    html += row("Ctrl O, Ctrl S", "open, save as SEG-Y");
    html += row("Ctrl ,", "preferences");
    html += row("F1", "this window");
    html += "</table>";
    QLabel* body = new QLabel(html);
    body->setTextFormat(Qt::RichText);
    layout->addWidget(body);
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();
    QPushButton* close = new QPushButton("Close");
    close->setDefault(true);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

void MainWindow::switchTheme(UiTheme theme) {
    if (theme == uiTheme()) return;
    QSettings settings(QDir::homePath() + "/.segyread_settings", QSettings::IniFormat);
    settings.setValue("uiTheme", theme == UiTheme::WindowsGrey ? "windowsGrey" : "dark");
    settings.sync();
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, "Theme", "TraceLab restarts to change the theme. Computed datasets and horizons that you have not "
                       "saved are lost.\n\nRestart now?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        flashStatusMessage("The new theme applies the next time TraceLab starts");
        return;
    }
    QStringList arguments;
    if (app_->loaded && !app_->foreground->filePath.empty() && app_->foreground->attribute.empty()) {
        arguments << QString::fromStdWString(app_->foreground->filePath.wstring());
    }
    QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments);
    qApp->quit();
}

void MainWindow::refreshWindowTitle() { setWindowTitle(QString("TraceLab") + datasetTitleSuffix(*app_)); }

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
        // The renderer's own clip math (segy::displayClipPosNeg), so the
        // legend never shows a range the seismic itself isn't using --
        // manual clip (ClipDialog), gain, and AGC's normalized units all
        // included. Guarded on `loaded`: foreground is null before a load.
        float posClip = 1.0f, negClip = 1.0f;
        if (p->app.loaded) {
            float rawClip = std::max(std::fabs(p->app.foreground->pyramid.globalMin),
                                     std::fabs(p->app.foreground->pyramid.globalMax));
            segy::displayClipPosNeg(rawClip, p->app.display.gainDb, p->app.display.manualClip,
                                    &p->app.display.processing, &posClip, &negClip);
        }
        p->canvas->setAmplitudeScale(posClip, negClip, visible, p->app.display.colorScale,
                                     p->app.display.colorScaleFlipped);
        if (p == octaveSourcePanel_ && octaveBandDialog_ && p->app.loaded) {
            // Octave panels follow the main display's clip live (without
            // AGC/computed gain, which the bands don't get).
            float bandPos = 1.0f, bandNeg = 1.0f;
            const float rawClip = std::max(std::fabs(p->app.foreground->pyramid.globalMin),
                                           std::fabs(p->app.foreground->pyramid.globalMax));
            segy::displayClipPosNeg(rawClip, p->app.display.gainDb, p->app.display.manualClip, nullptr, &bandPos, &bandNeg);
            octaveBandDialog_->setMainClip(bandPos, bandNeg);
        }
    }
}

void MainWindow::updateHoverInfo(const HoverInfo& info) {
    // Linked crosshair: show this cursor position on the other panel.
    if (linkedCrosshairAction_ && linkedCrosshairAction_->isChecked()) {
        for (Panel* p : {&panelA_, &panelB_}) {
            const bool isSource = p->canvas == info.sourceCanvas;
            if (isSource) {
                p->canvas->setLinkedCrosshair(false, 0.0, 0.0);
            } else {
                p->canvas->setLinkedCrosshair(info.valid && !info.frequencyAxis && p->canvas->isVisible(),
                                              info.traceCoord, info.timeMs);
            }
        }
    }
    // Trace Inspector, Follow cursor: the trace under the mouse (not on an
    // x-f display, whose traces are spectra).
    if (info.valid && !info.frequencyAxis && traceInspector_ && traceInspector_->isVisible() &&
        traceInspector_->followCursor()) {
        for (Panel* p : {&panelA_, &panelB_}) {
            if (p->app.loaded && p->app.foreground.get() == info.dataset) {
                if (traceInspector_->dataset() != info.dataset) {
                    traceInspector_->setDataset(p->app.foreground, datasetTitleSuffix(p->app));
                }
                traceInspector_->showTrace(info.traceIdx);
                break;
            }
        }
    }
    if (!info.valid) {
        traceValueLabel_->setText("--");
        seqValueLabel_->setText("--");
        timeValueLabel_->setText("--");
        amplitudeValueLabel_->setText("--");
        return;
    }
    traceValueLabel_->setText(QString::number(info.traceIdx + 1)); // 1-based, like the status line and CSV
    seqValueLabel_->setText(QString::number(info.seqNum));
    if (timeCaptionLabel_) timeCaptionLabel_->setText(info.frequencyAxis ? "Frequency" : info.depthAxis ? "Depth" : "Time");
    if (info.frequencyAxis) {
        timeValueLabel_->setText(QString("%1 Hz").arg(info.timeMs, 0, 'f', 2));
        amplitudeValueLabel_->setText(QString("%1 dB").arg(double(info.amplitude), 0, 'f', 1));
        return;
    }
    timeValueLabel_->setText(QString(info.depthAxis ? "%1 m" : "%1 ms").arg(info.timeMs, 0, 'f', 1));
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
    if (panel->app.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
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

void MainWindow::runCalculator(std::shared_ptr<Dataset> dsA, std::shared_ptr<Dataset> dsB, bool subtract,
                               const QString& outputName) {
    if (!dsA || !dsB || dsA->traceCount <= 0 || dsB->traceCount <= 0) {
        flashStatusMessage("No data selected");
        return;
    }
    // Clipped to the overlap when the two datasets differ in size, rather
    // than refusing -- combining a full line with a sub-window of it is a
    // reasonable thing to want.
    int64_t traceCount = std::min(dsA->traceCount, dsB->traceCount);
    int samplesPerTrace = std::min<int>(dsA->binHeader.samplesPerTrace, dsB->binHeader.samplesPerTrace);
    if (traceCount <= 0 || samplesPerTrace <= 0) {
        flashStatusMessage("No data selected");
        return;
    }

    // The background closure below reads through these shared_ptrs, so
    // it stays correct (and the underlying mmap'd files/pyramids stay
    // alive) even if a panel switches Foreground or one of these is
    // deleted from the pool while the computation is still running.

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
            *writeOk = streamTraces(
                outPath, textHeader.toStdString(), outHeader, traceCount,
                [&](int64_t t, segy::WriteTrace& out) {
                    thread_local std::vector<float> samplesB;
                    samplesB.resize(size_t(samplesPerTrace));
                    const uint8_t* traceA = dsA->file.data() + segy::kHeaderTotalSize + size_t(t) * dsA->traceStrideBytes;
                    const uint8_t* traceB = dsB->file.data() + segy::kHeaderTotalSize + size_t(t) * dsB->traceStrideBytes;
                    out.headerBytes.assign(traceA, traceA + segy::kTraceHeaderSize); // dataset A's own trace headers carry over
                    out.samples.resize(size_t(samplesPerTrace));
                    segy::decodeSamples(traceA + segy::kTraceHeaderSize, out.samples.data(), size_t(samplesPerTrace),
                                        dsA->binHeader.formatCode);
                    segy::decodeSamples(traceB + segy::kTraceHeaderSize, samplesB.data(), size_t(samplesPerTrace),
                                        dsB->binHeader.formatCode);
                    for (int s = 0; s < samplesPerTrace; ++s) {
                        out.samples[size_t(s)] = subtract ? out.samples[size_t(s)] - samplesB[size_t(s)]
                                                          : out.samples[size_t(s)] + samplesB[size_t(s)];
                    }
                },
                backgroundTaskDone_, writeError.get());
        },
        [this, writeOk, writeError, outPath, history]() {
            if (!*writeOk) {
                QMessageBox::warning(this, "Calculator", QString::fromStdString(*writeError));
                return;
            }
            panelA_.canvas->startLoading(outPath, history);
        });
}

void MainWindow::runBandpass(AppState& source, const segy::ButterworthBandParams& params, const QString& outputName) {
    if (!source.loaded || source.foreground->traceCount <= 0 || source.foreground->binHeader.samplesPerTrace <= 0) {
        flashStatusMessage("No data selected");
        return;
    }
    if (source.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
    // The result goes into the panel it came from, so filters cascade there.
    Panel* targetPanel = &source == &panelB_.app ? &panelB_ : &panelA_;
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
    // Designed up front (cheap), so an impossible request -- both corners
    // open, low corner above Nyquist, low >= high -- is refused with a
    // message instead of silently writing an unfiltered copy.
    const double sampleRateHz = ds->binHeader.sampleIntervalUs > 0 ? 1'000'000.0 / ds->binHeader.sampleIntervalUs : 0.0;
    std::string designError;
    const std::vector<segy::Biquad> sos = segy::designButterworthBand(params, sampleRateHz, &designError);
    if (sos.empty()) {
        QMessageBox::warning(this, "Bandpass Filter", QString::fromStdString(designError));
        return;
    }
    const double nyquistHz = sampleRateHz / 2.0;
    const bool hasLow = params.lowHz > 0.0;
    const bool hasHigh = params.highHz < nyquistHz * 0.999;
    QString filterText = hasLow && hasHigh ? QString("BANDPASS %1-%2 HZ").arg(params.lowHz, 0, 'g', 4).arg(params.highHz, 0, 'g', 4)
                         : hasLow          ? QString("HIGHPASS %1 HZ").arg(params.lowHz, 0, 'g', 4)
                                           : QString("LOWPASS %1 HZ").arg(params.highHz, 0, 'g', 4);
    std::vector<std::string> history = ds->processingHistory;
    const char* paddingText = params.padding == segy::EdgePadding::Even  ? "EVEN"
                              : params.padding == segy::EdgePadding::Odd ? "ODD"
                                                                          : "CONSTANT";
    history.push_back(QString("BUTTERWORTH %1, ORDER %2, ZERO-PHASE SOSFILTFILT, %3 PADDING")
                          .arg(filterText)
                          .arg(params.order)
                          .arg(paddingText)
                          .toStdString());
    const segy::EdgePadding padding = params.padding;

    QString textHeader = buildSegyTextHeader(inputBlocks, history, outputName, traceCount, samplesPerTrace,
                                              ds->binHeader.sampleIntervalUs);
    std::filesystem::path outPath = scratchSegyPath(outputName);
    segy::BinaryHeader outHeader = ds->binHeader;

    auto writeOk = std::make_shared<bool>(false);
    auto writeError = std::make_shared<std::string>();

    runBackgroundTask(
        QString("Bandpass: %1").arg(sourceName),
        [this, ds, traceCount, samplesPerTrace, sos, padding, outHeader, textHeader, outPath, writeOk, writeError]() {
            backgroundTaskTotal_ = traceCount;
            *writeOk = streamTraces(
                outPath, textHeader.toStdString(), outHeader, traceCount,
                [&](int64_t t, segy::WriteTrace& out) {
                    const uint8_t* traceBase = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                    out.headerBytes.assign(traceBase, traceBase + segy::kTraceHeaderSize);
                    out.samples.resize(size_t(samplesPerTrace));
                    segy::decodeSamples(traceBase + segy::kTraceHeaderSize, out.samples.data(), size_t(samplesPerTrace),
                                        ds->binHeader.formatCode);
                    segy::sosFiltFilt(sos, out.samples, -1, padding);
                },
                backgroundTaskDone_, writeError.get());
        },
        [this, writeOk, writeError, outPath, history, targetPanel]() {
            if (!*writeOk) {
                QMessageBox::warning(this, "Bandpass Filter", QString::fromStdString(*writeError));
                return;
            }
            targetPanel->canvas->startLoading(outPath, history);
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
            *writeOk = streamTraces(
                outPath, textHeader.toStdString(), outHeader, traceCount,
                [&](int64_t t, segy::WriteTrace& out) {
                    const uint8_t* traceBase = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                    out.headerBytes.assign(traceBase, traceBase + segy::kTraceHeaderSize);
                    out.samples.resize(size_t(samplesPerTrace));
                    segy::decodeSamples(traceBase + segy::kTraceHeaderSize, out.samples.data(), size_t(samplesPerTrace),
                                        ds->binHeader.formatCode);
                },
                backgroundTaskDone_, writeError.get());
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
    if (sourcePanel->app.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
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
    bool colorScaleFlipped = app.display.colorScaleFlipped;

    // Sample enough evenly-spaced traces to fill a panel at native raster
    // resolution, without filtering every trace in a large view. Each band
    // panel rasterizes these itself at its exact on-screen size (see
    // rasterizeOctaveBand).
    constexpr int kMaxTracesPerBand = 160;
    int decodeCount = int(std::min<int64_t>(visibleTraces, kMaxTracesPerBand));

    auto result = std::make_shared<OctaveBandResult>();
    result->traceStart = double(t0);
    result->traceEnd = double(t1 - 1);
    result->sampleStart = s0;
    result->sampleEnd = s1 - 1;
    result->sampleIntervalUs = app.foreground->binHeader.sampleIntervalUs;
    {
        // The main display's clip, exactly as the seismic renderer gets it.
        // Computed gain/AGC is not applied to the bands, so it is left out.
        const float rawClip = std::max(std::fabs(app.foreground->pyramid.globalMin), std::fabs(app.foreground->pyramid.globalMax));
        segy::displayClipPosNeg(rawClip, app.display.gainDb, app.display.manualClip, nullptr, &result->mainClipPos,
                                &result->mainClipNeg);
    }
    const bool reversePolarity = app.display.reversePolarity;
    octaveSourcePanel_ = sourcePanel;

    runBackgroundTask(
        QString("Octave Bands: %1-%2 Hz").arg(minFreqHz).arg(maxFreqHz),
        [this, ds, t0, t1, s0, visibleSamples, decodeCount, minFreqHz, maxFreqHz, colorScale, colorScaleFlipped, reversePolarity,
         result]() {
            // Bands as scipy users would build them: Butterworth, order
            // kOctaveButterworthOrder, applied zero-phase with sosfiltfilt
            // (odd-reflection padding + steady-state initial conditions, so
            // no edge transients) -- see viewer/iirfilter.h. A band reaching
            // Nyquist becomes a highpass (a bandpass edge at/above Nyquist
            // is undefined; scipy would refuse it); bands entirely above
            // Nyquist are dropped rather than shown blank.
            constexpr int kOctaveButterworthOrder = 4;
            const double sampleRateHz = 1'000'000.0 / ds->binHeader.sampleIntervalUs;
            const double nyquistHz = sampleRateHz / 2.0;
            struct BandSpec {
                int lo, hi;
                bool highpass;
            };
            std::vector<BandSpec> specs;
            for (int lo = minFreqHz; lo < maxFreqHz; lo *= 2) {
                if (lo >= nyquistHz * 0.999) break;
                int hi = std::min(lo * 2, maxFreqHz);
                bool highpass = hi >= nyquistHz * 0.999;
                specs.push_back({lo, hi, highpass});
                if (highpass) break;
            }
            backgroundTaskTotal_ = int64_t(specs.size()) + 1;

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

            result->bands.push_back({"Unfiltered", makeOctaveBandData(rawTraces, colorScale, colorScaleFlipped, reversePolarity)});
            backgroundTaskDone_ = 1;
            int bandsDone = 1;
            for (const BandSpec& spec : specs) {
                const std::vector<segy::Biquad> sos =
                    spec.highpass ? segy::butterworthHighpass(kOctaveButterworthOrder, spec.lo, sampleRateHz)
                                  : segy::butterworthBandpass(kOctaveButterworthOrder, spec.lo, spec.hi, sampleRateHz);
                std::vector<std::vector<float>> filtered = rawTraces;
                // Even (mirror) padding, the app-wide default: see iirfilter.h.
                for (std::vector<float>& tr : filtered) segy::sosFiltFilt(sos, tr, -1, segy::EdgePadding::Even);
                QString label = spec.highpass ? QString("%1 Hz - Nyquist").arg(spec.lo)
                                              : QString("%1 - %2 Hz").arg(spec.lo).arg(spec.hi);
                result->bands.push_back({label, makeOctaveBandData(std::move(filtered), colorScale, colorScaleFlipped, reversePolarity)});
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
        calculatorDialog_->setApplyCallback([this](int idxA, int idxB, int op, QString outputName) {
            if (idxA < 0 || idxB < 0 || idxA >= int(datasetPool_.size()) || idxB >= int(datasetPool_.size())) return;
            std::shared_ptr<Dataset> a = datasetPool_[size_t(idxA)];
            std::shared_ptr<Dataset> b = datasetPool_[size_t(idxB)];
            if (op == 2) {
                double minMs = 200.0, maxMs = 200.0;
                if (crossCorrelationDialog_) {
                    minMs = crossCorrelationDialog_->minLagMs();
                    maxMs = crossCorrelationDialog_->maxLagMs();
                }
                computeAndShowCrossCorrelation(a, b, minMs, maxMs);
            } else {
                runCalculator(a, b, op == 1, outputName);
            }
        });
    }
    // The whole dataset list (not just what the panels show), defaulting to
    // the two panels' Foregrounds.
    QStringList names;
    int defaultA = -1, defaultB = -1;
    for (size_t i = 0; i < datasetPool_.size(); ++i) {
        const Dataset& ds = *datasetPool_[i];
        QString label = QString::fromStdString(ds.name);
        if (!ds.attribute.empty()) label += QString("   [%1]").arg(QString::fromStdString(ds.attribute));
        names << label;
        if (panelA_.app.loaded && panelA_.app.foreground == datasetPool_[i]) defaultA = int(i);
        if (panelB_.app.loaded && panelB_.app.foreground == datasetPool_[i]) defaultB = int(i);
    }
    if (defaultA < 0 && !names.isEmpty()) defaultA = 0;
    if (defaultB < 0 && names.size() > 1) defaultB = defaultA == 0 ? 1 : 0;
    calculatorDialog_->refreshDatasets(names, defaultA, defaultB);
    calculatorDialog_->show();
    calculatorDialog_->raise();
    calculatorDialog_->activateWindow();
}

namespace {
// Small modal "one number + output name" dialog for the per-trace
// processing steps. Returns false on Cancel.
bool askProcessingValue(QWidget* parent, const QString& title, const QString& label, const QString& explanation,
                        double minimum, double maximum, double step, int decimals, const QString& suffix,
                        double& value, QString& outputName, const std::function<QString(double)>& defaultName) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QGridLayout* grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    QDoubleSpinBox* spin = new QDoubleSpinBox();
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setValue(value);
    QLineEdit* name = new QLineEdit(defaultName(value));
    bool nameEdited = false;
    QObject::connect(name, &QLineEdit::textEdited, &dialog, [&nameEdited]() { nameEdited = true; });
    // The default name follows the value until the user types one.
    QObject::connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog, [&](double v) {
        if (!nameEdited) name->setText(defaultName(v));
    });
    grid->addWidget(new QLabel(label), 0, 0);
    grid->addWidget(spin, 0, 1);
    grid->addWidget(new QLabel("Output name"), 1, 0);
    grid->addWidget(name, 1, 1);
    layout->addLayout(grid);
    QLabel* hint = new QLabel(explanation);
    hint->setWordWrap(true);
    hint->setStyleSheet(themed("color: #9a9ca3; font-size: 11px;"));
    layout->addWidget(hint);
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();
    QPushButton* ok = new QPushButton("OK");
    QPushButton* cancel = new QPushButton("Cancel");
    ok->setDefault(true);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    QObject::connect(ok, &QPushButton::clicked, &dialog, [&]() {
        if (!name->text().trimmed().isEmpty()) dialog.accept();
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.resize(420, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return false;
    value = spin->value();
    outputName = name->text().trimmed();
    return true;
}

QString compactNumber(double value) {
    QString text = QString::number(value, 'f', 3);
    while (text.contains('.') && (text.endsWith('0') || text.endsWith('.'))) text.chop(1);
    return text;
}
} // namespace

void MainWindow::openTraceDerivative(bool integrate) {
    Panel* panel = activePanel_;
    if (!panel->app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    if (panel->app.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
    const QString source = QString::fromStdString(panel->app.foreground->filePath.stem().string());
    double db = integrate ? lastIntegrationDb_ : lastDifferentiationDb_;
    QString outputName;
    const QString what = integrate ? "Integration" : "Differentiation";
    if (!askProcessingValue(this, "Trace " + what, "Slope",
                            integrate ? "Spectrum multiplied by (i 2 pi f)^-a, a = slope / 6.02: 6 dB/octave is full "
                                        "integration (-90 degrees), 3 dB/octave a half integral, 0 leaves the data as is."
                                      : "Spectrum multiplied by (i 2 pi f)^a, a = slope / 6.02: 6 dB/octave is full "
                                        "differentiation (+90 degrees), 3 dB/octave a half derivative, 0 leaves the data "
                                        "as is.",
                            0.0, 12.0, 0.5, 1, " dB/octave", db, outputName, [source, integrate](double v) {
                                return QString("%1_%2%3").arg(source, integrate ? "int" : "diff", compactNumber(v));
                            })) {
        return;
    }
    (integrate ? lastIntegrationDb_ : lastDifferentiationDb_) = db;
    const double slope = integrate ? -db : db;
    const QString history = QString("TRACE %1 %2 DB/OCTAVE%3")
                                .arg(what.toUpper())
                                .arg(compactNumber(db))
                                .arg(std::fabs(db - 20.0 * std::log10(2.0)) < 0.05 ? " (FULL)" : "");
    runTraceTransform(panel, "Trace " + what, outputName, history,
                      [slope](std::vector<float>& trace, double sampleIntervalUs) {
                          segy::traceDerivative(trace, sampleIntervalUs, slope);
                      });
}

void MainWindow::openTraceScale() {
    Panel* panel = activePanel_;
    if (!panel->app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    if (panel->app.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
    const QString source = QString::fromStdString(panel->app.foreground->filePath.stem().string());
    double factor = lastScaleFactor_;
    QString outputName;
    if (!askProcessingValue(this, "Trace Math: Scale", "Factor",
                            "Every sample is multiplied by this factor (negative flips the polarity).", -1e9, 1e9, 0.1,
                            4, QString(), factor, outputName,
                            [source](double v) { return QString("%1_x%2").arg(source, compactNumber(v)); })) {
        return;
    }
    lastScaleFactor_ = factor;
    runTraceTransform(panel, "Trace Math: Scale", outputName, QString("TRACE MATH: SCALE BY %1").arg(compactNumber(factor)),
                      [factor](std::vector<float>& trace, double) {
                          const float f = float(factor);
                          for (float& v : trace) v *= f;
                      });
}

constexpr int kFkFilterPanelTraces = 128; // f-k filter panel width (50% overlap)

namespace {
// One field of a processing dialog.
struct ProcessingField {
    QString label;
    double minimum, maximum, step;
    int decimals;
    QString suffix;
    double* value;
};

// The processing dialogs' shared shape: a few numbers, an output name, a
// one-paragraph explanation, OK/Cancel. Returns false on Cancel.
bool askProcessingValues(QWidget* parent, const QString& title, const QString& explanation,
                         const std::vector<ProcessingField>& fields, QString& outputName) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QGridLayout* grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    std::vector<QDoubleSpinBox*> spins;
    int row = 0;
    for (const ProcessingField& field : fields) {
        QDoubleSpinBox* spin = new QDoubleSpinBox();
        spin->setRange(field.minimum, field.maximum);
        spin->setSingleStep(field.step);
        spin->setDecimals(field.decimals);
        spin->setSuffix(field.suffix);
        spin->setValue(*field.value);
        grid->addWidget(new QLabel(field.label), row, 0);
        grid->addWidget(spin, row++, 1);
        spins.push_back(spin);
    }
    QLineEdit* name = new QLineEdit(outputName);
    grid->addWidget(new QLabel("Output name"), row, 0);
    grid->addWidget(name, row, 1);
    layout->addLayout(grid);
    QLabel* hint = new QLabel(explanation);
    hint->setWordWrap(true);
    hint->setStyleSheet(themed("color: #9a9ca3; font-size: 11px;"));
    layout->addWidget(hint);
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();
    QPushButton* ok = new QPushButton("OK");
    QPushButton* cancel = new QPushButton("Cancel");
    ok->setDefault(true);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    QObject::connect(ok, &QPushButton::clicked, &dialog, [&]() {
        if (!name->text().trimmed().isEmpty()) dialog.accept();
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.resize(440, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return false;
    for (size_t i = 0; i < fields.size(); ++i) *fields[i].value = spins[i]->value();
    outputName = name->text().trimmed();
    return true;
}

// Median distance (m) between consecutive traces' CDP X/Y (or source X/Y)
// over the first 500 traces; 0 when the headers have no coordinates.
double estimateTraceSpacing(const Dataset& ds) {
    std::vector<double> steps;
    const int64_t count = std::min<int64_t>(ds.traceCount, 501);
    double px = 0.0, py = 0.0;
    for (int64_t t = 0; t < count; ++t) {
        const segy::TraceHeader th =
            segy::parseTraceHeader(ds.file.data() + segy::kHeaderTotalSize + size_t(t) * ds.traceStrideBytes);
        const double scale = th.coordinateScalar < 0 ? 1.0 / -th.coordinateScalar
                             : th.coordinateScalar > 0 ? double(th.coordinateScalar)
                                                       : 1.0;
        const bool useCdp = th.cdpX != 0 || th.cdpY != 0;
        const double x = (useCdp ? th.cdpX : th.x) * scale, y = (useCdp ? th.cdpY : th.y) * scale;
        if (t > 0) {
            const double d = std::hypot(x - px, y - py);
            if (d > 0.0) steps.push_back(d);
        }
        px = x;
        py = y;
    }
    if (steps.empty()) return 0.0;
    std::nth_element(steps.begin(), steps.begin() + std::ptrdiff_t(steps.size() / 2), steps.end());
    return steps[steps.size() / 2];
}

// The processing guards shared by every dialog: data loaded, not x-f.
bool processingReady(MainWindow* window, Panel* panel, const std::function<void(const QString&)>& flash) {
    (void)window;
    if (!panel->app.loaded) {
        flash("No data selected");
        return false;
    }
    if (panel->app.xf.active) {
        flash("Not available on an x-f display");
        return false;
    }
    return true;
}
} // namespace

void MainWindow::openSpectralWhitening() {
    Panel* panel = activePanel_;
    if (!processingReady(this, panel, [this](const QString& m) { flashStatusMessage(m); })) return;
    segy::WhiteningParams p = lastWhitening_;
    QString name = QString::fromStdString(panel->app.foreground->filePath.stem().string()) + "_whiten";
    if (!askProcessingValues(this, "Spectral Whitening",
                             "Each trace's amplitude spectrum is divided by its own running average (the smoothing "
                             "width), so every frequency between From and To gets the same strength; the phase is "
                             "kept, outside the band it is tapered off, and the trace keeps its RMS.",
                             {{"From", 0.0, 1000.0, 1.0, 1, " Hz", &p.lowHz},
                              {"To", 1.0, 1000.0, 1.0, 1, " Hz", &p.highHz},
                              {"Smoothing", 1.0, 100.0, 1.0, 1, " Hz", &p.smoothingHz}},
                             name)) {
        return;
    }
    if (p.highHz <= p.lowHz) {
        QMessageBox::warning(this, "Spectral Whitening", "To must be above From.");
        return;
    }
    lastWhitening_ = p;
    runTraceTransform(panel, "Spectral Whitening", name,
                      QString("SPECTRAL WHITENING %1-%2 HZ, SMOOTHING %3 HZ")
                          .arg(compactNumber(p.lowHz), compactNumber(p.highHz), compactNumber(p.smoothingHz)),
                      [p](std::vector<float>& trace, double intervalUs) { segy::spectralWhitening(trace, intervalUs, p); });
}

void MainWindow::openDeconvolution() {
    Panel* panel = activePanel_;
    if (!processingReady(this, panel, [this](const QString& m) { flashStatusMessage(m); })) return;
    segy::DeconParams p = lastDecon_;
    const double dtMs = panel->app.foreground->binHeader.sampleIntervalUs / 1000.0;
    QString name = QString::fromStdString(panel->app.foreground->filePath.stem().string()) + "_decon";
    if (!askProcessingValues(this, "Deconvolution",
                             QString("Predictive (Wiener) deconvolution, designed per trace on the whole trace. It "
                                     "removes what can be predicted from the gap on: reverberations and short-period "
                                     "multiples. A gap of one sample (%1 ms) is spiking deconvolution, which whitens. "
                                     "White noise stabilises the filter.")
                                 .arg(compactNumber(dtMs)),
                             {{"Operator length", 2.0 * dtMs, 2000.0, 10.0, 1, " ms", &p.operatorMs},
                              {"Prediction gap", dtMs, 1000.0, dtMs, 1, " ms", &p.gapMs},
                              {"White noise", 0.0, 50.0, 0.1, 2, " %", &p.whiteNoisePercent}},
                             name)) {
        return;
    }
    lastDecon_ = p;
    runTraceTransform(panel, "Deconvolution", name,
                      QString("PREDICTIVE DECONVOLUTION, OPERATOR %1 MS, GAP %2 MS, WHITE NOISE %3%")
                          .arg(compactNumber(p.operatorMs), compactNumber(p.gapMs), compactNumber(p.whiteNoisePercent)),
                      [p](std::vector<float>& trace, double intervalUs) {
                          segy::predictiveDeconvolution(trace, intervalUs, p);
                      });
}

void MainWindow::runAttribute(int which) {
    Panel* panel = activePanel_;
    if (!processingReady(this, panel, [this](const QString& m) { flashStatusMessage(m); })) return;
    const QString source = QString::fromStdString(panel->app.foreground->filePath.stem().string());
    if (which == 3) { // phase rotation: the one attribute with a parameter
        double degrees = lastRotationDegrees_;
        QString name = source + "_rot";
        if (!askProcessingValues(this, "Phase Rotation",
                                 "Adds this angle to the phase of every frequency (x cos a - H[x] sin a, with H the "
                                 "Hilbert transform). 180 flips the polarity; 90 turns a zero-phase wavelet "
                                 "antisymmetric.",
                                 {{"Angle", -180.0, 180.0, 5.0, 1, QString::fromUtf8(" \xc2\xb0"), &degrees}}, name)) {
            return;
        }
        lastRotationDegrees_ = degrees;
        runTraceTransform(panel, "Phase Rotation", name, QString("PHASE ROTATION %1 DEGREES").arg(compactNumber(degrees)),
                          [degrees](std::vector<float>& trace, double) { segy::phaseRotate(trace, degrees); });
        return;
    }
    static const char* kNames[] = {"Envelope", "Instantaneous Phase", "Instantaneous Frequency"};
    static const char* kSuffix[] = {"_env", "_iphase", "_ifreq"};
    runTraceTransform(panel, kNames[which], source + kSuffix[which],
                      QString("ATTRIBUTE: %1%2")
                          .arg(QString(kNames[which]).toUpper())
                          .arg(which == 1 ? " (DEGREES)" : which == 2 ? " (HZ)" : ""),
                      [which](std::vector<float>& trace, double intervalUs) {
                          if (which == 0) segy::envelope(trace);
                          else if (which == 1) segy::instantaneousPhase(trace);
                          else segy::instantaneousFrequency(trace, intervalUs);
                      });
}

void MainWindow::openFkFilter() {
    Panel* panel = activePanel_;
    if (!processingReady(this, panel, [this](const QString& m) { flashStatusMessage(m); })) return;
    std::shared_ptr<Dataset> ds = panel->app.foreground;
    segy::FkFilterParams p = lastFk_;
    const double estimated = estimateTraceSpacing(*ds);
    if (estimated > 0.0) p.traceSpacing = estimated;
    QString name = QString::fromStdString(ds->filePath.stem().string()) + "_fk";
    if (!askProcessingValues(this, "f-k Filter",
                             QString("Removes events whose apparent velocity is below the cut (steep dips: ground "
                                     "roll, steep noise, diffraction tails), along consecutive traces in panels of %1 "
                                     "traces with 50% overlap. Flat events always pass. The taper ramps from (1 - "
                                     "taper) x velocity up to the velocity. Trace spacing %2.")
                                 .arg(kFkFilterPanelTraces)
                                 .arg(estimated > 0.0 ? QString("from the coordinates: %1 m").arg(compactNumber(estimated))
                                                      : QString("not in the headers: please check")),
                             {{"Remove below", 10.0, 100000.0, 100.0, 0, " m/s", &p.velocity},
                              {"Taper", 0.0, 90.0, 5.0, 0, " %", &p.taperPercent},
                              {"Trace spacing", 0.01, 100000.0, 0.5, 2, " m", &p.traceSpacing}},
                             name)) {
        return;
    }
    lastFk_ = p;
    std::vector<std::string> history = ds->processingHistory;
    history.push_back(QString("F-K FILTER: REMOVE BELOW %1 M/S, TAPER %2%, DX %3 M, PANELS %4 TRACES")
                          .arg(compactNumber(p.velocity), compactNumber(p.taperPercent), compactNumber(p.traceSpacing))
                          .arg(kFkFilterPanelTraces)
                          .toStdString());
    const int samplesPerTrace = ds->binHeader.samplesPerTrace;
    QStringList inputBlocks;
    inputBlocks << QString("FILE: %1  TRACES: %2").arg(QString::fromStdString(ds->name)).arg(ds->traceCount);
    const QString textHeader = buildSegyTextHeader(inputBlocks, history, name, ds->traceCount, samplesPerTrace,
                                                   ds->binHeader.sampleIntervalUs);
    const std::filesystem::path outPath = scratchSegyPath(name);
    struct Outcome {
        bool ok = false;
        std::string error;
    };
    auto outcome = std::make_shared<Outcome>();
    runBackgroundTask(
        QString("f-k Filter: %1").arg(QString::fromStdString(ds->name)),
        [this, ds, p, samplesPerTrace, textHeader, outPath, outcome]() {
            // Panels of N traces every N/2, weighted by sin^2 across the panel
            // (two overlapping weights add up to 1); each output chunk adds
            // the panels that touch it.
            const int64_t n = kFkFilterPanelTraces, hop = n / 2;
            const int64_t traceCount = ds->traceCount;
            const double intervalUs = ds->binHeader.sampleIntervalUs;
            backgroundTaskTotal_ = traceCount;
            segy::ThreadPool pool;
            auto decode = [&](int64_t t, float* out) {
                const uint8_t* base = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                segy::decodeSamples(base + segy::kTraceHeaderSize, out, size_t(samplesPerTrace), ds->binHeader.formatCode);
            };
            outcome->ok = segy::writeSegyFileChunked(
                outPath, textHeader.toStdString(), ds->binHeader, traceCount,
                [&](int64_t begin, int64_t end, std::vector<segy::WriteTrace>& chunk) {
                    for (int64_t t = begin; t < end; ++t) {
                        segy::WriteTrace& out = chunk[size_t(t - begin)];
                        const uint8_t* base = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                        out.headerBytes.assign(base, base + segy::kTraceHeaderSize);
                        out.samples.assign(size_t(samplesPerTrace), 0.0f);
                    }
                    // Panel p covers traces [(p - 1) hop, (p - 1) hop + n).
                    const int64_t pFirst = begin / hop - 1, pLast = end / hop + 1;
                    std::vector<int64_t> panelIds;
                    for (int64_t pid = std::max<int64_t>(0, pFirst); pid <= pLast; ++pid) {
                        const int64_t start = (pid - 1) * hop;
                        if (start < end && start + n > begin && start < traceCount) panelIds.push_back(pid);
                    }
                    std::vector<std::vector<std::vector<float>>> results(panelIds.size());
                    pool.parallelFor(panelIds.size(), 1, [&](size_t b, size_t e) {
                        for (size_t i = b; i < e; ++i) {
                            const int64_t start = (panelIds[i] - 1) * hop;
                            std::vector<std::vector<float>> panelTraces(size_t(n), std::vector<float>(size_t(samplesPerTrace), 0.0f));
                            for (int64_t k = 0; k < n; ++k) {
                                const int64_t t = start + k;
                                if (t >= 0 && t < traceCount) decode(t, panelTraces[size_t(k)].data());
                            }
                            segy::fkFilterPanel(panelTraces, intervalUs, p);
                            results[i] = std::move(panelTraces);
                        }
                    });
                    const double pi = 3.14159265358979323846;
                    for (size_t i = 0; i < panelIds.size(); ++i) {
                        const int64_t start = (panelIds[i] - 1) * hop;
                        for (int64_t k = 0; k < n; ++k) {
                            const int64_t t = start + k;
                            if (t < begin || t >= end) continue;
                            const float w = float(std::pow(std::sin(pi * (double(k) + 0.5) / double(n)), 2.0));
                            std::vector<float>& dst = chunk[size_t(t - begin)].samples;
                            const std::vector<float>& src = results[i][size_t(k)];
                            for (int s = 0; s < samplesPerTrace; ++s) dst[size_t(s)] += w * src[size_t(s)];
                        }
                    }
                    backgroundTaskDone_ = end;
                },
                &outcome->error, 4096);
        },
        [this, panel, outPath, history, outcome]() {
            if (!outcome->ok) {
                QMessageBox::warning(this, "f-k Filter", QString::fromStdString(outcome->error));
                return;
            }
            panel->canvas->startLoading(outPath, history);
        });
}

void MainWindow::runTraceTransform(Panel* panel, const QString& title, const QString& outputName,
                                   const QString& historyLine,
                                   std::function<void(std::vector<float>&, double)> transform) {
    AppState& source = panel->app;
    if (!source.loaded || source.foreground->traceCount <= 0 || source.foreground->binHeader.samplesPerTrace <= 0) {
        flashStatusMessage("No data selected");
        return;
    }
    std::shared_ptr<Dataset> ds = source.foreground;
    const int64_t traceCount = ds->traceCount;
    const int samplesPerTrace = ds->binHeader.samplesPerTrace;
    const double intervalUs = ds->binHeader.sampleIntervalUs;
    const QString sourceName = QString::fromStdString(ds->filePath.stem().string());
    QStringList inputBlocks;
    inputBlocks << QString("FILE: %1  TRACES: %2  SAMPLES/TRACE: %3  SAMPLE INTERVAL: %4 US")
                       .arg(sourceName)
                       .arg(traceCount)
                       .arg(samplesPerTrace)
                       .arg(ds->binHeader.sampleIntervalUs);
    std::vector<std::string> history = ds->processingHistory;
    history.push_back(historyLine.toStdString());
    const QString textHeader =
        buildSegyTextHeader(inputBlocks, history, outputName, traceCount, samplesPerTrace, ds->binHeader.sampleIntervalUs);
    const std::filesystem::path outPath = scratchSegyPath(outputName);
    const segy::BinaryHeader outHeader = ds->binHeader;
    auto writeOk = std::make_shared<bool>(false);
    auto writeError = std::make_shared<std::string>();
    runBackgroundTask(
        QString("%1: %2").arg(title, sourceName),
        [this, ds, traceCount, samplesPerTrace, intervalUs, transform, outHeader, textHeader, outPath, writeOk,
         writeError]() {
            backgroundTaskTotal_ = traceCount;
            // Traces are independent: in parallel (decoding from the mmap is thread-safe).
            *writeOk = streamTraces(
                outPath, textHeader.toStdString(), outHeader, traceCount,
                [&](int64_t t, segy::WriteTrace& out) {
                    const uint8_t* traceBase = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                    out.headerBytes.assign(traceBase, traceBase + segy::kTraceHeaderSize);
                    out.samples.resize(size_t(samplesPerTrace));
                    segy::decodeSamples(traceBase + segy::kTraceHeaderSize, out.samples.data(), size_t(samplesPerTrace),
                                        ds->binHeader.formatCode);
                    transform(out.samples, intervalUs);
                },
                backgroundTaskDone_, writeError.get());
        },
        [this, panel, title, writeOk, writeError, outPath, history]() {
            if (!*writeOk) {
                QMessageBox::warning(this, title, QString::fromStdString(*writeError));
                return;
            }
            panel->canvas->startLoading(outPath, history);
        });
}

void MainWindow::openBandpassDialog() {
    if (!bandpassDialog_) {
        bandpassDialog_ = new BandpassDialog(this);
        bandpassDialog_->setApplyCallback([this](segy::ButterworthBandParams params, QString outputName) {
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

void MainWindow::openPreferencesDialog() {
    if (!preferencesDialog_) {
        std::vector<ToolbarItemInfo> items;
        // Grouped by what the buttons do, for Preferences' Toolbar
        // Buttons section (finer than the toolbar's own separators,
        // which lump analysis, headers and panel controls together).
        auto categoryFor = [](const QString& id) -> QString {
            if (id == "zoom" || id == "mooz") return "Zoom";
            if (id == "pan" || id == "boxSelect" || id == "pick") return "Tools";
            if (id == "seismicMode" || id == "flipHorizontal" || id == "idents") return "Display";
            if (id == "displayParameters" || id == "gain" || id == "agc") return "Amplitude";
            if (id == "spectrum" || id == "histogram" || id == "octaveBands" || id == "autocorrelation" || id == "xf")
                return "Analysis";
            if (id == "textHeader" || id == "binaryHeader") return "Headers";
            return "Panels"; // splitView, lock, sidebar
        };
        for (const ToolbarItem& item : toolbarItems_) items.push_back({item.id, item.label, categoryFor(item.id)});
        preferencesDialog_ = new PreferencesDialog(prefs_, items, this);
        preferencesDialog_->setAppliedCallback([this](const Preferences& newPrefs) {
            // Skipped traces and Min correlation are edited in the Signal/Noise window, not here.
            const int nskip = prefs_.signalNoise.nskip;
            const double ccmin = prefs_.signalNoise.ccmin;
            prefs_ = newPrefs;
            prefs_.signalNoise.nskip = nskip;
            prefs_.signalNoise.ccmin = ccmin;
            applyToolbarPosition(prefs_.toolbarPosition); // also applies prefs_.drawerPosition
            savePreferences();
            refreshRecomputeButtons();
        });
    }
    preferencesDialog_->show();
    preferencesDialog_->raise();
    preferencesDialog_->activateWindow();
}

void MainWindow::refreshAfterDisplaySwap() {
    // Every toolbar/menu control mirrors the active panel's display
    // settings, which an x-f swap replaces wholesale -- re-sync them all.
    setActivePanel(activePanel_);
}

void MainWindow::toggleXf(Panel* panel, bool on) {
    AppState& app = panel->app;
    if (!on) {
        panel->canvas->exitXfMode();
        refreshAfterDisplaySwap();
        return;
    }
    if (!app.loaded || app.xf.active) {
        if (!app.loaded) flashStatusMessage("No data selected");
        refreshAfterDisplaySwap();
        return;
    }
    // Every trace in view (not the selection box), over the visible time range.
    std::shared_ptr<Dataset> ds = app.foreground;
    const int samplesPerTrace = ds->binHeader.samplesPerTrace;
    const double intervalUs = ds->binHeader.sampleIntervalUs;
    const int64_t t0 = std::clamp<int64_t>(int64_t(std::floor(app.view.traceStart)), 0, ds->traceCount);
    const int64_t t1 = std::clamp<int64_t>(int64_t(std::ceil(app.view.traceEnd)), t0, ds->traceCount);
    const int s0 = std::clamp(int(std::floor(app.view.sampleStart)), 0, samplesPerTrace);
    const int s1 = std::clamp(int(std::ceil(app.view.sampleEnd)), s0, samplesPerTrace);
    const segy::XfGrid grid = segy::makeXfGrid(s1 - s0, intervalUs);
    if (t1 <= t0 || grid.bins < 2) {
        flashStatusMessage("Not enough data in view for an x-f display");
        refreshAfterDisplaySwap();
        return;
    }
    xfAction_->setEnabled(false); // until the computation lands
    // A screen shows a few thousand columns at most: more traces than that
    // only cost time and memory (603k traces took 89 s and a 4.9 GB scratch
    // file). Every stride-th trace is used instead.
    const int64_t kMaxXfTraces = 4096;
    const int64_t stride = std::max<int64_t>(1, (t1 - t0 + kMaxXfTraces - 1) / kMaxXfTraces);
    const int64_t xfCount = (t1 - t0 + stride - 1) / stride;

    const QString sourceName = QString::fromStdString(ds->filePath.stem().string());
    const QString outputName = sourceName + "_xF";
    const std::filesystem::path outPath = scratchSegyPath(outputName);
    std::vector<std::string> history = ds->processingHistory;
    history.push_back(QString("X-F: AMPLITUDE SPECTRUM DB RE MAX, TRACES %1-%2 EVERY %7, %3-%4 MS, COS2 TAPER 80% FLAT")
                          .arg(t0)
                          .arg(t1 - 1)
                          .arg(s0 * intervalUs / 1000.0, 0, 'f', 0)
                          .arg(s1 * intervalUs / 1000.0, 0, 'f', 0)
                          .arg(stride)
                          .toStdString());
    QStringList inputBlocks;
    inputBlocks << QString("FILE: %1  TRACES: %2-%3  GATE: %4-%5 MS  SAMPLE INTERVAL: %6 US")
                       .arg(sourceName)
                       .arg(t0)
                       .arg(t1 - 1)
                       .arg(s0 * intervalUs / 1000.0, 0, 'f', 0)
                       .arg(s1 * intervalUs / 1000.0, 0, 'f', 0)
                       .arg(intervalUs);
    QString textHeader = buildSegyTextHeader(inputBlocks, history, outputName, xfCount, grid.bins, grid.binIntervalUs);
    segy::BinaryHeader outHeader = ds->binHeader;
    outHeader.samplesPerTrace = int16_t(grid.bins);
    outHeader.sampleIntervalUs = int16_t(grid.binIntervalUs);

    struct XfOutcome {
        bool ok = false;
        std::string error;
        segy::LoadResult load;
    };
    auto outcome = std::make_shared<XfOutcome>();
    runBackgroundTask(
        stride > 1 ? QString("x-f: %1 of %2 traces (every %3th)").arg(xfCount).arg(t1 - t0).arg(stride)
                   : QString("x-f: %1 traces").arg(t1 - t0),
        [this, ds, samplesPerTrace, t0, stride, xfCount, s0, s1, grid, outHeader, textHeader, outPath, outcome]() {
            const int64_t count = xfCount;
            backgroundTaskTotal_ = count + 1;
            // Spectra in parallel over the traces (decoding from the mmap is
            // thread-safe), then the headers serially.
            std::vector<std::vector<float>> spectra = segy::computeXfAmplitudes(
                [ds, samplesPerTrace, t0, stride](int64_t k, float* samples) {
                    const uint8_t* traceBase =
                        ds->file.data() + segy::kHeaderTotalSize + size_t(t0 + k * stride) * ds->traceStrideBytes;
                    segy::decodeSamples(traceBase + segy::kTraceHeaderSize, samples, size_t(samplesPerTrace),
                                        ds->binHeader.formatCode);
                },
                samplesPerTrace, 0, count, s0, s1, grid, [this](int64_t done) { backgroundTaskDone_ = done; });
            std::vector<segy::WriteTrace> traces(static_cast<size_t>(count));
            float maxAmp = 0.0f;
            for (int64_t i = 0; i < count; ++i) {
                const uint8_t* traceBase =
                    ds->file.data() + segy::kHeaderTotalSize + size_t(t0 + i * stride) * ds->traceStrideBytes;
                segy::WriteTrace& out = traces[size_t(i)];
                out.headerBytes.assign(traceBase, traceBase + segy::kTraceHeaderSize);
                // Trace header: no delay, the x-f sample count and interval
                // (big-endian int16 at bytes 109, 115 and 117).
                auto putInt16 = [&out](size_t offset, int value) {
                    out.headerBytes[offset] = uint8_t((value >> 8) & 0xff);
                    out.headerBytes[offset + 1] = uint8_t(value & 0xff);
                };
                putInt16(108, 0);
                putInt16(114, grid.bins);
                putInt16(116, grid.binIntervalUs);
                out.samples = std::move(spectra[size_t(i)]);
                for (float amp : out.samples) maxAmp = std::max(maxAmp, amp);
            }
            // dB re the largest amplitude in view: 0 dB at the maximum.
            // Silent bins bottom out at -120 dB instead of -inf.
            for (segy::WriteTrace& tr : traces) {
                for (float& a : tr.samples) {
                    a = maxAmp > 0.0f && a > 0.0f ? std::max(-120.0f, float(20.0 * std::log10(double(a) / maxAmp)))
                                                  : -120.0f;
                }
            }
            if (!segy::writeSegyFile(outPath, textHeader.toStdString(), outHeader, traces, &outcome->error)) return;
            traces.clear();
            // Own pool: the panel's pool keeps rendering the seismic meanwhile.
            segy::ThreadPool pool{std::max(1u, std::thread::hardware_concurrency())};
            outcome->load = segy::loadSegyFile(outPath, pool);
            outcome->ok = outcome->load.ok;
            if (!outcome->ok) outcome->error = outcome->load.error;
            backgroundTaskDone_ = count + 1;
        },
        [this, panel, outcome, outPath, history, t0, stride]() {
            xfAction_->setEnabled(true);
            if (!outcome->ok) {
                QMessageBox::warning(this, "x-f display", QString::fromStdString(outcome->error));
                refreshAfterDisplaySwap();
                return;
            }
            // The seismic may have changed while this ran (another file, or
            // x-f already on): only swap onto a still-loaded, plain display.
            if (!panel->app.loaded || panel->app.xf.active) {
                refreshAfterDisplaySwap();
                return;
            }
            auto dataset = std::make_shared<Dataset>();
            dataset->name = outPath.stem().string();
            dataset->filePath = outPath;
            dataset->file = std::move(outcome->load.file);
            dataset->binHeader = outcome->load.binHeader;
            dataset->traceCount = outcome->load.traceCount;
            dataset->traceStrideBytes = outcome->load.traceStrideBytes;
            dataset->pyramid = std::move(outcome->load.pyramid);
            dataset->processingHistory = history;
            panel->canvas->enterXfMode(dataset, t0, stride);
            refreshAfterDisplaySwap();
        });
}

bool sameSigcorParams(const segy::SigcorParams& a, const segy::SigcorParams& b) {
    return a.option == b.option && a.nskip == b.nskip && a.iransk == b.iransk && a.shmaxMs == b.shmaxMs &&
           a.ccmin == b.ccmin && a.kaiserBeta == b.kaiserBeta &&
           a.normalise == b.normalise && a.removeDc == b.removeDc;
}

bool sameSignalNoiseSource(const SignalNoiseSource& a, const SignalNoiseSource& b) {
    return a.valid == b.valid && a.app == b.app && a.dataset == b.dataset && a.t0 == b.t0 && a.t1 == b.t1 &&
           a.s0 == b.s0 && a.s1 == b.s1;
}

void MainWindow::ensureSignalNoiseWindow() {
    if (signalNoiseWindow_) return;
    signalNoiseWindow_ = new SignalNoiseWindow(this);
    signalNoiseWindow_->setWindowParams(prefs_.signalNoise);
    signalNoiseWindow_->setRecomputeCallback([this]() { computeAndShowSignalNoise(activePanel_); });
    signalNoiseWindow_->setParamsEditedCallback([this]() { refreshRecomputeButtons(); });
}

SignalNoiseSource MainWindow::resolveSignalNoiseSource(Panel* panel) const {
    SignalNoiseSource src;
    const AppState& app = panel->app;
    if (!app.loaded) return src;
    const Dataset& ds = *app.foreground;
    const int samplesPerTrace = ds.binHeader.samplesPerTrace;
    // The active selection box if there is one, else the visible data --
    // same choice as everywhere else the user marks "what to analyze".
    const SelectionBox* box = activeBox(app);
    if (box && box->complete) {
        src.t0 = int64_t(std::min(box->traceA, box->traceB));
        src.t1 = int64_t(std::max(box->traceA, box->traceB));
        src.s0 = int(std::min(box->sampleA, box->sampleB));
        src.s1 = int(std::max(box->sampleA, box->sampleB));
        src.sourceText = "selection box";
    } else {
        src.t0 = int64_t(std::floor(app.view.traceStart));
        src.t1 = int64_t(std::ceil(app.view.traceEnd));
        src.s0 = int(std::floor(app.view.sampleStart));
        src.s1 = int(std::ceil(app.view.sampleEnd));
        src.sourceText = "visible data";
    }
    src.t0 = std::clamp<int64_t>(src.t0, 0, ds.traceCount);
    src.t1 = std::clamp<int64_t>(src.t1, src.t0, ds.traceCount);
    src.s0 = std::clamp(src.s0, 0, samplesPerTrace);
    src.s1 = std::clamp(src.s1, src.s0, samplesPerTrace);
    src.app = &app;
    src.dataset = app.foreground.get();
    src.valid = ds.binHeader.sampleIntervalUs > 0;
    return src;
}

segy::SigcorParams MainWindow::currentSignalNoiseParams() const {
    segy::SigcorParams params = prefs_.signalNoise;
    if (signalNoiseWindow_) signalNoiseWindow_->applyWindowParams(params);
    return params;
}

void MainWindow::refreshSignalNoiseRecompute() {
    if (!signalNoiseWindow_) return;
    bool useful = true;
    if (signalNoiseComputed_) {
        useful = !sameSignalNoiseSource(resolveSignalNoiseSource(activePanel_), signalNoiseLastSource_) ||
                 !sameSigcorParams(currentSignalNoiseParams(), signalNoiseLastParams_);
    }
    if (!activePanel_->app.loaded) useful = false;
    signalNoiseWindow_->setRecomputeUseful(useful);
}

void MainWindow::computeAndShowSignalNoise(Panel* sourcePanel) {
    if (sourcePanel->app.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
    AppState& app = sourcePanel->app;
    if (!app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    ensureSignalNoiseWindow();
    const SignalNoiseSource src = resolveSignalNoiseSource(sourcePanel);
    if (!src.valid) return;
    prefs_.signalNoise = currentSignalNoiseParams();
    savePreferences();
    const segy::SigcorParams params = prefs_.signalNoise;
    signalNoiseComputed_ = true;
    signalNoiseLastSource_ = src;
    signalNoiseLastParams_ = params;
    refreshRecomputeButtons();

    std::shared_ptr<Dataset> ds = app.foreground;
    const int samplesPerTrace = ds->binHeader.samplesPerTrace;
    const double intervalUs = ds->binHeader.sampleIntervalUs;
    const int64_t t0 = src.t0, t1 = src.t1;
    const int s0 = src.s0, s1 = src.s1;
    auto result = std::make_shared<segy::SigcorResult>();
    runBackgroundTask(
        QString("Signal/Noise: %1 traces").arg(t1 - t0),
        [this, ds, samplesPerTrace, t0, t1, s0, s1, intervalUs, params, result]() {
            backgroundTaskDone_ = 0;
            backgroundTaskTotal_ = 1;
            auto fetch = [ds, samplesPerTrace](int64_t traceIdx, float* out) {
                const uint8_t* traceBase =
                    ds->file.data() + segy::kHeaderTotalSize + size_t(traceIdx) * ds->traceStrideBytes;
                segy::decodeSamples(traceBase + segy::kTraceHeaderSize, out, size_t(samplesPerTrace),
                                    ds->binHeader.formatCode);
            };
            *result = segy::computeSigcor(fetch, samplesPerTrace, t0, t1, s0, s1, intervalUs, params,
                                          [this](int done, int total) {
                                              backgroundTaskTotal_ = total;
                                              backgroundTaskDone_ = done;
                                          });
        },
        [this, sourcePanel, result, src, intervalUs, params]() {
            const segy::SigcorResult& r = *result;
            SignalNoiseDisplay display;
            auto ms = [](double v) { return QString("%1 ms").arg(v, 0, 'f', 2); };
            auto& d = display.details;
            const double dtMs = intervalUs / 1000.0;
            if (r.pairsUsed > 0) {
                double minMs = std::numeric_limits<double>::infinity(), maxMs = -minMs, correlationSum = 0.0;
                for (const segy::SigcorPair& pair : r.pairs) {
                    if (pair.status != segy::SigcorPairStatus::Accepted) continue;
                    minMs = std::min(minMs, pair.parabolicLag * dtMs);
                    maxMs = std::max(maxMs, pair.parabolicLag * dtMs);
                    correlationSum += pair.correlation;
                }
                d.push_back({"Time shift", ""});
                d.push_back({QString("Mean (%1t)").arg(QChar(0x0394)), ms(r.shiftMeanMs)});
                d.push_back({QString("1%1").arg(QChar(0x03C3)), ms(r.shiftSigmaMs)});
                d.push_back({"Minimum", ms(minMs)});
                d.push_back({"Maximum", ms(maxMs)});
                d.push_back({"Applied", "whole samples"});
                d.push_back({"Mean correlation", QString::number(correlationSum / r.pairsUsed, 'f', 3)});
            }
            d.push_back({"Trace pairs", ""});
            if (r.pairsCandidate > r.pairsTotal) {
                d.push_back({"Available", QString::number(r.pairsCandidate)});
                d.push_back({"Computed", QString("%1 (an even subset)").arg(r.pairsTotal)});
            }
            d.push_back({"Used", QString("%1 of %2").arg(r.pairsUsed).arg(r.pairsTotal)});
            d.push_back({"Below min correlation", QString::number(r.pairsLowCorrelation)});
            d.push_back({"At max shift", QString::number(r.pairsAtShiftLimit)});
            if (r.pairsDead > 0) d.push_back({"Dead traces", QString::number(r.pairsDead)});
            d.push_back({"Data", ""});
            d.push_back({"Source", src.sourceText});
            d.push_back({"Traces", QString("%1 %2 %3").arg(src.t0).arg(QChar(0x2013)).arg(std::max<int64_t>(src.t0, src.t1 - 1))});
            d.push_back({"Time gate", QString("%1 %2 %3 ms")
                                          .arg(src.s0 * intervalUs / 1000.0, 0, 'f', 0)
                                          .arg(QChar(0x2013))
                                          .arg(src.s1 * intervalUs / 1000.0, 0, 'f', 0)});
            d.push_back({"Parameters", ""});
            d.push_back({"Method", params.option == segy::SigcorOption::Autcor ? "Autocorrelation" : "Spectrum"});
            d.push_back({"Skipped traces", QString("%1 \xc2\xb1 %2 (pair distance %3 %4 %5)")
                                               .arg(params.nskip)
                                               .arg(params.iransk)
                                               .arg(params.nskip - params.iransk + 1)
                                               .arg(QChar(0x2013))
                                               .arg(params.nskip + params.iransk + 1)});
            d.push_back({"Max shift", QString("%1 ms (%2 samples)").arg(params.shmaxMs, 0, 'f', 1).arg(r.shmaxSamples)});
            d.push_back({"Min correlation", QString::number(params.ccmin, 'f', 2)});
            d.push_back({"Taper", QString("Kaiser, beta %1").arg(params.kaiserBeta, 0, 'f', 1)});
            d.push_back({"FFT length", QString::number(r.fftLength)});
            d.push_back({"Equalize energy", params.normalise ? "Yes" : "No"});
            d.push_back({"Remove DC", params.removeDc ? "Yes" : "No"});

            if (!r.error.empty()) {
                display.emptyReason = QString::fromStdString(r.error);
            } else if (r.pairsTotal == 0) {
                display.emptyReason =
                    QString("Not enough data: a pair needs at least %1 traces (skipped traces + 2).")
                        .arg(params.nskip - params.iransk + 2);
            } else if (r.pairsDead == r.pairsTotal) {
                display.emptyReason = "Every trace pair is dead (all-zero gates).";
            } else if (r.pairsUsed == 0) {
                display.emptyReason =
                    "No trace pair passed Min correlation and Max shift -- try a lower Min correlation, or a "
                    "larger Max shift in Edit > Preferences.";
            } else {
                const double ref = *std::max_element(r.tracePower.begin(), r.tracePower.end());
                const float nan = std::numeric_limits<float>::quiet_NaN();
                auto toDb = [ref, nan](double power) {
                    return power > 0.0 && ref > 0.0 ? float(10.0 * std::log10(power / ref)) : nan;
                };
                const size_t count = r.frequencyHz.size();
                display.frequencyHz = r.frequencyHz;
                display.traceDb.resize(count);
                display.signalDb.resize(count);
                display.noiseDb.resize(count);
                display.snDb.resize(count);
                for (size_t m = 0; m < count; ++m) {
                    display.traceDb[m] = toDb(r.tracePower[m]);
                    display.signalDb[m] = toDb(r.signalPower[m]);
                    display.noiseDb[m] = toDb(r.noisePower[m]);
                    display.snDb[m] = r.signalPower[m] > 0.0 && r.noisePower[m] > 0.0
                                          ? float(10.0 * std::log10(r.signalPower[m] / r.noisePower[m]))
                                          : nan;
                }
                display.hasShift = true;
                display.shiftSigmaMs = r.shiftSigmaMs;
                display.shiftMeanMs = r.shiftMeanMs;
            }
            ensureSignalNoiseWindow();
            signalNoiseWindow_->setResult(display);
            signalNoiseWindow_->setWindowTitle(QString("Signal/Noise") + datasetTitleSuffix(sourcePanel->app));
            signalNoiseWindow_->show();
            signalNoiseWindow_->raise();
            signalNoiseWindow_->activateWindow();
            refreshRecomputeButtons();
        });
}

void MainWindow::refreshRecomputeButtons() {
    refreshSignalNoiseRecompute();
    refreshFkRecompute();
}

void MainWindow::ensureFkWindow() {
    if (fkWindow_) return;
    fkWindow_ = new FkWindow(this);
    fkWindow_->setRecomputeCallback([this]() { computeAndShowFk(activePanel_); });
    fkWindow_->setParamsEditedCallback([this]() { refreshFkRecompute(); });
}

void MainWindow::refreshFkRecompute() {
    if (!fkWindow_) return;
    bool useful = true;
    if (fkComputed_) {
        useful = !sameSignalNoiseSource(resolveSignalNoiseSource(activePanel_), fkLastSource_) ||
                 fkWindow_->gatherMode() != fkLastGatherMode_ || fkWindow_->distanceMode() != fkLastDistanceMode_ ||
                 fkWindow_->offsetScalar() != fkLastOffsetScalar_ || fkWindow_->cdpDistance() != fkLastCdpDistance_ ||
                 fkWindow_->averageGathers() != fkLastAverage_;
    }
    if (!activePanel_->app.loaded || activePanel_->app.xf.active) useful = false;
    fkWindow_->setRecomputeUseful(useful);
}

namespace {
double medianOf(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(mid), values.end());
    return values[mid];
}

QString formatMetres(double metres) {
    return QString::number(metres, 'f', metres < 10.0 ? 2 : 1).remove(QRegularExpression("\\.?0+$"));
}
} // namespace

void MainWindow::computeAndShowFk(Panel* sourcePanel) {
    AppState& app = sourcePanel->app;
    if (!app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    if (app.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
    ensureFkWindow();
    const SignalNoiseSource src = resolveSignalNoiseSource(sourcePanel);
    if (!src.valid || src.t1 - src.t0 < 2 || src.s1 - src.s0 < 8) {
        flashStatusMessage("Not enough data for an f-k display");
        return;
    }
    const int gatherMode = fkWindow_->gatherMode();
    const int distanceMode = fkWindow_->distanceMode();
    const double offsetScalar = fkWindow_->offsetScalar();
    const double typedCdpDistance = fkWindow_->cdpDistance();
    const bool cdpDistanceTyped = fkWindow_->cdpDistanceTyped();
    const bool averageGathers = fkWindow_->averageGathers();
    fkComputed_ = true;
    fkLastAverage_ = averageGathers;
    fkLastSource_ = src;
    fkLastGatherMode_ = gatherMode;
    fkLastDistanceMode_ = distanceMode;
    fkLastOffsetScalar_ = offsetScalar;
    fkLastCdpDistance_ = typedCdpDistance;
    refreshFkRecompute();

    std::shared_ptr<Dataset> ds = app.foreground;
    const int samplesPerTrace = ds->binHeader.samplesPerTrace;
    const double intervalUs = ds->binHeader.sampleIntervalUs;
    struct FkOutcome {
        std::vector<segy::FkResult> fks; // one per plot
        std::vector<QString> labels;     // per plot, e.g. "CDP = 702"
        double dxMetres = 0.0;
        double proposedCdpDistance = 0.0;
        double cdpDistanceUsed = 0.0;
        QString panelText, rangeText, info, emptyReason;
    };
    auto out = std::make_shared<FkOutcome>();
    runBackgroundTask(
        QString("f-k: %1 traces").arg(src.t1 - src.t0),
        [this, ds, samplesPerTrace, intervalUs, src, gatherMode, distanceMode, offsetScalar, typedCdpDistance,
         cdpDistanceTyped, averageGathers, out]() {
            backgroundTaskDone_ = 0;
            backgroundTaskTotal_ = 1;
            const int64_t t0 = src.t0, t1 = src.t1, n = t1 - t0;
            std::vector<int32_t> ffid(static_cast<size_t>(n)), cdp(static_cast<size_t>(n)), offset(static_cast<size_t>(n));
            std::vector<double> xs(static_cast<size_t>(n)), ys(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i) {
                const uint8_t* base = ds->file.data() + segy::kHeaderTotalSize + size_t(t0 + i) * ds->traceStrideBytes;
                const segy::TraceHeader th = segy::parseTraceHeader(base);
                ffid[size_t(i)] = th.fieldRecord;
                cdp[size_t(i)] = th.cdp;
                offset[size_t(i)] = th.offset;
                const double scale = th.coordinateScalar < 0   ? 1.0 / -double(th.coordinateScalar)
                                     : th.coordinateScalar > 0 ? double(th.coordinateScalar)
                                                               : 1.0;
                const bool cdpXY = th.cdpX != 0 || th.cdpY != 0;
                xs[size_t(i)] = double(cdpXY ? th.cdpX : th.x) * scale;
                ys[size_t(i)] = double(cdpXY ? th.cdpY : th.y) * scale;
            }

            // Panels: gathers of the chosen ident, or sliding panels.
            auto plausibleGathers = [&](const std::vector<int32_t>& keys, std::vector<segy::FkPanel>& panels) {
                panels = segy::splitIntoGathers(keys, t0, 8);
                int64_t covered = 0, widest = 0;
                for (const segy::FkPanel& p : panels) {
                    covered += p.end - p.start;
                    widest = std::max(widest, p.end - p.start);
                }
                return !panels.empty() && widest <= 2048 && covered * 2 >= n;
            };
            std::vector<segy::FkPanel> panels;
            int mode = gatherMode;
            if (mode == 0) {
                mode = plausibleGathers(ffid, panels) ? 1 : plausibleGathers(cdp, panels) ? 2 : 3;
            } else if (mode == 1 || mode == 2) {
                panels = segy::splitIntoGathers(mode == 1 ? ffid : cdp, t0, 8);
            }
            if (mode == 3) panels = segy::slidingPanels(t0, t1, 256);
            if (panels.empty()) {
                out->emptyReason = QString("No %1 gathers of 8 or more traces in this data -- choose another panel "
                                           "ident, or None for a stack.")
                                       .arg(mode == 1 ? "FFID" : "CDP");
                return;
            }
            const size_t gathersFound = panels.size();
            constexpr size_t kMaxPanels = 300;
            if (panels.size() > kMaxPanels) {
                std::vector<segy::FkPanel> strided;
                for (size_t i = 0; i < kMaxPanels; ++i) strided.push_back(panels[i * panels.size() / kMaxPanels]);
                panels = std::move(strided);
            }

            // Distance ident -> trace spacing: the median step within panels times the scalar.
            const int dmode = distanceMode == 0 ? (mode == 3 ? 2 : 1) : distanceMode;
            const std::vector<int32_t>& dkey = dmode == 1 ? offset : cdp;
            // Gathers are put in order of the distance ident (prestack data
            // often stores offsets interleaved), so the space axis of the FFT
            // really runs from small to large. The spacing per panel is its
            // range over (traces - 1) -- robust to repeated offsets -- and the
            // panels' median is used.
            std::vector<double> steps;
            int32_t lo = std::numeric_limits<int32_t>::max(), hi = std::numeric_limits<int32_t>::min();
            for (segy::FkPanel& p : panels) {
                if (mode != 3) {
                    p.traces.clear();
                    for (int64_t t = p.start; t < p.end; ++t) p.traces.push_back(t);
                    std::stable_sort(p.traces.begin(), p.traces.end(), [&dkey, t0](int64_t a, int64_t b) {
                        return dkey[size_t(a - t0)] < dkey[size_t(b - t0)];
                    });
                }
                int32_t plo = std::numeric_limits<int32_t>::max(), phi = std::numeric_limits<int32_t>::min();
                for (int64_t t = p.start; t < p.end; ++t) {
                    const int32_t v = dkey[size_t(t - t0)];
                    plo = std::min(plo, v);
                    phi = std::max(phi, v);
                }
                lo = std::min(lo, plo);
                hi = std::max(hi, phi);
                if (p.end - p.start > 1 && phi > plo) steps.push_back((double(phi) - double(plo)) / double(p.end - p.start - 1));
            }
            const double stepRaw = medianOf(steps);
            // The CDP distance the coordinates imply: distance / CDP step between neighbours.
            std::vector<double> perCdp;
            for (int64_t i = 0; i + 1 < n; ++i) {
                const double dc = std::fabs(double(cdp[size_t(i + 1)]) - double(cdp[size_t(i)]));
                const double dist = std::hypot(xs[size_t(i + 1)] - xs[size_t(i)], ys[size_t(i + 1)] - ys[size_t(i)]);
                if (dc > 0.0 && dist > 0.0) perCdp.push_back(dist / dc);
            }
            out->proposedCdpDistance = medianOf(perCdp);
            const double cdpDistance =
                !cdpDistanceTyped && out->proposedCdpDistance > 0.0 ? out->proposedCdpDistance : typedCdpDistance;
            out->cdpDistanceUsed = cdpDistance;
            out->dxMetres = stepRaw * (dmode == 1 ? offsetScalar : cdpDistance);

            const QString dash(QChar(0x2013));
            if (mode == 3) {
                out->panelText = panels.size() == 1 ? QString("Panel: traces %1%2%3").arg(t0).arg(dash).arg(t1 - 1)
                                                    : QString("Panel: sliding, 256 traces (%1)").arg(panels.size());
            } else {
                const std::vector<int32_t>& key = mode == 1 ? ffid : cdp;
                int32_t first = std::numeric_limits<int32_t>::max(), last = std::numeric_limits<int32_t>::min();
                for (const segy::FkPanel& p : panels) {
                    first = std::min(first, key[size_t(p.start - t0)]);
                    last = std::max(last, key[size_t(p.start - t0)]);
                }
                const QString name = mode == 1 ? "FFID" : "CDP";
                out->panelText = gathersFound == 1 ? QString("Panel: %1 = %2").arg(name).arg(first)
                                                   : QString("Panel: %1 = %2%3%4 (%5 gathers%6)")
                                                         .arg(name)
                                                         .arg(std::min(first, last))
                                                         .arg(dash)
                                                         .arg(std::max(first, last))
                                                         .arg(gathersFound)
                                                         .arg(panels.size() < gathersFound
                                                                  ? QString(", %1 used").arg(panels.size())
                                                                  : QString());
            }
            const QString dname = dmode == 1 ? "offset" : "cdp";
            out->rangeText = QString("Range: %1 = %2%3%4").arg(dname).arg(lo).arg(dash).arg(hi);
            out->rangeText += dmode == 1 ? QString("   scalar %1").arg(formatMetres(offsetScalar))
                                         : QString("   %1 m/cdp").arg(formatMetres(cdpDistance));
            if (out->dxMetres > 0.0) {
                out->rangeText += QString("   %1x %2 m").arg(QChar(0x0394)).arg(formatMetres(out->dxMetres));
            }

            auto fetch = [ds, samplesPerTrace](int64_t traceIdx, float* samples) {
                const uint8_t* traceBase =
                    ds->file.data() + segy::kHeaderTotalSize + size_t(traceIdx) * ds->traceStrideBytes;
                segy::decodeSamples(traceBase + segy::kTraceHeaderSize, samples, size_t(samplesPerTrace),
                                    ds->binHeader.formatCode);
            };
            QString counts;
            if (mode == 3 || averageGathers) {
                // One plot: sliding panels (stack) or all gathers, power-averaged.
                out->fks.push_back(segy::computeFk(fetch, samplesPerTrace, panels, src.s0, src.s1, intervalUs,
                                                   [this](int done, int total) {
                                                       backgroundTaskTotal_ = total;
                                                       backgroundTaskDone_ = done;
                                                   }));
                out->labels.push_back(out->panelText);
                counts = QString("%1 panels averaged").arg(out->fks.front().panelsUsed);
            } else {
                // One plot per gather in view, side by side; at most 24, evenly spread.
                constexpr size_t kMaxShown = 24;
                std::vector<segy::FkPanel> shown;
                if (panels.size() <= kMaxShown) {
                    shown = panels;
                } else {
                    for (size_t i = 0; i < kMaxShown; ++i) shown.push_back(panels[i * (panels.size() - 1) / (kMaxShown - 1)]);
                }
                const std::vector<int32_t>& key = mode == 1 ? ffid : cdp;
                const QString name = mode == 1 ? "FFID" : "CDP";
                backgroundTaskTotal_ = int64_t(shown.size());
                for (size_t i = 0; i < shown.size(); ++i) {
                    out->fks.push_back(segy::computeFk(fetch, samplesPerTrace, {shown[i]}, src.s0, src.s1, intervalUs));
                    out->labels.push_back(QString("%1 = %2").arg(name).arg(key[size_t(shown[i].start - t0)]));
                    backgroundTaskDone_ = int64_t(i) + 1;
                }
                counts = shown.size() < gathersFound
                             ? QString("%1 of %2 gathers shown (evenly spread; zoom in on the seismic for specific ones)")
                                   .arg(shown.size())
                                   .arg(gathersFound)
                             : QString("%1 gather%2").arg(shown.size()).arg(shown.size() == 1 ? "" : "s");
            }
            out->info = QString("%1 | traces %2%3%4, %5%6%7 ms (%8)")
                            .arg(counts)
                            .arg(t0)
                            .arg(dash)
                            .arg(t1 - 1)
                            .arg(src.s0 * intervalUs / 1000.0, 0, 'f', 0)
                            .arg(dash)
                            .arg(src.s1 * intervalUs / 1000.0, 0, 'f', 0)
                            .arg(src.sourceText);
            if (!(out->dxMetres > 0.0)) {
                out->info += QString(" | no %1 step in the headers: wavenumber in cycles/trace").arg(dname);
            }
        },
        [this, sourcePanel, out]() {
            std::vector<FkDisplay> displays;
            for (size_t i = 0; i < out->fks.size(); ++i) {
                const segy::FkResult& fk = out->fks[i];
                if (fk.panelsUsed == 0) continue;
                FkDisplay display;
                display.panelText = out->labels[i];
                display.frequencies = fk.frequencies;
                display.wavenumbers = fk.wavenumbers;
                display.frequencyStepHz = fk.frequencyStepHz;
                display.db = fk.db;
                display.spacingKnown = out->dxMetres > 0.0;
                const double scale = display.spacingKnown ? 1000.0 / out->dxMetres : 1.0; // cycles/trace -> cycles/km
                display.wavenumberMin = -0.5 * scale;
                display.wavenumberStep = scale / display.wavenumbers;
                displays.push_back(std::move(display));
            }
            ensureFkWindow();
            fkWindow_->proposeCdpDistance(out->proposedCdpDistance);
            if (!fkWindow_->cdpDistanceTyped()) fkLastCdpDistance_ = fkWindow_->cdpDistance();
            fkWindow_->setResult(displays, out->rangeText, out->info, out->emptyReason);
            fkWindow_->setWindowTitle(QString("f-k display") + datasetTitleSuffix(sourcePanel->app));
            fkWindow_->show();
            fkWindow_->raise();
            fkWindow_->activateWindow();
            refreshFkRecompute();
        });
}

namespace {
// The inspected trace as a vertical wiggle (time down), positive lobes
// filled, scaled to its own peak -- a quick look beside the sample table.
class TraceWiggleWidget : public QWidget {
public:
    explicit TraceWiggleWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumWidth(150);
        setCursor(Qt::CrossCursor);
    }
    void setSamples(std::vector<float> samples, double sampleIntervalMs) {
        samples_ = std::move(samples);
        sampleIntervalMs_ = sampleIntervalMs;
        if (marker_ >= int(samples_.size())) marker_ = -1;
        update();
    }
    // The sample picked in the table: a red line and dot (-1 = none).
    void setMarker(int sample) {
        marker_ = sample;
        update();
    }
    std::function<void(int sample)> onSampleClicked; // a click on the wiggle, nearest sample

protected:
    QRect plotRect() const { return rect().adjusted(8, 10, -8, -10); }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || samples_.size() < 2) return;
        const QRect plot = plotRect();
        const double frac = (event->position().y() - plot.top()) / std::max(1, plot.height());
        const int sample = std::clamp(int(std::lround(frac * double(samples_.size() - 1))), 0, int(samples_.size()) - 1);
        marker_ = sample;
        update();
        if (onSampleClicked) onSampleClicked(sample);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), tc(QColor(0x1e, 0x1f, 0x22)));
        const QRect plot = plotRect();
        painter.fillRect(plot, Qt::white);
        if (samples_.size() < 2 || plot.height() <= 0) return;
        // Non-finite values (e.g. IBM floats read in the wrong byte order)
        // don't set the scale, and everything is clamped to the plot.
        float peak = 0.0f;
        for (float v : samples_) {
            if (std::isfinite(v)) peak = std::max(peak, std::fabs(v));
        }
        const double centre = plot.center().x();
        const double half = plot.width() / 2.0 - 2.0;
        auto xFor = [&](float v) {
            const double t = peak > 0.0f && std::isfinite(v) ? std::clamp(double(v) / peak, -1.0, 1.0) : 0.0;
            return centre + t * half;
        };
        painter.setClipRect(plot);
        auto yFor = [&](size_t i) { return plot.top() + double(i) / double(samples_.size() - 1) * plot.height(); };
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath fill;
        fill.moveTo(centre, yFor(0));
        for (size_t i = 0; i < samples_.size(); ++i) fill.lineTo(std::max(centre, xFor(samples_[i])), yFor(i));
        fill.lineTo(centre, yFor(samples_.size() - 1));
        painter.fillPath(fill, Qt::black);
        QPolygonF line;
        for (size_t i = 0; i < samples_.size(); ++i) line << QPointF(xFor(samples_[i]), yFor(i));
        painter.setPen(QPen(Qt::black, 1.0));
        painter.drawPolyline(line);
        painter.setPen(QPen(tc(QColor(0x9a, 0x9c, 0xa3)), 1, Qt::DashLine));
        painter.drawLine(QPointF(centre, plot.top()), QPointF(centre, plot.bottom()));
        if (marker_ >= 0 && marker_ < int(samples_.size())) {
            const double y = yFor(size_t(marker_));
            painter.setPen(QPen(QColor(0xe0, 0x2b, 0x2b), 1.5));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setBrush(QColor(0xe0, 0x2b, 0x2b));
            painter.drawEllipse(QPointF(xFor(samples_[size_t(marker_)]), y), 4.0, 4.0);
            painter.setPen(QColor(0xe0, 0x2b, 0x2b));
            painter.drawText(QRectF(plot.left() + 4, y - 18, plot.width() - 8, 16), Qt::AlignRight | Qt::AlignVCenter,
                             QString("%1 ms").arg(marker_ * sampleIntervalMs_, 0, 'f', 1));
        }
    }

private:
    std::vector<float> samples_;
    double sampleIntervalMs_ = 0.0;
    int marker_ = -1;
};
} // namespace

TraceInspectorWindow::TraceInspectorWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setWindowTitle("Trace Inspector");
    resize(1100, 760);
    setStyleSheet(themed("background-color: #1e1f22;"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* top = new QHBoxLayout();
    top->setSpacing(10);
    top->addWidget(new QLabel("Trace"));
    traceSpin_ = new QSpinBox();
    traceSpin_->setRange(1, 1);
    traceSpin_->setToolTip("Trace number in the file (1 = first trace)");
    top->addWidget(traceSpin_);
    followCheck_ = new QCheckBox("Follow cursor");
    followCheck_->setChecked(true);
    followCheck_->setToolTip("Show the trace under the mouse on the seismic display");
    top->addWidget(followCheck_);
    top->addSpacing(16);
    top->addWidget(new QLabel("Definition"));
    standardCombo_ = new QComboBox();
    for (segy::HeaderStandard s : {segy::HeaderStandard::ShellV21, segy::HeaderStandard::SegyRev1,
                                   segy::HeaderStandard::SegyRev2}) {
        standardCombo_->addItem(segy::headerStandardName(s), int(s));
    }
    standardCombo_->setToolTip("Trace header definition: Shell V2.1 (default), SEG-Y rev 1.0 or rev 2.0");
    top->addWidget(standardCombo_);
    top->addSpacing(16);
    top->addWidget(new QLabel("Byte order"));
    bigEndianRadio_ = new QRadioButton("Big endian");
    QRadioButton* littleEndian = new QRadioButton("Little endian");
    bigEndianRadio_->setChecked(true);
    bigEndianRadio_->setToolTip("Byte order for the header values and the samples");
    QButtonGroup* endianGroup = new QButtonGroup(this);
    endianGroup->addButton(bigEndianRadio_);
    endianGroup->addButton(littleEndian);
    top->addWidget(bigEndianRadio_);
    top->addWidget(littleEndian);
    top->addStretch();
    layout->addLayout(top);
    summaryLabel_ = new QLabel();
    summaryLabel_->setStyleSheet(themed("color: #9a9ca3;"));
    layout->addWidget(summaryLabel_);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    headerTable_ = new QTableWidget();
    headerTable_->setColumnCount(4);
    headerTable_->setHorizontalHeaderLabels({"Name", "Bytes", "Value", "Description"});
    headerTable_->verticalHeader()->hide();
    headerTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    headerTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    headerTable_->horizontalHeader()->setStretchLastSection(true);
    headerTable_->verticalHeader()->setDefaultSectionSize(20);
    samplesTable_ = new QTableWidget();
    samplesTable_->setColumnCount(3);
    samplesTable_->setHorizontalHeaderLabels({"#", "Time (ms)", "Value"});
    samplesTable_->verticalHeader()->hide();
    samplesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    samplesTable_->horizontalHeader()->setStretchLastSection(true);
    samplesTable_->verticalHeader()->setDefaultSectionSize(20);
    samplesTable_->setMinimumWidth(300); // room for the value column beside the scroll bar
    wiggle_ = new TraceWiggleWidget();
    splitter->addWidget(headerTable_);
    splitter->addWidget(samplesTable_);
    splitter->addWidget(wiggle_);
    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 1);
    layout->addWidget(splitter, 1);

    connect(traceSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        trace_ = int64_t(value) - 1;
        refresh();
    });
    connect(standardCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refresh(); });
    // Click on the wiggle: the sample table jumps there. Pick a sample in
    // the table: a red line and dot on the wiggle.
    auto* wiggle = static_cast<TraceWiggleWidget*>(wiggle_);
    wiggle->onSampleClicked = [this](int sample) {
        if (sample >= samplesTable_->rowCount()) return;
        QSignalBlocker block(samplesTable_);
        samplesTable_->selectRow(sample);
        samplesTable_->scrollToItem(samplesTable_->item(sample, 0), QAbstractItemView::PositionAtCenter);
    };
    connect(samplesTable_, &QTableWidget::currentCellChanged, this,
            [wiggle](int row, int, int, int) { wiggle->setMarker(row); });
    connect(bigEndianRadio_, &QRadioButton::toggled, this, [this](bool) { refresh(); });
}

bool TraceInspectorWindow::followCursor() const { return followCheck_->isChecked(); }

void TraceInspectorWindow::setDataset(std::shared_ptr<Dataset> dataset, const QString& titleSuffix) {
    dataset_ = std::move(dataset);
    setWindowTitle(QString("Trace Inspector") + titleSuffix);
    const int count = dataset_ ? int(std::min<int64_t>(dataset_->traceCount, std::numeric_limits<int>::max())) : 1;
    QSignalBlocker block(traceSpin_);
    traceSpin_->setRange(1, std::max(1, count));
    trace_ = std::clamp<int64_t>(trace_, 0, std::max(0, count - 1));
    traceSpin_->setValue(int(trace_) + 1);
    refresh();
}

void TraceInspectorWindow::showTrace(int64_t traceIndex) {
    if (!dataset_ || traceIndex == trace_) return;
    trace_ = std::clamp<int64_t>(traceIndex, 0, std::max<int64_t>(0, dataset_->traceCount - 1));
    QSignalBlocker block(traceSpin_);
    traceSpin_->setValue(int(trace_) + 1);
    refresh();
}

void TraceInspectorWindow::refresh() {
    if (!dataset_ || dataset_->traceCount <= 0) {
        headerTable_->setRowCount(0);
        samplesTable_->setRowCount(0);
        summaryLabel_->setText("No data");
        return;
    }
    const uint8_t* traceBase = dataset_->file.data() + segy::kHeaderTotalSize + size_t(trace_) * dataset_->traceStrideBytes;
    const bool bigEndian = bigEndianRadio_->isChecked();
    const auto standard = segy::HeaderStandard(standardCombo_->currentData().toInt());
    const std::vector<segy::HeaderFieldDef>& fields = segy::traceHeaderDefinition(standard);
    headerTable_->setRowCount(int(fields.size()));
    for (int row = 0; row < int(fields.size()); ++row) {
        const segy::HeaderFieldDef& f = fields[size_t(row)];
        const bool unused = f.kind == segy::HeaderFieldKind::Unassigned;
        const QString bytes = f.size == 1 ? QString::number(f.firstByte)
                                          : QString("%1-%2").arg(f.firstByte).arg(f.firstByte + f.size - 1);
        const QStringList cells = {f.name, bytes, QString::fromStdString(segy::formatHeaderField(traceBase, f, bigEndian)),
                                   f.description};
        for (int col = 0; col < 4; ++col) {
            QTableWidgetItem* item = new QTableWidgetItem(cells[col]);
            if (col == 1 || col == 2) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (unused) item->setForeground(tc(QColor(0x7a, 0x7c, 0x82)));
            if (col == 3) item->setToolTip(f.description);
            headerTable_->setItem(row, col, item);
        }
    }
    headerTable_->resizeColumnToContents(0);
    headerTable_->resizeColumnToContents(1);
    headerTable_->resizeColumnToContents(2);

    const int samples = dataset_->binHeader.samplesPerTrace;
    const double intervalMs = dataset_->binHeader.sampleIntervalUs / 1000.0;
    std::vector<float> values(static_cast<size_t>(std::max(0, samples)));
    if (samples > 0) {
        const uint8_t* sampleBytes = traceBase + segy::kTraceHeaderSize;
        // Little endian: reverse every sample word before decoding (the
        // decoder reads SEG-Y's big-endian order) -- on a big-endian file
        // this gives the nonsense values that show the wrong byte order.
        std::vector<uint8_t> swapped;
        const int wordSize = segy::sampleFormatSizeBytes(dataset_->binHeader.formatCode);
        if (!bigEndian && wordSize > 1) {
            swapped.assign(sampleBytes, sampleBytes + size_t(samples) * size_t(wordSize));
            for (size_t i = 0; i + size_t(wordSize) <= swapped.size(); i += size_t(wordSize)) {
                std::reverse(swapped.begin() + std::ptrdiff_t(i), swapped.begin() + std::ptrdiff_t(i + size_t(wordSize)));
            }
            sampleBytes = swapped.data();
        }
        segy::decodeSamples(sampleBytes, values.data(), size_t(samples), dataset_->binHeader.formatCode);
    }
    samplesTable_->setRowCount(samples);
    for (int i = 0; i < samples; ++i) {
        const QString cells[3] = {QString::number(i), QString::number(i * intervalMs, 'f', 1),
                                  QString::number(double(values[size_t(i)]), 'g', 6)};
        for (int col = 0; col < 3; ++col) {
            QTableWidgetItem* item = new QTableWidgetItem(cells[col]);
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            samplesTable_->setItem(i, col, item);
        }
    }
    // Wide enough for the largest sample number, not just the rows in view.
    samplesTable_->setColumnWidth(0, samplesTable_->fontMetrics().horizontalAdvance(QString::number(std::max(1, samples))) + 24);
    samplesTable_->resizeColumnToContents(1);
    static_cast<TraceWiggleWidget*>(wiggle_)->setSamples(values, intervalMs);
    (void)standard; // shown in the Definition box already
    summaryLabel_->setText(QString::fromUtf8("Trace %1 of %2   \xc2\xb7   %3 samples every %4 ms   \xc2\xb7   format %5")
                               .arg(trace_ + 1)
                               .arg(dataset_->traceCount)
                               .arg(samples)
                               .arg(dataset_->binHeader.sampleIntervalUs / 1000.0)
                               .arg(dataset_->binHeader.formatCode));
}

void MainWindow::openTraceInspector() {
    if (!app_->loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    if (!traceInspector_) traceInspector_ = new TraceInspectorWindow(this);
    if (traceInspector_->dataset() != app_->foreground.get()) {
        traceInspector_->setDataset(app_->foreground, datasetTitleSuffix(*app_));
    }
    traceInspector_->show();
    traceInspector_->raise();
    traceInspector_->activateWindow();
}

namespace {
// One row of File > Trace Header Ranges. `select` is the segy::IdentField a
// row can subset and sort on (-1: display only).
struct HeaderRangeRow {
    QString name, bytes;
    std::function<int64_t(const segy::TraceHeader&)> value;
    int select = -1;
    int64_t min = std::numeric_limits<int64_t>::max(), max = std::numeric_limits<int64_t>::min();
    int64_t changes = 0;
    std::map<int64_t, int64_t> stepCounts;
};

std::vector<HeaderRangeRow> makeHeaderRangeRows() {
    std::vector<HeaderRangeRow> rows;
    auto add = [&rows](const char* name, const char* bytes, int select,
                       std::function<int64_t(const segy::TraceHeader&)> value) {
        HeaderRangeRow row;
        row.name = name;
        row.bytes = bytes;
        row.select = select;
        row.value = std::move(value);
        rows.push_back(std::move(row));
    };
    using F = segy::IdentField;
    add("Seq (Line)", "1-4", int(F::TraceSequenceLine), [](const segy::TraceHeader& t) { return int64_t(t.traceSequenceLine); });
    add("Seq (File)", "5-8", int(F::TraceSequenceFile), [](const segy::TraceHeader& t) { return int64_t(t.traceSequenceFile); });
    add("Field Record (FFID)", "9-12", int(F::FieldRecord), [](const segy::TraceHeader& t) { return int64_t(t.fieldRecord); });
    add("Trace Number", "13-16", int(F::TraceNumber), [](const segy::TraceHeader& t) { return int64_t(t.traceNumber); });
    add("CDP", "21-24", int(F::Cdp), [](const segy::TraceHeader& t) { return int64_t(t.cdp); });
    add("Offset", "37-40", int(F::Offset), [](const segy::TraceHeader& t) { return int64_t(t.offset); });
    add("Coordinate scalar", "71-72", -1, [](const segy::TraceHeader& t) { return int64_t(t.coordinateScalar); });
    add("Source X", "73-76", int(F::X), [](const segy::TraceHeader& t) { return int64_t(t.x); });
    add("Source Y", "77-80", int(F::Y), [](const segy::TraceHeader& t) { return int64_t(t.y); });
    add("CDP X", "181-184", -1, [](const segy::TraceHeader& t) { return int64_t(t.cdpX); });
    add("CDP Y", "185-188", -1, [](const segy::TraceHeader& t) { return int64_t(t.cdpY); });
    return rows;
}
} // namespace

void MainWindow::openTraceHeaderRanges() {
    if (!app_->loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    // Every trace's header, scanned in the background: per field the byte
    // range, min, max, how often the value changes from trace to trace, and
    // the most common nonzero step.
    auto rows = std::make_shared<std::vector<HeaderRangeRow>>(makeHeaderRangeRows());
    std::shared_ptr<Dataset> ds = app_->foreground;
    const QString title = QString("Trace Header Ranges") + datasetTitleSuffix(*app_);
    runBackgroundTask(
        "Scanning trace headers",
        [this, ds, rows]() {
            backgroundTaskTotal_ = std::max<int64_t>(1, ds->traceCount);
            // Reading the headers is the slow part (one page per trace on a
            // file larger than the cache): in parallel, into columns; the
            // statistics then run over memory.
            const size_t rowCount = rows->size();
            std::vector<int64_t> values(size_t(ds->traceCount) * rowCount);
            {
                segy::ThreadPool pool;
                std::atomic<int64_t> done{0};
                pool.parallelFor(size_t(ds->traceCount), 2048, [&](size_t begin, size_t end) {
                    for (size_t t = begin; t < end; ++t) {
                        const segy::TraceHeader th =
                            segy::parseTraceHeader(ds->file.data() + segy::kHeaderTotalSize + t * ds->traceStrideBytes);
                        for (size_t r = 0; r < rowCount; ++r) values[t * rowCount + r] = (*rows)[r].value(th);
                    }
                    backgroundTaskDone_ = done.fetch_add(int64_t(end - begin)) + int64_t(end - begin);
                });
            }
            std::vector<int64_t> previous(rows->size(), 0);
            for (int64_t t = 0; t < ds->traceCount; ++t) {
                for (size_t r = 0; r < rowCount; ++r) {
                    HeaderRangeRow& row = (*rows)[r];
                    const int64_t v = values[size_t(t) * rowCount + r];
                    row.min = std::min(row.min, v);
                    row.max = std::max(row.max, v);
                    if (t > 0 && v != previous[r]) {
                        ++row.changes;
                        if (row.stepCounts.size() < 4096) ++row.stepCounts[v - previous[r]];
                    }
                    previous[r] = v;
                }
            }
            backgroundTaskDone_ = ds->traceCount;
        },
        [this, rows, title, ds]() {
            headerRangesDataset_ = ds;
            if (!headerRangesWidget_) {
                headerRangesWidget_ = new QWidget(this, Qt::Window);
                headerRangesWidget_->setStyleSheet(themed("background-color: #1e1f22;"));
                QVBoxLayout* layout = new QVBoxLayout(headerRangesWidget_);
                QTableWidget* table = new QTableWidget(headerRangesWidget_);
                table->setObjectName("rangesTable");
                table->setColumnCount(7);
                table->setHorizontalHeaderLabels({"Ident", "Bytes", "Min", "Max", "Inc", "Changes", "Typical step"});
                table->horizontalHeaderItem(4)->setToolTip(
                    "Increment: keep every value min, min + inc, min + 2 inc, ... (1 = all)");
                // The in-cell editor in the dark style (it was white on light grey).
                table->setStyleSheet(themed("QTableWidget QLineEdit { background: #2b2d30; color: #f0f2f5; "
                                     "border: 1px solid #e8905a; selection-background-color: #4a3626; }"));
                table->verticalHeader()->hide();
                table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                       QAbstractItemView::AnyKeyPressed);
                table->horizontalHeader()->setStretchLastSection(true);
                layout->addWidget(table);
                QLabel* hint = new QLabel("Edit Min/Max/Inc of an ident (double-click) to select a subset; changed "
                                          "cells turn orange. Inc 2 keeps every second value from Min.");
                hint->setStyleSheet(themed("color: #9a9ca3; font-size: 11px;"));
                layout->addWidget(hint);
                // Up to three sort keys, each ascending or descending.
                QGridLayout* sortGrid = new QGridLayout();
                sortGrid->setHorizontalSpacing(10);
                for (int k = 0; k < 3; ++k) {
                    sortGrid->addWidget(new QLabel(k == 0 ? "Sort by" : "then by"), k, 0);
                    QComboBox* key = new QComboBox();
                    key->setObjectName(QString("sortKey%1").arg(k));
                    key->addItem("(none)", -1);
                    for (const HeaderRangeRow& row : makeHeaderRangeRows()) {
                        if (row.select >= 0) key->addItem(row.name, row.select);
                    }
                    QCheckBox* descending = new QCheckBox("Descending");
                    descending->setObjectName(QString("sortDesc%1").arg(k));
                    sortGrid->addWidget(key, k, 1);
                    sortGrid->addWidget(descending, k, 2);
                }
                sortGrid->setColumnStretch(3, 1);
                layout->addLayout(sortGrid);
                QHBoxLayout* applyRow = new QHBoxLayout();
                applyRow->addWidget(new QLabel("Output name"));
                QLineEdit* name = new QLineEdit();
                name->setObjectName("subsetName");
                applyRow->addWidget(name, 1);
                QPushButton* apply = new QPushButton("Apply subset / sort");
                apply->setToolTip("The selected traces, in the chosen order, as a new dataset in the active panel");
                applyRow->addWidget(apply);
                layout->addLayout(applyRow);
                connect(apply, &QPushButton::clicked, this, [this, table]() {
                    if (!headerRangesDataset_) return;
                    std::vector<segy::IdentCriterion> criteria;
                    QStringList selectText, sortText;
                    const std::vector<HeaderRangeRow> defs = makeHeaderRangeRows();
                    for (int r = 0; r < table->rowCount() && r < int(defs.size()); ++r) {
                        if (defs[size_t(r)].select < 0) continue;
                        QTableWidgetItem* minItem = table->item(r, 2);
                        QTableWidgetItem* maxItem = table->item(r, 3);
                        QTableWidgetItem* incItem = table->item(r, 4);
                        if (!minItem || !maxItem || !incItem) continue;
                        bool okMin = false, okMax = false, okInc = false;
                        const qlonglong lo = minItem->text().trimmed().toLongLong(&okMin);
                        const qlonglong hi = maxItem->text().trimmed().toLongLong(&okMax);
                        const qlonglong inc = incItem->text().trimmed().toLongLong(&okInc);
                        if (!okMin || !okMax || !okInc || inc < 1) {
                            flashStatusMessage(
                                QString("%1: Min/Max must be whole numbers, Inc 1 or more").arg(defs[size_t(r)].name));
                            return;
                        }
                        const qlonglong fullLo = minItem->data(Qt::UserRole).toLongLong();
                        const qlonglong fullHi = maxItem->data(Qt::UserRole).toLongLong();
                        if (lo == fullLo && hi == fullHi && inc == 1) continue; // untouched: no criterion
                        segy::IdentCriterion criterion;
                        criterion.field = segy::IdentField(defs[size_t(r)].select);
                        criterion.min = int32_t(std::clamp<qlonglong>(std::min(lo, hi), INT32_MIN, INT32_MAX));
                        criterion.max = int32_t(std::clamp<qlonglong>(std::max(lo, hi), INT32_MIN, INT32_MAX));
                        criterion.inc = int32_t(std::min<qlonglong>(inc, INT32_MAX));
                        criteria.push_back(criterion);
                        selectText << QString("%1 %2-%3%4")
                                          .arg(defs[size_t(r)].name.toUpper())
                                          .arg(criterion.min)
                                          .arg(criterion.max)
                                          .arg(criterion.inc > 1 ? QString(" INC %1").arg(criterion.inc) : QString());
                    }
                    std::vector<segy::SortKey> sortKeys;
                    for (int k = 0; k < 3; ++k) {
                        auto* key = headerRangesWidget_->findChild<QComboBox*>(QString("sortKey%1").arg(k));
                        auto* desc = headerRangesWidget_->findChild<QCheckBox*>(QString("sortDesc%1").arg(k));
                        if (!key || key->currentData().toInt() < 0) continue;
                        sortKeys.push_back({segy::IdentField(key->currentData().toInt()), desc && desc->isChecked()});
                        sortText << QString("%1%2").arg(key->currentText().toUpper(), desc && desc->isChecked() ? "-" : "+");
                    }
                    if (criteria.empty() && sortKeys.empty()) {
                        flashStatusMessage("Change a Min/Max or choose a sort key first");
                        return;
                    }
                    auto* nameEdit = headerRangesWidget_->findChild<QLineEdit*>("subsetName");
                    QString outputName = nameEdit ? nameEdit->text().trimmed() : QString();
                    if (outputName.isEmpty()) outputName = QString::fromStdString(headerRangesDataset_->name) + "_sel";
                    QString history;
                    if (!selectText.isEmpty()) history += "SELECT " + selectText.join(", ");
                    if (!sortText.isEmpty()) history += QString(history.isEmpty() ? "" : "; ") + "SORT " + sortText.join(", ");
                    runSubsetSort(activePanel_, headerRangesDataset_, criteria, sortKeys, outputName, history);
                });
                connect(table, &QTableWidget::itemChanged, this, [](QTableWidgetItem* item) {
                    if (item->column() < 2 || item->column() > 4) return;
                    const bool changed = item->text().trimmed() != QString::number(item->data(Qt::UserRole).toLongLong());
                    item->setBackground(changed ? tc(QColor(0x4a, 0x36, 0x26)) : QColor(Qt::transparent));
                    item->setForeground(changed ? tc(QColor(0xe8, 0x90, 0x5a)) : tc(QColor(0xd4, 0xd4, 0xd8)));
                });
                headerRangesWidget_->resize(820, 560);
            }
            QTableWidget* table = headerRangesWidget_->findChild<QTableWidget*>("rangesTable");
            QSignalBlocker block(table);
            table->setRowCount(int(rows->size()));
            for (int r = 0; r < int(rows->size()); ++r) {
                const HeaderRangeRow& row = (*rows)[size_t(r)];
                int64_t step = 0, best = 0;
                for (const auto& [s, count] : row.stepCounts) {
                    if (count > best) {
                        best = count;
                        step = s;
                    }
                }
                const bool constant = row.changes == 0;
                const QStringList cells = {row.name,
                                           row.bytes,
                                           QString::number(row.min),
                                           QString::number(row.max),
                                           row.select >= 0 ? QString("1") : QString(),
                                           constant ? QString("constant") : QString::number(row.changes),
                                           constant ? QString(QChar(0x2013)) : QString::number(step)};
                for (int col = 0; col < cells.size(); ++col) {
                    QTableWidgetItem* item = new QTableWidgetItem(cells[col]);
                    if (col >= 2) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    // Only Min/Max/Inc of selectable idents can be edited.
                    const bool editable = col >= 2 && col <= 4 && row.select >= 0;
                    if (!editable) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                    if (col == 2) item->setData(Qt::UserRole, qlonglong(row.min));
                    if (col == 3) item->setData(Qt::UserRole, qlonglong(row.max));
                    if (col == 4) item->setData(Qt::UserRole, qlonglong(1));
                    if (constant) item->setForeground(tc(QColor(0x7a, 0x7c, 0x82)));
                    table->setItem(r, col, item);
                }
            }
            table->resizeColumnsToContents();
            if (auto* nameEdit = headerRangesWidget_->findChild<QLineEdit*>("subsetName")) {
                nameEdit->setText(QString::fromStdString(ds->name) + "_sel");
            }
            headerRangesWidget_->setWindowTitle(title + QString("  (%1 traces)").arg(ds->traceCount));
            headerRangesWidget_->show();
            headerRangesWidget_->raise();
            headerRangesWidget_->activateWindow();
        });
}

void MainWindow::runSubsetSort(Panel* panel, std::shared_ptr<Dataset> dataset, std::vector<segy::IdentCriterion> criteria,
                               std::vector<segy::SortKey> sortKeys, const QString& outputName,
                               const QString& historyLine) {
    if (!dataset || dataset->traceCount <= 0) return;
    const int samplesPerTrace = dataset->binHeader.samplesPerTrace;
    const QString sourceName = QString::fromStdString(dataset->name);
    std::vector<std::string> history = dataset->processingHistory;
    history.push_back(historyLine.toStdString());
    const std::filesystem::path outPath = scratchSegyPath(outputName);
    struct SubsetOutcome {
        bool ok = false;
        std::string error;
        int64_t traces = 0;
    };
    auto outcome = std::make_shared<SubsetOutcome>();
    runBackgroundTask(
        QString("Subset/sort: %1").arg(sourceName),
        [this, dataset, criteria, sortKeys, samplesPerTrace, outputName, history, outPath, sourceName, outcome]() {
            segy::ThreadPool pool;
            const segy::TraceHeaderColumns columns =
                segy::scanTraceHeaders(dataset->file.data(), dataset->traceCount, dataset->traceStrideBytes, pool);
            std::vector<int64_t> order = segy::selectTraces(columns, criteria);
            segy::sortTraceIndices(order, columns, sortKeys);
            outcome->traces = int64_t(order.size());
            if (order.empty()) {
                outcome->error = "No traces match the selection.";
                return;
            }
            backgroundTaskTotal_ = int64_t(order.size());
            QStringList inputBlocks;
            inputBlocks << QString("FILE: %1  TRACES: %2 OF %3  SAMPLES/TRACE: %4  SAMPLE INTERVAL: %5 US")
                               .arg(sourceName)
                               .arg(order.size())
                               .arg(dataset->traceCount)
                               .arg(samplesPerTrace)
                               .arg(dataset->binHeader.sampleIntervalUs);
            const QString textHeader = buildSegyTextHeader(inputBlocks, history, outputName, int64_t(order.size()),
                                                           samplesPerTrace, dataset->binHeader.sampleIntervalUs);
            outcome->ok = streamTraces(
                outPath, textHeader.toStdString(), dataset->binHeader, int64_t(order.size()),
                [&](int64_t i, segy::WriteTrace& out) {
                    const uint8_t* traceBase =
                        dataset->file.data() + segy::kHeaderTotalSize + size_t(order[size_t(i)]) * dataset->traceStrideBytes;
                    out.headerBytes.assign(traceBase, traceBase + segy::kTraceHeaderSize);
                    out.samples.resize(size_t(samplesPerTrace));
                    segy::decodeSamples(traceBase + segy::kTraceHeaderSize, out.samples.data(), size_t(samplesPerTrace),
                                        dataset->binHeader.formatCode);
                },
                backgroundTaskDone_, &outcome->error);
        },
        [this, panel, outcome, outPath, history]() {
            if (!outcome->ok) {
                QMessageBox::warning(this, "Subset / sort", QString::fromStdString(outcome->error));
                return;
            }
            flashStatusMessage(QString("%1 traces selected").arg(outcome->traces));
            panel->canvas->startLoading(outPath, history);
        });
}

namespace {
const QColor kHorizonColors[] = {tc(QColor(0xe8, 0x90, 0x5a)), QColor(0x19, 0x9e, 0x70), QColor(0xd5, 0x51, 0x81),
                                 QColor(0xc9, 0x85, 0x00), QColor(0x90, 0x85, 0xe9), QColor(0x39, 0x87, 0xe5)};

segy::HorizonTraceFetch fetchFor(const std::shared_ptr<Dataset>& ds) {
    return [ds](int64_t traceIdx, float* out) {
        const uint8_t* traceBase = ds->file.data() + segy::kHeaderTotalSize + size_t(traceIdx) * ds->traceStrideBytes;
        segy::decodeSamples(traceBase + segy::kTraceHeaderSize, out, size_t(ds->binHeader.samplesPerTrace),
                            ds->binHeader.formatCode);
    };
}
} // namespace

Horizon* MainWindow::activeHorizonFor(Panel* panel, bool create) {
    if (!panel->app.loaded) return nullptr;
    Dataset& ds = *panel->app.foreground;
    if (ds.activeHorizon >= 0 && ds.activeHorizon < int(ds.horizons.size())) return &ds.horizons[size_t(ds.activeHorizon)];
    if (!create) return nullptr;
    Horizon hz;
    hz.name = QString("Horizon %1").arg(ds.horizons.size() + 1);
    hz.color = kHorizonColors[ds.horizons.size() % std::size(kHorizonColors)];
    hz.timeMs.assign(size_t(ds.traceCount), std::numeric_limits<float>::quiet_NaN());
    ds.horizons.push_back(std::move(hz));
    ds.activeHorizon = int(ds.horizons.size()) - 1;
    return &ds.horizons.back();
}

void MainWindow::handlePick(Panel* panel, double traceCoord, double sampleCoord, bool remove) {
    if (!panel->app.loaded || panel->app.xf.active) return;
    std::shared_ptr<Dataset> ds = panel->app.foreground;
    Horizon* hz = activeHorizonFor(panel, !remove);
    if (!hz) return;
    const double sampleMs = ds->binHeader.sampleIntervalUs / 1000.0;
    const int64_t trace = std::clamp<int64_t>(int64_t(std::floor(traceCoord)), 0, ds->traceCount - 1);
    const int searchSamples = std::max(1, int(std::lround(pickSearchMs_ / sampleMs)));
    const int windowSamples = std::max(4, int(std::lround(pickWindowMs_ / sampleMs)));
    const segy::HorizonTraceFetch fetch = fetchFor(ds);
    if (remove) {
        // The nearest anchor goes; the horizon is rebuilt along the
        // remaining anchors (at their own picked times).
        if (hz->anchors.empty()) return;
        auto nearest = std::min_element(hz->anchors.begin(), hz->anchors.end(), [trace](int64_t a, int64_t b) {
            return std::llabs(a - trace) < std::llabs(b - trace);
        });
        hz->anchors.erase(nearest);
        std::vector<std::pair<int64_t, double>> keep;
        for (int64_t a : hz->anchors) {
            if (std::isfinite(hz->timeMs[size_t(a)])) keep.push_back({a, hz->timeMs[size_t(a)] / sampleMs});
        }
        std::fill(hz->timeMs.begin(), hz->timeMs.end(), std::numeric_limits<float>::quiet_NaN());
        for (size_t k = 0; k < keep.size(); ++k) {
            const auto [from, fromSample] = keep[k];
            const auto [to, toSample] = k + 1 < keep.size() ? keep[k + 1] : keep[k];
            std::vector<double> picks = segy::pickBetween(fetch, ds->binHeader.samplesPerTrace, from, fromSample, to,
                                                          toSample, searchSamples, windowSamples, pickTrust_, pickMode_);
            const int64_t dir = to >= from ? 1 : -1;
            for (size_t i = 0; i < picks.size(); ++i) hz->timeMs[size_t(from + dir * int64_t(i))] = float(picks[i] * sampleMs);
        }
        panel->canvas->update();
        refreshHorizonManager();
        return;
    }
    if (hz->anchors.empty()) {
        std::vector<double> single =
            segy::pickBetween(fetch, ds->binHeader.samplesPerTrace, trace, sampleCoord, trace, sampleCoord, searchSamples,
                              windowSamples, pickTrust_, pickMode_);
        hz->timeMs[size_t(trace)] = float(single[0] * sampleMs);
    } else {
        // Track from the last anchor (at its picked time) to this click.
        const int64_t from = hz->anchors.back();
        const double fromSample = std::isfinite(hz->timeMs[size_t(from)]) ? hz->timeMs[size_t(from)] / sampleMs : sampleCoord;
        std::vector<double> picks = segy::pickBetween(fetch, ds->binHeader.samplesPerTrace, from, fromSample, trace,
                                                      sampleCoord, searchSamples, windowSamples, pickTrust_, pickMode_);
        const int64_t dir = trace >= from ? 1 : -1;
        for (size_t i = 0; i < picks.size(); ++i) hz->timeMs[size_t(from + dir * int64_t(i))] = float(picks[i] * sampleMs);
    }
    hz->anchors.push_back(trace);
    panel->canvas->update();
    refreshHorizonManager();
}

void MainWindow::autoPickSeabed(Panel* panel) {
    if (!panel->app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    if (panel->app.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
    std::shared_ptr<Dataset> ds = panel->app.foreground;
    const double threshold = seabedThresholdPercent_ / 100.0;
    const segy::PickMode mode = pickMode_;
    auto picks = std::make_shared<std::vector<double>>();
    runBackgroundTask(
        "Auto-picking the seabed",
        [this, ds, threshold, mode, picks]() {
            backgroundTaskTotal_ = 1;
            *picks = segy::autoPickSeabed(fetchFor(ds), 0, ds->traceCount, ds->binHeader.samplesPerTrace, threshold, mode);
            backgroundTaskDone_ = 1;
        },
        [this, panel, ds, picks]() {
            if (panel->app.foreground != ds) return;
            Horizon hz;
            hz.name = "Seabed";
            hz.color = QColor(0x39, 0x87, 0xe5);
            const double sampleMs = ds->binHeader.sampleIntervalUs / 1000.0;
            hz.timeMs.resize(picks->size());
            for (size_t i = 0; i < picks->size(); ++i) hz.timeMs[i] = float((*picks)[i] * sampleMs);
            ds->horizons.push_back(std::move(hz));
            ds->activeHorizon = int(ds->horizons.size()) - 1;
            panel->canvas->update();
            refreshHorizonManager();
            flashStatusMessage("Seabed picked: horizon \"Seabed\" (edit it with the Pick tool)");
        });
}

void MainWindow::exportHorizon(Panel* panel) {
    Horizon* hz = activeHorizonFor(panel, false);
    if (!hz) {
        flashStatusMessage("No horizon to export");
        return;
    }
    std::shared_ptr<Dataset> ds = panel->app.foreground;
    const QString path = QFileDialog::getSaveFileName(this, "Export Horizon", hz->name + ".csv", "CSV (*.csv)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Horizon", "Could not write " + path);
        return;
    }
    // One line per picked trace: trace number (1-based), CDP, FFID, offset, time.
    QTextStream out(&file);
    out << "trace,cdp,ffid,offset,time_ms\n";
    int64_t written = 0;
    for (int64_t t = 0; t < int64_t(hz->timeMs.size()); ++t) {
        if (!std::isfinite(hz->timeMs[size_t(t)])) continue;
        const segy::TraceHeader th = segy::parseTraceHeader(ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes);
        out << (t + 1) << ',' << th.cdp << ',' << th.fieldRecord << ',' << th.offset << ','
            << QString::number(hz->timeMs[size_t(t)], 'f', 2) << '\n';
        ++written;
    }
    flashStatusMessage(QString("%1 picks exported to %2").arg(written).arg(QFileInfo(path).fileName()));
}

void MainWindow::importHorizon(Panel* panel) {
    if (!panel->app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    std::shared_ptr<Dataset> ds = panel->app.foreground;
    const QString path = QFileDialog::getOpenFileName(this, "Import Horizon", QString(), "CSV (*.csv);;All files (*)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Import Horizon", "Could not read " + path);
        return;
    }
    // The exported layout: a header row naming the columns; "trace" (1-based)
    // and "time_ms" are what's needed.
    QTextStream in(&file);
    const QStringList header = in.readLine().split(',');
    const int traceColumn = header.indexOf("trace"), timeColumn = header.indexOf("time_ms");
    if (traceColumn < 0 || timeColumn < 0) {
        QMessageBox::warning(this, "Import Horizon", "Expected a header row with \"trace\" and \"time_ms\" columns.");
        return;
    }
    Horizon hz;
    hz.name = QFileInfo(path).completeBaseName();
    hz.color = kHorizonColors[ds->horizons.size() % std::size(kHorizonColors)];
    hz.timeMs.assign(size_t(ds->traceCount), std::numeric_limits<float>::quiet_NaN());
    int64_t read = 0;
    while (!in.atEnd()) {
        const QStringList fields = in.readLine().split(',');
        if (fields.size() <= std::max(traceColumn, timeColumn)) continue;
        bool okTrace = false, okTime = false;
        const qlonglong trace = fields[traceColumn].toLongLong(&okTrace);
        const double ms = fields[timeColumn].toDouble(&okTime);
        if (!okTrace || !okTime || trace < 1 || trace > ds->traceCount) continue;
        hz.timeMs[size_t(trace - 1)] = float(ms);
        ++read;
    }
    ds->horizons.push_back(std::move(hz));
    ds->activeHorizon = int(ds->horizons.size()) - 1;
    panel->canvas->update();
    refreshHorizonManager();
    flashStatusMessage(QString("%1 picks imported").arg(read));
}

namespace {
// A time per trace from a trace-header integer (`msPerUnit` ms per count);
// 0 counts as "no value" (NaN). The raw values are read once per field, in
// parallel (on a 6 GB file the serial read was an 18 s freeze), and kept
// on the dataset.
std::vector<double> headerTimesMs(Dataset& ds, int firstByte, int size, double msPerUnit) {
    std::vector<int32_t>& raw = ds.headerFieldCache[{firstByte, size}];
    if (raw.size() != size_t(ds.traceCount)) {
        raw.resize(size_t(ds.traceCount));
        segy::ThreadPool pool;
        pool.parallelFor(size_t(ds.traceCount), 1024, [&](size_t begin, size_t end) {
            for (size_t t = begin; t < end; ++t) {
                const uint8_t* header = ds.file.data() + segy::kHeaderTotalSize + t * ds.traceStrideBytes;
                raw[t] = int32_t(segy::readHeaderInt(header, firstByte, size, true));
            }
        });
    }
    std::vector<double> ms(raw.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t t = 0; t < raw.size(); ++t) {
        if (raw[t] != 0) ms[t] = double(raw[t]) * msPerUnit;
    }
    return ms;
}

void writeHeaderIntBigEndian(std::vector<uint8_t>& header, int firstByte, int size, int64_t value) {
    for (int k = 0; k < size; ++k) {
        header[size_t(firstByte - 1 + k)] = uint8_t((uint64_t(value) >> (8 * (size - 1 - k))) & 0xff);
    }
}

// The header-field controls shared by Flatten and Write Horizon to Header.
struct HeaderTimeControls {
    QSpinBox* byte = nullptr;
    QComboBox* size = nullptr;
    QDoubleSpinBox* unit = nullptr;
};

HeaderTimeControls addHeaderTimeControls(QGridLayout* grid, int row, int firstByte, int size, double msPerUnit) {
    HeaderTimeControls c;
    c.byte = new QSpinBox();
    c.byte->setRange(1, 237);
    c.byte->setValue(firstByte);
    c.byte->setToolTip("First byte (1-based) of the header field; 57 = Shell SEABED_TWT");
    c.size = new QComboBox();
    c.size->addItem("4 bytes", 4);
    c.size->addItem("2 bytes", 2);
    c.size->setCurrentIndex(size == 2 ? 1 : 0);
    c.unit = new QDoubleSpinBox();
    c.unit->setRange(0.0001, 1000.0);
    c.unit->setDecimals(4);
    c.unit->setValue(msPerUnit);
    c.unit->setSuffix(" ms per unit");
    c.unit->setToolTip("Time of one header count; PARAGON's SEABED_TWT is in 0.01 ms (405097 = 4050.97 ms)");
    grid->addWidget(new QLabel("Header byte"), row, 0);
    grid->addWidget(c.byte, row, 1);
    grid->addWidget(c.size, row, 2);
    grid->addWidget(c.unit, row, 3);
    return c;
}
} // namespace

void MainWindow::toggleFlatten(Panel* panel, bool on) {
    AppState& app = panel->app;
    if (!on) {
        if (app.flatten.active && app.flatten.original) {
            const segy::ViewRange view = app.view;
            std::shared_ptr<Dataset> original = app.flatten.original;
            std::shared_ptr<Dataset> flattened = app.foreground;
            panel->canvas->setForegroundDataset(original); // also clears app.flatten
            // Unmapping a large file takes a while (1.4 s for 6 GB): drop the
            // flattened copy on another thread, not the UI thread.
            std::thread([flattened = std::move(flattened)]() mutable { flattened.reset(); }).detach();
            app.view = view; // same trace/sample space
            panel->canvas->update();
            refreshHorizonManager();
            flashStatusMessage("Flatten off");
        }
        setActivePanel(activePanel_);
        return;
    }
    auto refuse = [&](const QString& why) {
        flashStatusMessage(why);
        setActivePanel(activePanel_);
    };
    if (!app.loaded || app.loading) return refuse("No data selected");
    if (app.xf.active) return refuse("Not available on an x-f display");
    std::shared_ptr<Dataset> ds = app.foreground;

    QDialog dialog(this);
    dialog.setWindowTitle(QString("Flatten") + datasetTitleSuffix(app));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QGridLayout* grid = new QGridLayout();
    QComboBox* source = new QComboBox();
    for (size_t i = 0; i < ds->horizons.size(); ++i) source->addItem("Horizon: " + ds->horizons[i].name, int(i));
    source->addItem("Trace header time", -1);
    if (ds->activeHorizon >= 0 && ds->activeHorizon < int(ds->horizons.size())) source->setCurrentIndex(ds->activeHorizon);
    grid->addWidget(new QLabel("Flatten on"), 0, 0);
    grid->addWidget(source, 0, 1, 1, 3);
    HeaderTimeControls header = addHeaderTimeControls(grid, 1, headerTimeByte_, headerTimeSize_, headerTimeMsPerUnit_);
    QLabel* summary = new QLabel();
    summary->setStyleSheet(themed("color: #9a9ca3;"));
    grid->addWidget(summary, 2, 0, 1, 4);
    QDoubleSpinBox* reference = new QDoubleSpinBox();
    reference->setRange(-1e7, 1e7);
    reference->setDecimals(1);
    reference->setSuffix(" ms");
    reference->setToolTip("The time the horizon is moved to (default: its minimum)");
    QPushButton* minimumButton = new QPushButton("Minimum");
    grid->addWidget(new QLabel("Flatten to"), 3, 0);
    grid->addWidget(reference, 3, 1);
    grid->addWidget(minimumButton, 3, 2);
    layout->addLayout(grid);
    QCheckBox* newDataset = new QCheckBox("Create a new dataset (default: a display you switch off again)");
    layout->addWidget(newDataset);
    QLineEdit* nameEdit = new QLineEdit(QString::fromStdString(ds->name) + "_flat");
    nameEdit->setEnabled(false);
    layout->addWidget(nameEdit);
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();
    QPushButton* ok = new QPushButton("Flatten");
    QPushButton* cancel = new QPushButton("Cancel");
    ok->setDefault(true);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);

    std::vector<double> values;
    double minimum = 0.0;
    auto recompute = [&](bool resetReference) {
        const bool fromHeader = source->currentData().toInt() < 0;
        header.byte->setEnabled(fromHeader);
        header.size->setEnabled(fromHeader);
        header.unit->setEnabled(fromHeader);
        if (fromHeader) {
            values = headerTimesMs(*ds, header.byte->value(), header.size->currentData().toInt(), header.unit->value());
        } else {
            const Horizon& hz = ds->horizons[size_t(source->currentData().toInt())];
            values.assign(hz.timeMs.begin(), hz.timeMs.end());
        }
        int64_t count = 0;
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (double v : values) {
            if (!std::isfinite(v)) continue;
            ++count;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        minimum = count ? lo : 0.0;
        summary->setText(count ? QString("%1 of %2 traces have a time: %3 to %4 ms; gaps are interpolated.")
                                     .arg(count)
                                     .arg(ds->traceCount)
                                     .arg(lo, 0, 'f', 1)
                                     .arg(hi, 0, 'f', 1)
                               : QString("No trace has a time here."));
        ok->setEnabled(count > 0);
        if (resetReference) reference->setValue(minimum);
    };
    recompute(true);
    connect(source, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int) { recompute(true); });
    connect(header.byte, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [&](int) { recompute(true); });
    connect(header.size, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int) { recompute(true); });
    connect(header.unit, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog, [&](double) { recompute(true); });
    connect(minimumButton, &QPushButton::clicked, &dialog, [&]() { reference->setValue(minimum); });
    connect(newDataset, &QCheckBox::toggled, nameEdit, &QWidget::setEnabled);
    connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        setActivePanel(activePanel_);
        return;
    }
    const bool fromHeader = source->currentData().toInt() < 0;
    if (fromHeader) {
        headerTimeByte_ = header.byte->value();
        headerTimeSize_ = header.size->currentData().toInt();
        headerTimeMsPerUnit_ = header.unit->value();
    }
    const QString what = fromHeader
                             ? QString("header bytes %1-%2").arg(headerTimeByte_).arg(headerTimeByte_ + headerTimeSize_ - 1)
                             : source->currentText().mid(9);
    const QString history = QString("FLATTEN ON %1 TO %2 MS").arg(what.toUpper()).arg(reference->value(), 0, 'f', 1);
    runFlatten(panel, values, reference->value(), newDataset->isChecked(),
               newDataset->isChecked() ? nameEdit->text() : QString::fromStdString(ds->name) + "_flatten_display",
               history);
    setActivePanel(activePanel_);
}

void MainWindow::runFlatten(Panel* panel, std::vector<double> sourceMs, double referenceMs, bool newDataset,
                            const QString& outputName, const QString& historyLine) {
    std::shared_ptr<Dataset> ds = panel->app.foreground;
    const int samplesPerTrace = ds->binHeader.samplesPerTrace;
    const double sampleMs = ds->binHeader.sampleIntervalUs / 1000.0;
    if (sampleMs <= 0.0) return;
    auto filled = std::make_shared<std::vector<double>>(segy::fillHorizonGaps(std::move(sourceMs)));
    std::vector<std::string> history = ds->processingHistory;
    history.push_back(historyLine.toStdString());
    const std::filesystem::path outPath = scratchSegyPath(outputName);
    struct Outcome {
        bool ok = false;
        std::string error;
    };
    auto outcome = std::make_shared<Outcome>();
    runBackgroundTask(
        QString("Flatten: %1").arg(QString::fromStdString(ds->name)),
        [this, ds, filled, referenceMs, sampleMs, samplesPerTrace, outPath, history, outputName, outcome]() {
            backgroundTaskTotal_ = ds->traceCount;
            QStringList inputBlocks;
            inputBlocks << QString("FILE: %1  TRACES: %2  SAMPLES/TRACE: %3  SAMPLE INTERVAL: %4 US")
                               .arg(QString::fromStdString(ds->name))
                               .arg(ds->traceCount)
                               .arg(samplesPerTrace)
                               .arg(ds->binHeader.sampleIntervalUs);
            const QString textHeader = buildSegyTextHeader(inputBlocks, history, outputName, ds->traceCount,
                                                           samplesPerTrace, ds->binHeader.sampleIntervalUs);
            outcome->ok = streamTraces(
                outPath, textHeader.toStdString(), ds->binHeader, ds->traceCount,
                [&](int64_t t, segy::WriteTrace& out) {
                    const uint8_t* traceBase = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                    out.headerBytes.assign(traceBase, traceBase + segy::kTraceHeaderSize);
                    out.samples.resize(size_t(samplesPerTrace));
                    segy::decodeSamples(traceBase + segy::kTraceHeaderSize, out.samples.data(), size_t(samplesPerTrace),
                                        ds->binHeader.formatCode);
                    segy::flattenTrace(out.samples, (*filled)[size_t(t)] / sampleMs, referenceMs / sampleMs);
                },
                backgroundTaskDone_, &outcome->error);
        },
        [this, panel, ds, filled, referenceMs, newDataset, outPath, history, outcome]() {
            if (!outcome->ok) {
                QMessageBox::warning(this, "Flatten", QString::fromStdString(outcome->error));
                return;
            }
            if (newDataset) {
                panel->canvas->startLoading(outPath, history);
                return;
            }
            if (panel->app.foreground != ds) return; // the user moved on meanwhile
            panel->canvas->startLoading(outPath, history, [this, panel, ds, filled, referenceMs](std::shared_ptr<Dataset> flat) {
                const segy::ViewRange view = panel->app.view;
                flat->name = ds->name + " (flattened)";
                flat->depth = ds->depth;
                // The horizons, flattened along: horizon time - (source - reference).
                for (const Horizon& h : ds->horizons) {
                    Horizon copy = h;
                    for (size_t t = 0; t < copy.timeMs.size() && t < filled->size(); ++t) {
                        if (std::isfinite(copy.timeMs[t])) copy.timeMs[t] -= float((*filled)[t] - referenceMs);
                    }
                    flat->horizons.push_back(std::move(copy));
                }
                flat->activeHorizon = ds->activeHorizon;
                panel->canvas->setForegroundDataset(flat);
                panel->app.flatten.active = true;
                panel->app.flatten.original = ds;
                panel->app.view = view;
                panel->canvas->update();
                setActivePanel(activePanel_);
                refreshWindowTitle();
                refreshHorizonManager();
                flashStatusMessage("Flattened display (Horizons > Flatten again to switch it off)");
            });
        });
}

void MainWindow::writeHorizonToHeader(Panel* panel) {
    Horizon* hz = activeHorizonFor(panel, false);
    if (!hz) {
        flashStatusMessage("No horizon to write");
        return;
    }
    std::shared_ptr<Dataset> ds = panel->app.foreground;
    QDialog dialog(this);
    dialog.setWindowTitle(QString("Write Horizon to Header") + datasetTitleSuffix(panel->app));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLabel* intro = new QLabel(QString("Writes \"%1\" into the trace headers of a new dataset "
                                       "(traces without a pick keep their value).")
                                   .arg(hz->name));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    QGridLayout* grid = new QGridLayout();
    HeaderTimeControls header = addHeaderTimeControls(grid, 0, headerTimeByte_, headerTimeSize_, headerTimeMsPerUnit_);
    layout->addLayout(grid);
    QLineEdit* nameEdit = new QLineEdit(QString::fromStdString(ds->name) + "_" + hz->name.toLower());
    layout->addWidget(nameEdit);
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();
    QPushButton* ok = new QPushButton("Write");
    QPushButton* cancel = new QPushButton("Cancel");
    ok->setDefault(true);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    headerTimeByte_ = header.byte->value();
    headerTimeSize_ = header.size->currentData().toInt();
    headerTimeMsPerUnit_ = header.unit->value();
    const int firstByte = headerTimeByte_, size = headerTimeSize_;
    const double msPerUnit = headerTimeMsPerUnit_;
    auto times = std::make_shared<std::vector<float>>(hz->timeMs);
    const QString outputName = nameEdit->text();
    std::vector<std::string> history = ds->processingHistory;
    history.push_back(QString("HORIZON %1 TO HEADER BYTES %2-%3 (%4 MS PER UNIT)")
                          .arg(hz->name.toUpper())
                          .arg(firstByte)
                          .arg(firstByte + size - 1)
                          .arg(msPerUnit)
                          .toStdString());
    const std::filesystem::path outPath = scratchSegyPath(outputName);
    struct Outcome {
        bool ok = false;
        std::string error;
    };
    auto outcome = std::make_shared<Outcome>();
    runBackgroundTask(
        QString("Horizon to header: %1").arg(QString::fromStdString(ds->name)),
        [this, ds, times, firstByte, size, msPerUnit, outPath, history, outputName, outcome]() {
            const int samplesPerTrace = ds->binHeader.samplesPerTrace;
            backgroundTaskTotal_ = ds->traceCount;
            const int64_t limit = size == 2 ? 32767 : 2147483647;
            QStringList inputBlocks;
            inputBlocks << QString("FILE: %1  TRACES: %2").arg(QString::fromStdString(ds->name)).arg(ds->traceCount);
            const QString textHeader = buildSegyTextHeader(inputBlocks, history, outputName, ds->traceCount,
                                                           samplesPerTrace, ds->binHeader.sampleIntervalUs);
            outcome->ok = streamTraces(
                outPath, textHeader.toStdString(), ds->binHeader, ds->traceCount,
                [&](int64_t t, segy::WriteTrace& out) {
                    const uint8_t* traceBase = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                    out.headerBytes.assign(traceBase, traceBase + segy::kTraceHeaderSize);
                    if (size_t(t) < times->size() && std::isfinite((*times)[size_t(t)])) {
                        const int64_t v = std::clamp<int64_t>(std::llround((*times)[size_t(t)] / msPerUnit), -limit, limit);
                        writeHeaderIntBigEndian(out.headerBytes, firstByte, size, v);
                    }
                    out.samples.resize(size_t(samplesPerTrace));
                    segy::decodeSamples(traceBase + segy::kTraceHeaderSize, out.samples.data(), size_t(samplesPerTrace),
                                        ds->binHeader.formatCode);
                },
                backgroundTaskDone_, &outcome->error);
        },
        [this, panel, outPath, history, outcome]() {
            if (!outcome->ok) {
                QMessageBox::warning(this, "Write Horizon to Header", QString::fromStdString(outcome->error));
                return;
            }
            panel->canvas->startLoading(outPath, history);
        });
}

void MainWindow::setDatasetDomain(Panel* panel, bool depth) {
    if (!panel->app.loaded || panel->app.xf.active) {
        setActivePanel(activePanel_);
        return;
    }
    std::shared_ptr<Dataset> ds = panel->app.foreground;
    if (ds->depth != depth) {
        // An annotation like the horizons (UI thread only): derived datasets
        // inherit it through the history line.
        ds->depth = depth;
        ds->processingHistory.push_back(depth ? "DOMAIN: DEPTH" : "DOMAIN: TIME");
        for (Panel* p : {&panelA_, &panelB_}) {
            if (p->app.foreground != ds) continue;
            p->app.display.depthAxis = depth;
            p->canvas->update();
        }
        flashStatusMessage(QString("%1 is now %2 data").arg(QString::fromStdString(ds->name), depth ? "depth" : "time"));
    }
    setActivePanel(activePanel_);
}

void MainWindow::openTimeDepthConversion() {
    Panel* panel = activePanel_;
    if (!panel->app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    if (panel->app.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
    std::shared_ptr<Dataset> ds = panel->app.foreground;
    const bool toDepth = !ds->depth;
    std::vector<std::shared_ptr<Dataset>> candidates;
    for (const auto& d : datasetPool_) {
        if (d != ds && QString::fromStdString(d->attribute).contains("velocity", Qt::CaseInsensitive)) candidates.push_back(d);
    }
    for (const auto& d : datasetPool_) { // other datasets too, after the tagged velocities
        if (d != ds && std::find(candidates.begin(), candidates.end(), d) == candidates.end()) candidates.push_back(d);
    }
    if (candidates.empty()) {
        QMessageBox::information(this, "Time-Depth Conversion",
                                 "Load a velocity model first (File > Read Attribute SEG-Y).");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QString(toDepth ? "Time to Depth" : "Depth to Time") + datasetTitleSuffix(panel->app));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLabel* intro = new QLabel(
        toDepth ? "The Foreground is time data: converts it to depth (below sea level, no datum correction)."
                : "The Foreground is depth data: converts it to two-way time.");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    QGridLayout* grid = new QGridLayout();
    QComboBox* velocityCombo = new QComboBox();
    for (const auto& d : candidates) {
        QString label = QString::fromStdString(d->name);
        if (!d->attribute.empty()) label += "  (" + QString::fromStdString(d->attribute) + ")";
        velocityCombo->addItem(label);
    }
    QComboBox* typeCombo = new QComboBox();
    typeCombo->addItem("Interval velocity", int(segy::VelocityType::Interval));
    typeCombo->addItem("Average velocity", int(segy::VelocityType::Average));
    typeCombo->addItem("RMS velocity (Dix)", int(segy::VelocityType::Rms));
    QComboBox* velocityDomain = new QComboBox();
    velocityDomain->addItem("in time", 0);
    velocityDomain->addItem("in depth", 1);
    QDoubleSpinBox* intervalSpin = new QDoubleSpinBox();
    intervalSpin->setRange(0.1, 32.0);
    intervalSpin->setDecimals(1);
    intervalSpin->setSuffix(toDepth ? " m" : " ms");
    intervalSpin->setValue(toDepth ? 8.0 : 4.0); // decided defaults
    QDoubleSpinBox* lengthSpin = new QDoubleSpinBox();
    lengthSpin->setRange(1.0, 1e6);
    lengthSpin->setDecimals(0);
    lengthSpin->setSuffix(toDepth ? " m" : " ms");
    grid->addWidget(new QLabel("Velocity"), 0, 0);
    grid->addWidget(velocityCombo, 0, 1, 1, 2);
    grid->addWidget(new QLabel("Type"), 1, 0);
    grid->addWidget(typeCombo, 1, 1);
    grid->addWidget(velocityDomain, 1, 2);
    grid->addWidget(new QLabel("Output sample interval"), 2, 0);
    grid->addWidget(intervalSpin, 2, 1);
    grid->addWidget(new QLabel(toDepth ? "Maximum depth" : "Maximum time"), 3, 0);
    grid->addWidget(lengthSpin, 3, 1);
    layout->addLayout(grid);
    QLabel* note = new QLabel("The velocity model is matched on CDP and interpolated between CDPs; "
                              "its own sampling is interpolated linearly. Velocities in m/s.");
    note->setWordWrap(true);
    note->setStyleSheet(themed("color: #9a9ca3;"));
    layout->addWidget(note);
    QLineEdit* nameEdit = new QLineEdit(QString::fromStdString(ds->name) + (toDepth ? "_depth" : "_time"));
    layout->addWidget(nameEdit);
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();
    QPushButton* ok = new QPushButton("Convert");
    QPushButton* cancel = new QPushButton("Cancel");
    ok->setDefault(true);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);

    const double inputUnit = ds->binHeader.sampleIntervalUs / (toDepth ? 1e6 : 1000.0); // s or m
    const double inputLength = inputUnit * std::max(0, ds->binHeader.samplesPerTrace - 1);
    auto velocityInterval = [](const Dataset& v, bool inDepth) {
        return v.binHeader.sampleIntervalUs / (inDepth ? 1000.0 : 1e6);
    };
    // Defaults from the chosen velocity: its tag, its domain flag, and the
    // output length that covers the input (from its first trace).
    auto refreshDefaults = [&](bool fromTag) {
        const std::shared_ptr<Dataset>& v = candidates[size_t(velocityCombo->currentIndex())];
        if (fromTag) {
            const QString tag = QString::fromStdString(v->attribute);
            if (tag.contains("RMS", Qt::CaseInsensitive)) typeCombo->setCurrentIndex(2);
            else if (tag.contains("Average", Qt::CaseInsensitive)) typeCombo->setCurrentIndex(1);
            else typeCombo->setCurrentIndex(0);
            velocityDomain->setCurrentIndex(v->depth ? 1 : 0);
        }
        const int count = v->binHeader.samplesPerTrace;
        std::string error;
        segy::TimeDepthMap map;
        if (v->traceCount > 0 && count > 0) {
            std::vector<float> trace(static_cast<size_t>(count));
            segy::decodeSamples(v->file.data() + segy::kHeaderTotalSize + segy::kTraceHeaderSize, trace.data(),
                                size_t(count), v->binHeader.formatCode);
            map = segy::buildTimeDepthMap(trace.data(), count, velocityInterval(*v, velocityDomain->currentIndex() == 1),
                                          velocityDomain->currentIndex() == 1,
                                          segy::VelocityType(typeCombo->currentData().toInt()), &error);
        }
        ok->setEnabled(!map.time.empty());
        ok->setToolTip(map.time.empty() ? QString::fromStdString(error) : QString());
        if (map.time.empty()) return;
        lengthSpin->setValue(toDepth ? std::ceil(map.depthAt(inputLength) / 100.0) * 100.0
                                     : std::ceil(map.timeAt(inputLength) * 1000.0 / 100.0) * 100.0);
    };
    refreshDefaults(true);
    connect(velocityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int) { refreshDefaults(true); });
    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int) { refreshDefaults(false); });
    connect(velocityDomain, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
            [&](int) { refreshDefaults(false); });
    connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    std::shared_ptr<Dataset> velocity = candidates[size_t(velocityCombo->currentIndex())];
    const auto type = segy::VelocityType(typeCombo->currentData().toInt());
    const bool velocityInDepth = velocityDomain->currentIndex() == 1;
    const double velocityStep = velocityInterval(*velocity, velocityInDepth);
    const double outputStep = toDepth ? intervalSpin->value() : intervalSpin->value() / 1000.0; // m or s
    const int outputCount = std::clamp(int(std::floor(lengthSpin->value() / intervalSpin->value())) + 1, 2, 32767);
    const int16_t outputIntervalHeader = int16_t(std::lround(intervalSpin->value() * 1000.0)); // mm or us
    const QString outputName = nameEdit->text();
    std::vector<std::string> history = ds->processingHistory;
    history.push_back(QString("%1 CONVERSION, %2 %3 (%4), D%5 %6")
                          .arg(toDepth ? "TIME TO DEPTH" : "DEPTH TO TIME")
                          .arg(typeCombo->currentText().toUpper())
                          .arg(QString::fromStdString(velocity->name).toUpper())
                          .arg(velocityInDepth ? "IN DEPTH" : "IN TIME")
                          .arg(toDepth ? "Z" : "T")
                          .arg(toDepth ? QString("%1 M").arg(intervalSpin->value()) : QString("%1 MS").arg(intervalSpin->value()))
                          .toStdString());
    history.push_back(toDepth ? "DOMAIN: DEPTH" : "DOMAIN: TIME");
    const std::filesystem::path outPath = scratchSegyPath(outputName);
    struct Outcome {
        bool ok = false;
        std::string error;
    };
    auto outcome = std::make_shared<Outcome>();
    runBackgroundTask(
        QString("%1: %2").arg(toDepth ? "Time to depth" : "Depth to time", QString::fromStdString(ds->name)),
        [this, ds, velocity, type, velocityInDepth, velocityStep, toDepth, inputUnit, outputStep, outputCount,
         outputIntervalHeader, outPath, history, outputName, outcome]() {
            // The velocity traces by CDP (ascending; the first of equal CDPs).
            const int vCount = velocity->binHeader.samplesPerTrace;
            std::vector<std::pair<int64_t, int64_t>> byCdp; // cdp, trace
            for (int64_t t = 0; t < velocity->traceCount; ++t) {
                const uint8_t* base = velocity->file.data() + segy::kHeaderTotalSize + size_t(t) * velocity->traceStrideBytes;
                byCdp.push_back({segy::parseTraceHeader(base).cdp, t});
            }
            std::stable_sort(byCdp.begin(), byCdp.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            byCdp.erase(std::unique(byCdp.begin(), byCdp.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
                        byCdp.end());
            std::vector<int64_t> cdps;
            std::vector<std::vector<float>> velocities;
            for (const auto& [cdp, t] : byCdp) {
                cdps.push_back(cdp);
                std::vector<float> v(static_cast<size_t>(vCount));
                const uint8_t* base = velocity->file.data() + segy::kHeaderTotalSize + size_t(t) * velocity->traceStrideBytes;
                segy::decodeSamples(base + segy::kTraceHeaderSize, v.data(), size_t(vCount), velocity->binHeader.formatCode);
                velocities.push_back(std::move(v));
            }
            if (velocities.empty() || vCount < 1) {
                outcome->error = "The velocity dataset has no traces.";
                return;
            }
            const int samplesPerTrace = ds->binHeader.samplesPerTrace;
            backgroundTaskTotal_ = ds->traceCount;
            std::mutex errorMutex;
            std::string mapError;
            segy::BinaryHeader binHeader = ds->binHeader;
            binHeader.samplesPerTrace = int16_t(outputCount);
            binHeader.sampleIntervalUs = outputIntervalHeader;
            QStringList inputBlocks;
            inputBlocks << QString("FILE: %1  VELOCITY: %2")
                               .arg(QString::fromStdString(ds->name), QString::fromStdString(velocity->name));
            const QString textHeader =
                buildSegyTextHeader(inputBlocks, history, outputName, ds->traceCount, outputCount, outputIntervalHeader);
            outcome->ok = streamTraces(
                outPath, textHeader.toStdString(), binHeader, ds->traceCount,
                [&](int64_t t, segy::WriteTrace& out) {
                    thread_local std::vector<float> v, input;
                    v.resize(size_t(vCount));
                    input.resize(size_t(samplesPerTrace));
                    const uint8_t* base = ds->file.data() + segy::kHeaderTotalSize + size_t(t) * ds->traceStrideBytes;
                    segy::interpolateVelocity(cdps, velocities, segy::parseTraceHeader(base).cdp, vCount, v.data());
                    std::string error;
                    const segy::TimeDepthMap map =
                        segy::buildTimeDepthMap(v.data(), vCount, velocityStep, velocityInDepth, type, &error);
                    out.headerBytes.assign(base, base + segy::kTraceHeaderSize);
                    writeHeaderIntBigEndian(out.headerBytes, 115, 2, outputCount);
                    writeHeaderIntBigEndian(out.headerBytes, 117, 2, outputIntervalHeader);
                    out.samples.assign(size_t(outputCount), 0.0f);
                    if (map.time.empty()) {
                        std::lock_guard<std::mutex> lock(errorMutex);
                        mapError = error;
                        return;
                    }
                    segy::decodeSamples(base + segy::kTraceHeaderSize, input.data(), size_t(samplesPerTrace),
                                        ds->binHeader.formatCode);
                    segy::convertTrace(input.data(), samplesPerTrace, inputUnit, map, toDepth, out.samples.data(),
                                       outputCount, outputStep);
                },
                backgroundTaskDone_, &outcome->error);
            if (!mapError.empty()) {
                outcome->ok = false;
                outcome->error = mapError;
            }
        },
        [this, panel, outPath, history, outcome]() {
            if (!outcome->ok) {
                QMessageBox::warning(this, "Time-Depth Conversion", QString::fromStdString(outcome->error));
                return;
            }
            panel->canvas->startLoading(outPath, history);
        });
}

void MainWindow::openHorizonManager() {
    if (!horizonManager_) {
        horizonManager_ = new QWidget(this, Qt::Window);
        horizonManager_->setWindowTitle("Horizon Manager");
        horizonManager_->setStyleSheet(themed("background-color: #1e1f22;"));
        QVBoxLayout* layout = new QVBoxLayout(horizonManager_);
        QListWidget* list = new QListWidget();
        list->setObjectName("horizonList");
        layout->addWidget(list, 1);
        QHBoxLayout* buttons = new QHBoxLayout();
        auto button = [&](const QString& text, const QString& tip) {
            QPushButton* b = new QPushButton(text);
            b->setToolTip(tip);
            buttons->addWidget(b);
            return b;
        };
        QPushButton* newButton = button("New", "A new, empty horizon (becomes the active one)");
        QPushButton* renameButton = button("Rename", "Rename the selected horizon");
        QPushButton* colourButton = button("Colour", "Change the selected horizon's colour");
        QPushButton* clearButton = button("Clear", "Remove all picks of the selected horizon");
        QPushButton* deleteButton = button("Delete", "Delete the selected horizon");
        layout->addLayout(buttons);

        QGroupBox* pickGroup = new QGroupBox("Picking");
        QGridLayout* grid = new QGridLayout(pickGroup);
        QComboBox* modeCombo = new QComboBox();
        modeCombo->addItem("Peak", int(segy::PickMode::Peak));
        modeCombo->addItem("Trough", int(segy::PickMode::Trough));
        modeCombo->addItem("Zero crossing", int(segy::PickMode::ZeroCrossing));
        QDoubleSpinBox* searchSpin = new QDoubleSpinBox();
        searchSpin->setRange(1.0, 500.0);
        searchSpin->setDecimals(0);
        searchSpin->setPrefix(QString(QChar(0x00B1)));
        searchSpin->setSuffix(" ms");
        searchSpin->setValue(pickSearchMs_);
        searchSpin->setToolTip("Search window around the straight line between two points");
        QDoubleSpinBox* trustSpin = new QDoubleSpinBox();
        trustSpin->setRange(0.3, 1.0);
        trustSpin->setDecimals(2);
        trustSpin->setSingleStep(0.05);
        trustSpin->setValue(pickTrust_);
        trustSpin->setToolTip("Trace-to-trace correlation needed to follow the event; below it the pick stays on the line");
        QDoubleSpinBox* windowSpin = new QDoubleSpinBox();
        windowSpin->setRange(8.0, 500.0);
        windowSpin->setDecimals(0);
        windowSpin->setSuffix(" ms");
        windowSpin->setValue(pickWindowMs_);
        windowSpin->setToolTip("Correlation window around the previous pick");
        QDoubleSpinBox* thresholdSpin = new QDoubleSpinBox();
        thresholdSpin->setRange(1.0, 100.0);
        thresholdSpin->setDecimals(0);
        thresholdSpin->setSuffix(" %");
        thresholdSpin->setValue(seabedThresholdPercent_);
        thresholdSpin->setToolTip("Seabed: first amplitude above this share of the trace maximum");
        grid->addWidget(new QLabel("Snap to"), 0, 0);
        grid->addWidget(modeCombo, 0, 1);
        grid->addWidget(new QLabel("Search"), 0, 2);
        grid->addWidget(searchSpin, 0, 3);
        grid->addWidget(new QLabel("Trust"), 1, 0);
        grid->addWidget(trustSpin, 1, 1);
        grid->addWidget(new QLabel("Window"), 1, 2);
        grid->addWidget(windowSpin, 1, 3);
        grid->addWidget(new QLabel("Seabed threshold"), 2, 0);
        grid->addWidget(thresholdSpin, 2, 1);
        QPushButton* seabedButton = new QPushButton("Auto-pick seabed");
        grid->addWidget(seabedButton, 2, 2, 1, 2);
        layout->addWidget(pickGroup);
        QHBoxLayout* files = new QHBoxLayout();
        QPushButton* exportButton = new QPushButton("Export CSV...");
        QPushButton* importButton = new QPushButton("Import CSV...");
        files->addWidget(exportButton);
        files->addWidget(importButton);
        files->addStretch();
        layout->addLayout(files);
        QLabel* hint = new QLabel("Pick Horizon tool: click points along an event; the traces in between are tracked. "
                                  "Right click removes the nearest point.");
        hint->setWordWrap(true);
        hint->setStyleSheet(themed("color: #9a9ca3; font-size: 11px;"));
        layout->addWidget(hint);

        connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, modeCombo](int) { pickMode_ = segy::PickMode(modeCombo->currentData().toInt()); });
        connect(searchSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { pickSearchMs_ = v; });
        connect(trustSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { pickTrust_ = v; });
        connect(windowSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { pickWindowMs_ = v; });
        connect(thresholdSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this](double v) { seabedThresholdPercent_ = v; });
        connect(seabedButton, &QPushButton::clicked, this, [this]() { autoPickSeabed(activePanel_); });
        connect(exportButton, &QPushButton::clicked, this, [this]() { exportHorizon(activePanel_); });
        connect(importButton, &QPushButton::clicked, this, [this]() { importHorizon(activePanel_); });
        connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
            if (!app_->loaded || row < 0 || row >= int(app_->foreground->horizons.size())) return;
            app_->foreground->activeHorizon = row;
            activePanel_->canvas->update();
        });
        connect(newButton, &QPushButton::clicked, this, [this]() {
            if (!app_->loaded) return;
            app_->foreground->activeHorizon = -1;
            activeHorizonFor(activePanel_, true);
            refreshHorizonManager();
            activePanel_->canvas->update();
        });
        connect(renameButton, &QPushButton::clicked, this, [this]() {
            Horizon* hz = activeHorizonFor(activePanel_, false);
            if (!hz) return;
            bool ok = false;
            const QString name = QInputDialog::getText(this, "Rename Horizon", "Name", QLineEdit::Normal, hz->name, &ok);
            if (ok && !name.trimmed().isEmpty()) hz->name = name.trimmed();
            refreshHorizonManager();
        });
        connect(colourButton, &QPushButton::clicked, this, [this]() {
            Horizon* hz = activeHorizonFor(activePanel_, false);
            if (!hz) return;
            const QColor colour = QColorDialog::getColor(hz->color, this, "Horizon Colour");
            if (colour.isValid()) hz->color = colour;
            refreshHorizonManager();
            activePanel_->canvas->update();
        });
        connect(clearButton, &QPushButton::clicked, this, [this]() {
            Horizon* hz = activeHorizonFor(activePanel_, false);
            if (!hz) return;
            std::fill(hz->timeMs.begin(), hz->timeMs.end(), std::numeric_limits<float>::quiet_NaN());
            hz->anchors.clear();
            refreshHorizonManager();
            activePanel_->canvas->update();
        });
        connect(deleteButton, &QPushButton::clicked, this, [this]() {
            if (!app_->loaded) return;
            Dataset& ds = *app_->foreground;
            if (ds.activeHorizon < 0 || ds.activeHorizon >= int(ds.horizons.size())) return;
            ds.horizons.erase(ds.horizons.begin() + ds.activeHorizon);
            ds.activeHorizon = ds.horizons.empty() ? -1 : 0;
            refreshHorizonManager();
            activePanel_->canvas->update();
        });
        horizonManager_->resize(520, 520);
    }
    refreshHorizonManager();
    horizonManager_->show();
    horizonManager_->raise();
    horizonManager_->activateWindow();
}

void MainWindow::refreshHorizonManager() {
    if (!horizonManager_) return;
    auto* list = horizonManager_->findChild<QListWidget*>("horizonList");
    if (!list) return;
    QSignalBlocker block(list);
    list->clear();
    if (!app_->loaded) return;
    const Dataset& ds = *app_->foreground;
    for (const Horizon& hz : ds.horizons) {
        int64_t picked = 0;
        for (float v : hz.timeMs) picked += std::isfinite(v) ? 1 : 0;
        QPixmap swatch(14, 14);
        swatch.fill(hz.color);
        list->addItem(new QListWidgetItem(QIcon(swatch), QString("%1   (%2 picks)").arg(hz.name).arg(picked)));
    }
    list->setCurrentRow(ds.activeHorizon);
    horizonManager_->setWindowTitle(QString("Horizon Manager") + datasetTitleSuffix(*app_));
}

void MainWindow::openAutocorrelationDialog() {
    if (!autocorrelationDialog_) {
        autocorrelationDialog_ = new AutocorrelationDialog(this);
        // Recomputes from whichever panel is active at Apply time (non-modal).
        autocorrelationDialog_->setApplyCallback(
            [this](double minMs, double maxMs) { computeAndShowAutocorrelation(activePanel_, minMs, maxMs); });
    }
}

void MainWindow::computeAndShowAutocorrelation(Panel* sourcePanel, double minLagMs, double maxLagMs) {
    if (sourcePanel->app.xf.active) {
        flashStatusMessage("Not available on an x-f display");
        return;
    }
    AppState& app = sourcePanel->app;
    if (!app.loaded) {
        flashStatusMessage("No data selected");
        return;
    }
    // The panel's current view, same as Octave Bands: the user picks what
    // to analyze by zooming/panning to it.
    int64_t t0 = std::max<int64_t>(0, int64_t(std::floor(app.view.traceStart)));
    int64_t t1 = std::min<int64_t>(app.foreground->traceCount, int64_t(std::ceil(app.view.traceEnd)));
    int s0 = std::max(0, int(std::floor(app.view.sampleStart)));
    int s1 = std::min<int>(app.foreground->binHeader.samplesPerTrace, int(std::ceil(app.view.sampleEnd)));
    const double intervalUs = app.foreground->binHeader.sampleIntervalUs;
    if (t1 - t0 <= 0 || s1 - s0 < 2 || intervalUs <= 0) return;
    const int visible = s1 - s0;
    const int minLag = -std::clamp(int(std::lround(minLagMs * 1000.0 / intervalUs)), 0, visible - 1);
    const int maxLag = std::clamp(int(std::lround(maxLagMs * 1000.0 / intervalUs)), 0, visible - 1);
    if (maxLag - minLag < 2) {
        flashStatusMessage("Lag window too small");
        return;
    }
    openAutocorrelationDialog();
    runCorrelation(autocorrelationDialog_, app.foreground, app.foreground, t0, t1, s0, s1, minLag, maxLag,
                   app.display.colorScale, app.display.colorScaleFlipped,
                   QString("Autocorrelation") + datasetTitleSuffix(app), QString());
}

void MainWindow::computeAndShowCrossCorrelation(std::shared_ptr<Dataset> a, std::shared_ptr<Dataset> b,
                                                double minLagMs, double maxLagMs) {
    if (!a || !b) return;
    crossA_ = a;
    crossB_ = b;
    if (!crossCorrelationDialog_) {
        crossCorrelationDialog_ = new AutocorrelationDialog(this, /*crossCorrelation=*/true);
        crossCorrelationDialog_->setApplyCallback([this](double minMs, double maxMs) {
            computeAndShowCrossCorrelation(crossA_, crossB_, minMs, maxMs);
        });
    }
    // Trace i of A with trace i of B over the whole common trace length.
    const int64_t traces = std::min(a->traceCount, b->traceCount);
    const int samples = std::min<int>(a->binHeader.samplesPerTrace, b->binHeader.samplesPerTrace);
    const double intervalUs = a->binHeader.sampleIntervalUs;
    if (traces <= 0 || samples < 2 || intervalUs <= 0) {
        flashStatusMessage("Not enough data to cross-correlate");
        return;
    }
    if (a->binHeader.sampleIntervalUs != b->binHeader.sampleIntervalUs) {
        flashStatusMessage("Different sample intervals: cross-correlation needs the same");
        return;
    }
    const int minLag = -std::clamp(int(std::lround(minLagMs * 1000.0 / intervalUs)), 0, samples - 1);
    const int maxLag = std::clamp(int(std::lround(maxLagMs * 1000.0 / intervalUs)), 0, samples - 1);
    const QString nameA = QString::fromStdString(a->name), nameB = QString::fromStdString(b->name);
    runCorrelation(crossCorrelationDialog_, a, b, 0, traces, 0, samples, minLag, maxLag, app_->display.colorScale,
                   app_->display.colorScaleFlipped, QString("Cross-correlation %1 %2 %3").arg(nameA, QChar(0x00D7), nameB),
                   QString("%1 %2 %3").arg(nameA, QChar(0x00D7), nameB));
}

void MainWindow::runCorrelation(AutocorrelationDialog* dialog, std::shared_ptr<Dataset> a, std::shared_ptr<Dataset> b,
                                int64_t t0, int64_t t1, int s0, int s1, int minLag, int maxLag,
                                segy::ColorScale colorScale, bool colorScaleFlipped, const QString& title,
                                const QString& description) {
    const int visibleSamples = s1 - s0;
    const double intervalUs = a->binHeader.sampleIntervalUs;
    // Evenly spaced traces across the range, like the octave panels.
    constexpr int kMaxCorrelationTraces = 240;
    const int decodeCount = int(std::min<int64_t>(t1 - t0, kMaxCorrelationTraces));
    auto result = std::make_shared<AutocorrelationResult>();
    result->traceStart = double(t0);
    result->traceEnd = double(t1 - 1);
    result->minLagSamples = minLag;
    result->maxLagSamples = maxLag;
    result->sampleIntervalUs = intervalUs;
    result->traceCount = decodeCount;
    result->description = description;

    runBackgroundTask(
        QString("%1: %2 traces").arg(description.isEmpty() ? QString("Autocorrelation") : QString("Cross-correlation"))
            .arg(decodeCount),
        [this, a, b, t0, t1, s0, visibleSamples, decodeCount, minLag, maxLag, colorScale, colorScaleFlipped, result]() {
            backgroundTaskTotal_ = decodeCount;
            const int width = maxLag - minLag + 1;
            std::vector<std::vector<float>> panel(size_t(decodeCount), std::vector<float>(size_t(width), 0.0f));
            std::vector<double> sum(size_t(width), 0.0);
            int contributing = 0;
            const double step = double(t1 - t0) / double(decodeCount);
            std::vector<float> sa(static_cast<size_t>(visibleSamples)), sb(static_cast<size_t>(visibleSamples));
            auto decode = [visibleSamples, s0](const Dataset& ds, int64_t traceIdx, float* out) {
                const int sampleSize = segy::sampleFormatSizeBytes(ds.binHeader.formatCode);
                const uint8_t* traceBase = ds.file.data() + segy::kHeaderTotalSize + size_t(traceIdx) * ds.traceStrideBytes;
                segy::decodeSamples(traceBase + segy::kTraceHeaderSize + size_t(s0) * size_t(sampleSize), out,
                                    size_t(visibleSamples), ds.binHeader.formatCode);
            };
            for (int i = 0; i < decodeCount; ++i) {
                const int64_t traceIdx = std::min<int64_t>(t0 + int64_t((double(i) + 0.5) * step), t1 - 1);
                decode(*a, traceIdx, sa.data());
                const float* other = sa.data();
                if (b != a) {
                    decode(*b, traceIdx, sb.data());
                    other = sb.data();
                }
                std::vector<float> corr = segy::normalizedCrossCorrelation(sa.data(), other, visibleSamples, minLag, maxLag);
                const bool dead = std::all_of(corr.begin(), corr.end(), [](float v) { return v == 0.0f; });
                panel[size_t(i)] = corr;
                // Dead traces (all zero) don't drag the average down.
                if (!dead) {
                    ++contributing;
                    for (int j = 0; j < width; ++j) sum[size_t(j)] += corr[size_t(j)];
                }
                backgroundTaskDone_ = i + 1;
            }
            result->average.assign(size_t(width), 0.0f);
            if (contributing > 0) {
                for (int j = 0; j < width; ++j) result->average[size_t(j)] = float(sum[size_t(j)] / contributing);
            }
            // Peak-to-peak: nearest side peak of the average >= 0.5.
            result->peakLagSamples = segy::nearestSidePeakLag(result->average, minLag, 0.5);
            if (!std::isnan(result->peakLagSamples)) {
                const int idx = std::clamp(int(std::lround(result->peakLagSamples)) - minLag, 0, width - 1);
                result->peakValue = result->average[size_t(idx)];
            }
            // Power spectrum of the average: lags tapered (cosine squared
            // to each end) and placed circularly (lag k at index k mod N).
            const size_t len = segy::nextPowerOfTwo(size_t(std::max(64, 2 * width)));
            std::vector<std::complex<float>> buf(len, std::complex<float>(0.0f, 0.0f));
            const double pi = std::acos(-1.0);
            for (int j = 0; j < width; ++j) {
                const int lag = minLag + j;
                const double side = lag >= 0 ? double(std::max(1, maxLag)) : double(std::max(1, -minLag));
                const double w = std::cos(0.5 * pi * std::min(1.0, std::fabs(double(lag)) / (side + 1.0)));
                buf[size_t((lag % int(len) + int(len)) % int(len))] = float(result->average[size_t(j)] * w * w);
            }
            segy::fft(buf);
            const size_t half = len / 2;
            double peak = 0.0;
            std::vector<double> power(half + 1);
            for (size_t m = 0; m <= half; ++m) {
                power[m] = std::abs(buf[m]);
                peak = std::max(peak, power[m]);
            }
            const double dtSeconds = result->sampleIntervalUs * 1e-6;
            for (size_t m = 0; m <= half; ++m) {
                result->spectrumHz.push_back(float(double(m) / (double(len) * dtSeconds)));
                result->spectrumDb.push_back(peak > 0.0 && power[m] > 0.0
                                                 ? std::max(-120.0f, float(10.0 * std::log10(power[m] / peak)))
                                                 : std::numeric_limits<float>::quiet_NaN());
            }
            auto data = std::make_shared<OctaveBandData>();
            data->traces = std::move(panel);
            data->clip = 1.0f;
            data->colorScale = colorScale;
            data->colorScaleFlipped = colorScaleFlipped;
            result->panel = std::move(data);
        },
        [dialog, result, title]() {
            dialog->setResult(*result);
            dialog->setWindowTitle(title);
            dialog->show();
            dialog->raise();
            dialog->activateWindow();
        });
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

    std::thread([this, label, backgroundWork = std::move(backgroundWork), uiCompletion = std::move(uiCompletion)]() mutable {
        QElapsedTimer workTimer;
        workTimer.start();
        backgroundWork();
        perfLog(QString("task \"%1\": %2 ms in the background").arg(label).arg(workTimer.elapsed()));
        QMetaObject::invokeMethod(
            this,
            [this, label, uiCompletion = std::move(uiCompletion)]() mutable {
                PerfScope perf(QString("task \"%1\" completion on the UI thread").arg(label), 0);
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

void MainWindow::openAttributeFile() {
    // Deferred like openFile(), for the same menu-closing reason.
    QTimer::singleShot(0, this, [this]() {
        SegyOpenDialog dialog(this);
        dialog.setWindowTitle("Read Attribute SEG-Y");
        if (dialog.exec() != QDialog::Accepted) return;
        const std::filesystem::path path = dialog.selectedPath();
        if (path.empty()) return;
        static const QStringList kAttributes = {"Interval velocity", "Average velocity", "RMS velocity",
                                                "Delta",             "Epsilon",          "Eta"};
        bool ok = false;
        const QString attribute = QInputDialog::getItem(this, "Read Attribute SEG-Y",
                                                        QString("What does %1 contain?")
                                                            .arg(QString::fromStdString(path.filename().string())),
                                                        kAttributes, 0, false, &ok);
        if (!ok) return;
        auto result = std::make_shared<segy::LoadResult>();
        runBackgroundTask(
            QString("Reading attribute: %1").arg(QString::fromStdString(path.filename().string())),
            [path, result]() {
                // Own pool: the panels keep rendering meanwhile.
                segy::ThreadPool pool;
                *result = segy::loadSegyFile(path, pool);
            },
            [this, path, attribute, result]() {
                if (!result->ok) {
                    QMessageBox::warning(this, "Read Attribute SEG-Y", QString::fromStdString(result->error));
                    return;
                }
                auto dataset = std::make_shared<Dataset>();
                dataset->name = path.stem().string();
                dataset->filePath = path;
                dataset->file = std::move(result->file);
                dataset->binHeader = result->binHeader;
                dataset->traceCount = result->traceCount;
                dataset->traceStrideBytes = result->traceStrideBytes;
                dataset->pyramid = std::move(result->pyramid);
                dataset->attribute = attribute.toStdString();
                const QString name = QString::fromStdString(dataset->name);
                registerDataset(std::move(dataset));
                flashStatusMessage(QString("%1 (%2) added to the dataset list -- pick it as Background")
                                       .arg(name, attribute));
            });
    });
}

} // namespace segyqt

namespace {

// Windows Grey: native controls; just what our own widgets need on top.
QString windowsGreyStyleSheet() {
    return QStringLiteral(R"(
        QToolBar { spacing: 4px; padding: 4px; border-bottom: 1px solid #d0d0d0; }
        QToolButton:checked { background: #cce4f7; border: 1px solid #cce4f7; border-radius: 4px; }
        QStatusBar { border-top: 1px solid #d0d0d0; }
    )");
}

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
        QToolButton:checked { background: #4a3626; color: #e8905a; border: 1px solid #4a3626; }
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
        QTableWidget::item:selected, QListWidget::item:selected, QTableView::item:selected { background: #4a3626; color: #f0f2f5; }
        QTableCornerButton::section { background: #2b2d30; border: none; }
        QLineEdit { background: #17181a; color: #d4d4d8; border: 1px solid #3c3f41; border-radius: 4px; padding: 2px 6px; selection-background-color: #4a3626; selection-color: #f0f2f5; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 1px solid #e8905a; }
        QLineEdit:disabled { color: #68696e; }
        QListWidget, QListView, QTreeView { background: #17181a; color: #d4d4d8; border: 1px solid #3c3f41; }
        QPlainTextEdit, QTextEdit { background: #17181a; color: #d4d4d8; border: 1px solid #3c3f41; selection-background-color: #4a3626; }
        QToolTip { background: #2b2d30; color: #f0f2f5; border: 1px solid #4a4c50; padding: 4px 6px; }
        QTabBar::tab { background: #2b2d30; color: #9a9ca3; padding: 6px 12px; border: 1px solid #3c3f41; }
        QTabBar::tab:selected { background: #3c3f41; color: #f0f2f5; }
        QScrollBar:horizontal { background: #17181a; height: 12px; margin: 0; }
        QScrollBar:vertical { background: #17181a; width: 12px; margin: 0; }
        QScrollBar::handle:horizontal { background: #4a4c50; min-width: 24px; border-radius: 5px; margin: 2px; }
        QScrollBar::handle:vertical { background: #4a4c50; min-height: 24px; border-radius: 5px; margin: 2px; }
        QScrollBar::handle:hover { background: #6b6d72; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QAbstractScrollArea::corner { background: #17181a; }
    )");
}

} // namespace

namespace {
// Times every event dispatch when profiling is on (SEGYREAD_PROFILE): a
// dispatch of 50 ms or more is a UI stall, logged with the receiver.
class ProfilingApplication : public QApplication {
public:
    using QApplication::QApplication;
    bool notify(QObject* receiver, QEvent* event) override {
        if (!segyqt::perfEnabled()) return QApplication::notify(receiver, event);
        const int type = int(event->type());
        const QString receiverClass = QString::fromLatin1(receiver->metaObject()->className());
        QElapsedTimer timer;
        timer.start();
        const bool result = QApplication::notify(receiver, event);
        if (timer.elapsed() >= 50) {
            segyqt::perfLog(QString("UI stall: %1 ms in event %2 for %3").arg(timer.elapsed()).arg(type).arg(receiverClass));
        }
        return result;
    }
};
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
    ProfilingApplication app(argc, argv);
    {
        // The theme is fixed for the session (View > Theme restarts the app).
        QSettings settings(QDir::homePath() + "/.segyread_settings", QSettings::IniFormat);
        segyqt::setUiTheme(settings.value("uiTheme").toString() == "windowsGrey" ? segyqt::UiTheme::WindowsGrey
                                                                                   : segyqt::UiTheme::Dark);
    }
    // Window/taskbar icon on both platforms -- compiled in via app_icon.qrc
    // (native/assets/icons/LEESMIJ.md), so this doesn't depend on the built
    // executable's location at runtime. The Windows exe's own icon
    // (Explorer, taskbar before the window paints) is separate, embedded by
    // app.rc.
    app.setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));
    {
        // Disabled text is drawn embossed with the palette's Light colour on
        // Windows (a white shadow: "Selection" looked doubled and blurry on
        // the dark background). Make that shadow the background colour.
        if (!segyqt::lightTheme()) {
            QPalette palette = app.palette();
            palette.setColor(QPalette::Disabled, QPalette::Light, QColor(0x1e, 0x1f, 0x22));
            palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x68, 0x69, 0x6e));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x68, 0x69, 0x6e));
            app.setPalette(palette);
        }
    }
    if (segyqt::lightTheme()) {
        // Windows Grey: the native Windows controls, light palette, and only
        // the few rules our own widgets need.
        app.setStyle(QStringLiteral("windows11"));
        // Windows 11 light surfaces, not the classic beige standard palette.
        QPalette palette = app.style()->standardPalette();
        palette.setColor(QPalette::Window, QColor(0xf0, 0xf0, 0xf0));
        palette.setColor(QPalette::Button, QColor(0xfb, 0xfb, 0xfb));
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::AlternateBase, QColor(0xf7, 0xf7, 0xf7));
        palette.setColor(QPalette::WindowText, QColor(0x1b, 0x1b, 0x1b));
        palette.setColor(QPalette::Text, QColor(0x1b, 0x1b, 0x1b));
        palette.setColor(QPalette::ButtonText, QColor(0x1b, 0x1b, 0x1b));
        palette.setColor(QPalette::Highlight, QColor(0x00, 0x67, 0xc0));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::ToolTipBase, Qt::white);
        palette.setColor(QPalette::ToolTipText, QColor(0x1b, 0x1b, 0x1b));
        app.setPalette(palette);
        app.setStyleSheet(windowsGreyStyleSheet());
    } else {
        app.setStyleSheet(darkStyleSheet());
        segyqt::installDarkTitleBars(app); // Windows: black title bars on every window, matching the theme
    }
    segyqt::MainWindow window;
    if (segyqt::perfEnabled()) {
        // Which action started what follows in the log.
        for (QAction* action : window.findChildren<QAction*>()) {
            QObject::connect(action, &QAction::triggered, action,
                             [action]() { segyqt::perfLog("action: " + action->text().remove('&')); });
        }
    }
    window.show();
    if (argc > 1) window.openInitialFile(std::filesystem::path(argv[1]));
    return app.exec();
}
