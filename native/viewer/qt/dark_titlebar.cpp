#include "dark_titlebar.h"

#include <QApplication>
#include <QEvent>
#include <QWidget>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace segyqt {

#ifdef Q_OS_WIN
namespace {

// DWMWA_USE_IMMERSIVE_DARK_MODE: 20 on Windows 10 20H1+ and Windows 11.
// Older SDK headers lack the name, hence the literal.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;

class DarkTitleBarFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        // Polish runs once per widget before it is first shown, so the
        // title bar is dark from its very first frame (setting it on Show
        // could flash the light bar first).
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            auto* widget = qobject_cast<QWidget*>(watched);
            if (widget && widget->isWindow()) {
                const Qt::WindowType type = widget->windowType();
                if (type == Qt::Window || type == Qt::Dialog) {
                    BOOL dark = TRUE;
                    // winId() creates the native window if it doesn't exist
                    // yet; a top-level window needs one anyway.
                    DwmSetWindowAttribute(reinterpret_cast<HWND>(widget->winId()), kDwmUseImmersiveDarkMode, &dark,
                                          sizeof(dark));
                }
            }
        }
        return false; // never swallow the event
    }
};

} // namespace

void installDarkTitleBars(QApplication& app) { app.installEventFilter(new DarkTitleBarFilter(&app)); }
#else
void installDarkTitleBars(QApplication&) {}
#endif

} // namespace segyqt
