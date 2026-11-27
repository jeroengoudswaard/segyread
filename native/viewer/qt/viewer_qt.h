// Qt shell for the SEG-Y viewer: one QMainWindow/QWidget-based shell that
// builds and runs identically on Windows and Linux, replacing the separate
// Win32 (viewer/win32/viewer_win32.cpp) and FLTK (viewer/linux/viewer_fltk.cpp)
// shells those platforms used before. All rendering/view-math/file-loading
// logic still lives in the shared renderer.h/loader.h/chrome.h, reused
// unchanged -- Qt's job here is strictly window/menu/dialog/text/threading.
#pragma once

#include <QColor>
#include <QDialog>
#include <QImage>
#include <QMainWindow>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "analysis.h"
#include "horizons.h"
#include "processing.h"
#include "iirfilter.h"

// Processing > Bandpass Filter's request: Butterworth (corners, a slope per
// side) or a trapezoid (Ormsby, four corners).
struct BandpassRequest {
    bool trapezoid = false;
    segy::ButterworthBandParams butterworth;
    segy::OrmsbyParams ormsby;
};
#include "segy_select.h"
#include "trace_header_defs.h"
#include "iirfilter.h"
#include "chrome.h"
#include "colormap.h"
#include "theme.h"
#include "loader.h"
#include "renderer.h"
#include "segy_mmap.h"
#include "segy_pyramid.h"
#include "segy_writer.h"
#include "threadpool.h"
#include "wiggle.h"
#include "header_fields.h"
#include <array>

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QKeyEvent;
class QResizeEvent;
class QShowEvent;
class QAction;
class QStatusBar;
class QPushButton;
class QDockWidget;
class QLabel;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QRadioButton;
class QCheckBox;
class QComboBox;
class QSplitter;
class QPlainTextEdit;
class QTableWidget;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPainter;
class QProgressBar;
class QTimer;
class QToolButton;
class QGroupBox;
class QScrollArea;
class QHBoxLayout;
class QVBoxLayout;
class QToolBar;
class QAbstractSpinBox;
class QScrollBar;

namespace segyqt {

// Opt-in profiling (see native/README.md, "Profiling"): with the
// environment variable SEGYREAD_PROFILE set to a file path, loads,
// background tasks, slow paints and UI-thread stalls are appended to it.
bool perfEnabled();
void perfLog(const QString& line); // thread-safe; a no-op when disabled

// Left-click behavior on the canvas depends on which toolbar tool is
// active: Arrow only hovers/reads (no drag), Pan drag-pans, BoxSelect
// places/drags the selection box's corners. Wheel-zoom and 'R' reset are
// independent of this -- they work the same in every mode.
enum class ToolMode { Arrow, Pan, BoxSelect, Pick };

// Stored in data (trace/sample) coordinates, not pixels, so it stays
// correctly positioned across pan/zoom -- converted to plot-local pixel
// coordinates fresh every paintEvent, exactly like the density plot
// itself. The 4 visual corners are the combinations of {traceA,traceB} x
// {sampleA,sampleB} (always an axis-aligned rectangle in data space).
//
// Any number of these can coexist on one panel (AppState::selectionBoxes,
// below) -- each gets a distinct color (colorIndex, assigned once at
// creation from a fixed palette that cycles past its own length rather
// than reassigning as boxes are added/removed) so Histogram/Spectrum can
// visibly match "the line/bars are the same color as the box they came
// from." Which one Delete/Zoom/Histogram/Spectrum act on, and the
// drag-in-progress state, are tracked on AppState instead of here --
// see activeBoxIndex/draggingBoxIndex/draggingCorner.
struct SelectionBox {
    bool hasFirstCorner = false;
    bool complete = false;
    double traceA = 0, sampleA = 0, traceB = 0, sampleB = 0;
    int colorIndex = 0;
};

// HistogramDialog's current settings -- stored on AppState (same
// read-fresh-every-frame, Preferences-ready pattern as DisplaySettings)
// even though nothing consumes it yet; the histogram feature itself is
// still "to be built later" (see native/README.md), only its parameters
// dialog exists so far.
struct HistogramSettings {
    bool useSelection = false; // false = Full View
    int numBins = 51;
    bool useUserClip = false; // false = Data (use the data's own range)
    double userMin = 0.0;
    double userMax = 0.0;
};

// SpectrumDialog's settings. The spectrum is always normalised to its own
// peak (segy::computeSpectrum); the never-wired normalise modes are gone.
struct SpectrumSettings {
    bool useSelection = false; // false = the visible data
    int smoothPoints = 1; // one of kSpectrumSmoothPoints in viewer_qt.cpp
};

// How wiggles are drawn (Display Parameters). Every field is read by the
// wiggle painter; the placeholders that were never wired (fill styles,
// blending, overlap, baselines, spacings) are gone.
struct WigglePresentationSettings {
    bool centerLine = false;
    bool positiveFill = true;
    bool negativeFill = false;
};

// Idents: small rows of trace-header values shown above/below the plot
// (for visual trace-position orientation), one optional line-graph "ident
// plot" above those, and up to two header fields drawn as colored lines
// directly over the seismic (the field's raw value reinterpreted as a
// time in ms) -- modeled on a reference seismic tool's own Idents
// submenu. Scoped to the 7 fields segy::TraceHeader already decodes --
// see native/README.md. Per-panel (AppState), read fresh every frame
// like DisplaySettings, and deliberately NOT persisted (confirmed with
// the user: idents reset to all-off every session, unlike Preferences
// below).
enum class IdentField { TraceSequenceLine, TraceSequenceFile, FieldRecord, TraceNumber, Cdp, Offset, X, Y };
constexpr int kIdentFieldCount = 8;
struct IdentSettings {
    // Index = IdentField, in its declaration order -- fixed, so toggling
    // one field on/off never reshuffles the other enabled rows' paint
    // order. Max-4/max-2 is enforced where these are written (the
    // Idents menu's handlers), not here.
    bool topEnabled[kIdentFieldCount] = {};    // max 4 true
    bool bottomEnabled[kIdentFieldCount] = {}; // max 2 true
    int plotField = -1;        // IdentField index, or -1 = ident plot hidden
    int overlayCyanField = -1; // -1 = no cyan line on the seismic
    int overlayRedField = -1;  // -1 = no red line on the seismic
};

// App-wide (not per-panel/per-dataset) preferences -- persisted to
// $HOME/.segyread_settings via QSettings (see MainWindow::loadPreferences/
// savePreferences), unlike every other *Settings struct in this file,
// which lives on a per-panel AppState and resets with a fresh session.
enum class ToolbarPosition { Left, Top, Bottom, Right };
enum class DrawerPosition { Left, Right };
enum class DatasetLabelPosition { TopLeft, BottomLeft, TopRight, BottomRight, Off };
struct Preferences {
    ToolbarPosition toolbarPosition = ToolbarPosition::Left;
    // Only independently meaningful when the toolbar is Top/Bottom -- when
    // the toolbar is Left/Right, the drawer is always forced to the other
    // side (see PreferencesDialog), but still stored here so it's whatever
    // was last explicitly chosen if the toolbar later moves back to
    // Top/Bottom.
    DrawerPosition drawerPosition = DrawerPosition::Right;
    // Windows is already per-monitor DPI-aware, so the control is disabled
    // there and this is always 100 -- see PreferencesDialog. On Linux this
    // exists because there's no reliable way to *detect* the desktop's
    // scaling (WSLg in particular reports none at all, see
    // native/README.md), so the user sets it to match manually. Applied via
    // QT_SCALE_FACTOR, which Qt only reads at QApplication construction, so
    // a changed value takes effect on next launch, not live.
    double uiScalePercent = 100.0;
    // Toolbar buttons the user switched off (MainWindow::ToolbarItem ids),
    // and optional buttons (off until chosen) the user switched on; both
    // edited only in File > Preferences > Toolbar Buttons.
    QStringList hiddenToolbarItems;
    QStringList shownToolbarExtras;
    // Analysis > Signal/Noise (SIGCOR) parameters, edited in the
    // Signal/Noise window itself (persisted here). Defaults are the user's own SIPMAP deck:
    // NSKIP=3 IRANSK=2 SPECTR TCROSS=BASIC SHMAX=30 CCMIN=0.3, cosine-squared
    // window 5% flat, normalise and remove DC on.
    segy::SigcorParams signalNoise;
};

// One toolbar button as Preferences lists it: stable id + display label.
struct ToolbarItemInfo {
    QString id;
    QString label;
    QString category; // Preferences groups consecutive items of one category under one heading
    bool optional = false; // off until the user ticks it
};

// One loaded SEG-Y dataset's data -- kept alive independent of which
// panel/slot currently references it (see native/README.md, "Dataset
// pool: Foreground/Background"). Held via shared_ptr so switching a
// panel's Foreground/Background to an already-loaded dataset is an O(1)
// pointer swap, never a reload: MainWindow::datasetPool_ is the one place
// that can hold the *last* reference (deletion drops it there), while any
// number of panels' AppState::foreground/background may also point at it
// meanwhile. Never mutate a Dataset's fields in place once constructed --
// always build a new one and reassign the shared_ptr (see
// SegyCanvas::startLoading) -- a dataset may be shared with the pool or
// another panel's slot, and in-place mutation would corrupt it for them.
// A picked horizon (see native/README.md, "Horizons"): one time in ms per
// trace of its dataset (NaN = no pick), plus the traces the user clicked
// (anchors; picks between consecutive anchors are filled automatically).
struct Horizon {
    QString name;
    QColor color;
    std::vector<float> timeMs;
    std::vector<int64_t> anchors; // in click order
};

struct Dataset {
    std::string name; // display name -- filePath.stem(), shown in the Foreground/Background boxes and pool dialog
    // File > Dataset Manager's Show tick: listed in the Foreground/Background
    // picker. Unticking a displayed dataset empties its panel.
    bool listed = true;
    std::filesystem::path filePath;
    segy::MappedFile file;
    segy::BinaryHeader binHeader{};
    int64_t traceCount = 0;
    size_t traceStrideBytes = 0;
    segy::Pyramid pyramid;
    // Empty for a file opened directly via File > Open; populated when
    // Calculator/Bandpass synthesize a new dataset (one entry per step
    // that produced it, oldest first) -- see SegyCanvas::startLoading's
    // `processingHistory` parameter. Save SEG-Y writes this straight into
    // the output file's own EBCDIC text header, so a chain of derived
    // datasets keeps a readable record of how each one was produced.
    std::vector<std::string> processingHistory;
    // Non-empty for File > Read Attribute SEG-Y: what the values are
    // ("Interval velocity", "Epsilon", ...). Attribute datasets go to the
    // dataset list only (not shown on load); the idea is to use them as a
    // Background. Shown next to the name in the dataset picker.
    std::string attribute;
    // User horizons on this dataset (in memory; File > export/import as
    // CSV). Annotations, not data: edited on the UI thread only, the one
    // exception to "never mutate a Dataset in place".
    std::vector<Horizon> horizons;
    int activeHorizon = -1;
    // Domain flag (SEG-Y does not record it): false = time (the default),
    // true = depth (samples in m, sample interval in mm). Set by View >
    // Data Domain or by Time-Depth Conversion; derived datasets inherit it
    // through the "DOMAIN: ..." line of their processing history.
    bool depth = false;
    // Metres between neighbouring traces, for the scale bar and the f-k
    // filter: -1 = not looked up yet, 0 = unknown (no coordinates; the user
    // can type it with View > Trace Spacing).
    double traceSpacingM = -1.0;
    // Raw trace-header integers already read, by (first byte, size): a
    // cache for Flatten on a header time (UI thread only).
    std::map<std::pair<int, int>, std::vector<int32_t>> headerFieldCache;
};

// Same fields as the removed shells' AppState -- one shared struct instead
// of two divergent copies. Owned by MainWindow, referenced by SegyCanvas.
// The x-f display toggle (see native/README.md, "x-f display"): while
// active, the panel shows a computed dataset of per-trace amplitude spectra
// (dB) and everything the seismic view had is parked here, to be restored
// verbatim when the toggle goes off.
struct XfState {
    bool active = false;
    std::shared_ptr<Dataset> seismic; // the Foreground to go back to
    segy::ViewRange view;
    segy::DisplaySettings display;
    std::vector<SelectionBox> selectionBoxes;
    int activeBoxIndex = -1;
    std::vector<segy::ViewRange> zoomHistory;
    int64_t traceOffset = 0; // x-f trace 0 = this seismic trace
    int64_t traceStride = 1; // x-f trace k = seismic trace traceOffset + k * traceStride
    // The x-f color scale, remembered across toggles within a session
    // (default green -40 dB .. red 0 dB; the user may change it in x-f).
    segy::ColorScale colorScale = segy::ColorScale::RedYellowGreen;
    bool colorScaleFlipped = true;
};

// Horizons > Flatten as a display (see native/README.md, "Horizons"): the
// panel shows a flattened copy that is not in the dataset list; switching
// it off goes back to `original` with the same view.
struct FlattenState {
    bool active = false;
    std::shared_ptr<Dataset> original;
};

struct AppState {
    segy::ThreadPool pool{std::thread::hardware_concurrency()};

    bool loaded = false;
    bool loading = false;
    std::atomic<int64_t> progressDone{0};
    std::atomic<int64_t> progressTotal{1};

    // Never null -- defaults to an empty, never-loaded Dataset rather than
    // nullptr, so every existing call site that reads e.g.
    // foreground->pyramid.globalMin without first checking `loaded` (there
    // are several, relying on a loaded-or-default-zero value) stays safe.
    // `loaded` is still its own independent bool (not derived from this),
    // matching exactly the window during startLoading/a failed load where
    // the old behavior already showed "nothing loaded" before/without
    // necessarily touching this pointer.
    std::shared_ptr<Dataset> foreground = std::make_shared<Dataset>();
    // Drawn left of the curtain when set (see SegyCanvas::curtainActive);
    // an empty Dataset means "(None)".
    std::shared_ptr<Dataset> background = std::make_shared<Dataset>();
    // The fence (toolbar Fence, off by default): the Background on one
    // side, the Foreground on the other. Vertical: Background left of
    // `curtainFraction` of the plot width; horizontal: Background above
    // that fraction of its height. `gripFraction` is where the grip sits
    // along the fence. Both dragged by the user.
    bool fenceOn = false;
    bool fenceHorizontal = false;
    double curtainFraction = 0.5;
    double gripFraction = 0.5;
    // View > Scale Bar (on by default; needs a trace spacing).
    bool showScaleBar = true;

