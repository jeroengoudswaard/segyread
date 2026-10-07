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
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "analysis.h"
#include "bandpass.h"
#include "chrome.h"
#include "colormap.h"
#include "loader.h"
#include "renderer.h"
#include "segy_mmap.h"
#include "segy_pyramid.h"
#include "segy_writer.h"
#include "threadpool.h"
#include "wiggle.h"

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

namespace segyqt {

// Left-click behavior on the canvas depends on which toolbar tool is
// active: Arrow only hovers/reads (no drag), Pan drag-pans, BoxSelect
// places/drags the selection box's corners. Wheel-zoom and 'R' reset are
// independent of this -- they work the same in every mode.
enum class ToolMode { Arrow, Pan, BoxSelect };

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

// SpectrumDialog's current settings -- same "parameters UI only, real
// computation built later" status as HistogramSettings. Field names and
// the three normalize modes come straight from a reference frequency-
// spectrum tool's own Utilities > Normalize/Smooth menus.
enum class SpectrumNormalizeMode { AbsoluteSelectedCurve, RelativeEachCurve, AbsoluteFromUser };
struct SpectrumSettings {
    bool useSelection = false; // false = Full View
    SpectrumNormalizeMode normalizeMode = SpectrumNormalizeMode::AbsoluteSelectedCurve;
    double userNormalizeValue = 0.0;
    int smoothPoints = 1; // one of kSpectrumSmoothPoints in viewer_qt.cpp
};

// DisplayParametersDialog's settings, opened from the toolbar's gain
// disclosure button -- modeled on a reference tool's "PanelDisplayDialog".
// Wired to real rendering: seismicMode (app_.display.seismicMode), gain
// (app_.display.gainDb), centerLine/positiveFill/negativeFill below, and
// Reverse Polarity (moved to app_.display.reversePolarity -- see chrome.h --
// since it applies to both display modes, not just wiggle). Everything else
// here is still a placeholder the same way HistogramSettings/SpectrumSettings
// are -- stored so the dialog's edits stick between opens, not yet read by
// any renderer.
enum class WiggleFillStyle { Solid, PeakAmplitude, Varifill, Band };
struct WigglePresentationSettings {
    bool centerLine = false;
    bool positiveFill = true;
    bool negativeFill = false;
    bool wiggleTrace = true;
    bool positiveOverNegative = true;
    WiggleFillStyle variableAreaStyle = WiggleFillStyle::Solid;
    WiggleFillStyle wiggleControlStyle = WiggleFillStyle::Solid;
    double variableDensityBlending = 50.0;
    double percentOverlap = 300.0;
    double fillBaseline = 0.0;
    double wiggleMinimumSpacing = 3.0;
    double varMinimumSpacing = 1.0;
};

// App-wide (not per-panel/per-dataset) preferences -- persisted to
// $HOME/.segyread_settings via QSettings (see MainWindow::loadPreferences/
// savePreferences), unlike every other *Settings struct in this file,
// which lives on a per-panel AppState and resets with a fresh session.
enum class ToolbarPosition { Left, Top, Bottom, Right };
enum class DrawerPosition { Left, Right };
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
};

// Same fields as the removed shells' AppState -- one shared struct instead
// of two divergent copies. Owned by MainWindow, referenced by SegyCanvas.
struct AppState {
    segy::ThreadPool pool{std::thread::hardware_concurrency()};

    bool loaded = false;
    bool loading = false;
    std::atomic<int64_t> progressDone{0};
    std::atomic<int64_t> progressTotal{1};

    segy::MappedFile file;
    std::filesystem::path filePath; // set once startLoading() succeeds; used for window titles
    segy::BinaryHeader binHeader{};
    int64_t traceCount = 0;
    size_t traceStrideBytes = 0;
    segy::Pyramid pyramid;

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

    // Empty for a file opened directly via File > Open; populated when
    // Calculator/Bandpass synthesize a new dataset (one entry per step that
    // produced it, oldest first) -- see SegyCanvas::startLoading's
    // `processingHistory` parameter. Save SEG-Y writes this straight into
    // the output file's own EBCDIC text header, so a chain of derived
    // datasets keeps a readable record of how each one was produced.
    std::vector<std::string> processingHistory;
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
    double timeMs = 0.0;
    float amplitude = 0.0f;
};

