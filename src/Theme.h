#pragma once

#include <QString>
#include <QObject>

// Which visual theme the app renders in. Lives at this top level (not
// under src/ui/) and pulls in nothing beyond QString/QObject specifically
// so AppSettings.h can include it without breaking app-settings-test,
// which deliberately links Qt6::Core only, no Widgets — the same
// Core-vs-Widgets layering reason ProtocolCombo.h documents for keeping
// itself separate from Protocol.h/AppSettings.h. IconTheme.h (a
// Widgets-adjacent header) includes this one too, not the other way
// around.
enum class Theme {
    Dark,
    Light
};

// Shown in the theme combo box (Preferences).
inline QString displayNameFor(Theme theme)
{
    switch (theme) {
    case Theme::Dark:  return QObject::tr("Dark");
    case Theme::Light: return QObject::tr("Light");
    }
    return QObject::tr("Dark");
}

// Stable string keys for JSON persistence — same reasoning as
// Protocol.h's protocolToKey()/protocolFromKey(): not the raw enum's
// integer value, which would silently change meaning if the enum were
// ever reordered.
inline QString themeToKey(Theme theme)
{
    switch (theme) {
    case Theme::Dark:  return QStringLiteral("dark");
    case Theme::Light: return QStringLiteral("light");
    }
    return QStringLiteral("dark");
}

// Anything unrecognized — including the key being absent entirely, which
// is exactly what every settings.json written before this field existed
// looks like — reads back as Dark, the app's existing (and only, until
// now) behavior.
inline Theme themeFromKey(const QString &key)
{
    if (key == QStringLiteral("light")) return Theme::Light;
    return Theme::Dark;
}
