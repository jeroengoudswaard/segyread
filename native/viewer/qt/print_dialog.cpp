#include "print_dialog.h"

#include "axis_ticks.h"
#include "colormap.h"
#include "renderer.h"
#include "theme.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDate>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontMetricsF>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace segyqt {

namespace {

QColor rgb(uint32_t c) { return QColor(int((c >> 16) & 0xFF), int((c >> 8) & 0xFF), int(c & 0xFF)); }

QFont pageFont(const segy::PrintTemplate& t, double pt, double pxPerMm, bool bold = false) {
    QFont f(QString::fromStdString(t.fontFamily));
    f.setPixelSize(std::max(1, int(std::lround(pt * segy::kMmPerPoint * pxPerMm))));
    f.setBold(bold);
    return f;
}

QRectF toPx(const segy::RectMm& r, double k) { return QRectF(r.x * k, r.y * k, r.w * k, r.h * k); }

QString templateDir() { return QDir::homePath() + "/.segyread_print"; }
QString settingsPath() { return QDir::homePath() + "/.segyread_settings"; }

// Nice scale-bar length: >= 5 % of the width, 1/2/5 x 10^n m.
double scaleBarLength(double metresAcross) {
    const double target = 0.05 * metresAcross;
    const double decade = std::pow(10.0, std::floor(std::log10(target)));
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        if (m * decade >= target - 1e-9) return m * decade;
    }
    return 10.0 * decade;
}

// Short numbers without exponents: 10004, 1.9, 0.035.
QString compact(double v) {
    if (std::fabs(v) >= 100.0) return QString::number(v, 'f', 0);
    return QString::number(v, 'g', 3);
}

} // namespace

QImage renderPrintSeismic(const PrintSource& s, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    QImage img(width, height, QImage::Format_RGB32);
    img.fill(Qt::white);
    if (!s.dataset || s.dataset->traceCount <= 0) return img;
    static segy::ThreadPool pool;
    segy::RenderContext ctx;
    ctx.file = &s.dataset->file;
    ctx.pyramid = &s.dataset->pyramid;
    ctx.binHeader = s.dataset->binHeader;
    ctx.traceCount = s.dataset->traceCount;
    ctx.traceStrideBytes = s.dataset->traceStrideBytes;
    ctx.pool = &pool;
    std::vector<uint32_t> px(size_t(width) * size_t(height), 0xFFFFFFu);
    const segy::DisplaySettings& d = s.display;
    segy::renderFrame(ctx, s.view, px.data(), width, height, d.gainDb, d.colorScale, d.manualClip, d.reversePolarity,
                      d.colorScaleFlipped, &d.processing);
    for (int y = 0; y < height; ++y) {
        std::memcpy(img.scanLine(y), px.data() + size_t(y) * size_t(width), size_t(width) * 4);
    }
    if (d.flipHorizontal) img = img.mirrored(true, false);
    return img;
}