// A tall, narrow amplitude-to-color legend (gradient plus +clip/0/-clip
// labels), overlaid directly on the seismic display area -- replacing the
// color bar that used to be drawn into the canvas's pixel buffer by
// chrome.cpp (see native/README.md for why that relocation doesn't touch
// chrome.cpp/renderer.cpp at all) and, briefly, a dock-widget version of
// this same legend (also documented in native/README.md). Paints its own
// semi-transparent dark backing so it stays legible sitting on top of
// whatever seismic colors happen to be underneath it. Right-clickable (see
// SegyCanvas's constructor) to change AppState::display.colorScale -- no
// longer transparent-for-mouse-events like it briefly was as a pure
// overlay, since it now needs to receive that click itself.
class AmplitudeScaleWidget : public QWidget {
public:
    explicit AmplitudeScaleWidget(QWidget* parent = nullptr);
    // posClip/negClip are always positive magnitudes -- see
    // renderer.h's ManualClip. Equal values (the common, non-manual-clip
    // case) reproduce the old single-clip symmetric legend exactly; when
    // they differ, "0" on the gradient sits off-center at the matching
    // fraction and the two end labels show their own distinct values.
    void setClipRange(float posClip, float negClip);
    void setColorScale(segy::ColorScale scale);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    float posClip_ = 1.0f;
    float negClip_ = 1.0f;
    segy::ColorScale colorScale_ = segy::ColorScale::RedWhiteBlue;
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
    void setClipValues(float posClip, float negClip);
    void setSymmetrical(bool symmetrical);

    std::function<void(float posClip, float negClip)> onChanged;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    segy::HistogramResult result_;
    float posClip_ = 1.0f;
    float negClip_ = 1.0f;
    bool symmetrical_ = true;
    int draggingLine_ = -1; // -1 none, 0 = negative (left) line, 1 = positive (right) line

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
    QDoubleSpinBox* negSpin_ = nullptr;
    QDoubleSpinBox* posSpin_ = nullptr;
    QCheckBox* symmetricalCheck_ = nullptr;
    segy::ManualClip original_;
    std::function<void()> onChanged_;

    void recomputeHistogram(float rangeMin, float rangeMax);
    void applyLive(float posClip, float negClip, bool fromSymmetricalDrag);
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
    QRadioButton* absoluteSelectedRadio_ = nullptr;
    QRadioButton* relativeEachRadio_ = nullptr;
    QRadioButton* absoluteUserRadio_ = nullptr;
    QDoubleSpinBox* userValueSpin_ = nullptr;
    QComboBox* smoothCombo_ = nullptr;
    std::function<void()> onApplied_;

