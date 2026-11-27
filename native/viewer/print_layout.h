// File > Print and Export: the page model, Qt-free and covered by segytest.
// A print template (paper, orientation, margins, output format, which
// elements show and how they look), its text form for saving, the page
// layout in millimetres (fit to page: the seismic takes what the other
// elements leave), and the title fields read from the text header. The Qt
// dialog (viewer/qt/print_dialog.*) draws a page from this.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace segy {

enum class PaperSize { A4, Letter, A3 };
const char* paperName(PaperSize paper);
// Width and height in mm for the orientation.
void paperSizeMm(PaperSize paper, bool landscape, double* widthMm, double* heightMm);

// One element of the page. Keys: "title", "axes", "colorbar", "scalebar",
// "horizons", "history", "footer". The seismic itself always shows.
struct PrintElement {
    std::string key;
    std::string label;
    bool visible = true;
    double fontPt = 8.0;
    uint32_t color = 0x1d1d1f; // 0xRRGGBB, text and lines
    bool frame = false;        // a thin box around it
};

struct PrintTemplate {
    std::string name = "Default";
    PaperSize paper = PaperSize::A4;
    bool landscape = true;
    double marginMm = 10.0;
    std::string format = "PDF"; // PDF, PNG or JPG
    int dpi = 300;
    std::string fontFamily = "Arial";
    std::vector<PrintElement> elements;
    PrintElement* find(const std::string& key);
    const PrintElement* find(const std::string& key) const;
    bool shows(const std::string& key) const; // visible (false when missing)
};

PrintTemplate defaultPrintTemplate();

// "key = value" lines; element lines "element.<key> = visible fontPt
// RRGGBB frame". Unknown keys are ignored, missing ones keep the default.
std::string printTemplateToText(const PrintTemplate& tpl);
bool printTemplateFromText(const std::string& text, PrintTemplate& tpl, std::string* error);

struct RectMm {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    bool valid() const { return w > 0.0 && h > 0.0; }
    double right() const { return x + w; }
    double bottom() const { return y + h; }
};

// Where everything goes, in mm from the page's top left. A hidden element
// gets an empty (invalid) rectangle and gives its room to the seismic.
// The scale bar and horizons are drawn inside `seismic`.
struct PrintLayout {
    RectMm page, content;
    RectMm title;
    RectMm timeAxis;  // left of the seismic: tick labels and the axis title
    RectMm traceAxis; // above the seismic: header values
    RectMm seismic;
    RectMm colorbar;  // right of the seismic
    RectMm history;   // bottom left
    RectMm footer;    // bottom right
};

// `historyLines`: lines of processing history to make room for (at least 1).
PrintLayout layoutPrintPage(const PrintTemplate& tpl, int historyLines);

constexpr double kMmPerPoint = 25.4 / 72.0;

// Title fields from a decoded 3200-byte text header: a value only when its
// line labels it ("CLIENT: Shell", "SURVEY = Paragon", "LINE: 1630"; also
// COMPANY, AREA, PROSPECT, LINE NAME); otherwise empty -- never a guess.
struct TextHeaderFields {
    std::string client, survey, line;
};
TextHeaderFields extractTextHeaderFields(const std::string& text3200);

} // namespace segy
