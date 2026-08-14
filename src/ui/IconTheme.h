#pragma once

#include <QIcon>
#include <QColor>
#include <QString>
#include "../Theme.h"

// Loads a vendored SVG (see resources/icons.qrc) and recolors it at render
// time via QPainter compositing, rather than keeping a separately-colored
// copy of every icon for every accent it might appear in. This is the
// approach ICON-MAP.md's own "Shipping icons in Qt6" section calls for.
//
// The semantic accent colors are exposed as named constants/functions here
// (rather than scattered hex literals through the UI code) so any future
// re-tuning touches one place.
namespace IconTheme {

// Matches assets/zephyr-theme.css's --zf-* custom properties from the
// design package exactly. These four stay constant across both themes —
// saturated enough to read against either a dark or light background, and
// keeping them fixed means every existing call site (accent colors are
// used far more often than Gray/GrayMuted) needs no change at all for
// light-theme support.
inline const QColor Blue{0x5b, 0x8d, 0xef};    // primary / navigation / download / info
inline const QColor Green{0x34, 0xc7, 0x8e};   // connect / create / upload / success
inline const QColor Red{0xe0, 0x57, 0x4f};     // disconnect / delete / error
inline const QColor Amber{0xe8, 0xa8, 0x3c};   // pause / caution / in-progress warning

// Sets which theme Gray()/GrayMuted() (and, indirectly, every
// tintedIcon() call using them) resolve against. Called once at startup
// from main.cpp (before any icon is ever tinted, reading AppSettings::
// theme()) and again from MainWindow::onThemeChanged() on a live switch.
// GUI-thread only, same reasoning tintedIcon()'s own cache comment
// already gives — nothing in the backend/worker-thread layer touches
// icons.
void setTheme(Theme theme);
Theme currentTheme();

// Gray/GrayMuted are functions, not plain constants, specifically
// because they're the two colors that actually differ between themes —
// see setTheme() above. Gray == --zf-text-secondary, GrayMuted ==
// --zf-text-muted; both match resources/theme.qss's own tokens of the
// same name exactly, for whichever theme is currently active.
QColor Gray();
QColor GrayMuted();

// resourcePath is a Qt resource path, e.g. ":/icons/plug.svg". size is in
// logical pixels (square). Returns a null QIcon if the resource can't be
// loaded — callers should treat that as "no icon" rather than crashing,
// same as any other missing-resource case.
QIcon tintedIcon(const QString &resourcePath, const QColor &color, int size = 20);

}