    segy::ViewRange view;
    double lastRenderMs = 0.0;

    bool dragging = false;
    int dragAnchorX = 0, dragAnchorY = 0;
    segy::ViewRange dragAnchorView;

    // Not yet user-editable -- Edit > Preferences will flip these once it
    // exists (see native/README.md). Rendering already reads them fresh
    // every frame, so that hookup will be the only thing Preferences needs.
    segy::DisplaySettings display;
    HistogramSettings histogram;
    SpectrumSettings spectrum;
    WigglePresentationSettings wigglePresentation;
    IdentSettings idents;

    ToolMode tool = ToolMode::Arrow;
    // Any number of boxes can coexist (e.g. a large one left over from an
    // earlier zoom, plus a new smaller one drawn inside it) -- see
    // SelectionBox's own comment. activeBoxIndex is which one a right-click
    // most recently selected (-1 = none); Delete/Zoom/Histogram/Spectrum
    // all act on it, and it's drawn with a thicker outline so which one is
    // active is obvious. draggingBoxIndex/draggingCorner are transient
    // interaction state for an in-progress corner drag (-1 = not dragging);
    // kept here rather than per-box since only one box is ever mid-drag.
    std::vector<SelectionBox> selectionBoxes;
    int activeBoxIndex = -1;
    int draggingBoxIndex = -1;
    int draggingCorner = -1;
    // Monotonically increasing, never reused -- so an existing box's color
    // never changes just because an earlier box was deleted. Colors cycle
    // (boxColorForIndex()) once more boxes exist than the palette has.
    int nextBoxColorIndex = 0;
    // Pushed to only by the toolbar's Zoom button, popped by Mooz -- wheel
    // zoom/pan/reset don't touch this, so "remember all zoom levels" means
    // all box-zoom levels, not every wheel notch.
    std::vector<segy::ViewRange> zoomHistory;

    std::vector<uint32_t> pixels;

    XfState xf;
    FlattenState flatten;

    // Lock View: the view (zoom/pan/axis ranges) can't change -- see
    // MainWindow's Lock View action and SegyCanvas's guards.
    bool viewLocked = false;
};

class SegyCanvas;

// One side of the split view -- each panel owns its own file/view/tool
// state completely independently (two different, or the same, datasets
// open side by side). MainWindow's toolbar/menu actions target whichever
// panel is "active" (last clicked), or both at once when the Lock toggle
// is on -- see MainWindow::targetPanels().
struct Panel {
    AppState app;
    SegyCanvas* canvas = nullptr;
};

// Reported to MainWindow (dock's Trace Info section) whenever the Arrow
// tool's hover position changes -- see SegyCanvas::currentHoverInfo().
struct HoverInfo {
    bool valid = false;
    int64_t traceIdx = 0;
    int32_t seqNum = 0;
    double timeMs = 0.0; // Hz on an x-f display (frequencyAxis)
    float amplitude = 0.0f;
    bool frequencyAxis = false; // x-f: timeMs is Hz, amplitude is dB
    bool depthAxis = false;     // depth data: timeMs is m
    const Dataset* dataset = nullptr; // the hovered panel's Foreground (Trace Inspector's Follow cursor)
    const void* sourceCanvas = nullptr; // the hovered SegyCanvas (linked crosshair)
    double traceCoord = 0.0;            // continuous trace position under the cursor
};

// A tall, narrow amplitude-to-color legend (gradient plus +clip/0/-clip
// labels), overlaid directly on the seismic display area -- replacing the
// color bar that used to be drawn into the canvas's pixel buffer by
// chrome.cpp (see native/README.md for why that relocation doesn't touch
// chrome.cpp/renderer.cpp at all) and, briefly, a dock-widget version of
// this same legend (also documented in native/README.md). Paints its own
// semi-transparent dark backing so it stays legible sitting on top of
// whatever seismic colors happen to be underneath it. Right-clickable (see
// SegyCanvas's constructor) to choose a color scale from visual previews or
// flip it -- no longer transparent-for-mouse-events like it briefly was as
// a pure overlay, since it now needs to receive that click itself.
class AmplitudeScaleWidget : public QWidget {
public:
    explicit AmplitudeScaleWidget(QWidget* parent = nullptr);
    // posClip/negClip are always positive magnitudes -- see
    // renderer.h's ManualClip. Equal values (the common, non-manual-clip
    // case) reproduce the old single-clip symmetric legend exactly; when
    // they differ, "0" on the gradient sits off-center at the matching
    // fraction and the two end labels show their own distinct values.
    void setClipRange(float posClip, float negClip);
    void setColorScale(segy::ColorScale scale, bool flipped);
    // Value at the middle of the scale (ManualClip::center) and a unit
    // suffix for the labels -- x-f shows "0 dB" / "-20 dB" / "-40 dB".
    void setCenter(float center, const QString& unit);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    float posClip_ = 1.0f;
    float negClip_ = 1.0f;
    float center_ = 0.0f;
    QString unit_;
    segy::ColorScale colorScale_ = segy::ColorScale::RedWhiteBlue;
    bool colorScaleFlipped_ = false;
};

// The histogram-with-two-draggable-lines control inside ClipDialog, below.
// Bars: the same computeHistogram() bin data every other histogram display
// in this app uses. The two vertical lines mark -negClip/+posClip; dragging
// either fires onChanged() continuously (ClipDialog pushes that straight
// into AppState::display.manualClip for a live-updating seismic display).
// Both lines are pinned to stay strictly on their own side of zero -- they
// can never cross, by construction (each is stored as a positive magnitude,
// not a signed position).
class ClipHistogramWidget : public QWidget {
public:
    explicit ClipHistogramWidget(QWidget* parent = nullptr);
    void setHistogram(const segy::HistogramResult& result);
    // The data values at the bottom and top of the colour scale (low < high).
    void setClipValues(float low, float high);
    void setSymmetrical(bool symmetrical);

    std::function<void(float low, float high)> onChanged;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    segy::HistogramResult result_;
    float low_ = -1.0f;
    float high_ = 1.0f;
    bool symmetrical_ = true;
    int draggingLine_ = -1; // -1 none, 0 = low (left) line, 1 = high (right) line

    QRect chartRect() const;
    int xForValue(float value) const;
    float valueForX(int x) const;
    int hitTestLine(int x) const;
    void setValueFromDrag(int lineIndex, float value);
};

// Edit > (Display Parameters' gain popup already covers gain; this is the
// amplitude scale legend's own right-click "Clip...") -- lets the user pick
// an exact positive/negative clip by dragging two lines over a histogram of
// the *currently displayed* traces/samples (AppState::view, not a
// selection box or the whole file -- a third, dialog-local scoping
// distinct from Histogram/Spectrum's Full View/Selection). Non-modal, same
// lazy-create/Apply-without-closing shape as the other parameter dialogs.
// Live: every drag and every spin-box edit immediately updates
// AppState::display.manualClip and repaints the canvas -- Apply/OK don't
// "commit" that (it's already live), they just decide whether the dialog
// itself keeps a wider live-editing scope (Apply also re-bins this
// dialog's own histogram to the current clip range) or closes. Cancel
// reverts the live clip to whatever it was when the dialog opened.
class ClipDialog : public QDialog {
public:
    explicit ClipDialog(AppState& app, QWidget* parent = nullptr);
    // Fired on every live change (drag, spin-box edit, Apply/OK/Cancel) so
    // the caller can repaint the canvas and the amplitude scale legend.
    void setChangedCallback(std::function<void()> callback);

protected:
    void showEvent(QShowEvent* event) override;

private:
    AppState& app_;
    ClipHistogramWidget* histogramWidget_ = nullptr;
    QDoubleSpinBox* lowSpin_ = nullptr;
    QDoubleSpinBox* highSpin_ = nullptr;
    QCheckBox* symmetricalCheck_ = nullptr;
    segy::ManualClip original_;
    std::function<void()> onChanged_;

    void recomputeHistogram(float rangeMin, float rangeMax);
    void recomputeHistogramAround(float low, float high);
    void applyLive(float low, float high);
};

// Parameters dialog for the Histogram feature (see native/README.md).
// A single instance is created lazily and reused (MainWindow owns it), so
// edits stay "sticky" across opens like a typical modeless parameters
// palette. Non-modal (shown via show(), not exec()) with Apply that
// writes settings without closing, matching the "Update on Apply"-style
// workflow of similar tool dialogs in other seismic software. OK/Apply
// also fire setAppliedCallback so MainWindow can (re)compute and show the
// actual histogram plot.
class HistogramDialog : public QDialog {
public:
    explicit HistogramDialog(AppState& app, QWidget* parent = nullptr);
    void setAppliedCallback(std::function<void()> callback);

protected:
    void showEvent(QShowEvent* event) override;

private:
    AppState& app_;
    QRadioButton* fullViewRadio_ = nullptr;
    QRadioButton* selectionRadio_ = nullptr;
    QSpinBox* binsSpinBox_ = nullptr;
    QRadioButton* dataClipRadio_ = nullptr;
    QRadioButton* userClipRadio_ = nullptr;
    QDoubleSpinBox* minValueSpin_ = nullptr;
    QDoubleSpinBox* maxValueSpin_ = nullptr;
    std::function<void()> onApplied_;

    void updateClipFieldsEnabled();
    void applySettings();
};

// Parameters dialog for the amplitude-spectrum feature -- same lazy/sticky/
// non-modal/Apply-without-closing shape as HistogramDialog. Field
// names/options (the three Normalize modes, the Smooth window sizes) come
// from a reference frequency-spectrum tool's own Parameters dialog.
class SpectrumDialog : public QDialog {
public:
    explicit SpectrumDialog(AppState& app, QWidget* parent = nullptr);
    void setAppliedCallback(std::function<void()> callback);

protected:
    void showEvent(QShowEvent* event) override;

private:
    AppState& app_;
    QRadioButton* fullViewRadio_ = nullptr;
    QRadioButton* selectionRadio_ = nullptr;
    QComboBox* smoothCombo_ = nullptr;
    std::function<void()> onApplied_;

    void applySettings();
};

// The actual paint surface for HistogramPlotWidget, below -- split out so
// the outer window can put a menu bar above it in a QVBoxLayout (a plain
// QWidget can host a QMenuBar as a normal child widget; it doesn't require
// QMainWindow). Plots computeHistogram()'s result: a bar chart of
// percent-of-total over amplitude, stacked above a same-bins bar chart of
// cumulative percent (not a smoothed curve -- matches the top chart's own
// bins) -- same two charts (over the same shared, regularly-ticked
// amplitude axis) as the reference tool's own Histogram window. The
// cumulative chart's background is pure black, not just this app's usual
// dark canvas tone, per the original request.
class HistogramChartArea : public QWidget {
public:
    explicit HistogramChartArea(QWidget* parent = nullptr);
    // sourceColor: white when computed over the Full View, or the
    // originating selection box's own color when computed over a
    // selection -- see MainWindow::computeAndShowHistogram.
    void setResult(const segy::HistogramResult& result, QColor sourceColor);
    void setGridLinesVisible(bool visible);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    segy::HistogramResult result_;
    QColor sourceColor_ = Qt::white;
    bool gridLinesVisible_ = true;
};

// Standalone result window (not a QDialog -- it's a display, not something
// with OK/Cancel semantics), with a File/Edit/View/Utilities menu bar
// (File > Close; Edit intentionally empty, matching this app's own
// convention for a menu with nothing to do yet; View > Grid Lines toggles
// HistogramChartArea::setGridLinesVisible; Utilities > Parameters... reopens
// the same HistogramDialog the toolbar's own Histogram button does, via
// setParametersCallback) above a HistogramChartArea.
class HistogramPlotWidget : public QWidget {
public:
    explicit HistogramPlotWidget(QWidget* parent = nullptr);
    void setResult(const segy::HistogramResult& result, QColor sourceColor);
    void setParametersCallback(std::function<void()> callback);

private:
    HistogramChartArea* chart_ = nullptr;
    std::function<void()> onParameters_;
};

// One curve of a DbCurveChart: dB values on the chart's shared frequency
// axis (NaN = no value, the line breaks there).
struct DbCurve {
    QString name;
    QColor color;
    std::vector<float> db;
    bool visible = true;
    bool dashed = false;
};

// Zoom and axis-range machinery shared by the dB charts and the F-K chart,
// working like the seismic display: Box Select draws a box, Zoom zooms to
// it, Unzoom steps back through the zoom history (to the automatic range
// at the bottom); right-click on an axis gives Axis Ranges... / Reset
// Axes. Subclasses supply chartRect() and the automatic range, and draw
// with view_.
class ZoomableChart : public QWidget {
public:
    explicit ZoomableChart(QWidget* parent = nullptr);
    // Charts in one group zoom together: a box drawn on any of them, Zoom,
    // Unzoom, Axis Ranges and Reset Axes apply to all (side-by-side f-k
    // plots). `group` includes this chart; empty = on its own.
    void linkGroup(const std::vector<ZoomableChart*>& group);
    void setBoxMode(bool on);
    bool canZoom() const;
    bool canUnzoom() const { return manualRange_; }
    void zoomToBox();
    void unzoom();
    void setAxisRanges(double x0, double x1, double y0, double y1); // pushes the current range first
    void resetAxes();
    // Fires whenever canZoom()/canUnzoom() may have changed, for the toolbar buttons.
    void setZoomStateCallback(std::function<void()> callback);

protected:
    struct Range {
        double x0 = 0.0, x1 = 1.0, y0 = 0.0, y1 = 1.0;
    };
    virtual QRect chartRect() const = 0;
    virtual void viewChanged() { update(); }
    void setAutoRange(const Range& range); // also the view, unless zoomed
    QPointF dataAt(const QPointF& pos) const;
    QPointF pixelAt(double x, double y) const;
    // Mouse helpers for the box; each returns true when it used the event.
    bool handleBoxPress(QMouseEvent* event);
    bool handleBoxMove(QMouseEvent* event);
    bool handleBoxRelease(QMouseEvent* event);
    void paintBox(QPainter& painter) const;
    void showAxisMenu(const QPoint& globalPos, const QString& xName, const QString& yName, int xDecimals,
                      int yDecimals);
    // The app's zoom gestures (see native/README.md, "Zoom"): double-click
    // goes back one zoom, the wheel zooms around the cursor.
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

