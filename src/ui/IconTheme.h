#pragma once

#include <QIcon>
#include <QColor>
#include <QString>

// Loads a vendored SVG (see resources/icons.qrc) and recolors it at render
// time via QPainter compositing, rather than keeping a separately-colored
// copy of every icon for every accent it might appear in. This is the
// approach ICON-MAP.md's own "Shipping icons in Qt6" section calls for.
//
// The four semantic accent colors are exposed as named constants here
// (rather than scattered hex literals through the UI code) so any future
// re-tuning — the design README explicitly calls out this as the
// only thing a light-mode variant would need to change — touches one place.
namespace IconTheme {

// Matches assets/zephyr-theme.css's --zf-* custom properties from the
// design package exactly.
inline const QColor Blue{0x5b, 0x8d, 0xef};    // primary / navigation / download / info
inline const QColor Green{0x34, 0xc7, 0x8e};   // connect / create / upload / success
inline const QColor Red{0xe0, 0x57, 0x4f};     // disconnect / delete / error
inline const QColor Amber{0xe8, 0xa8, 0x3c};   // pause / caution / in-progress warning
inline const QColor Gray{0x87, 0x90, 0xa0};    // neutral / no semantic weight
inline const QColor GrayMuted{0x5c, 0x64, 0x72}; // dimmer neutral, for "done/finished" rows

// resourcePath is a Qt resource path, e.g. ":/icons/plug.svg". size is in
// logical pixels (square). Returns a null QIcon if the resource can't be
// loaded — callers should treat that as "no icon" rather than crashing,
// same as any other missing-resource case.
QIcon tintedIcon(const QString &resourcePath, const QColor &color, int size = 20);

}