void paintPrintPage(QPainter& p, double k, const segy::PrintTemplate& t, const segy::PrintLayout& L,
                    const PrintSource& s, const PrintTexts& texts, const QImage& seismic) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.fillRect(toPx(L.page, k), Qt::white);
    const double hair = std::max(1.0, 0.2 * k);
    auto frame = [&](const segy::RectMm& r, const segy::PrintElement* e) {
        if (!e || !e->frame || !r.valid()) return;
        p.setPen(QPen(QColor(0xc8, 0xc9, 0xcc), hair));
        p.setBrush(Qt::NoBrush);
        p.drawRect(toPx(r, k));
    };

    // Title block: title, a subtitle from the text fields, a rule.
    if (const segy::PrintElement* e = t.find("title"); e && e->visible && L.title.valid()) {
        const QRectF r = toPx(L.title, k);
        p.setPen(rgb(e->color));
        p.setFont(pageFont(t, e->fontPt, k, true));
        const double titleH = e->fontPt * segy::kMmPerPoint * 1.25 * k;
        p.drawText(QRectF(r.left(), r.top(), r.width(), titleH), Qt::AlignLeft | Qt::AlignVCenter, texts.title);
        QStringList sub;
        for (const QString& part : {texts.client, texts.survey, texts.line}) {
            if (!part.trimmed().isEmpty()) sub << part.trimmed();
        }
        p.setPen(rgb(e->color).lighter(160));
        p.setFont(pageFont(t, e->fontPt * 0.6, k));
        p.drawText(QRectF(r.left(), r.top() + titleH, r.width(), r.height() - titleH - 2.0 * k),
                   Qt::AlignLeft | Qt::AlignVCenter, sub.join(QString::fromUtf8(" \xc2\xb7 ")));
        p.setPen(QPen(rgb(e->color), std::max(1.0, 0.5 * k)));
        p.drawLine(QPointF(r.left(), r.bottom() - 0.5 * k), QPointF(r.right(), r.bottom() - 0.5 * k));
        frame(L.title, e);
    }

    // Seismic, then what is drawn over it.
    const QRectF sr = toPx(L.seismic, k);
    if (L.seismic.valid()) {
        p.drawImage(sr, seismic);
        p.setPen(QPen(Qt::black, hair));
        p.setBrush(Qt::NoBrush);
        p.drawRect(sr);
    }
    const double traceSpan = std::max(1e-9, s.view.traceEnd - s.view.traceStart);
    const double sampleSpan = std::max(1e-9, s.view.sampleEnd - s.view.sampleStart);
    auto xForTrace = [&](double trace) {
        double f = (trace - s.view.traceStart) / traceSpan;
        if (s.display.flipHorizontal) f = 1.0 - f;
        return sr.left() + f * sr.width();
    };
    auto yForValue = [&](double value) {
        const double sample = s.sampleMs > 0.0 ? value / s.sampleMs : 0.0;
        return sr.top() + (sample - s.view.sampleStart) / sampleSpan * sr.height();
    };
    if (t.shows("horizons") && L.seismic.valid()) {
        p.save();
        p.setClipRect(sr);
        for (const PrintSource::Line& line : s.horizons) {
            QPainterPath path;
            bool open = false;
            for (const QPointF& pt : line.points) {
                if (!std::isfinite(pt.y())) {
                    open = false;
                    continue;
                }
                const QPointF xy(xForTrace(pt.x()), yForValue(pt.y()));
                if (open) path.lineTo(xy);
                else path.moveTo(xy);
                open = true;
            }
            p.setPen(QPen(line.color, std::max(1.0, 0.35 * k)));
            p.drawPath(path);
        }
        p.restore();
    }
    if (const segy::PrintElement* e = t.find("scalebar"); e && e->visible && s.traceSpacingM > 0.0 && L.seismic.valid()) {
        const double across = traceSpan * s.traceSpacingM;
        const double length = scaleBarLength(across);
        const double barW = length / across * sr.width();
        if (barW > 4.0 * k && barW < sr.width() / 2.0) {
            const bool km = length >= 10000.0;
            const QString label = km ? QString("%1 km").arg(compact(length / 1000.0)) : QString("%1 m").arg(compact(length));
            const QFont f = pageFont(t, e->fontPt, k);
            const QFontMetricsF fm(f);
            const double pad = 1.5 * k, barH = 1.5 * k;
            const double boxW = barW + 2 * pad + fm.horizontalAdvance(label) / 2.0 + 2.0;
            const double boxH = barH + fm.height() + 2.0 * pad;
            const QRectF box(sr.right() - boxW - 2.0 * k, sr.bottom() - boxH - 2.0 * k, boxW, boxH);
            p.fillRect(box, QColor(255, 255, 255, 225));
            const double bx = box.left() + pad, by = box.top() + pad;
            for (int i = 0; i < 4; ++i) {
                p.fillRect(QRectF(bx + barW * i / 4.0, by, barW / 4.0, barH), i % 2 == 0 ? rgb(e->color) : QColor(Qt::white));
            }
            p.setPen(QPen(rgb(e->color), hair));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(bx, by, barW, barH));
            p.setFont(f);
            p.drawText(QRectF(bx - 20.0 * k, by + barH, 40.0 * k, fm.height()), Qt::AlignHCenter | Qt::AlignTop, "0");
            p.drawText(QRectF(bx + barW - 30.0 * k, by + barH, 60.0 * k, fm.height()), Qt::AlignHCenter | Qt::AlignTop, label);
        }
    }

    // Axes: time or depth on the left, header values above.
    if (const segy::PrintElement* e = t.find("axes"); e && e->visible && L.timeAxis.valid()) {
        const QFont f = pageFont(t, e->fontPt, k);
        const QFontMetricsF fm(f);
        p.setFont(f);
        p.setPen(QPen(rgb(e->color), hair));
        const double tick = 1.0 * k;
        const QRectF ta = toPx(L.timeAxis, k);
        const double v0 = s.view.sampleStart * s.sampleMs, v1 = s.view.sampleEnd * s.sampleMs;
        const std::vector<segy::AxisTick> ticks = segy::computeNiceAxisTicks(
            v0, v1, std::max(1, int(sr.height())), 11, [&fm](const std::string&) { return int(fm.height() * 1.5); });
        for (const segy::AxisTick& tk : ticks) {
            const double y = sr.top() + tk.pixelPos;
            p.drawLine(QPointF(sr.left() - tick, y), QPointF(sr.left(), y));
            // Kept within the seismic's height (clear of the header row).
            const double ly = std::clamp(y, sr.top() + fm.height() / 2.0, sr.bottom() - fm.height() / 2.0);
            p.drawText(QRectF(ta.left(), ly - fm.height() / 2.0, ta.width() - tick - 0.8 * k, fm.height()),
                       Qt::AlignRight | Qt::AlignVCenter, QString::fromStdString(tk.label));
        }
        p.save();
        p.translate(ta.left() + fm.height() * 0.6, ta.center().y());
        p.rotate(-90);
        p.drawText(QRectF(-ta.height() / 2.0, -fm.height() / 2.0, ta.height(), fm.height()), Qt::AlignCenter,
                   s.verticalTitle);
        p.restore();
        // Header values along the top; the header's name in the corner.
        const QRectF xa = toPx(L.traceAxis, k);
        p.drawText(QRectF(ta.left(), xa.top(), ta.width() - tick - 0.8 * k, xa.height()), Qt::AlignRight | Qt::AlignVCenter,
                   s.traceAxisName);
        const int labelSlots = std::clamp(int(L.seismic.w / 28.0), 2, 10);
        double lastRight = -1e9;
        for (int i = 0; i < labelSlots; ++i) {
            const double trace = s.view.traceStart + (traceSpan - 1.0) * i / double(labelSlots - 1);
            const int64_t idx = std::clamp<int64_t>(int64_t(std::llround(trace)), 0,
                                                    s.dataset ? s.dataset->traceCount - 1 : 0);
            const QString label = s.traceLabel ? s.traceLabel(idx) : QString::number(idx + 1);
            const double x = xForTrace(double(idx) + 0.5);
            const double w = fm.horizontalAdvance(label);
            const double left = std::clamp(x - w / 2.0, sr.left(), sr.right() - w);
            if (left < lastRight + 2.0 * k) continue;
            p.drawLine(QPointF(x, sr.top() - tick), QPointF(x, sr.top()));
            p.drawText(QRectF(left, xa.top(), w + 1.0, xa.height() - tick), Qt::AlignLeft | Qt::AlignVCenter, label);
            lastRight = left + w;
        }
        frame(L.timeAxis, e);
    }

    // Colour bar: the display's colour scale, its two ends and the middle.
    if (const segy::PrintElement* e = t.find("colorbar"); e && e->visible && L.colorbar.valid()) {
        const QRectF cr = toPx(L.colorbar, k);
        const QRectF bar(cr.left(), cr.top(), 4.0 * k, cr.height());
        const int steps = std::max(2, int(bar.height()));
        const float dir = s.display.colorScaleFlipped ? -1.0f : 1.0f;
        for (int i = 0; i < steps; ++i) {
            const float tt = (1.0f - 2.0f * float(i) / float(steps - 1)) * dir;
            uint8_t r, g, b;
            segy::applyColorScale(s.display.colorScale, tt, &b, &g, &r);
            p.fillRect(QRectF(bar.left(), bar.top() + bar.height() * i / steps, bar.width(), bar.height() / steps + 1.0),
                       QColor(r, g, b));
        }
        p.setPen(QPen(rgb(e->color), hair));
        p.setBrush(Qt::NoBrush);
        p.drawRect(bar);
        const QFont f = pageFont(t, e->fontPt, k);
        const QFontMetricsF fm(f);
        p.setFont(f);
        const double tx = bar.right() + 1.5 * k;
        p.drawText(QRectF(tx, bar.top(), cr.right() - tx, fm.height()), Qt::AlignLeft | Qt::AlignTop, compact(s.clipHigh));
        p.drawText(QRectF(tx, bar.center().y() - fm.height() / 2.0, cr.right() - tx, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   compact(0.5 * (double(s.clipLow) + double(s.clipHigh))));
        p.drawText(QRectF(tx, bar.bottom() - fm.height(), cr.right() - tx, fm.height()), Qt::AlignLeft | Qt::AlignBottom,
                   compact(s.clipLow));
    }

    // Processing history and footer boxes.
    if (const segy::PrintElement* e = t.find("history"); e && e->visible && L.history.valid()) {
        const QRectF r = toPx(L.history, k).adjusted(1.5 * k, 1.0 * k, -1.5 * k, -1.0 * k);
        p.setPen(rgb(e->color));
        p.setFont(pageFont(t, e->fontPt, k, true));
        const QFontMetricsF fm(pageFont(t, e->fontPt, k));
        p.drawText(QRectF(r.left(), r.top(), r.width(), fm.height()), Qt::AlignLeft | Qt::AlignVCenter, "Processing");
        p.setFont(pageFont(t, e->fontPt, k));
        const QString body = s.history.isEmpty() ? QString("None (as recorded)") : s.history.join(QString::fromUtf8(" \xc2\xb7 "));
        p.drawText(QRectF(r.left(), r.top() + fm.height(), r.width(), r.height() - fm.height()),
                   Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, body);
        frame(L.history, e);
    }
    if (const segy::PrintElement* e = t.find("footer"); e && e->visible && L.footer.valid()) {
        const QRectF r = toPx(L.footer, k).adjusted(1.5 * k, 1.0 * k, -1.5 * k, -1.0 * k);
        p.setPen(rgb(e->color));
        p.setFont(pageFont(t, e->fontPt, k));
        const QFontMetricsF fm(p.font());
        const int64_t t0 = int64_t(std::floor(s.view.traceStart)) + 1, t1 = int64_t(std::ceil(s.view.traceEnd));
        const QString dash = QString::fromUtf8("\xe2\x80\x93"), dot = QString::fromUtf8(" \xc2\xb7 ");
        const QString first = QString("Traces %1%2%3%4%5%6%7 %8")
                                  .arg(t0)
                                  .arg(dash)
                                  .arg(t1)
                                  .arg(dot)
                                  .arg(compact(s.view.sampleStart * s.sampleMs))
                                  .arg(dash)
                                  .arg(compact(s.view.sampleEnd * s.sampleMs))
                                  .arg(s.verticalTitle.contains("Depth") ? "m" : "ms");
        const QString second = QString("%1%2TraceLab%3%4")
                                   .arg(QDate::currentDate().toString("dd-MM-yyyy"), dot, dot, s.fileName);
        p.drawText(QRectF(r.left(), r.top(), r.width(), fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(first, Qt::ElideRight, r.width()));
        p.drawText(QRectF(r.left(), r.top() + fm.height() * 1.2, r.width(), fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(second, Qt::ElideMiddle, r.width()));
        frame(L.footer, e);
    }
    p.restore();
}

// --- Preview ------------------------------------------------------------------

PrintPreview::PrintPreview(QWidget* parent) : QWidget(parent) {
    setMinimumSize(420, 320);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PrintPreview::setContent(const segy::PrintTemplate* tpl, const PrintSource* source, const PrintTexts* texts) {
    tpl_ = tpl;
    source_ = source;
    texts_ = texts;
    invalidateSeismic();
}

void PrintPreview::setSelected(const QString& key) {
    selected_ = key;
    update();
}

void PrintPreview::invalidateSeismic() {
    seismic_ = QImage();
    update();
}

void PrintPreview::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x3a, 0x3b, 0x3f)); // the desk around the paper, the same in both themes
    if (!tpl_ || !source_ || !texts_) return;
    layout_ = segy::layoutPrintPage(*tpl_, std::max(1, int(source_->history.size())));
    const double margin = 24.0;
    pxPerMm_ = std::min((width() - 2 * margin) / layout_.page.w, (height() - 2 * margin) / layout_.page.h);
    if (pxPerMm_ <= 0.0) return;
    origin_ = QPointF((width() - layout_.page.w * pxPerMm_) / 2.0, (height() - layout_.page.h * pxPerMm_) / 2.0);
    // The seismic at the preview's own size (cheap; redone on resize).
    const QSize need(std::max(1, int(layout_.seismic.w * pxPerMm_)), std::max(1, int(layout_.seismic.h * pxPerMm_)));
    if (seismic_.size() != need) seismic_ = renderPrintSeismic(*source_, need.width(), need.height());
    p.fillRect(QRectF(origin_ + QPointF(4, 6), QSizeF(layout_.page.w * pxPerMm_, layout_.page.h * pxPerMm_)),
               QColor(0, 0, 0, 90)); // shadow
    p.translate(origin_);
    paintPrintPage(p, pxPerMm_, *tpl_, layout_, *source_, *texts_, seismic_);
    // The selected element, for the style controls.
    const segy::RectMm* r = nullptr;
    if (selected_ == "title") r = &layout_.title;
    else if (selected_ == "axes") r = &layout_.timeAxis;
    else if (selected_ == "colorbar") r = &layout_.colorbar;
    else if (selected_ == "history") r = &layout_.history;
    else if (selected_ == "footer") r = &layout_.footer;
    else if (selected_ == "scalebar" || selected_ == "horizons") r = &layout_.seismic;
    if (r && r->valid()) {
        p.setPen(QPen(QColor(0xe8, 0x90, 0x5a), 2.0, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(toPx(*r, pxPerMm_).adjusted(-2, -2, 2, 2));
    }
}

void PrintPreview::mousePressEvent(QMouseEvent* event) {
    if (!onSelect || pxPerMm_ <= 0.0) return;
    const QPointF mm = (QPointF(event->pos()) - origin_) / pxPerMm_;
    auto in = [&mm](const segy::RectMm& r) {
        return r.valid() && mm.x() >= r.x && mm.x() <= r.right() && mm.y() >= r.y && mm.y() <= r.bottom();
    };
    if (in(layout_.title)) onSelect("title");
    else if (in(layout_.timeAxis) || in(layout_.traceAxis)) onSelect("axes");
    else if (in(layout_.colorbar)) onSelect("colorbar");
    else if (in(layout_.history)) onSelect("history");
    else if (in(layout_.footer)) onSelect("footer");
    else if (in(layout_.seismic)) onSelect("scalebar");
}

// --- Dialog -------------------------------------------------------------------

PrintDialog::PrintDialog(PrintSource source, QWidget* parent) : QDialog(parent), source_(std::move(source)) {
    setWindowTitle(QString("Print and Export") + QString::fromUtf8(" \xe2\x80\x94 ") + source_.datasetName);
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint |
                   Qt::WindowCloseButtonHint);
    resize(1320, 860);
    tpl_ = segy::defaultPrintTemplate();
    tpl_.fontFamily = QApplication::font().family().toStdString();

    // Title fields: the dataset's name, and what the text header labels.
    const segy::TextHeaderFields fields = segy::extractTextHeaderFields(source_.textHeader);
    headerTexts_.title = source_.datasetName;
    headerTexts_.client = QString::fromStdString(fields.client);
    headerTexts_.survey = QString::fromStdString(fields.survey);
    headerTexts_.line = QString::fromStdString(fields.line);
    texts_ = headerTexts_;

    QHBoxLayout* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    preview_ = new PrintPreview(this);
    root->addWidget(preview_, 1);

    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(360);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget* panel = new QWidget();
    panel->setObjectName("printPanel");
    panel->setStyleSheet(themed("#printPanel { background: #1a1b1e; }"));
    QVBoxLayout* side = new QVBoxLayout(panel);
    side->setContentsMargins(16, 14, 16, 14);
    side->setSpacing(10);
    scroll->setWidget(panel);
    root->addWidget(scroll);

    auto section = [&](const QString& text) {
        QLabel* l = new QLabel(text);
        l->setStyleSheet(themed("color: #8c8e95; font-size: 11px; font-weight: 600; letter-spacing: 1px; margin-top: 6px;"));
        side->addWidget(l);
    };
    auto caption = [](const QString& text) {
        QLabel* l = new QLabel(text);
        l->setStyleSheet(themed("color: #9a9ca3;"));
        return l;
    };
    // A row of exclusive buttons.
    auto choices = [&](const QStringList& labels, std::vector<QPushButton*>& out, std::function<void(int)> picked) {
        QHBoxLayout* row = new QHBoxLayout();
        row->setSpacing(4);
        QButtonGroup* group = new QButtonGroup(this);
        for (int i = 0; i < labels.size(); ++i) {
            QPushButton* b = new QPushButton(labels[i]);
            b->setCheckable(true);
            b->setStyleSheet(themed("QPushButton:checked { background: #4a3626; color: #f0a06c; border: 1px solid #e8905a; }"));
            group->addButton(b, i);
            row->addWidget(b);
            out.push_back(b);
            connect(b, &QPushButton::clicked, this, [this, picked, i]() {
                if (!syncing_) picked(i);
            });
        }
        row->addStretch();
        return row;
    };

    section("TEMPLATE");
    templateCombo_ = new QComboBox();
    templateCombo_->setToolTip("Template: a saved page design (paper, elements, colours, sizes)");
    side->addWidget(templateCombo_);
    QHBoxLayout* tplRow = new QHBoxLayout();
    QPushButton* saveTpl = new QPushButton("Save Template...");
    saveTpl->setToolTip("Save Template: this page design under a name, for other sections");
    everyPrintCheck_ = new QCheckBox("Use for every print");
    everyPrintCheck_->setToolTip("Use for every print: Print opens with this template for any section");
    tplRow->addWidget(saveTpl);
    tplRow->addWidget(everyPrintCheck_);
    tplRow->addStretch();
    side->addLayout(tplRow);

    section("OUTPUT");
    side->addLayout(choices({"PDF", "PNG", "JPG"}, formatButtons_, [this](int i) {
        tpl_.format = i == 0 ? "PDF" : i == 1 ? "PNG" : "JPG";
        templateChanged(false);
    }));
    side->addWidget(caption("Paper"));
    side->addLayout(choices({"A4", "Letter", "A3"}, paperButtons_, [this](int i) {
        tpl_.paper = i == 0 ? segy::PaperSize::A4 : i == 1 ? segy::PaperSize::Letter : segy::PaperSize::A3;
        templateChanged(true);
    }));
    side->addLayout(choices({"Landscape", "Portrait"}, orientButtons_, [this](int i) {
        tpl_.landscape = i == 0;
        templateChanged(true);
    }));
    QGridLayout* out = new QGridLayout();
    dpiCombo_ = new QComboBox();
    for (int dpi : {150, 300, 600}) dpiCombo_->addItem(QString("%1 dpi").arg(dpi), dpi);
    dpiCombo_->setToolTip("Resolution: of the seismic image in a PDF, and of the whole page in PNG or JPG");
    marginSpin_ = new QDoubleSpinBox();
    marginSpin_->setRange(0.0, 40.0);
    marginSpin_->setDecimals(0);
    marginSpin_->setSuffix(" mm");
    out->addWidget(caption("Resolution"), 0, 0);
    out->addWidget(dpiCombo_, 0, 1);
    out->addWidget(caption("Margins"), 1, 0);
    out->addWidget(marginSpin_, 1, 1);
    side->addLayout(out);
    connect(dpiCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (syncing_) return;
        tpl_.dpi = dpiCombo_->currentData().toInt();
    });
    connect(marginSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (syncing_) return;
        tpl_.marginMm = v;
        templateChanged(true);
    });

    section("TEXT");
    QGridLayout* textGrid = new QGridLayout();
    struct TextRow {
        const char* label;
        QLineEdit** edit;
        QString PrintTexts::*field;
    };
    const TextRow rows[] = {{"Title", &titleEdit_, &PrintTexts::title},
                            {"Client", &clientEdit_, &PrintTexts::client},
                            {"Survey", &surveyEdit_, &PrintTexts::survey},
                            {"Line", &lineEdit_, &PrintTexts::line}};
    int r = 0;
    for (const TextRow& row : rows) {
        QLineEdit* edit = new QLineEdit(texts_.*(row.field));
        edit->setCursorPosition(0);
        edit->setPlaceholderText("not in the text header");
        *row.edit = edit;
        textGrid->addWidget(caption(row.label), r, 0);
        textGrid->addWidget(edit, r, 1);
        const auto field = row.field;
        connect(edit, &QLineEdit::textEdited, this, [this, field](const QString& text) {
            texts_.*field = text;
            refreshTextBorders();
            preview_->update();
        });
        ++r;
    }
    side->addLayout(textGrid);
    QPushButton* resetText = new QPushButton("Reset to Text Header");
    resetText->setToolTip("Reset to Text Header: the title from the dataset name, the rest from labelled header lines");
    side->addWidget(resetText, 0, Qt::AlignLeft);
    connect(resetText, &QPushButton::clicked, this, [this]() {
        texts_ = headerTexts_;
        titleEdit_->setText(texts_.title);
        clientEdit_->setText(texts_.client);
        surveyEdit_->setText(texts_.survey);
        lineEdit_->setText(texts_.line);
        refreshTextBorders();
        preview_->update();
    });

    section("ELEMENTS");
    elementList_ = new QListWidget();
    elementList_->setToolTip("Tick to show; select to style it (or click it on the page)");
    elementList_->setMinimumHeight(170);
    side->addWidget(elementList_);
    for (const segy::PrintElement& e : tpl_.elements) {
        QListWidgetItem* item = new QListWidgetItem(QString::fromStdString(e.label));
        item->setData(Qt::UserRole, QString::fromStdString(e.key));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        elementList_->addItem(item);
    }
    connect(elementList_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (syncing_) return;
        if (segy::PrintElement* e = tpl_.find(item->data(Qt::UserRole).toString().toStdString())) {
            e->visible = item->checkState() == Qt::Checked;
            templateChanged(true);
        }
    });
    connect(elementList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        if (item && !syncing_) selectElement(item->data(Qt::UserRole).toString());
    });

    styleCaption_ = new QLabel();
    styleCaption_->setStyleSheet(themed("color: #8c8e95; font-size: 11px; font-weight: 600; letter-spacing: 1px; margin-top: 6px;"));
    side->addWidget(styleCaption_);
    QGridLayout* style = new QGridLayout();
    fontCombo_ = new QFontComboBox();
    fontCombo_->setToolTip("Font: for all text on the page");
    sizeSpin_ = new QDoubleSpinBox();
    sizeSpin_->setRange(4.0, 72.0);
    sizeSpin_->setDecimals(1);
    sizeSpin_->setSingleStep(0.5);
    sizeSpin_->setSuffix(" pt");
    colourButton_ = new QPushButton();
    colourButton_->setToolTip("Colour: of the element's text and lines");
    frameCheck_ = new QCheckBox("Frame");
    frameCheck_->setToolTip("Frame: a thin box around the element");
    style->addWidget(caption("Font"), 0, 0);
    style->addWidget(fontCombo_, 0, 1, 1, 2);
    style->addWidget(caption("Size"), 1, 0);
    style->addWidget(sizeSpin_, 1, 1);
    style->addWidget(colourButton_, 1, 2);
    style->addWidget(frameCheck_, 2, 1);
    side->addLayout(style);
    connect(fontCombo_, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) {
        if (syncing_) return;
        tpl_.fontFamily = f.family().toStdString();
        templateChanged(true);
    });
    connect(sizeSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (syncing_) return;
        if (segy::PrintElement* e = tpl_.find(selected_.toStdString())) e->fontPt = v;
        templateChanged(true);
    });
    connect(frameCheck_, &QCheckBox::toggled, this, [this](bool on) {
        if (syncing_) return;
        if (segy::PrintElement* e = tpl_.find(selected_.toStdString())) e->frame = on;
        templateChanged(false);
    });
    connect(colourButton_, &QPushButton::clicked, this, [this]() {
        segy::PrintElement* e = tpl_.find(selected_.toStdString());
        if (!e) return;
        const QColor c = QColorDialog::getColor(rgb(e->color), this, "Element Colour");
        if (!c.isValid()) return;
        e->color = (uint32_t(c.red()) << 16) | (uint32_t(c.green()) << 8) | uint32_t(c.blue());
        syncControls();
        preview_->update();
    });

    side->addStretch();
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();
    QPushButton* close = new QPushButton("Close");
    exportButton_ = new QPushButton();
    exportButton_->setDefault(true);
    exportButton_->setStyleSheet(themed("QPushButton { background: #e8905a; color: #1e1f22; border: 1px solid #e8905a; "
                                        "font-weight: 600; padding: 6px 14px; } QPushButton:hover { background: #f0a06c; }"));
    buttons->addWidget(close);
    buttons->addWidget(exportButton_);
    side->addLayout(buttons);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(exportButton_, &QPushButton::clicked, this, [this]() { exportPage(); });

    preview_->onSelect = [this](const QString& key) { selectElement(key); };
    connect(saveTpl, &QPushButton::clicked, this, [this]() { saveTemplate(); });
    connect(everyPrintCheck_, &QCheckBox::toggled, this, [this](bool on) {
        if (syncing_) return;
        QSettings settings(settingsPath(), QSettings::IniFormat);
        const QString path = templateCombo_->currentData().toString();
        if (on && !path.isEmpty()) settings.setValue("printTemplate", path);
        else settings.remove("printTemplate");
        if (on && path.isEmpty()) {
            QMessageBox::information(this, "Print and Export", "Save the template first, then tick Use for every print.");
            QSignalBlocker block(everyPrintCheck_);
            everyPrintCheck_->setChecked(false);
        }
    });
    connect(templateCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        const QString path = templateCombo_->currentData().toString();
        if (!path.isEmpty()) loadTemplateFile(path);
    });

    // The template used for every print, when one is set.
    refreshTemplateList();
    const QString every = QSettings(settingsPath(), QSettings::IniFormat).value("printTemplate").toString();
    if (!every.isEmpty() && QFileInfo::exists(every)) loadTemplateFile(every);

    preview_->setContent(&tpl_, &source_, &texts_);
    syncControls();
    selectElement("title");
}