    Range auto_;
    Range view_;
    bool manualRange_ = false;
    std::vector<Range> history_; // empty with manualRange_ = back to auto
    bool boxMode_ = true; // dragging always draws a zoom box
    bool draggingBox_ = false;
    bool hasBox_ = false;
    QPointF boxA_, boxB_; // data coordinates

private:
    void notifyZoomState();
    std::vector<ZoomableChart*> peers() const; // the group, or just this
    void applyBoxMode(bool on);
    void applyAxisRanges(const Range& range);
    void applyUnzoom();
    void applyResetAxes();
    void applyWheelRange(const Range& range, bool newGesture);
    qint64 lastWheelMs_ = -1000000;
    std::function<void()> onZoomState_;
    std::vector<ZoomableChart*> group_;
};

// dB vs. frequency line chart shared by Spectrum and Signal/Noise: nice
// 1-2-5 ticks and gridlines on both axes, one dB axis for every curve, and
// an optional crosshair that snaps to the nearest frequency bin, marks each
// visible curve there and reports it through the hover callback (index -1
// when the cursor leaves) so a CurveReadoutPanel beside it can show values.
// Zoom and axis ranges: see ZoomableChart.
class DbCurveChart : public ZoomableChart {
public:
    explicit DbCurveChart(QWidget* parent = nullptr);
    void setCurves(std::vector<float> frequencyHz, std::vector<DbCurve> curves);
    void setCurveVisible(int index, bool visible);
    void setTitle(const QString& title);
    void setEmptyText(const QString& text);
    void setGridLinesVisible(bool visible);
    void setCrosshairEnabled(bool enabled);
    void setHoverCallback(std::function<void(int frequencyIndex, double cursorDb)> callback);
    // For charts that are not dB vs. frequency (the phase scan): axis names,
    // a fixed y range, and vertical marker lines.
    void setAxisLabels(const QString& xLabel, const QString& yLabel);
    void setFixedYRange(double y0, double y1);
    void setMarkers(std::vector<std::pair<double, QColor>> markers);
    const std::vector<float>& frequencies() const { return frequencyHz_; }
    const std::vector<DbCurve>& curves() const { return curves_; }

protected:
    QRect chartRect() const override;
    void viewChanged() override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    std::vector<float> frequencyHz_;
    std::vector<DbCurve> curves_;
    QString title_ = "Amplitude (dB) vs. Frequency (Hz)";
    QString xLabel_ = "Frequency (Hz)";
    QString yLabel_ = "Level (dB)";
    bool fixedY_ = false;
    double fixedY0_ = 0.0, fixedY1_ = 1.0;
    std::vector<std::pair<double, QColor>> markers_;
    QString emptyText_;
    bool gridLinesVisible_ = true;
    bool crosshairEnabled_ = true;
    int hoverIndex_ = -1;
    double hoverY_ = 0.0;
    std::function<void(int, double)> onHover_;

    void updateRange();
};

// Side panel beside a DbCurveChart, styled like the main window's Trace
// Info dock: the frequency and dB level under the cursor, then each
// visible curve's value at that frequency (with its color swatch).
class CurveReadoutPanel : public QWidget {
public:
    explicit CurveReadoutPanel(QWidget* parent = nullptr);
    // Rebuilds the rows from the chart's curves (names, colors, visibility).
    void syncRows(const DbCurveChart& chart);
    // frequencyIndex -1 = cursor outside the chart: every value shows "--".
    void showValues(const DbCurveChart& chart, int frequencyIndex, double cursorDb);
    // A PEAK section: the frequency of each curve's maximum (spectra).
    void setPeakSectionVisible(bool visible);

private:
    QLabel* frequencyValue_ = nullptr;
    QLabel* cursorValue_ = nullptr;
    QWidget* curveRows_ = nullptr;
    std::vector<std::pair<QWidget*, QLabel*>> rows_; // per curve: row, value label
    QLabel* peakCaption_ = nullptr;
    QWidget* peakRows_ = nullptr;
    std::vector<std::pair<QWidget*, QLabel*>> peakRowsList_;
    bool peakVisible_ = false;
    void syncPeaks(const DbCurveChart& chart);
};

// Writes the chart's visible curves as CSV (frequency, then one dB column
// per curve, a header row with the names). False when cancelled/failed.
bool saveCurvesCsv(QWidget* parent, const DbCurveChart& chart, const QString& suggestedName);

// Same File/Edit/View/Utilities menu bar shape as HistogramPlotWidget,
// Utilities > Parameters... reopening SpectrumDialog instead.
// One spectrum in the Spectrum window: computed (a box or the view) or
// loaded from CSV. `key` identifies the source, so recomputing the same
// box replaces its curve instead of adding another.
struct SpectrumEntry {
    QString key, name;
    QColor color;
    std::vector<float> frequencyHz, db;
    bool visible = true;
};

class SpectrumPlotWidget : public QWidget {
public:
    explicit SpectrumPlotWidget(QWidget* parent = nullptr);
    // Adds a spectrum, or replaces the one with the same key.
    void addSpectrum(SpectrumEntry entry);
    // A colour for a spectrum that has none of its own (view, loaded).
    QColor nextColor();
    void setParametersCallback(std::function<void()> callback);
    void openManager();

private:
    DbCurveChart* chart_ = nullptr;
    CurveReadoutPanel* readout_ = nullptr;
    std::function<void()> onParameters_;
    std::vector<SpectrumEntry> entries_;
    QWidget* manager_ = nullptr;
    QListWidget* managerList_ = nullptr;
    int colorCounter_ = 0;
    void rebuildChart();
    void refreshManager();
    void loadCsv();
};

// Edit > Preferences. App-wide, not per-panel/per-dataset -- see
// Preferences above. Toolbar Position is a plain 4-way dropdown; Drawer
// Position is two radio buttons that are only independently choosable
// when the toolbar is Top/Bottom -- with the toolbar Left/Right, the
// drawer is forced to the opposite side and the radio buttons disable
// themselves, pre-selected to whichever side that forces.
// File > Open, modeled on a reference seismic tool's "2D Seismic File
// Selection" dialog (Load Options panel above a custom Filter/Directories/
// Files/Selection browser), rather than the platform-native QFileDialog.
// The reference's inert extras (a trace Range/Min/Max subset, "Use Default
// Select/Sort Values", "Clip at abs max/min") were removed: they did
// nothing. "Use clipping from previously loaded window" is wired:
// MainWindow::openFile()
// reads it and, when unchecked (the reference tool's own default),
// resets gain to 0 dB so each newly opened file gets a fresh auto-scaled
// clip instead of inheriting whatever gain the previous file was left at.
// Opened from a Foreground/Background box's icon button (see
// DatasetSlotWidget below) -- lists every dataset in MainWindow's shared
// pool, lets the user pick one for that slot (OK) or drop one from memory
// entirely (Delete), matching the reference tool's own "OK/Delete/Cancel"
// picker. One instance is lazily created and reused for all four
// Foreground/Background icons across both panels, reconfigured fresh via
// setDatasets()/setCallbacks() right before each show() -- same
// lazy-create/reconfigure-before-show pattern as every other on-demand
// dialog in this file.
class DatasetPickerDialog : public QDialog {
public:
    explicit DatasetPickerDialog(QWidget* parent = nullptr);

    // `datasets` is the pool (MainWindow::datasetPool_); `current` is
    // pre-selected if it's one of them. `allowNone` adds a synthetic
    // "(None)" row at the top (Background only -- Foreground always needs
    // something to render); picking it calls `onPicked` with a fresh empty
    // Dataset rather than any pool entry. `isInUse` gates the Delete
    // button per-row -- a dataset currently assigned to *any* panel's
    // Foreground/Background (including the slot this dialog was opened
    // for) can't be deleted out from under it.
    void setDatasets(const std::vector<std::shared_ptr<Dataset>>& datasets, const std::shared_ptr<Dataset>& current,
                      bool allowNone, std::function<bool(const std::shared_ptr<Dataset>&)> isInUse);
    // `onPicked` fires once, on OK, with the selected row's dataset (or a
    // fresh empty one for "(None)"). `onDeleted` fires once per Delete
    // click (the dialog stays open afterward -- deleting doesn't imply a
    // pick), with the removed dataset, so MainWindow can drop it from the
    // pool.
    void setCallbacks(std::function<void(std::shared_ptr<Dataset>)> onPicked,
                       std::function<void(std::shared_ptr<Dataset>)> onDeleted);

private:
    QListWidget* list_ = nullptr;
    QPushButton* okButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    // Row i's dataset is datasets_[i]; a null entry is the synthetic
    // "(None)" row.
    std::vector<std::shared_ptr<Dataset>> datasets_;
    std::function<bool(const std::shared_ptr<Dataset>&)> isInUse_;
    std::function<void(std::shared_ptr<Dataset>)> onPicked_;
    std::function<void(std::shared_ptr<Dataset>)> onDeleted_;

    void updateButtonsEnabled();
};

// One Foreground or Background box, overlaid on a panel's SegyCanvas (same
// technique as AmplitudeScaleWidget) -- a name label plus a small icon
// button that opens DatasetPickerDialog scoped to this slot. See
// native/README.md, "Dataset pool: Foreground/Background" -- positioning
// (top-left of this panel's own plot area, aligned with its left axis) is
// handled by whoever owns this widget (SegyCanvas), not by the widget
// itself.
class DatasetSlotWidget : public QWidget {
public:
    // `label` is the fixed prefix ("Foreground 1", "Background 2", etc.)
    DatasetSlotWidget(const QString& label, QWidget* parent = nullptr);
    void setLabel(const QString& label);
    void setDatasetName(const QString& name);
    void setSlotWidth(int width);
    void setPickCallback(std::function<void()> callback);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString label_;
    QString datasetName_;
    QPushButton* pickButton_ = nullptr;
    std::function<void()> onPick_;
};

class SegyOpenDialog : public QDialog {
public:
    explicit SegyOpenDialog(QWidget* parent = nullptr);
    // Empty if the dialog was cancelled or nothing valid was ever selected.
    std::filesystem::path selectedPath() const;
    bool keepPreviousClip() const;

private:
    QCheckBox* keepPrevClipCheck_ = nullptr;

    QLineEdit* filterEdit_ = nullptr;
    QListWidget* dirList_ = nullptr;
    QListWidget* fileList_ = nullptr;
    QLineEdit* selectionEdit_ = nullptr;

    void refreshLists();
    void navigateTo(const QString& dirName);
    void tryAccept();
};

// File > Open with Analysis: look at a SEG-Y file before reading it (see
// native/README.md, "Open with Analysis"). Tabs: Trace Headers (byte rows x
// trace columns, by standard or raw 2I/4I/4F/4B), Mapping (where this file
// keeps the fields the app uses; scan, text-header proposal, templates),
// Selection (trace range and every Nth, header key ranges) and File Header
// (text, extended text, binary labelled or raw). Notes say what was found
// or assumed. Open reads the file directly when nothing differs from the
// standard; otherwise MainWindow writes a normalised copy and opens that.
class ImportDialog : public QDialog {
public:
    ImportDialog(const std::filesystem::path& path, QWidget* parent = nullptr);
    bool fileOk() const { return fileOk_; }
    QString openError() const { return openError_; }
    const segy::HeaderMapping& mapping() const { return mapping_; }
    segy::TraceSelection selection() const;
    const segy::SegyLayout& layout() const { return layout_; }
    const segy::BinaryHeader& binaryHeader() const { return bin_; }
    int64_t traceCount() const { return traceCount_; }
    size_t traceStride() const { return stride_; }
    // True when the file can be opened directly (standard mapping, every
    // trace, data at byte 3600).
    bool opensDirectly() const;

private:
    std::filesystem::path path_;
    segy::MappedFile file_;
    bool fileOk_ = false;
    QString openError_;
    segy::BinaryHeader bin_{};
    segy::SegyLayout layout_;
    bool samplesDecodable_ = false;
    int64_t traceCount_ = 0;
    size_t stride_ = 0;
    segy::HeaderMapping mapping_;
    std::string textHeader_;

    // Trace Headers tab
    QComboBox* modeCombo_ = nullptr;
    QSpinBox* startSpin_ = nullptr;
    QSpinBox* countSpin_ = nullptr;
    QSpinBox* skipSpin_ = nullptr;
    QTableWidget* headerTable_ = nullptr;
    // Mapping tab
    QTableWidget* mappingTable_ = nullptr;
    QLabel* scanStatus_ = nullptr;
    bool scanCurrent_ = false;
    std::vector<int64_t> sampleTraces_;              // traces read by the last scan
    std::vector<std::array<uint8_t, 240>> sampleHeaders_;
    // Selection tab
    QSpinBox* firstSpin_ = nullptr;
    QSpinBox* lastSpin_ = nullptr;
    QSpinBox* everySpin_ = nullptr;
    struct RangeRow {
        QComboBox* key = nullptr;
        QDoubleSpinBox* min = nullptr;
        QDoubleSpinBox* max = nullptr;
        QDoubleSpinBox* step = nullptr;
    };
    std::vector<RangeRow> rangeRows_;
    QLabel* selectionEstimate_ = nullptr;
    // Per mapping key from the last scan: min, max, smallest step (0 = one value).
    std::map<std::string, std::array<double, 3>> keyStats_;
    // Right-click > Reset on a selection field: back to the data's own value.
    void addResetMenu(QAbstractSpinBox* spin, std::function<void()> reset);
    void resetAllSelection();
    bool statsFor(const QString& key, std::array<double, 3>& out);
    // Notes and buttons
    QListWidget* notes_ = nullptr;
    QPushButton* openButton_ = nullptr;