    void updateUserValueEnabled();
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

// Paint surface for SpectrumPlotWidget -- same split-out reasoning as
// HistogramChartArea. Plots computeSpectrum()'s result: amplitude (dB) vs
// frequency (Hz), one connected line, over regularly-ticked/gridlined axes
// on both dimensions -- same axes as the reference tool's own Spectra1d
// window.
class SpectrumChartArea : public QWidget {
public:
    explicit SpectrumChartArea(QWidget* parent = nullptr);
    void setResult(const segy::SpectrumResult& result, QColor sourceColor);
    void setGridLinesVisible(bool visible);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    segy::SpectrumResult result_;
    QColor sourceColor_ = Qt::white;
    bool gridLinesVisible_ = true;
};

// Same File/Edit/View/Utilities menu bar shape as HistogramPlotWidget,
// Utilities > Parameters... reopening SpectrumDialog instead.
class SpectrumPlotWidget : public QWidget {
public:
    explicit SpectrumPlotWidget(QWidget* parent = nullptr);
    void setResult(const segy::SpectrumResult& result, QColor sourceColor);
    void setParametersCallback(std::function<void()> callback);

private:
    SpectrumChartArea* chart_ = nullptr;
    std::function<void()> onParameters_;
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
// Two things from that reference are deliberately NOT wired to real
// behavior, present but inert: the Range/Min/Max trace-subset fields --
// loading only a subset of a file's traces is a real segycore/pyramid-
// builder feature, which always loads every trace unconditionally today,
// scoped out of this dialog rather than half-implemented -- and "Use
// Default Select/Sort Values" and "Clip at abs max/min", which don't map
// onto anything this app currently has an alternate mode for. "Use
// clipping from previously loaded window" IS wired: MainWindow::openFile()
// reads it and, when unchecked (the reference tool's own default),
// resets gain to 0 dB so each newly opened file gets a fresh auto-scaled
// clip instead of inheriting whatever gain the previous file was left at.
class SegyOpenDialog : public QDialog {
public:
    explicit SegyOpenDialog(QWidget* parent = nullptr);
    // Empty if the dialog was cancelled or nothing valid was ever selected.
    std::filesystem::path selectedPath() const;
    bool keepPreviousClip() const;

private:
    QComboBox* rangeCombo_ = nullptr;
    QSpinBox* minSpin_ = nullptr;
    QSpinBox* maxSpin_ = nullptr;
    QCheckBox* useDefaultsCheck_ = nullptr;
    QCheckBox* clipAbsCheck_ = nullptr;
    QCheckBox* keepPrevClipCheck_ = nullptr;

    QLineEdit* filterEdit_ = nullptr;
    QListWidget* dirList_ = nullptr;
    QListWidget* fileList_ = nullptr;
    QLineEdit* selectionEdit_ = nullptr;

    void refreshLists();
    void navigateTo(const QString& dirName);
    void tryAccept();
};

class PreferencesDialog : public QDialog {
public:
    explicit PreferencesDialog(const Preferences& initial, QWidget* parent = nullptr);
    void setAppliedCallback(std::function<void(const Preferences&)> callback);

private:
    QComboBox* toolbarPositionCombo_ = nullptr;
    QRadioButton* drawerLeftRadio_ = nullptr;
    QRadioButton* drawerRightRadio_ = nullptr;
    QSpinBox* uiScaleSpin_ = nullptr;
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
};

// Parameters dialog opened from the toolbar's gain disclosure button --
// modeled on a reference tool's "PanelDisplayDialog", covering wiggle/
// variable-density presentation style far beyond just gain. Only the
// display-mode radio (synced with app_.display.seismicMode, same as the
// toolbar's own toggle and the View menu) and the Gain field (synced with
// the toolbar's gainSlider_/gainValueLabel_ via MainWindow::setGainDb) are
// wired to real rendering; everything else writes to
// AppState::wigglePresentation as a placeholder (see native/README.md).
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
    QCheckBox* wiggleTraceCheck_ = nullptr;
    QCheckBox* positiveOverNegativeCheck_ = nullptr;
    QRadioButton* variableAreaStyleRadios_[4] = {};
    QRadioButton* wiggleControlStyleRadios_[4] = {};
    QDoubleSpinBox* blendingSpin_ = nullptr;
    QDoubleSpinBox* gainSpin_ = nullptr;
    QDoubleSpinBox* percentOverlapSpin_ = nullptr;
    QDoubleSpinBox* fillBaselineSpin_ = nullptr;
    QDoubleSpinBox* wiggleMinSpacingSpin_ = nullptr;
    QDoubleSpinBox* varMinSpacingSpin_ = nullptr;
    std::function<void(double)> onGainChanged_;
    std::function<void(segy::SeismicDisplayMode)> onModeChanged_;

    void updateBlendingEnabled();
    void applySettings();
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

    // `panelIndex` in the callback below is 0 for the panel named by
    // `nameA`/`loadedA` here, 1 for `nameB`/`loadedB` -- i.e. MainWindow's
    // panelA_/panelB_, not to be confused with the dialog's own "Dataset A"/
    // "Dataset B" combo boxes, which each independently pick one of the two.
    void refreshPanels(const QString& nameA, bool loadedA, const QString& nameB, bool loadedB);
    void setApplyCallback(std::function<void(int operandAPanelIndex, int operandBPanelIndex, bool subtract,
                                              QString outputName)>
                              callback);

private:
    QComboBox* datasetACombo_ = nullptr;
    QComboBox* datasetBCombo_ = nullptr;
    QRadioButton* addRadio_ = nullptr;
    QRadioButton* subtractRadio_ = nullptr;
    QLineEdit* outputNameEdit_ = nullptr;
    bool outputNameEdited_ = false; // true once the user types into it directly -- stop auto-updating it
    std::function<void(int, int, bool, QString)> onApply_;