void PrintDialog::refreshTextBorders() {
    auto mark = [](QLineEdit* edit, const QString& now, const QString& header) {
        edit->setStyleSheet(now == header ? QString() : themed("QLineEdit { border: 1px solid #e8905a; }"));
        edit->setToolTip(now == header ? QString("From the text header") : QString("Typed (the header has: %1)").arg(header));
    };
    mark(titleEdit_, texts_.title, headerTexts_.title);
    mark(clientEdit_, texts_.client, headerTexts_.client);
    mark(surveyEdit_, texts_.survey, headerTexts_.survey);
    mark(lineEdit_, texts_.line, headerTexts_.line);
}

void PrintDialog::syncControls() {
    syncing_ = true;
    formatButtons_[tpl_.format == "PNG" ? 1 : tpl_.format == "JPG" ? 2 : 0]->setChecked(true);
    paperButtons_[tpl_.paper == segy::PaperSize::Letter ? 1 : tpl_.paper == segy::PaperSize::A3 ? 2 : 0]->setChecked(true);
    orientButtons_[tpl_.landscape ? 0 : 1]->setChecked(true);
    dpiCombo_->setCurrentIndex(std::max(0, dpiCombo_->findData(tpl_.dpi)));
    marginSpin_->setValue(tpl_.marginMm);
    for (int i = 0; i < elementList_->count(); ++i) {
        QListWidgetItem* item = elementList_->item(i);
        const segy::PrintElement* e = tpl_.find(item->data(Qt::UserRole).toString().toStdString());
        item->setCheckState(e && e->visible ? Qt::Checked : Qt::Unchecked);
        if (item->data(Qt::UserRole).toString() == selected_) elementList_->setCurrentItem(item);
    }
    fontCombo_->setCurrentFont(QFont(QString::fromStdString(tpl_.fontFamily)));
    if (const segy::PrintElement* e = tpl_.find(selected_.toStdString())) {
        styleCaption_->setText("STYLE " + QString::fromUtf8("\xc2\xb7 ") + QString::fromStdString(e->label).toUpper());
        sizeSpin_->setValue(e->fontPt);
        frameCheck_->setChecked(e->frame);
        const QColor c = rgb(e->color);
        colourButton_->setText(c.name().toUpper());
        colourButton_->setStyleSheet(QString("QPushButton { background: %1; color: %2; border: 1px solid #3c3f41; }")
                                         .arg(c.name(), c.lightness() > 140 ? "#1e1f22" : "#f0f2f5"));
    }
    exportButton_->setText(QString("Export %1...").arg(QString::fromStdString(tpl_.format)));
    syncing_ = false;
}