    const uint8_t* traceHeader(int64_t trace) const;
    void addNote(const QString& text);
    void refreshHeaderTable();
    void fillMappingTable();
    void readMappingTable();
    void scanMapping();
    void markScanOutOfDate();
    void refreshEstimate();
    void proposeFromText();
    void loadTemplate();
    void saveTemplate();
};

// Copies a table (with its column headers) as tab-separated text.
void copyTableToClipboard(QTableWidget* table);

class PreferencesDialog : public QDialog {
public:
    // `toolbarItems`: every toolbar button, in toolbar order, for the
    // Toolbar Buttons checkboxes.
    PreferencesDialog(const Preferences& initial, const std::vector<ToolbarItemInfo>& toolbarItems,
                      QWidget* parent = nullptr);
    void setAppliedCallback(std::function<void(const Preferences&)> callback);

private:
    QComboBox* toolbarPositionCombo_ = nullptr;
    std::vector<std::pair<QString, QCheckBox*>> toolbarItemChecks_; // id -> "show this button"
    QRadioButton* drawerLeftRadio_ = nullptr;
    QRadioButton* drawerRightRadio_ = nullptr;
    QSpinBox* uiScaleSpin_ = nullptr;
    // Signal/Noise parameters that are not in the Signal/Noise window.
    segy::SigcorParams initialSignalNoise_;
    QComboBox* snMethodCombo_ = nullptr;
    QSpinBox* snRandomSpin_ = nullptr;
    QDoubleSpinBox* snMaxShiftSpin_ = nullptr;
    QDoubleSpinBox* snBetaSpin_ = nullptr;
    QCheckBox* snEqualizeCheck_ = nullptr;
    QCheckBox* snRemoveDcCheck_ = nullptr;
    std::function<void(const Preferences&)> onApplied_;

    void updateDrawerControlState();
    Preferences currentPreferences() const;
    void applySettings();
};

// Shows the file's 3200-byte EBCDIC text header, decoded to ASCII (see
// segy::decodeEbcdicText) -- 40 lines of 80 characters each, with no CR/LF
// in the source data at all (a fixed-layout punch-card-era convention, not
// paragraphs), so the display re-wraps it into 80-column lines itself and
// is a fixed 80x40 character grid the user cannot resize narrower/wider --
// that would misrepresent the actual on-disk layout. A plain QWidget
// (standalone window), not a QDialog -- a read-only viewer, no OK/Cancel.
class EbcdicHeaderWidget : public QWidget {
public:
    explicit EbcdicHeaderWidget(QWidget* parent = nullptr);
    void setHeaderText(const QString& rawText); // exactly 3200 chars, no newlines

private:
    QPlainTextEdit* textEdit_ = nullptr;
};

// Shows the parsed 400-byte binary header's fields as a simple key/value
// list. Unlike Histogram/Spectrum/DisplayParameters, this has no settings
// to keep "sticky" between opens -- just re-populated from whichever panel
// is active every time the toolbar button is clicked, so it doesn't need
// the "bound to whichever panel was active at first open" workaround those
// three document (see native/README.md).
class BinaryHeaderWidget : public QWidget {
public:
    explicit BinaryHeaderWidget(QWidget* parent = nullptr);
    // `binHeaderBase` must point at file byte offset 3200 (start of the
    // 400-byte binary header) -- reads every meaningful SEG-Y rev1 field
    // directly from the raw bytes (not just the handful segy::BinaryHeader
    // parses), so unlike that struct this doesn't need a shared/tested
    // parser change to grow.
    void setHeader(const uint8_t* binHeaderBase);

private:
    QTableWidget* table_ = nullptr;
    QTableWidget* rawTable_ = nullptr; // every 2- and 4-byte word, unnamed
};

// View > Display Parameters: display mode, wiggle fill and centre line,
// gain and polarity -- every control does something. The mode and gain stay
// in sync with the toolbar and View menu through the callbacks below.
class DisplayParametersDialog : public QDialog {
public:
    explicit DisplayParametersDialog(AppState& app, QWidget* parent = nullptr);

    // Kept in sync with the toolbar's own gain control and seismic-mode
    // toggle, in both directions, the same way the toolbar and View menu
    // already sync with each other.
    void refreshFromAppState();

    // Routes Gain edits through MainWindow::setGainDb so the toolbar
    // slider/spin box stay in sync too -- same plain-callback pattern as
    // SegyCanvas's setStatusWidgets/setHoverInfoCallback, rather than a
    // qobject_cast<MainWindow*>(window()) that would need Q_OBJECT on a
    // class that otherwise has no need for it.
    void setGainChangedCallback(std::function<void(double)> callback);
    // Routes the mode radio through MainWindow::setSeismicMode so the
    // toolbar icon, View menu, amplitude scale, and canvas all stay in
    // sync too -- same reasoning as setGainChangedCallback above.
    void setModeChangedCallback(std::function<void(segy::SeismicDisplayMode)> callback);

protected:
    void showEvent(QShowEvent* event) override;

private:
    AppState& app_;
    QRadioButton* variableDensityRadio_ = nullptr;
    QRadioButton* wiggleRadio_ = nullptr;
    QCheckBox* reversePolarityCheck_ = nullptr;
    QCheckBox* centerLineCheck_ = nullptr;
    QCheckBox* positiveFillCheck_ = nullptr;
    QCheckBox* negativeFillCheck_ = nullptr;
    QDoubleSpinBox* gainSpin_ = nullptr;
    std::function<void(double)> onGainChanged_;
    std::function<void(segy::SeismicDisplayMode)> onModeChanged_;

    void updateWiggleEnabled();
    void applySettings();
};

// View > Amplitude Processing: computed gain (time-gain pairs) and AGC
// (COPILOT.md items 9/10) -- display-time processing, see
// viewer/amplitude.h. Non-modal, and every edit applies immediately (a
// re-render is cheap: one O(samples) pass per visible trace) through the
// changed callback, which MainWindow routes to targetPanels() so Lock
// applies it to both panels. Holds no AppState reference -- MainWindow
// pushes the active panel's settings in via setProcessing(), same loose
// coupling as CalculatorDialog.
class AmplitudeProcessingDialog : public QDialog {
public:
    explicit AmplitudeProcessingDialog(QWidget* parent = nullptr);

    // Loads `processing` into the controls. `recordLengthMs` seeds the
    // default curve the first time computed gain is switched on with no
    // points yet.
    void setProcessing(const segy::AmplitudeProcessing& processing, double recordLengthMs);
    void setOutputName(const QString& name);
    QString outputName() const;
    segy::AmplitudeProcessing collect() const;

private:
    QLineEdit* outputNameEdit_ = nullptr;
    QGroupBox* gainGroup_ = nullptr;
    QTableWidget* gainTable_ = nullptr;
    QGroupBox* agcGroup_ = nullptr;
    QDoubleSpinBox* agcWindowSpin_ = nullptr;
    double recordLengthMs_ = 1000.0;

    void addGainRow(double timeMs, double gainDb);
};

// Edit > Calculator: combines two currently-loaded panels' datasets
// (Add/Subtract) into a new one, auto-named and auto-displayed in the left
// panel (see MainWindow::runCalculator). Holds no AppState/Panel reference
// itself -- MainWindow feeds it the two panels' current load state/names via
// refreshPanels() (called right before each show()) and receives the user's
// choice back through the Apply callback, same loose-coupling pattern as
// DisplayParametersDialog's setGainChangedCallback.
class CalculatorDialog : public QDialog {
public:
    explicit CalculatorDialog(QWidget* parent = nullptr);

    // Every dataset in the dataset list; `defaultA`/`defaultB` index into it
    // (the two panels' Foregrounds, normally).
    void refreshDatasets(const QStringList& names, int defaultA, int defaultB);
    // op: 0 = add, 1 = subtract, 2 = cross-correlate (a popup, no dataset).
    void setApplyCallback(std::function<void(int datasetA, int datasetB, int op, QString outputName)> callback);

private:
    QComboBox* datasetACombo_ = nullptr;
    QComboBox* datasetBCombo_ = nullptr;
    QRadioButton* addRadio_ = nullptr;
    QRadioButton* subtractRadio_ = nullptr;
    QRadioButton* crossRadio_ = nullptr;
    QLineEdit* outputNameEdit_ = nullptr;
    bool outputNameEdited_ = false; // true once the user types into it directly -- stop auto-updating it
    std::function<void(int, int, int, QString)> onApply_;

    void updateDefaultOutputName();
    void emitApply();
};

// Processing > Bandpass Filter: Butterworth (low/high corner, a slope per
// side in dB/octave) or a trapezoid (Ormsby, f1-f2-f3-f4, zero-phase in the
// frequency domain). The Butterworth is
// applied zero-phase with sosfiltfilt -- mirrored (odd) padding at both trace
// ends, see viewer/iirfilter.h -- to every trace of the active panel's
// dataset, auto-named and auto-displayed in the left panel
// (see MainWindow::runBandpass). Same loose-coupling shape as
// CalculatorDialog -- no AppState reference, just a name in and a callback
// out.
class BandpassDialog : public QDialog {
public:
    explicit BandpassDialog(QWidget* parent = nullptr);

    // Called right before each show() with the active panel's dataset name
    // (for the default output name) and whether one is even loaded.
    void refreshSource(const QString& sourceName, bool loaded);
    void setApplyCallback(std::function<void(BandpassRequest request, QString outputName)> callback);

private:
    QComboBox* typeCombo_ = nullptr;     // 0 Butterworth, 1 trapezoid
    QWidget* butterworthPage_ = nullptr;
    QWidget* trapezoidPage_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QDoubleSpinBox* lowSpin_ = nullptr;  // 0 = no low cut (lowpass)
    QDoubleSpinBox* highSpin_ = nullptr; // at/above Nyquist = no high cut (highpass)
    QComboBox* lowSlopeCombo_ = nullptr;  // dB/octave after filtfilt = 12 x order
    QComboBox* highSlopeCombo_ = nullptr;
    QComboBox* edgesCombo_ = nullptr; // segy::EdgePadding
    QDoubleSpinBox* trapezoidSpins_[4] = {}; // f1..f4
    QLineEdit* outputNameEdit_ = nullptr;
    QString sourceName_;
    bool outputNameEdited_ = false;
    std::function<void(BandpassRequest, QString)> onApply_;

    void updateType();
    // Keeps f1 <= f2 <= f3 <= f4 by pushing the neighbours along.
    void enforceTrapezoidOrdering(int edited);

    // Keeps low < high as the user edits either: pushes the other corner
    // out of the way rather than rejecting the edit.
    void enforceOrdering(QDoubleSpinBox* edited);
    void updateDefaultOutputName();
    void emitApply();
};

// File > Save SEG-Y: writes the active panel's current dataset (whatever
// produced it -- a directly-opened file, or a Calculator/Bandpass result)
// to a new file, with a synthesized EBCDIC text header recording the input,
// processing history, and output dataset (see MainWindow::runSaveSegy and
// native/README.md). The EBCDIC preview is display-only -- what actually
// gets written is rebuilt fresh at Save time from the same data.
class SaveSegyDialog : public QDialog {
public:
    explicit SaveSegyDialog(QWidget* parent = nullptr);

    // Called right before each show(): `defaultFileName` seeds the file name
    // field, `datasetSummary` is the one-line "N traces x M samples" label,
    // `ebcdicPreview` is the exact 3200-character text the header would be
    // built from if saved right now (already 40 lines x 80 cols, joined with
    // '\n' for display).
    void setPreview(const QString& defaultFileName, const QString& datasetSummary, const QString& ebcdicPreview);
    void setSaveCallback(std::function<void(QString filePath)> callback);

private:
    QLineEdit* fileNameEdit_ = nullptr;
    QLabel* datasetSummaryLabel_ = nullptr;
    QPlainTextEdit* ebcdicPreview_ = nullptr;
    std::function<void(QString)> onSave_;

    void browseForFile();
};

// One band's rendered result in the Octave Band Display window -- a
// higher-resolution variable-density raster (QImage, same 0x00RRGGBB packing renderFrame()
// itself produces) plus its frequency-range label. `bands[0]` is always the
// unfiltered source; the rest are octave bands, low frequency first.
// One band's filtered traces, kept (rather than a pre-rendered image) so
// each OctaveBandPlotWidget can rasterize at its own exact pixel size on
// resize -- stretching one fixed-size raster with nearest-neighbor scaling
// is what made the panels look gritty. Immutable once built; shared, not
// copied, between the background task and the widget.
struct OctaveBandData {
    std::vector<std::vector<float>> traces; // evenly spaced across the visible range, each the visible samples
    float clip = 1.0f;                      // 99.5th percentile |amplitude| of this band
    float rms = 0.0f;                       // RMS amplitude of this band (for its dB level vs. unfiltered)
    segy::ColorScale colorScale = segy::ColorScale::RedWhiteBlue;
    bool colorScaleFlipped = false;
    bool reversePolarity = false; // the main display's, so the colors match it
};

struct OctaveBandResult {
    struct Band {
        QString label;
        std::shared_ptr<const OctaveBandData> data;
    };
    std::vector<Band> bands;
    double traceStart = 0.0;
    double traceEnd = 0.0;
    int sampleStart = 0;
    int sampleEnd = 0;
    double sampleIntervalUs = 0.0;
    // The source panel's own display clip (auto clip, gain and manual clip,
    // as segy::displayClipPosNeg gives the seismic renderer) -- the
    // default "Same as main display" scaling.
    float mainClipPos = 1.0f;
    float mainClipNeg = 1.0f;
};

// Spawned from the toolbar: bandpass-filters the *visible* traces/samples of
// a chosen panel (Left/Right) into a row of octave bands (2^N Hz steps,
// user-adjustable range), each rendered as its own variable-density panel
// with its own auto-scaled clip. Cached rasters scale on resize, and a
// normalized crosshair follows the cursor across every band -- see
// MainWindow::computeAndShowOctaveBands, which does the actual decode/
// filter/rasterize work and feeds the result back via setResult(). Like
// CalculatorDialog/BandpassDialog, holds no AppState reference itself.
class OctaveBandDialog : public QDialog {
public:
    explicit OctaveBandDialog(QWidget* parent = nullptr);

