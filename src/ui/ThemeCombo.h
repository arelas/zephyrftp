#pragma once

#include <QComboBox>
#include <QVariant>
#include "../Theme.h"

// Small shared helper for populating a theme-choice combo box — same
// header-only shape and same reasoning as ProtocolCombo.h (kept separate
// from Theme.h/AppSettings.h since those stay Core-only, no Widgets
// dependency, for app-settings-test's sake).
namespace ThemeCombo {

inline void populate(QComboBox *combo)
{
    combo->addItem(displayNameFor(Theme::Dark), QVariant::fromValue(int(Theme::Dark)));
    combo->addItem(displayNameFor(Theme::Light), QVariant::fromValue(int(Theme::Light)));
}

}