void PrintDialog::templateChanged(bool seismicToo) {
    if (seismicToo) preview_->invalidateSeismic();
    else preview_->update();
    syncControls();
}

void PrintDialog::selectElement(const QString& key) {
    selected_ = key;
    preview_->setSelected(key);
    syncControls();
}

void PrintDialog::refreshTemplateList() {
    QSignalBlocker block(templateCombo_);
    templateCombo_->clear();
    templateCombo_->addItem("(This page)", QString());
    const QFileInfoList files = QDir(templateDir()).entryInfoList({"*.tlprint"}, QDir::Files, QDir::Name);
    for (const QFileInfo& f : files) templateCombo_->addItem(f.completeBaseName(), f.absoluteFilePath());
    const QString every = QSettings(settingsPath(), QSettings::IniFormat).value("printTemplate").toString();
    QSignalBlocker blockCheck(everyPrintCheck_);
    everyPrintCheck_->setChecked(false);
    for (int i = 0; i < templateCombo_->count(); ++i) {
        if (!every.isEmpty() && templateCombo_->itemData(i).toString() == every) everyPrintCheck_->setChecked(true);
    }
}

void PrintDialog::loadTemplateFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Print and Export", "Could not read " + path);
        return;
    }
    std::string error;
    segy::PrintTemplate loaded;
    if (!segy::printTemplateFromText(QString::fromUtf8(f.readAll()).toStdString(), loaded, &error)) {
        QMessageBox::warning(this, "Print and Export", QString::fromStdString(error));
        return;
    }
    tpl_ = loaded;
    const int index = templateCombo_->findData(path);
    if (index >= 0) templateCombo_->setCurrentIndex(index);
    const QString every = QSettings(settingsPath(), QSettings::IniFormat).value("printTemplate").toString();
    {
        QSignalBlocker block(everyPrintCheck_);
        everyPrintCheck_->setChecked(every == path);
    }
    templateChanged(true);
}