    void refreshPanels(const QString& nameA, bool loadedA, const QString& nameB, bool loadedB);
    // Selects `panelIndex` (0 = left/panelA_, 1 = right/panelB_) in the
    // Source combo without firing its recompute-on-change handler -- used
    // when the toolbar button opens straight onto the active panel.
    void setSourcePanel(int panelIndex);
    void setResult(const OctaveBandResult& result);
    // Live update of the main display's clip (gain slider, Clip dialog).
    void setMainClip(float clipPos, float clipNeg);
    void setApplyCallback(std::function<void(int sourcePanelIndex, int minFreqHz, int maxFreqHz)> callback);

private:
    QComboBox* sourceCombo_ = nullptr;
    QComboBox* minFreqCombo_ = nullptr;
    QComboBox* maxFreqCombo_ = nullptr;
    // Common (default): every band uses the Unfiltered band's clip, so a
    // band's true strength shows -- a weak 1-2 Hz band reads near-white.
    // Per band: each band auto-clipped to itself, to see a weak band's
    // structure. Switching only re-rasterizes (no re-filtering).
    QComboBox* scaleCombo_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* bandsRow_ = nullptr;
    // Widens the window (never narrows it) so every band column fits without
    // horizontal scrolling, capped to the screen -- called per new result.
    void fitWidthToBands();
    std::vector<std::function<void(double, double, bool)>> bandCrosshairUpdaters_;
    std::vector<std::function<void(float, float)>> bandClipSetters_; // one per band, same order as bandOwnClips_
    std::vector<float> bandOwnClips_;
    float mainClipPos_ = 1.0f;
    float mainClipNeg_ = 1.0f;
    void applyScaling();
    std::function<void(int, int, int)> onApply_;
    QPushButton* recomputeButton_ = nullptr;
    int appliedMinHz_ = -1, appliedMaxHz_ = -1; // what the bands on screen were filtered with
    void refreshRecompute();

    void emitApply();
};

// Analysis > Autocorrelation, and Processing > Calculator's Cross-correlate:
// per-trace normalized correlation over lags -lag..+lag (each side set on
// its own, so the window can be asymmetric), shown as a correlogram (one
// column per trace, the Octave Band Display's raster) beside the average
// over all traces. Above the plots: the peak-to-peak distance (nearest side
// peak of the average >= 0.5, marked on the curve), and Zoom / Unzoom / Box
// on the lag axis. Clicking the average curve twice measures a delta-t. A
// Spectrum toggle (off by default) shows the average's power spectrum
// below. MainWindow computes it and feeds it back via setResult().
struct AutocorrelationResult {
    std::shared_ptr<const OctaveBandData> panel; // traces = per-trace correlations, minLag..maxLag, clip 1
    std::vector<float> average;                  // minLag..maxLag, same length as each panel trace
    double traceStart = 0.0;
    double traceEnd = 0.0;
    int minLagSamples = 0;
    int maxLagSamples = 0;
    double sampleIntervalUs = 0.0;
    int traceCount = 0;
    QString description; // e.g. "A x B" for a cross-correlation
    double traceSpacingM = 0.0; // for the correlogram's scale bar (0: none)
};

class AutocorrelationDialog : public QDialog {
public:
    explicit AutocorrelationDialog(QWidget* parent = nullptr, bool crossCorrelation = false);

    double minLagMs() const; // negative side, as a positive number of ms
    double maxLagMs() const;
    void setResult(const AutocorrelationResult& result);
    void setApplyCallback(std::function<void(double minLagMs, double maxLagMs)> callback);

private:
    QDoubleSpinBox* minLagSpin_ = nullptr;
    QDoubleSpinBox* maxLagSpin_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    QLabel* peakLabel_ = nullptr;
    QDoubleSpinBox* thresholdSpin_ = nullptr; // side-peak threshold for peak-to-peak (default 0.3)
    QPushButton* measureButton_ = nullptr;    // two clicks on the average measure a delta-t
    QPushButton* recomputeButton_ = nullptr;
    std::vector<float> lastAverage_;
    int lastMinLag_ = 0;
    double lastIntervalUs_ = 0.0;
    void refreshPeak();
    void refreshRecompute();
    QAction* zoomAction_ = nullptr;
    QAction* unzoomAction_ = nullptr;
    QAction* boxAction_ = nullptr;
    QWidget* panelPlot_ = nullptr;   // OctaveBandPlotWidget (defined in viewer_qt.cpp)
    QWidget* averagePlot_ = nullptr; // AutocorrelationAverageWidget (likewise)
    std::vector<std::pair<double, double>> zoomHistory_; // previous (minLagMs, maxLagMs)
    bool hasLagBox_ = false;
    double lagBoxA_ = 0.0, lagBoxB_ = 0.0; // ms, signed
    bool zoomWhenBoxDrawn_ = false;
    double lastAppliedMin_ = -1.0, lastAppliedMax_ = -1.0; // the result on screen; see the editingFinished guard
    std::function<void(double, double)> onApply_;

    void applyWindow(double minLagMs, double maxLagMs);
    void zoomToLagBox();
    void refreshZoomActions();
};

// Analysis > Signal/Noise: SIGCOR estimate (segy::computeSigcor) of the
// active panel's selection box, or its visible data without one. Trace,
// signal, noise (dB relative to the trace spectrum's peak) and S/N (dB)
// can each be switched on/off; signal and noise are on by default. Only
// Skipped traces and Min correlation are in the window; the other
// parameters are in Edit > Preferences > Signal/Noise. The time-shift
// statistics are a button (Delta t = mean +/- 1 sigma) above Recompute that
// opens a details table; Recompute turns orange when the data or the
// parameters differ from what is on screen.
struct SignalNoiseDisplay {
    std::vector<float> frequencyHz;
    std::vector<float> traceDb, signalDb, noiseDb, snDb;
    bool hasShift = false;
    double shiftMeanMs = 0.0;
    double shiftSigmaMs = 0.0;
    // Details table rows: {label, value}; an empty value makes a section heading.
    std::vector<std::pair<QString, QString>> details;
    QString emptyReason; // non-empty: nothing to plot, show this instead
};

// What a Signal/Noise result was computed from -- compared against the
// active panel's current state to decide whether Recompute is useful.
struct SignalNoiseSource {
    bool valid = false;
    const AppState* app = nullptr;
    const void* dataset = nullptr;
    int64_t t0 = 0, t1 = 0;
    int s0 = 0, s1 = 0;
    QString sourceText; // "selection box" / "visible data"
};

class SignalNoiseWindow : public QWidget {
public:
    explicit SignalNoiseWindow(QWidget* parent = nullptr);
    void setResult(const SignalNoiseDisplay& display);
    // Skipped traces and Min correlation live in the window; the rest of
    // `params` is left alone (it comes from Preferences).
    void setWindowParams(const segy::SigcorParams& params);
    void applyWindowParams(segy::SigcorParams& params) const;
    void setRecomputeCallback(std::function<void()> callback);
    void setParamsEditedCallback(std::function<void()> callback);
    // Orange when recomputing would change something, plain otherwise.
    void setRecomputeUseful(bool useful);

private:
    DbCurveChart* chart_ = nullptr;
    CurveReadoutPanel* readout_ = nullptr;
    QCheckBox* curveChecks_[4] = {};
    QSpinBox* distanceSpin_ = nullptr;
    QDoubleSpinBox* minCorrelationSpin_ = nullptr;
    QPushButton* shiftButton_ = nullptr;
    QLabel* shiftLabel_ = nullptr; // rich text inside shiftButton_
    QPushButton* recomputeButton_ = nullptr;
    QDialog* detailsDialog_ = nullptr;
    QTableWidget* detailsTable_ = nullptr;
    std::vector<std::pair<QString, QString>> details_;
    std::function<void()> onRecompute_;
    std::function<void()> onParamsEdited_;

    void matchButtonSizes();
    void showDetails();
};

// Analysis > F-K: the 2D amplitude spectrum (dB) of the active panel's
// selection box, or its visible data without one, power-averaged over its
// gathers (prestack) or over sliding 256-trace panels (stacks). Frequency
// 0..Nyquist upward, wavenumber -Nyquist..+Nyquist with 0 in the middle,
// in cycles/km when the trace spacing is known (offsets, coordinates, or
// typed in), otherwise cycles/trace. Colors as on the x-f display.
struct FkDisplay {
    int frequencies = 0;
    int wavenumbers = 0;
    double frequencyStepHz = 0.0;
    double wavenumberMin = 0.0;  // display units (cycles/km or cycles/trace)
    double wavenumberStep = 0.0;
    bool spacingKnown = false;   // k in cycles/km
    std::vector<float> db;       // frequencies x wavenumbers, row-major
    QString panelText;           // above the chart, e.g. "Panel: CDP = 543"
    QString rangeText;           // above the chart, e.g. "Range: offset = 0-3000"
    QString info;
    QString emptyReason;
};

class FkChart : public ZoomableChart {
public:
    explicit FkChart(QWidget* parent = nullptr);
    void setData(const FkDisplay& display);
    void setVelocityLinesVisible(bool visible);
    void setCrosshairEnabled(bool enabled);
    void setGridLinesVisible(bool visible);
    // The dB legend is drawn by the last of several side-by-side plots only.
    void setShowLegend(bool show);
    // valid = false when the cursor leaves the plot.
    void setHoverCallback(std::function<void(bool valid, double frequencyHz, double wavenumber, float db)> callback);
    const FkDisplay& display() const { return data_; }

protected:
    QRect chartRect() const override;
    void viewChanged() override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    FkDisplay data_;
    bool velocityLines_ = true;
    bool crosshairEnabled_ = true;
    bool gridLinesVisible_ = false;
    bool showLegend_ = true;
    bool hovering_ = false;
    QPointF hoverPos_;
    QImage raster_;
    std::function<void(bool, double, double, float)> onHover_;

    float valueAt(double frequencyHz, double wavenumber, bool* inside) const;
    void rebuildRaster(const QRect& chart);
};

class PhaseEstimateWindow;

class FkWindow : public QWidget {
public:
    explicit FkWindow(QWidget* parent = nullptr);
    // One plot per panel (gather), side by side and zoomed together, or a
    // single averaged plot. `rangeText` describes the k axis of all of them.
    void setResult(const std::vector<FkDisplay>& panels, const QString& rangeText, const QString& info,
                   const QString& emptyReason);
    // Panel ident: 0 = auto, 1 = field record, 2 = CDP, 3 = none (sliding panels).
    int gatherMode() const;
    // Distance ident for k: 0 = auto, 1 = offset, 2 = CDP number.
    int distanceMode() const;
    double offsetScalar() const;  // metres per offset unit (prestack: 1)
    double cdpDistance() const;   // metres per CDP number
    bool averageGathers() const;  // one averaged plot instead of one per gather
    // The CDP distance the trace coordinates imply, offered as the CDP
    // distance until the user types one (<= 0: none found).
    void proposeCdpDistance(double metres);
    bool cdpDistanceTyped() const { return cdpDistanceTyped_; }
    // What Auto resolved to in the last computation (1 FFID, 2 CDP, 3 None;
    // 1 Offset, 2 CDP), shown as "Auto (FFID)" in the combos.
    void setResolvedModes(int gatherMode, int distanceMode);
    void setRecomputeCallback(std::function<void()> callback);
    void setParamsEditedCallback(std::function<void()> callback);
    void setRecomputeUseful(bool useful);

private:
    std::vector<FkChart*> charts_;
    QVBoxLayout* mainLayout_ = nullptr;
    QToolBar* zoomBar_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* chartsHost_ = nullptr;
    QHBoxLayout* chartsLayout_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    QComboBox* gatherCombo_ = nullptr;
    QComboBox* distanceCombo_ = nullptr;
    QLabel* scalarCaption_ = nullptr;
    QDoubleSpinBox* scalarSpin_ = nullptr; // shows the offset scalar or the CDP distance
    QCheckBox* averageCheck_ = nullptr;
    QCheckBox* velocityCheck_ = nullptr;
    QLabel* rangeLabel_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    QPushButton* recomputeButton_ = nullptr;
    QWidget* readout_ = nullptr;
    QLabel* readPanel_ = nullptr;
    QLabel* readFrequency_ = nullptr;
    QLabel* readWavenumber_ = nullptr;
    QLabel* readVelocity_ = nullptr;
    QLabel* readLevel_ = nullptr;
    double offsetScalar_ = 1.0;
    double cdpDistance_ = 1.0;
    bool cdpDistanceTyped_ = false;
    bool showingCdp_ = false;
    int resolvedGather_ = 0; // what Auto chose in the last computation
    bool gridLinesVisible_ = false;
    bool crosshairEnabled_ = true;
    std::function<void()> onRecompute_;
    std::function<void()> onParamsEdited_;
    void syncScalarField();
};

// File > Trace Inspector: one trace's 240-byte header decoded per a chosen
// definition (Shell V2.1 by default, SEG-Y rev 1.0 or rev 2.0) -- name,
// bytes, value, description -- beside the trace's samples (table and a
// small wiggle). The trace is picked by number, or follows the cursor on
// the seismic.
class TraceInspectorWindow : public QWidget {
public:
    explicit TraceInspectorWindow(QWidget* parent = nullptr);
    void setDataset(std::shared_ptr<Dataset> dataset, const QString& titleSuffix);
    const Dataset* dataset() const { return dataset_.get(); }
    void showTrace(int64_t traceIndex); // 0-based
    bool followCursor() const;

private:
    std::shared_ptr<Dataset> dataset_;
    int64_t trace_ = 0;
    QSpinBox* traceSpin_ = nullptr;
    QComboBox* standardCombo_ = nullptr;
    QRadioButton* bigEndianRadio_ = nullptr;
    QCheckBox* followCheck_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QTableWidget* headerTable_ = nullptr;
    QTableWidget* samplesTable_ = nullptr;
    QWidget* wiggle_ = nullptr; // TraceWiggleWidget (viewer_qt.cpp)
    void refresh();
};

// The density-plot canvas: owns drawing and mouse/keyboard input. Reuses
// the exact same shared renderer/chrome calls the removed shells used.
class SegyCanvas : public QWidget {
public:
    explicit SegyCanvas(AppState& app, QWidget* parent = nullptr);