    void updateDefaultOutputName();
    void emitApply();
};

// Edit > Processing > Bandpass: Hanning-tapered trapezoidal bandpass filter
// (segy::BandpassParams/applyBandpassFilter) applied to every trace of the
// active panel's dataset, auto-named and auto-displayed in the left panel
// (see MainWindow::runBandpass). Same loose-coupling shape as
// CalculatorDialog -- no AppState reference, just a name in and a callback
// out.
class BandpassDialog : public QDialog {
public:
    explicit BandpassDialog(QWidget* parent = nullptr);

    // Called right before each show() with the active panel's dataset name
    // (for the default output name) and whether one is even loaded.
    void refreshSource(const QString& sourceName, bool loaded);
    void setApplyCallback(std::function<void(segy::BandpassParams params, QString outputName)> callback);

private:
    QDoubleSpinBox* lowCutSpin_ = nullptr;
    QDoubleSpinBox* lowPassSpin_ = nullptr;
    QDoubleSpinBox* highPassSpin_ = nullptr;
    QDoubleSpinBox* highCutSpin_ = nullptr;
    QLineEdit* outputNameEdit_ = nullptr;
    QString sourceName_;
    bool outputNameEdited_ = false;
    std::function<void(segy::BandpassParams, QString)> onApply_;

    // Keeps lowCut <= lowPass <= highPass <= highCut as the user drags any
    // one of the four -- pushes the others out of the way just enough
    // rather than rejecting the edit, so the four spin boxes never need a
    // separate validation error state.
    void enforceOrdering();
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

// One band's rendered result in the Octave Band Display window -- a small
// variable-density raster (QImage, same 0x00RRGGBB packing renderFrame()
// itself produces) plus its frequency-range label. `bands[0]` is always the
// unfiltered source; the rest are octave bands, low frequency first.
struct OctaveBandResult {
    struct Band {
        QString label;
        QImage image;
    };
    std::vector<Band> bands;
};

// Spawned from the toolbar: bandpass-filters the *visible* traces/samples of
// a chosen panel (Left/Right) into a row of octave bands (2^N Hz steps,
// user-adjustable range), each rendered as its own small variable-density
// panel with its own auto-scaled clip -- see
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
    void setApplyCallback(std::function<void(int sourcePanelIndex, int minFreqHz, int maxFreqHz)> callback);

private:
    QComboBox* sourceCombo_ = nullptr;
    QComboBox* minFreqCombo_ = nullptr;
    QComboBox* maxFreqCombo_ = nullptr;
    QWidget* bandsRow_ = nullptr;
    std::function<void(int, int, int)> onApply_;

    void emitApply();
};

// The density-plot canvas: owns drawing and mouse/keyboard input. Reuses
// the exact same shared renderer/chrome calls the removed shells used.
class SegyCanvas : public QWidget {
public:
    explicit SegyCanvas(AppState& app, QWidget* parent = nullptr);

    // `processingHistory` is stashed on AppState once the load completes --
    // empty for a normal File > Open, populated when Calculator/Bandpass
    // load a dataset they just synthesized (see AppState::processingHistory).
    void startLoading(const std::filesystem::path& path, std::vector<std::string> processingHistory = {});

    // MainWindow owns the status bar (and, via the callback, the Del
    // button's visibility); handed to the canvas as a plain pointer/
    // callback rather than introducing Qt signals/slots for what is
    // otherwise a single, controlled producer/consumer link.
    void setStatusWidgets(QStatusBar* statusBar, std::function<void()> onStateChanged);