void PrintDialog::saveTemplate() {
    bool ok = false;
    const QString current = templateCombo_->currentData().toString().isEmpty() ? QString("My template")
                                                                                 : templateCombo_->currentText();
    const QString name = QInputDialog::getText(this, "Save Template", "Template name", QLineEdit::Normal, current, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QString safe = name;
    for (QChar& c : safe) {
        if (!c.isLetterOrNumber() && c != ' ' && c != '-' && c != '_') c = '_';
    }
    QDir().mkpath(templateDir());
    const QString path = templateDir() + "/" + safe + ".tlprint";
    tpl_.name = name.toStdString();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, "Save Template", "Could not write " + path);
        return;
    }
    f.write(QByteArray::fromStdString(segy::printTemplateToText(tpl_)));
    f.close();
    refreshTemplateList();
    templateCombo_->setCurrentIndex(std::max(0, templateCombo_->findData(path)));
    const QString every = QSettings(settingsPath(), QSettings::IniFormat).value("printTemplate").toString();
    QSignalBlocker block(everyPrintCheck_);
    everyPrintCheck_->setChecked(every == path);
}

void PrintDialog::exportPage() {
    const QString ext = tpl_.format == "PNG" ? "png" : tpl_.format == "JPG" ? "jpg" : "pdf";
    QSettings settings(settingsPath(), QSettings::IniFormat);
    const QString dir = settings.value("printLastDir", QDir::homePath()).toString();
    QString safe = texts_.title.isEmpty() ? source_.datasetName : texts_.title;
    for (QChar& c : safe) {
        if (!c.isLetterOrNumber() && c != '-' && c != '_' && c != '.') c = '_';
    }
    const QString path = QFileDialog::getSaveFileName(this, "Export " + QString::fromStdString(tpl_.format),
                                                      dir + "/" + safe + "." + ext,
                                                      QString("%1 (*.%2)").arg(QString::fromStdString(tpl_.format), ext));
    if (path.isEmpty()) return;
    settings.setValue("printLastDir", QFileInfo(path).absolutePath());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = exportTo(path);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, "Export", "Could not write " + path);
        return;
    }
    QMessageBox::information(this, "Export", "Saved " + QDir::toNativeSeparators(path));
}