    // `processingHistory` is stashed on the new Dataset once the load
    // completes -- empty for a normal File > Open, populated when
    // Calculator/Bandpass load a dataset they just synthesized (see
    // Dataset::processingHistory).
    // `onLoaded`, when given, receives the loaded dataset instead of it
    // becoming the Foreground and joining the dataset list (Flatten as a
    // display uses this).
    void startLoading(const std::filesystem::path& path, std::vector<std::string> processingHistory = {},
                      std::function<void(std::shared_ptr<Dataset>)> onLoaded = {});
    // The Clip dialog for this panel (legend right-click, View > Clip...).
    void openClipDialog();

    // Fires once per successful load, right after the new Dataset becomes
    // this panel's Foreground -- MainWindow uses it to register the
    // dataset in the shared pool (see native/README.md, "Dataset pool").
    void setDatasetLoadedCallback(std::function<void(std::shared_ptr<Dataset>)> callback);

    // Makes `dataset` this panel's active Foreground/Background -- an O(1)
    // pointer swap for a dataset already in memory (the Foreground/
    // Background picker dialog's OK path), or the final step after a
    // fresh startLoading() completes. Foreground resets the view/selection
    // (a different dataset invalidates the old trace/sample space);
    // Background (phase 1) is purely a held reference, nothing else reads
    // it yet.
    void setForegroundDataset(std::shared_ptr<Dataset> dataset);
    void setBackgroundDataset(std::shared_ptr<Dataset> dataset);
    // Leaves the panel empty (no Foreground): the dataset was removed or
    // unticked in the Dataset Manager.
    void clearForeground();
    // Foreground becomes Background and the other way round; the view is
    // kept when both have the same size. False (nothing done) without a
    // Background.
    bool swapForegroundBackground();
    // A Background is set and drawable: the curtain is shown.
    bool curtainActive() const;

    // x-f display toggle: park the seismic Foreground, view, display
    // settings and selection boxes in AppState::xf and show `xfDataset`
    // (trace 0 = seismic trace `traceOffset`) with the x-f display
    // settings; exitXfMode() restores everything as it was. Choosing a
    // different Foreground while in x-f leaves x-f (display restored, the
    // parked dataset dropped).
    void enterXfMode(std::shared_ptr<Dataset> xfDataset, int64_t traceOffset, int64_t traceStride = 1);
    void exitXfMode();

    // Right-click on an axis (outside the plot): Axis Ranges... (trace and
    // time/frequency min/max) and Reset View. Both go on the zoom history,
    // so Mooz steps back.
    void showAxisRangeMenu(const QPoint& globalPos);
    // View helpers for the View menu / toolbar (respect Lock View).
    void showFullView();
    void zoomByFactor(double factor); // around the centre; < 1 zooms in
    void setPreviousZoomCallback(std::function<void()> callback) { onPreviousZoom_ = std::move(callback); }
    // The cursor that goes with the current tool (pointer, pan, box, pick).
    void unsetCursorForTool();

    // Linked crosshair: the other panel's cursor position (trace index and
    // time in ms), drawn as a dashed crosshair; valid = false hides it.
    void setLinkedCrosshair(bool valid, double traceCoord, double timeMs);
    // Fires when a selection box has just been completed (Zoom without a
    // box: zoom as soon as the box is drawn).
    void setBoxCompletedCallback(std::function<void()> callback);
    // Pick tool: a left click at (trace, sample) on the plot; MainWindow adds
    // the anchor and fills the picks. Right click: remove the nearest anchor.
    void setPickCallback(std::function<void(double traceCoord, double sampleCoord, bool remove)> callback);

private:
    std::function<void(const QPoint& globalPos)> colorScaleMenu_; // the color bar's right-click menu
    bool linkedCrosshairValid_ = false;
    double linkedTraceCoord_ = 0.0;
    double linkedTimeMs_ = 0.0;
    std::function<void()> onBoxCompleted_;
    std::function<void(double, double, bool)> onPick_;
    bool refuseIfViewLocked(); // flashes a status message when locked
    // Zoom gestures (see native/README.md, "Zoom"): a left-drag rubber band
    // in the pointer tool, middle-drag panning in any tool.
    bool zoomBandActive_ = false;
    QPoint zoomBandStart_, zoomBandEnd_;
    bool middlePanning_ = false;
    bool identNamesOnRight_ = false;
    bool curtainDragging_ = false;
    bool gripDragging_ = false; // the grip also moves along the fence
    int curtainHover_ = 0;      // 0 none, 1 fence, 2 grip
    std::vector<uint32_t> curtainPixels_;
    int lastPlotTop_ = 0, lastPlotHeight_ = 0;
    // What the mouse is over: 0 nothing, 1 the fence, 2 its grip (widget
    // coordinates, last paint's plot geometry).
    int fenceHitTest(const QPoint& pos) const;
    void dragFence(const QPoint& pos);
    Qt::CursorShape fenceCursor(int hit) const;
    // The view in the Background's own samples (same traces and times).
    segy::ViewRange curtainView() const;
    qint64 lastWheelMs_ = -1000000;
    std::function<void()> onPreviousZoom_;
    void finishZoomBand();
public:

    // MainWindow owns the status bar (and, via the callback, the Del
    // button's visibility); handed to the canvas as a plain pointer/
    // callback rather than introducing Qt signals/slots for what is
    // otherwise a single, controlled producer/consumer link.
    void setStatusWidgets(QStatusBar* statusBar, std::function<void()> onStateChanged);

    // For MainWindow to call after actions that change app_.view/selection
    // state without going through a mouse event on the canvas itself (the
    // Zoom/Mooz toolbar buttons).
    void refreshStatusBar();

    // While a MainWindow-level background task (Calculator/Bandpass/Save
    // SEG-Y/Octave Bands) is running, this panel's own hover/tool status
    // text would otherwise fight with the task's progress message every
    // time the mouse moves over the canvas -- MainWindow sets this true
    // on both panels for the task's duration so updateStatusBar() backs
    // off and leaves the status bar alone.
    void setStatusOverrideActive(bool active);

    // Split view: fires on mousePressEvent so MainWindow knows which panel
    // was just clicked into and can route toolbar/menu actions to it.
    void setActivatedCallback(std::function<void()> callback);

    // Dock's Trace Info section is driven by this instead of the status
    // bar now (see native/README.md, "Dark Pro" relocation notes).
    void setHoverInfoCallback(std::function<void(const HoverInfo&)> callback);

    // Fires whenever app_.view changes via direct mouse/keyboard
    // interaction on *this* canvas (drag-pan, wheel-zoom, 'R' reset) --
    // MainWindow uses it to mirror the new view into the other panel when
    // Lock is on. Deliberately not fired from the toolbar's own Zoom/Mooz
    // (those already go through MainWindow::targetPanels(), which sets
    // both panels' views directly -- firing this there too would just
    // copy an already-equal value back onto itself).
    void setViewChangedCallback(std::function<void()> callback);

    // MainWindow computes the gain-adjusted clip value (it already owns
    // that math for the gain control); the canvas owns the overlay widget
    // itself since only it knows the plot's on-screen geometry to position
    // against.
    void setAmplitudeScale(float posClip, float negClip, bool visible, segy::ColorScale colorScale,
                           bool colorScaleFlipped);

    // "Foreground 1"/"Background 1" vs "...2" etc -- set once, right after
    // construction, since only MainWindow knows which panel this is.
    void setDatasetSlotLabels(const QString& foregroundLabel, const QString& backgroundLabel);
    // Where the ident rows' header names go: left of the plot (the main or
    // left panel), or right of it (the right panel of a side-by-side split).
    void setIdentNamesOnRight(bool right) {
        identNamesOnRight_ = right;
        update();
    }
    void setDatasetLabelPosition(DatasetLabelPosition position);
    // `isForeground` tells MainWindow which of this panel's two slots the
    // click was for (true = Foreground, false = Background); it owns the
    // dataset pool and the picker dialog, this class only owns the boxes.
    void setDatasetSlotPickCallback(std::function<void(bool isForeground)> callback);
    // Re-reads app_.foreground/background's names into the two boxes --
    // call after anything that could change either (a fresh load, a
    // picker OK, or another panel's Delete removing the dataset this one
    // is currently showing... though that case is blocked by
    // isDatasetInUse, so it shouldn't actually happen).
    void refreshDatasetSlotNames();
    // Call after toggling anything in app_.idents from outside this class
    // (the Idents toolbar menu, in MainWindow) -- repositionAmplitudeScale/
    // repositionDatasetSlots are otherwise only reached from resizeEvent/
    // paintEvent, so without this their on-screen position would go stale
    // (not shift to account for the new top inset) until the next real
    // resize, even though update() alone would repaint everything else
    // correctly.
    void refreshIdentLayout();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    AppState& app_;
    QStatusBar* statusBar_ = nullptr;
    bool statusOverrideActive_ = false;
    std::function<void()> notifyStateChanged_;
    std::function<void(const HoverInfo&)> notifyHoverChanged_;
    std::function<void()> notifyViewChanged_;
    std::function<void()> notifyActivated_;
    std::function<void(std::shared_ptr<Dataset>)> onDatasetLoaded_;
    AmplitudeScaleWidget* amplitudeScale_ = nullptr;
    // Scroll bars in the surround (bottom: traces, left: time), shown only
    // while zoomed in on that axis; the thumb is the visible window.
    QScrollBar* hScroll_ = nullptr;
    QScrollBar* vScroll_ = nullptr;
    bool syncingScroll_ = false;
    void syncScrollBars(int plotLeft, int plotTop, int plotWidth, int plotHeight);
    void scrollTo(bool horizontal, int value);
    // Lazily created from the legend's own right-click menu -- see this
    // class's constructor. Owns its lifetime like the other per-panel
    // lazy dialogs, but unlike those, it's naturally already bound to the
    // correct panel forever (it's owned by that panel's own canvas), no
    // "bound to whichever was active at creation" caveat needed.
    ClipDialog* clipDialog_ = nullptr;
    // Foreground/Background boxes (see native/README.md, "Dataset pool")
    // -- overlaid on this panel's plot area at the selected corner.
    // MainWindow owns the pool/picker dialog and
    // supplies isForeground's meaning via setDatasetSlotPickCallback;
    // this class only owns the boxes' existence/position/displayed name.
    DatasetSlotWidget* foregroundSlot_ = nullptr;
    DatasetSlotWidget* backgroundSlot_ = nullptr;
    DatasetLabelPosition datasetLabelPosition_ = DatasetLabelPosition::TopLeft;
    std::function<void(bool isForeground)> onDatasetSlotPick_;
    void repositionDatasetSlots();
    void repositionAmplitudeScale();
    // Inset width available to the plot itself -- width() minus the
    // margins minus the amplitude-scale column reserved whenever
    // showColorBar is on (independent of seismicMode, so the plot's size
    // stays constant across Variable Density/Wiggle switches). Used
    // wherever pixel<->data math needs the plot's *actual* width, not the
    // raw inset width.
    int plotAreaWidth() const;
    // Extra top/bottom inset (beyond kCanvasMargin) reserved for idents --
    // see native/README.md. Deliberately recomputed fresh every call
    // rather than cached on the widget: it's cheap (a handful of bool
    // reads), and caching it would open a stale-reinset window between an
    // Idents-menu toggle and the next mouse/paint event reading it.
    struct IdentInsets { int top = 0; int bottom = 0; };
    IdentInsets identInsets() const;
    void paintIdentText(QPainter& painter, const segy::ChromeLayout& chrome, int topInset, int bottomInset,
                         int insetHeight) const;
    void paintIdentOverlayLines(QPainter& painter, const segy::ChromeLayout& chrome, int topInset,
                                 int insetHeight) const;

    bool hovering_ = false;
    int hoverPixelX_ = 0, hoverPixelY_ = 0; // canvas-local
    // Most recent plot-area geometry from paintEvent's ChromeLayout --
    // mouse handlers need it to map pixels to data coordinates but only
    // paintEvent computes it (the color bar/time-scale columns shift where
    // the plot itself starts), so it's cached here after each paint.
    int lastPlotX_ = 0, lastPlotWidth_ = 0;

    std::string statusLineText() const;
    segy::RenderContext makeRenderContext() const;
    void updateStatusBar();
    void notifyStateChanged();
    HoverInfo currentHoverInfo() const;
    void notifyHoverChanged();

    // Box-select / hover helpers, using lastPlotX_/lastPlotWidth_ above.
    bool dataCoordAt(int pixelX, int pixelY, double& traceCoord, double& sampleCoord) const;
    void pixelForData(double traceCoord, double sampleCoord, double& pixelX, double& pixelY) const;
    // Searches every box, not just one -- outBoxIndex (when given) receives
    // which box the returned corner (0..3, or -1 for no hit) belongs to.
    int hitTestCorner(int pixelX, int pixelY, int* outBoxIndex = nullptr) const;
    void handleBoxSelectPress(QMouseEvent* event);
};

class MainWindow : public QMainWindow {
public:
    MainWindow();

