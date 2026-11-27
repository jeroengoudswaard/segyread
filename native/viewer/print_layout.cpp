#include "print_layout.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>
#include <sstream>

namespace segy {

const char* paperName(PaperSize paper) {
    switch (paper) {
    case PaperSize::Letter: return "Letter";
    case PaperSize::A3: return "A3";
    default: return "A4";
    }
}

void paperSizeMm(PaperSize paper, bool landscape, double* widthMm, double* heightMm) {
    double w = 210.0, h = 297.0; // portrait
    if (paper == PaperSize::Letter) {
        w = 215.9;
        h = 279.4;
    } else if (paper == PaperSize::A3) {
        w = 297.0;
        h = 420.0;
    }
    if (landscape) std::swap(w, h);
    *widthMm = w;
    *heightMm = h;
}

PrintElement* PrintTemplate::find(const std::string& key) {
    for (PrintElement& e : elements) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

const PrintElement* PrintTemplate::find(const std::string& key) const {
    for (const PrintElement& e : elements) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

bool PrintTemplate::shows(const std::string& key) const {
    const PrintElement* e = find(key);
    return e && e->visible;
}

PrintTemplate defaultPrintTemplate() {
    PrintTemplate t;
    t.elements = {
        {"title", "Title block", true, 14.0, 0x1d1d1f, false},
        {"axes", "Axes", true, 7.0, 0x1d1d1f, false},
        {"colorbar", "Colour bar", true, 7.0, 0x1d1d1f, false},
        {"scalebar", "Scale bar", true, 7.0, 0x000000, false},
        {"horizons", "Horizons", true, 7.0, 0x1d1d1f, false},
        {"history", "Processing history", true, 6.5, 0x333333, true},
        {"footer", "Footer", true, 6.5, 0x333333, true},
    };
    return t;
}

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
} // namespace

std::string printTemplateToText(const PrintTemplate& t) {
    std::ostringstream out;
    out << "# TraceLab print template\n";
    out << "name = " << t.name << '\n';
    out << "paper = " << paperName(t.paper) << '\n';
    out << "orientation = " << (t.landscape ? "landscape" : "portrait") << '\n';
    out << "margin_mm = " << t.marginMm << '\n';
    out << "format = " << t.format << '\n';
    out << "dpi = " << t.dpi << '\n';
    out << "font = " << t.fontFamily << '\n';
    for (const PrintElement& e : t.elements) {
        char colour[8];
        std::snprintf(colour, sizeof(colour), "%06X", unsigned(e.color & 0xFFFFFF));
        out << "element." << e.key << " = " << (e.visible ? 1 : 0) << ' ' << e.fontPt << ' ' << colour << ' '
            << (e.frame ? 1 : 0) << '\n';
    }
    return out.str();
}

bool printTemplateFromText(const std::string& text, PrintTemplate& tpl, std::string* error) {
    PrintTemplate t = defaultPrintTemplate();
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "Line " + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        try {
            if (key == "name") {
                t.name = value;
            } else if (key == "paper") {
                t.paper = value == "Letter" ? PaperSize::Letter : value == "A3" ? PaperSize::A3 : PaperSize::A4;
            } else if (key == "orientation") {
                t.landscape = value != "portrait";
            } else if (key == "margin_mm") {
                t.marginMm = std::clamp(std::stod(value), 0.0, 50.0);
            } else if (key == "format") {
                t.format = (value == "PNG" || value == "JPG") ? value : std::string("PDF");
            } else if (key == "dpi") {
                t.dpi = std::clamp(std::stoi(value), 72, 1200);
            } else if (key == "font") {
                if (!value.empty()) t.fontFamily = value;
            } else if (key.rfind("element.", 0) == 0) {
                PrintElement* e = t.find(key.substr(8));
                if (!e) continue; // an element this version doesn't have
                std::istringstream v(value);
                int visible = 1, frame = 0;
                double pt = e->fontPt;
                std::string colour;
                v >> visible >> pt >> colour >> frame;
                e->visible = visible != 0;
                e->fontPt = std::clamp(pt, 4.0, 72.0);
                if (colour.size() == 6) e->color = uint32_t(std::stoul(colour, nullptr, 16));
                e->frame = frame != 0;
            }
        } catch (...) {
            if (error) *error = "Line " + std::to_string(lineNo) + ": not a valid value";
            return false;
        }
    }
    tpl = std::move(t);
    return true;
}

PrintLayout layoutPrintPage(const PrintTemplate& t, int historyLines) {
    PrintLayout L;
    paperSizeMm(t.paper, t.landscape, &L.page.w, &L.page.h);
    const double m = std::clamp(t.marginMm, 0.0, std::min(L.page.w, L.page.h) / 4.0);
    L.content = {m, m, L.page.w - 2 * m, L.page.h - 2 * m};
    const double gap = 3.0;
    double top = L.content.y;
    double bottom = L.content.bottom();

    if (const PrintElement* e = t.find("title"); e && e->visible) {
        // Title line, a subtitle at 60 %, and a rule under them.
        const double h = e->fontPt * kMmPerPoint * 1.25 + e->fontPt * 0.6 * kMmPerPoint * 1.35 + 2.0;
        L.title = {L.content.x, top, L.content.w, h};
        top += h + gap;
    }
    const bool history = t.shows("history"), footer = t.shows("footer");
    if (history || footer) {
        const double histPt = history ? t.find("history")->fontPt : 0.0;
        const double footPt = footer ? t.find("footer")->fontPt : 0.0;
        const double histH = history ? (1 + std::max(1, historyLines)) * histPt * kMmPerPoint * 1.35 + 3.0 : 0.0;
        const double footH = footer ? 2 * footPt * kMmPerPoint * 1.35 + 3.0 : 0.0;
        const double h = std::max(histH, footH);
        bottom -= h;
        const double footW = footer ? std::min(80.0, L.content.w * (history ? 0.4 : 1.0)) : 0.0;
        if (footer) L.footer = {L.content.right() - footW, bottom, footW, h};
        if (history) L.history = {L.content.x, bottom, L.content.w - (footer ? footW + gap : 0.0), h};
        bottom -= gap;
    }

    double left = L.content.x, right = L.content.right();
    double seismicTop = top;
    if (const PrintElement* e = t.find("axes"); e && e->visible) {
        const double em = e->fontPt * kMmPerPoint;
        // Labels like "10000" plus the rotated axis title.
        const double axisW = em * 0.6 * 6 + em * 1.4 + 1.5;
        const double axisH = em * 1.4 + 1.0;
        L.timeAxis = {left, seismicTop + axisH, axisW, 0.0};
        left += axisW;
        L.traceAxis = {left, seismicTop, 0.0, axisH};
        seismicTop += axisH;
    }
    if (const PrintElement* e = t.find("colorbar"); e && e->visible) {
        const double em = e->fontPt * kMmPerPoint;
        const double barW = 4.0 + 1.5 + em * 0.6 * 7;
        L.colorbar = {right - barW, seismicTop, barW, 0.0};
        right -= barW + 2.0;
    }
    L.seismic = {left, seismicTop, std::max(0.0, right - left), std::max(0.0, bottom - seismicTop)};
    if (L.timeAxis.w > 0) L.timeAxis.h = L.seismic.h;
    if (L.traceAxis.h > 0) L.traceAxis.w = L.seismic.w;
    if (L.colorbar.w > 0) L.colorbar.h = L.seismic.h;
    return L;
}

TextHeaderFields extractTextHeaderFields(const std::string& text) {
    TextHeaderFields f;
    // LABEL: value, ending at two spaces or the line's end. The label must
    // be followed by ':' or '=' -- a bare word is not a label.
    static const std::regex re(
        R"((CLIENT|COMPANY|SURVEY|AREA|PROSPECT|LINE NAME|LINE NO\.?|LINE)\s*[:=]\s*([^ :=][^:=]*?)(?=\s{2,}|\s*$))",
        std::regex::icase);
    const size_t lines = (text.size() + 79) / 80;
    for (size_t li = 0; li < lines; ++li) {
        std::string line = text.substr(li * 80, 80);
        // "C 1 " / "C01 " card numbers are not part of the text.
        if (line.size() >= 4 && (line[0] == 'C' || line[0] == 'c') && std::isdigit(static_cast<unsigned char>(line[2]))) {
            line = line.substr(4);
        } else if (line.size() >= 3 && (line[0] == 'C' || line[0] == 'c') && std::isdigit(static_cast<unsigned char>(line[1]))) {
            line = line.substr(3);
        }
        for (std::sregex_iterator it(line.begin(), line.end(), re), end; it != end; ++it) {
            std::string label = (*it)[1].str();
            for (char& c : label) c = char(std::toupper(static_cast<unsigned char>(c)));
            const std::string value = trim((*it)[2].str());
            if (value.empty()) continue;
            if ((label == "CLIENT" || label == "COMPANY") && f.client.empty()) f.client = value;
            else if ((label == "SURVEY" || label == "AREA" || label == "PROSPECT") && f.survey.empty()) f.survey = value;
            else if (label.rfind("LINE", 0) == 0 && f.line.empty()) f.line = value;
        }
    }
    return f;
}

} // namespace segy
