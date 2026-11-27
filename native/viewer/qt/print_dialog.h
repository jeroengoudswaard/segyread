// File > Print and Export: a page preview on A4, Letter or A3 with the
// panel's seismic and the elements around it (title block, axes, colour
// bar, scale bar, horizons, processing history, footer), exported as PDF,
// PNG or JPG. Fit to page only. The page model is print_layout.h (tested);
// this file draws it, once for the preview and once at the export
// resolution, with the same code. Templates are text files in
// ~/.segyread_print; "Use for every print" makes Print open with one.
#pragma once

#include "print_layout.h"
#include "viewer_qt.h"

#include <QDialog>
#include <QImage>
#include <QWidget>

#include <functional>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QLabel;
class QPainter;

namespace segyqt {

// What the page shows, gathered from a panel when Print opens.
struct PrintSource {
    std::shared_ptr<Dataset> dataset;
    segy::ViewRange view;
    segy::DisplaySettings display;
    float clipLow = -1.0f, clipHigh = 1.0f; // the colour bar's ends (data values)
    double sampleMs = 0.0;                  // per sample (m for depth)
    QString verticalTitle = "Time (ms)";
    double traceSpacingM = 0.0;             // 0: no scale bar
    QString traceAxisName = "Trace";        // the header on the trace axis
    std::function<QString(int64_t)> traceLabel;
    struct Line {
        QColor color;
        std::vector<QPointF> points; // x = trace (0-based, fractional), y = time/depth
    };
    std::vector<Line> horizons;
    QStringList history;
    QString datasetName, fileName;
    std::string textHeader; // decoded 3200 bytes, for the title fields
};

struct PrintTexts {
    QString title, client, survey, line;
};

class PrintPreview : public QWidget {
public:
    explicit PrintPreview(QWidget* parent = nullptr);
    void setContent(const segy::PrintTemplate* tpl, const PrintSource* source, const PrintTexts* texts);
    void setSelected(const QString& key);
    void invalidateSeismic(); // the template or the data changed
    std::function<void(const QString&)> onSelect;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    const segy::PrintTemplate* tpl_ = nullptr;
    const PrintSource* source_ = nullptr;
    const PrintTexts* texts_ = nullptr;
    QString selected_;
    QImage seismic_;
    double pxPerMm_ = 1.0;
    QPointF origin_;
    segy::PrintLayout layout_;
};

class PrintDialog : public QDialog {
public:
    PrintDialog(PrintSource source, QWidget* parent = nullptr);
    // Writes the page to `path` as the template's format; false on failure.
    bool exportTo(const QString& path);

private:
    PrintSource source_;
    segy::PrintTemplate tpl_;
    PrintTexts texts_, headerTexts_;
    QString selected_ = "title";
    PrintPreview* preview_ = nullptr;
    QComboBox* templateCombo_ = nullptr;
    QCheckBox* everyPrintCheck_ = nullptr;
    std::vector<QPushButton*> formatButtons_, paperButtons_, orientButtons_;
    QComboBox* dpiCombo_ = nullptr;
    QDoubleSpinBox* marginSpin_ = nullptr;
    QLineEdit* titleEdit_ = nullptr;
    QLineEdit* clientEdit_ = nullptr;
    QLineEdit* surveyEdit_ = nullptr;
    QLineEdit* lineEdit_ = nullptr;
    QListWidget* elementList_ = nullptr;
    QLabel* styleCaption_ = nullptr;
    QFontComboBox* fontCombo_ = nullptr;
    QDoubleSpinBox* sizeSpin_ = nullptr;
    QPushButton* colourButton_ = nullptr;
    QCheckBox* frameCheck_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    bool syncing_ = false;

    void syncControls();   // template -> widgets
    void templateChanged(bool seismicToo);
    void selectElement(const QString& key);
    void refreshTemplateList();
    void loadTemplateFile(const QString& path);
    void saveTemplate();
    void exportPage();
    void refreshTextBorders();
};

// Draws one page at `pxPerMm` (the painter's origin = the page's top left).
// `seismic` is drawn into the layout's seismic rectangle (any size).
void paintPrintPage(QPainter& painter, double pxPerMm, const segy::PrintTemplate& tpl, const segy::PrintLayout& layout,
                    const PrintSource& source, const PrintTexts& texts, const QImage& seismic);

// The seismic as the panel shows it (variable density), `width` x `height` px.
QImage renderPrintSeismic(const PrintSource& source, int width, int height);

} // namespace segyqt