    // For the argv[1]-as-initial-file convenience the removed shells had.
    void openInitialFile(const std::filesystem::path& path);

protected:
    // Recomputes toolbar overflow (see toolbarOverflowCandidates_ below)
    // on every resize -- a short/narrow window is exactly when buttons
    // start not fitting.
    void resizeEvent(QResizeEvent* event) override;

private:
    // Split view: two independent panels (each its own file/view/tool
    // state), side by side in a QSplitter. `app_`/`canvas_` always alias
    // the *active* panel (whichever was last clicked into) -- every
    // existing single-panel toolbar/menu handler keeps working unchanged
    // by reading/writing through them. `targetPanels()` returns just the
    // active panel normally, or both when the Lock toggle is on, so a
    // handful of handlers (tool mode, gain, seismic mode, zoom/mooz, wiggle
    // infill, box delete) that should apply to "both views at once when
    // locked" loop over it instead of using app_/canvas_ directly.
    Panel panelA_, panelB_;
    Panel* activePanel_ = &panelA_;
    AppState* app_ = &panelA_.app;
    SegyCanvas* canvas_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QAction* splitViewAction_ = nullptr;
    std::vector<Panel*> targetPanels();
    // Mirrors `source`'s view into the other panel when Lock is on --
    // wired to both panels' SegyCanvas::setViewChangedCallback, fired on
    // direct mouse/keyboard view changes (drag-pan, wheel-zoom, 'R'
    // reset). The toolbar's own Zoom/Mooz already sync both panels via
    // targetPanels() directly and don't go through this.
    void syncLockedView(Panel* source);
    void setActivePanel(Panel* panel);

    QAction* zoomAction_ = nullptr;
    QAction* moozAction_ = nullptr;
    QAction* fullViewAction_ = nullptr;
    QAction* panAction_ = nullptr;
    QAction* boxSelectAction_ = nullptr;

    // Seismic Display mode is driven from both the toolbar (a single
    // toggle button whose icon shows the mode it would switch to) and the
    // View menu (submenu) -- both act on the same app_.display.seismicMode
    // and stay in sync with each other.
    QAction* seismicModeToolbarAction_ = nullptr;
    QAction* variableDensityMenuAction_ = nullptr;
    QAction* wiggleMenuAction_ = nullptr;

    QAction* flipHorizontalAction_ = nullptr;

    // Opens a QMenu (built fresh from the active panel's app_->idents each
    // time, like toolbarMoreMenu_) with Display at Top/Bottom, Ident Plot,
    // and Display on Seismic submenus -- see native/README.md.
    QAction* identsAction_ = nullptr;

    QAction* gainDetailAction_ = nullptr; // opens DisplayParametersDialog
    DisplayParametersDialog* displayParametersDialog_ = nullptr;
    QSlider* gainSlider_ = nullptr;
    // The QWidgetAction QToolBar::addWidget(gainSlider_) implicitly created
    // -- toggling *this* action's visibility (not just gainSlider_'s own)
    // is what actually keeps it hidden; see applyToolbarPosition().
    QAction* gainSliderAction_ = nullptr;
    // Shown instead of gainSlider_ when the toolbar is docked Left/Right --
    // see their construction in MainWindow::MainWindow. gainValueLabel_
    // (the actual dB readout, right-click for an exact value) sits between
    // the two arrows there, but is shown next to gainSlider_ in horizontal
    // mode too -- it's the one gain control visible in both orientations.
    QAction* gainUpAction_ = nullptr;
    QLabel* gainValueLabel_ = nullptr;
    QAction* gainDownAction_ = nullptr;

    // AGC on the display, on the fly (see viewer/amplitude.h): agcAction_
    // toggles it, right-click sets its window and level. Processing >
    // Amplitude Processing... (gain curve and/or AGC) writes a new dataset.
    QAction* agcAction_ = nullptr;
    segy::AmplitudeProcessing lastAmplitudeProcessing_;
    // Applies `processing` to targetPanels() (Lock-aware), then refreshes
    // the legend, canvases, and the AGC toggle's checked state.
    void setAmplitudeProcessing(const segy::AmplitudeProcessing& processing);
    void openAmplitudeProcessingDialog();
    void openAgcSettings();
    // Re-syncs agcAction_ to the active panel's settings.
    void refreshAmplitudeProcessingControls();

    // Spectrum and Histogram open a parameters dialog plus, on Apply/OK, a
    // real computed result plot -- see native/README.md and
    // viewer/analysis.h. Lock is a real two-state toggle (icon swaps
    // lock-open/lock-closed, and the checked style gives it a pressed-down
    // look) that makes split view's toolbar/menu actions apply to both
    // panels at once -- see targetPanels().
    QAction* spectrumAction_ = nullptr;
    SpectrumDialog* spectrumDialog_ = nullptr;
    SpectrumPlotWidget* spectrumPlot_ = nullptr;
    QAction* histogramAction_ = nullptr;
    HistogramDialog* histogramDialog_ = nullptr;
    HistogramPlotWidget* histogramPlot_ = nullptr;
    QAction* lockAction_ = nullptr;

    // Edit > Calculator, Edit > Processing > Bandpass, File > Save SEG-Y,
    // and the toolbar's Octave Band Display -- see native/README.md,
    // "Producing and displaying a new dataset."
    CalculatorDialog* calculatorDialog_ = nullptr;
    BandpassDialog* bandpassDialog_ = nullptr;
    SaveSegyDialog* saveSegyDialog_ = nullptr;
    QAction* octaveBandAction_ = nullptr;
    OctaveBandDialog* octaveBandDialog_ = nullptr;
    Panel* octaveSourcePanel_ = nullptr; // whose display clip the octave panels follow
    QAction* autocorrelationAction_ = nullptr;
    QAction* xfAction_ = nullptr;
    QAction* lockViewAction_ = nullptr;
    QAction* pickAction_ = nullptr;
    QWidget* horizonManager_ = nullptr; // Horizons > Horizon Manager
    // Pick settings (Horizon Manager), decided with the user: search +/-20 ms,
    // trust 0.7, peak; seabed threshold 30% of the trace maximum.
    segy::PickMode pickMode_ = segy::PickMode::Peak;
    double pickSearchMs_ = 20.0;
    double pickWindowMs_ = 40.0;
    double pickTrust_ = 0.7;
    double seabedThresholdPercent_ = 30.0;
    void openHorizonManager();
    void refreshHorizonManager();
    Horizon* activeHorizonFor(Panel* panel, bool create);
    void handlePick(Panel* panel, double traceCoord, double sampleCoord, bool remove);
    void autoPickSeabed(Panel* panel);
    // File > Dataset Manager: every dataset in memory, which ones the
    // Foreground/Background picker lists, and removing them.
    QWidget* datasetManager_ = nullptr;
    QTableWidget* datasetManagerTable_ = nullptr;
    void openDatasetManager();
    void refreshDatasetManager();
    // Drops `dataset` from the pool and every panel slot (a scratch copy no
    // one else holds is deleted from disk).
    void removeDataset(std::shared_ptr<Dataset> dataset);
    // Empties every panel slot showing `dataset` (unticked or removed).
    void releaseDatasetFromPanels(const std::shared_ptr<Dataset>& dataset);
    // View > Swap Foreground and Background (X), created before the toolbar.
    QAction* swapDatasetsAction_ = nullptr;
    // Toolbar Fence: Background and Foreground side by side in one panel.
    QAction* fenceAction_ = nullptr;
    void exportHorizon(Panel* panel);
    void importHorizon(Panel* panel);
    // Flatten (Horizons > Flatten...): on a horizon or a trace-header time
    // (default bytes 57-60, Shell SEABED_TWT, 0.01 ms units).
    QAction* flattenAction_ = nullptr;
    int headerTimeByte_ = 57;
    int headerTimeSize_ = 4;
    double headerTimeMsPerUnit_ = 0.01;
    void toggleFlatten(Panel* panel, bool on);
    void runFlatten(Panel* panel, std::vector<double> sourceMs, double referenceMs, bool newDataset,
                    const QString& outputName, const QString& historyLine);
    void writeHorizonToHeader(Panel* panel);
    // Data domain (View > Data Domain) and Processing > Time-Depth Conversion.
    QAction* domainTimeAction_ = nullptr;
    QAction* domainDepthAction_ = nullptr;
    void setDatasetDomain(Panel* panel, bool depth);
    void openTimeDepthConversion();
    QAction* linkedCrosshairAction_ = nullptr;
    bool pendingBoxZoom_ = false; // Zoom clicked without a box: zoom once one is drawn
    void sendZoomToOtherPanel();
    void sendDisplayToOtherPanel();
    QAction* signalNoiseAction_ = nullptr;
    QAction* fkAction_ = nullptr;
    FkWindow* fkWindow_ = nullptr;
    // Analysis > Phase Estimate (see native/README.md).
    PhaseEstimateWindow* phaseWindow_ = nullptr;
    Panel* phaseSourcePanel_ = nullptr;
    void computeAndShowPhaseEstimate(Panel* sourcePanel);
    void applyPhaseRotation(Panel* panel, double degrees);
    bool fkComputed_ = false;
    SignalNoiseSource fkLastSource_;
    int fkLastGatherMode_ = 0;
    int fkLastDistanceMode_ = 0;
    bool fkLastAverage_ = false;
    double fkLastOffsetScalar_ = 1.0;
    double fkLastCdpDistance_ = 1.0;
    AutocorrelationDialog* autocorrelationDialog_ = nullptr;
    AutocorrelationDialog* crossCorrelationDialog_ = nullptr;
    std::shared_ptr<Dataset> crossA_, crossB_; // the last cross-correlation's inputs
    Panel* crossPanel_ = nullptr;               // set: over that panel's view (Analysis menu); null: whole traces
    SignalNoiseWindow* signalNoiseWindow_ = nullptr;
    bool signalNoiseComputed_ = false; // false until the first compute: Recompute is then always useful
    SignalNoiseSource signalNoiseLastSource_;
    segy::SigcorParams signalNoiseLastParams_;

    // Dataset pool: every dataset either panel has ever loaded/computed,
    // kept alive here independent of which panel's Foreground/Background
    // currently points at it -- see native/README.md, "Dataset pool:
    // Foreground/Background." Lazily created/reconfigured like every
    // other on-demand dialog in this file.
    std::vector<std::shared_ptr<Dataset>> datasetPool_;
    DatasetPickerDialog* datasetPickerDialog_ = nullptr;

    // Generic background-task runner -- Calculator/Bandpass/Save SEG-Y/
    // Octave Bands all move their heavy per-trace work off the UI thread
    // through this, with progress shown in the status bar (see
    // native/README.md, "Background processing"). One task at a time;
    // `runBackgroundTask` refuses (with a flashStatusMessage) while
    // another is already running, same re-entrancy guard shape as
    // SegyCanvas::startLoading's own `if (app_.loading) return;`.
    bool backgroundTaskRunning_ = false;
    QString backgroundTaskLabel_;
    std::atomic<int64_t> backgroundTaskDone_{0};
    std::atomic<int64_t> backgroundTaskTotal_{1};
    QTimer* backgroundTaskTimer_ = nullptr;   // polls the two atomics above while a task runs
    QProgressBar* backgroundProgressBar_ = nullptr; // status bar permanent widget, visible only while running

    // Read-only viewers -- re-populated from the active panel on every
    // click, no settings to keep sticky between opens (see
    // EbcdicHeaderWidget/BinaryHeaderWidget above).
    QAction* ebcdicHeaderAction_ = nullptr;
    EbcdicHeaderWidget* ebcdicHeaderWidget_ = nullptr;
    QAction* binaryHeaderAction_ = nullptr;
    BinaryHeaderWidget* binaryHeaderWidget_ = nullptr;
    QWidget* headerRangesWidget_ = nullptr; // File > Trace Header Ranges
    TraceInspectorWindow* traceInspector_ = nullptr; // File > Trace Inspector
    void openTraceInspector();
    void openTraceHeaderRanges();
    // Trace Header Ranges' Apply: the traces of `dataset` matching
    // `criteria`, in `sortKeys` order, as a new dataset loaded into `panel`
    // (a new item in the dataset list, so it cascades).
    void runSubsetSort(Panel* panel, std::shared_ptr<Dataset> dataset, std::vector<segy::IdentCriterion> criteria,
                       std::vector<segy::SortKey> sortKeys, const QString& outputName, const QString& historyLine);
    std::shared_ptr<Dataset> headerRangesDataset_; // what the Trace Header Ranges window shows

    // Shows/hides the right dock entirely -- the dock itself can't be
    // closed any other way (NoDockWidgetFeatures), so this button is the
    // one and only source of truth for its visibility.
    QAction* sidebarToggleAction_ = nullptr;

    // Right dock: relocated hover info plus a QC Notes placeholder -- see
    // native/README.md. The amplitude scale legend used to live here too;
    // it's now an overlay on the canvas itself (see AmplitudeScaleWidget).
    // "Right dock"/"drawer" both refer to this same QDockWidget -- Qt's own
    // name for it is dock, Preferences' own name for the same concept
    // (matching the feature request) is "drawer"; not two different things.
    QDockWidget* dock_ = nullptr;
    QLabel* traceValueLabel_ = nullptr;
    QLabel* seqValueLabel_ = nullptr;
    QLabel* timeValueLabel_ = nullptr;
    QLabel* timeCaptionLabel_ = nullptr; // "Time", or "Frequency" while hovering an x-f display
    QLabel* amplitudeValueLabel_ = nullptr;

    // Edit > Preferences -- app-wide, persisted to $HOME/.segyread_settings
    // (see loadPreferences/savePreferences), unlike every panel-local
    // *Settings struct elsewhere in this file. toolbar_ was a constructor
    // local until this needed to reposition it after the fact.
    Preferences prefs_;
    DatasetLabelPosition datasetLabelPosition_ = DatasetLabelPosition::TopLeft;
    QToolBar* toolbar_ = nullptr;
    PreferencesDialog* preferencesDialog_ = nullptr;
    void loadPreferences();
    void savePreferences();
    void applyDatasetLabelPosition(DatasetLabelPosition position);
    void applyToolbarPosition(ToolbarPosition position);
    void applyDrawerPosition(DrawerPosition position);