bool PrintDialog::exportTo(const QString& path) {
    const QString ext = tpl_.format == "PNG" ? "png" : tpl_.format == "JPG" ? "jpg" : "pdf";
    const segy::PrintLayout L = segy::layoutPrintPage(tpl_, std::max(1, int(source_.history.size())));
    const double k = tpl_.dpi / 25.4; // device pixels per mm
    // The seismic at the export resolution (at most 8000 px a side).
    const double sw = L.seismic.w * k, sh = L.seismic.h * k;
    const double shrink = std::min(1.0, 8000.0 / std::max(sw, sh));
    const QImage seismic = renderPrintSeismic(source_, int(sw * shrink), int(sh * shrink));
    bool ok = false;
    if (ext == "pdf") {
        QPdfWriter writer(path);
        writer.setResolution(tpl_.dpi);
        const QPageSize::PageSizeId id = tpl_.paper == segy::PaperSize::Letter ? QPageSize::Letter
                                         : tpl_.paper == segy::PaperSize::A3   ? QPageSize::A3
                                                                               : QPageSize::A4;
        writer.setPageSize(QPageSize(id));
        writer.setPageOrientation(tpl_.landscape ? QPageLayout::Landscape : QPageLayout::Portrait);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle(texts_.title);
        writer.setCreator("TraceLab");
        QPainter painter;
        if (painter.begin(&writer)) {
            paintPrintPage(painter, double(writer.resolution()) / 25.4, tpl_, L, source_, texts_, seismic);
            ok = painter.end();
        }
    } else {
        QImage page(std::max(1, int(std::lround(L.page.w * k))), std::max(1, int(std::lround(L.page.h * k))),
                    QImage::Format_RGB32);
        page.fill(Qt::white);
        page.setDotsPerMeterX(int(std::lround(tpl_.dpi / 0.0254)));
        page.setDotsPerMeterY(int(std::lround(tpl_.dpi / 0.0254)));
        {
            QPainter painter(&page);
            paintPrintPage(painter, k, tpl_, L, source_, texts_, seismic);
        }
        ok = page.save(path, ext == "png" ? "PNG" : "JPG", ext == "png" ? -1 : 95);
    }
    return ok;
}

} // namespace segyqt
