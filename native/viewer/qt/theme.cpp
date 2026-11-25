#include "theme.h"

#include <QRegularExpression>

#include <unordered_map>

namespace segyqt {

namespace {

UiTheme gTheme = UiTheme::Dark;

// Dark palette colour -> Windows Grey counterpart (0xRRGGBB). The dark
// palette is the one the UI is written in; only its own greys and the
// accent are mapped, never data colours.
const std::unordered_map<unsigned, unsigned>& greyMap() {
    static const std::unordered_map<unsigned, unsigned> map = {
        {0x1e1f22, 0xf0f0f0}, // window background
        {0x2b2d30, 0xf7f7f7}, // menu bar, tool bar, table headers
        {0x2b2d31, 0xf7f7f7},
        {0x17181a, 0xffffff}, // fields, lists, text views
        {0x1a1b1e, 0xfbfbfb},
        {0x201f23, 0xffffff}, // combo box popups
        {0x141416, 0xe6e6e6}, // canvas surround, chart backgrounds
        {0x000000, 0xe6e6e6}, // canvas axis columns (painted black in Dark)
        {0x3c3f41, 0xd0d0d0}, // borders, hover
        {0x34363b, 0xd9d9d9},
        {0x303238, 0xe3e3e3},
        {0x4a4c50, 0xb4b4b4}, // scroll bar handles, strong borders
        {0x6b6d72, 0x8c8c8c},
        {0xd4d4d8, 0x1b1b1b}, // text
        {0xe0e2e7, 0x1b1b1b},
        {0xd0d2d6, 0x2b2b2b},
        {0xf0f2f5, 0x000000}, // bright text
        {0xffffff, 0x000000}, // white text on the dark surround
        {0x9a9ca3, 0x505050}, // secondary text
        {0x7a7c82, 0x666666}, // section headings
        {0x68696e, 0x9a9a9a}, // disabled / placeholder text
        {0x66686d, 0x8a8a8a},
        {0xc7c9cc, 0x3a3a3a}, // icon strokes
        {0xe8905a, 0x0067c0}, // accent (Windows blue)
        {0xf0a06c, 0x1975c5}, // accent, hover
        {0x4a3626, 0xcce4f7}, // selection background
    };
    return map;
}

} // namespace

UiTheme uiTheme() { return gTheme; }
void setUiTheme(UiTheme theme) { gTheme = theme; }
bool lightTheme() { return gTheme == UiTheme::WindowsGrey; }

QString themed(QString css) {
    if (!lightTheme()) return css;
    static const QRegularExpression hex(QStringLiteral("#([0-9a-fA-F]{6})\\b"));
    QString out;
    out.reserve(css.size());
    qsizetype last = 0;
    auto it = hex.globalMatch(css);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += css.mid(last, m.capturedStart() - last);
        const unsigned rgb = m.captured(1).toUInt(nullptr, 16);
        const auto found = greyMap().find(rgb);
        out += found == greyMap().end() ? m.captured(0) : QString("#%1").arg(found->second, 6, 16, QChar('0'));
        last = m.capturedEnd();
    }
    out += css.mid(last);
    // Named colours used in a few style sheets.
    out.replace(QStringLiteral("background-color: black"), QStringLiteral("background-color: #ffffff"));
    out.replace(QStringLiteral("color: white"), QStringLiteral("color: #000000"));
    return out;
}

QColor tc(const QColor& dark) {
    if (!lightTheme()) return dark;
    const auto found = greyMap().find(unsigned(dark.rgb() & 0xffffff));
    if (found == greyMap().end()) return dark;
    QColor light = QColor::fromRgb(QRgb(0xff000000u | found->second));
    light.setAlpha(dark.alpha());
    return light;
}

} // namespace segyqt