    // Toolbar overflow: when the window is too short (vertical toolbar,
    // the normal case) or too narrow (horizontal) to fit every button,
    // the ones that don't fit are hidden (QAction::setVisible(false), the
    // one reliable way to actually collapse a toolbar button's space --
    // see native/README.md) and this button -- a chevron fused to the
    // bottom/right edge of whatever's currently visible, with a small
    // badge showing how many actions are hidden (renderOverflowIcon()) --
    // reaches them via a popup menu instead of leaving them simply gone
    // with no way back. "Option A" from the mockups the user reviewed.
    //
    // Parented to toolbar_, but deliberately never added via
    // toolbar->addWidget() -- positioned manually in
    // updateToolbarOverflow() instead of being one more QToolBarLayout
    // child. Two earlier versions both tried letting the layout manage
    // it: first added the same way as every candidate, which failed
    // because predicting fit from sizeHint() sums (sizeHint() doesn't
    // report inter-item spacing/separators QToolBarLayout adds) was
    // wrong by varying margins; moving it to the status bar sidestepped
    // that, but once the empirical fit check below replaced the
    // sizeHint()-sum approach for candidates, it seemed safe to add it
    // back as a normal toolbar child too -- except geometry-level
    // debug logging then showed QToolBarLayout's actual *last one or two
    // children* reliably come back with stale, never-laid-out (0,0,100,30)
    // geometry regardless of overflow/available space (confirmed:
    // Lock/Sidebar -- always the last two toolbarOverflowCandidates_ --
    // showed the exact same stale rect whenever *they* were hidden and
    // therefore the layout's actual last children, with no relation to
    // how much room was available). Manual positioning sidesteps
    // QToolBarLayout for this one widget entirely -- same technique this
    // file already uses for DatasetSlotWidget/AmplitudeScaleWidget.
    QToolButton* toolbarMoreButton_ = nullptr;
    QMenu* toolbarMoreMenu_ = nullptr; // rebuilt fresh from whatever's currently hidden, each time it's about to show
    // The expanding spacer that pushes the sidebar toggle to the toolbar's
    // far end (see its own construction comment) -- hidden whenever
    // overflow is active, since an Expanding QWidgetAction still claims a
    // layout slot even at zero size and was found (via geometry-level
    // debug logging) to leave the sidebar-toggle button after it without
    // a valid laid-out position once the toolbar is genuinely out of room.
    QAction* toolbarSpacerAction_ = nullptr;
    // Fixed order, built once right after the toolbar is fully
    // constructed. updateToolbarOverflow() measures fit empirically --
    // show every candidate, let Qt actually lay the toolbar out, then
    // hide whichever ones come back positioned past the toolbar's own
    // real size -- rather than predicting it from cached sizeHint()
    // sums (see toolbarMoreButton_'s comment for why that didn't work).
    std::vector<QAction*> toolbarOverflowCandidates_;
    void updateToolbarOverflow();

    // Customizable toolbar (COPILOT.md item 1): every toolbar button is one
    // ToolbarItem the user can show or hide -- only in Edit > Preferences >
    // Toolbar Buttons (per the user: right-click menus stay about the button
    // itself, not toolbar layout). "Gain" is a single item covering all four of its
    // widgets (slider, +/- arrows, value label), since only some of them
    // are ever shown at once anyway (see applyToolbarPosition). Hidden item
    // ids live in prefs_.hiddenToolbarItems, persisted to
    // $HOME/.segyread_settings ("toolbarHiddenItems").
    // Every item's function is also reachable from a menu, so hiding a
    // button never makes a feature unreachable -- see addMenuMirror.
    struct ToolbarItem {
        QString id;    // stable persisted key -- never rename once shipped
        QString label; // shown in the customize menus
        int group;     // separator group, 0..kToolbarSeparatorGroups (last = after the spacer)
        std::vector<QAction*> actions;
        bool optional = false; // off until switched on in Preferences
    };
    bool isToolbarItemShown(const ToolbarItem& item) const;
    QAction* reversePolarityAction_ = nullptr;
    std::vector<QAction*> extraToolbarActions_;
    std::vector<ToolbarItem> toolbarItems_;
    // toolbarSeparators_[g] sits between group g and group g+1; shown only
    // while group g and some later (pre-spacer) group both have a shown
    // item, so hiding a whole group never leaves doubled/dangling dividers.
    std::vector<QAction*> toolbarSeparators_;
    QAction* gainValueLabelAction_ = nullptr;
    // False if `action` belongs to a user-hidden item, or is the gain
    // control for the other toolbar orientation (slider: horizontal only;
    // +/- arrows: vertical only). updateToolbarOverflow() only measures/
    // shows wanted actions, and its More menu never lists unwanted ones.
    bool isToolbarActionWanted(QAction* action) const;
    void applyToolbarCustomization();
    // A menu entry that triggers `source` (a toolbar action) and tracks its
    // enabled/checked state. A separate QAction rather than `source`
    // itself added to the menu: QAction visibility is shared by every
    // widget showing it, and the toolbar hides its actions for both
    // overflow and customization -- which must not hide the menu entries.
    QAction* addMenuMirror(QMenu* menu, QAction* source, const QString& text = QString(), QAction* before = nullptr);
    // Shared by the Idents toolbar button's popup and View > Idents.
    void populateIdentsMenu(QMenu& menu);
    // Shared by the gain value label's right-click and View > Gain.
    void promptForGainDb();
    // Shared by the Split View button's right-click and View > Split Layout.
    void setSplitOrientation(Qt::Orientation orientation);
    // Forces splitter_ to an even 50/50 split along its current
    // orientation -- see splitViewAction_'s triggered handler and its
    // right-click menu.
    void resetSplitToHalf();
    // Briefly overrides the status bar's normal text (bottom-left) with an
    // orange warning for 2 seconds, then restores whatever the active
    // panel's canvas would normally show there -- used when Spectrum/
    // Histogram are clicked with no file loaded, in place of opening their
    // parameters dialog.
    void flashStatusMessage(const QString& text);

    void openFile();
    // File > Read Attribute SEG-Y: pick a file and what it holds; loaded in
    // the background into the dataset list, not into a panel.
    void openAttributeFile();
    // File > Open with Analysis: ImportDialog, then a direct open or a
    // normalised copy (selection, mapping, rev 2 data start).
    void openFileWithAnalysis();
    void zoomToBox();
    void moozBack();
    void refreshToolChrome();
    void refreshWindowTitle();
    void setSeismicMode(segy::SeismicDisplayMode mode);
    void refreshSeismicModeToolbarIcon();
    void setGainDb(double value);
    void refreshAmplitudeScale();
    void updateHoverInfo(const HoverInfo& info);
    void computeAndShowHistogram(Panel* panel);
    void computeAndShowSpectrum(Panel* panel);
    // Lazily create-and-show histogramDialog_/spectrumDialog_ -- shared by
    // the toolbar's own Histogram/Spectrum buttons and each result
    // window's Utilities > Parameters... menu item, so the two don't
    // duplicate this.
    void openHistogramDialog();
    void openSpectrumDialog();

    // Combines panelA_/panelB_'s datasets (Add/Subtract), writes the result
    // to a temp SEG-Y file, and loads it into panelA_ (see
    // native/README.md). `outputName` becomes both the output file's stem
    // and the display name shown everywhere via datasetTitleSuffix().
    void runCalculator(std::shared_ptr<Dataset> dsA, std::shared_ptr<Dataset> dsB, bool subtract,
                       const QString& outputName);
    // Bandpass-filters every trace of `source`'s full sample range, writes
    // the result to a temp SEG-Y file, and loads it into panelA_.
    void runBandpass(AppState& source, const BandpassRequest& request, const QString& outputName);
    // Writes `source`'s current dataset to `filePath` with a synthesized
    // EBCDIC header recording its processing history (see
    // buildSegyTextHeader in viewer_qt.cpp).
    void runSaveSegy(AppState& source, const QString& filePath);
    // Bandpass-filters the *visible* traces/samples of `sourcePanel` into a
    // row of octave bands and shows them in octaveBandDialog_.
    void computeAndShowOctaveBands(Panel* sourcePanel, int minFreqHz, int maxFreqHz);
    // Autocorrelation of `sourcePanel`'s visible traces/samples, lags up to
    // `lengthMs`, shown in autocorrelationDialog_ (background task).
    void computeAndShowAutocorrelation(Panel* sourcePanel, double minLagMs, double maxLagMs);
    void computeAndShowCrossCorrelation(std::shared_ptr<Dataset> a, std::shared_ptr<Dataset> b, double minLagMs,
                                        double maxLagMs);
    // Analysis > Cross-correlation: the panel's Foreground with its
    // Background (or a dataset the user picks) over the visible data, like
    // the autocorrelation.
    void computeAndShowViewCrossCorrelation(Panel* sourcePanel, std::shared_ptr<Dataset> other, double minLagMs,
                                            double maxLagMs);
    void startViewCrossCorrelation();
    void ensureCrossCorrelationDialog();
    // Shared by both: per-trace correlations of `a` with `b` (the same
    // dataset for an autocorrelation) over traces [t0, t1) and samples
    // [s0, s1), up to 240 evenly spaced traces, plus the average, its side
    // peak and power spectrum; shown in `dialog`.
    void runCorrelation(AutocorrelationDialog* dialog, std::shared_ptr<Dataset> a, std::shared_ptr<Dataset> b,
                        int64_t t0, int64_t t1, int s0, int s1, int minLag, int maxLag, segy::ColorScale colorScale,
                        bool colorScaleFlipped, const QString& title, const QString& description);
    void openAutocorrelationDialog();
    // Signal/Noise (SIGCOR, prefs_.signalNoise) of `sourcePanel`'s active
    // selection box, or its visible data without one; background task,
    // shown in signalNoiseWindow_.
    void computeAndShowSignalNoise(Panel* sourcePanel);
    void ensureSignalNoiseWindow();
    SignalNoiseSource resolveSignalNoiseSource(Panel* panel) const;
    segy::SigcorParams currentSignalNoiseParams() const; // Preferences + the window's two fields
    void refreshSignalNoiseRecompute();
    // Recompute buttons of Signal/Noise and F-K: orange when stale.
    void refreshRecomputeButtons();
    // Phase Estimate's Recompute: orange when the view/box moved since.
    void refreshPhaseRecompute();
    SignalNoiseSource phaseLastSource_{};
    bool phaseComputed_ = false;
    void ensureFkWindow();
    void computeAndShowFk(Panel* sourcePanel);
    void refreshFkRecompute();
    // x-f toggle for the active panel: on computes the amplitude spectra
    // of the visible traces over the visible time range (background task)
    // and swaps them in; off restores the seismic.
    void toggleXf(Panel* panel, bool on);
    void refreshAfterDisplaySwap();
    void openPreferencesDialog();
    // Help menu, and View > Theme (stored, then the app restarts).
    void showAbout();
    void reportBug();
    void showMouseAndKeyboardHelp();
    void switchTheme(UiTheme theme);

    void openCalculatorDialog();
    void openBandpassDialog();
    // Per-trace processing into a new dataset (Processing > Filter > Trace
    // Integration/Differentiation, Processing > Trace Math > Scale): runs
    // `transform` on every trace of `panel`'s Foreground in parallel, writes
    // the result with `historyLine` appended to the processing history, and
    // loads it into the same panel -- a new item in the dataset list, so
    // operations cascade (integrate, then bandpass, ...).
    void runTraceTransform(Panel* panel, const QString& title, const QString& outputName, const QString& historyLine,
                           std::function<void(std::vector<float>& trace, double sampleIntervalUs)> transform);
    void openTraceDerivative(bool integrate);
    void openTraceScale();
    double lastIntegrationDb_ = 6.0;
    double lastDifferentiationDb_ = 6.0;
    double lastScaleFactor_ = 1.0;
    // Filters, deconvolution and attributes (Processing menu); the last
    // values are the next defaults.
    segy::WhiteningParams lastWhitening_;
    segy::DeconParams lastDecon_;
    bool lastDeconSpiking_ = false;
    segy::FkFilterParams lastFk_;
    double lastRotationDegrees_ = 90.0;
    void openSpectralWhitening();
    void openDeconvolution();
    void openFkFilter();
    void runAttribute(int which); // 0 envelope, 1 inst. phase, 2 inst. frequency, 3 phase rotation
    void openSaveSegyDialog();
    void openOctaveBandDialog();

    // Adds `dataset` to datasetPool_ (deduplicating its name against
    // existing entries, e.g. "Line_100 (2)") and refreshes both panels'
    // Foreground/Background boxes -- called once per successful load,
    // via SegyCanvas::setDatasetLoadedCallback.
    void registerDataset(std::shared_ptr<Dataset> dataset);
    // True if `dataset` is any panel's current Foreground or Background --
    // gates DatasetPickerDialog's Delete button (see native/README.md).
    bool isDatasetInUse(const std::shared_ptr<Dataset>& dataset) const;
    // Opens datasetPickerDialog_ (lazily created) scoped to `panel`'s
    // Foreground (isForeground=true) or Background slot.
    void openDatasetPicker(Panel* panel, bool isForeground);
    void refreshAllDatasetSlotNames();

    // Runs `backgroundWork` on a detached std::thread (not app_.pool --
    // that's per-panel and sized for pyramid building, this is a single
    // sequential job), showing `label` plus a progress bar in the status
    // bar for its duration (driven by backgroundTaskTimer_ polling
    // backgroundTaskDone_/backgroundTaskTotal_, which `backgroundWork`
    // updates as it goes). `uiCompletion` then runs back on the UI
    // thread. `backgroundWork` must not touch any QWidget/QObject --
    // it's safe to read a Dataset's mmap'd `file` (read-only, same as
    // the pyramid builder's own worker threads already do concurrently)
    // and to call plain file I/O (segy::writeSegyFile), nothing more.
    void runBackgroundTask(const QString& label, std::function<void()> backgroundWork,
                            std::function<void()> uiCompletion);
    void updateBackgroundTaskStatusBar();
};

} // namespace segyqt