    // For MainWindow to call after actions that change app_.view/selection
    // state without going through a mouse event on the canvas itself (the
    // Zoom/Mooz toolbar buttons).
    void refreshStatusBar();

    // Split view: fires on mousePressEvent so MainWindow knows which panel
    // was just clicked into and can route toolbar/menu actions to it.
    void setActivatedCallback(std::function<void()> callback);

    // Dock's Trace Info section is driven by this instead of the status
    // bar now (see native/README.md, "Dark Pro" relocation notes).
    void setHoverInfoCallback(std::function<void(const HoverInfo&)> callback);

    // MainWindow computes the gain-adjusted clip value (it already owns
    // that math for the gain control); the canvas owns the overlay widget
    // itself since only it knows the plot's on-screen geometry to position
    // against.
    void setAmplitudeScale(float posClip, float negClip, bool visible, segy::ColorScale colorScale);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    AppState& app_;
    QStatusBar* statusBar_ = nullptr;
    std::function<void()> notifyStateChanged_;
    std::function<void(const HoverInfo&)> notifyHoverChanged_;
    std::function<void()> notifyActivated_;
    AmplitudeScaleWidget* amplitudeScale_ = nullptr;
    // Lazily created from the legend's own right-click menu -- see this
    // class's constructor. Owns its lifetime like the other per-panel
    // lazy dialogs, but unlike those, it's naturally already bound to the
    // correct panel forever (it's owned by that panel's own canvas), no
    // "bound to whichever was active at creation" caveat needed.
    ClipDialog* clipDialog_ = nullptr;
    void repositionAmplitudeScale();
    // Inset width available to the plot itself -- width() minus the
    // margins minus the amplitude-scale column reserved whenever
    // showColorBar is on (independent of seismicMode, so the plot's size
    // stays constant across Variable Density/Wiggle switches). Used
    // wherever pixel<->data math needs the plot's *actual* width, not the
    // raw inset width.
    int plotAreaWidth() const;

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
    void setActivePanel(Panel* panel);

    QAction* zoomAction_ = nullptr;
    QAction* moozAction_ = nullptr;
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

    // Read-only viewers -- re-populated from the active panel on every
    // click, no settings to keep sticky between opens (see
    // EbcdicHeaderWidget/BinaryHeaderWidget above).
    QAction* ebcdicHeaderAction_ = nullptr;
    EbcdicHeaderWidget* ebcdicHeaderWidget_ = nullptr;
    QAction* binaryHeaderAction_ = nullptr;
    BinaryHeaderWidget* binaryHeaderWidget_ = nullptr;

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
    QLabel* amplitudeValueLabel_ = nullptr;

    // Edit > Preferences -- app-wide, persisted to $HOME/.segyread_settings
    // (see loadPreferences/savePreferences), unlike every panel-local
    // *Settings struct elsewhere in this file. toolbar_ was a constructor
    // local until this needed to reposition it after the fact.
    Preferences prefs_;
    QToolBar* toolbar_ = nullptr;
    PreferencesDialog* preferencesDialog_ = nullptr;
    void loadPreferences();
    void savePreferences();
    void applyToolbarPosition(ToolbarPosition position);
    void applyDrawerPosition(DrawerPosition position);
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
    void runCalculator(AppState& a, AppState& b, bool subtract, const QString& outputName);
    // Bandpass-filters every trace of `source`'s full sample range, writes
    // the result to a temp SEG-Y file, and loads it into panelA_.
    void runBandpass(AppState& source, const segy::BandpassParams& params, const QString& outputName);
    // Writes `source`'s current dataset to `filePath` with a synthesized
    // EBCDIC header recording its processing history (see
    // buildSegyTextHeader in viewer_qt.cpp).
    void runSaveSegy(AppState& source, const QString& filePath);
    // Bandpass-filters the *visible* traces/samples of `sourcePanel` into a
    // row of octave bands and shows them in octaveBandDialog_.
    void computeAndShowOctaveBands(Panel* sourcePanel, int minFreqHz, int maxFreqHz);

    void openCalculatorDialog();
    void openBandpassDialog();
    void openSaveSegyDialog();
    void openOctaveBandDialog();
};

} // namespace segyqt
