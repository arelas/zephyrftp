#pragma once

#include <QComboBox>
#include <QVariant>
#include "../backends/Protocol.h"

// Small shared helper for populating a protocol-choice combo box — kept
// header-only since it's a one-shot, not because a .cpp would be wrong.
// Not folded into Protocol.h itself: that header is included from
// AppSettings.h, which in turn is linked into Core-only targets (e.g.
// app-settings-test, which deliberately links only Qt6::Core, no
// Qt6::Widgets) — a QComboBox dependency there would break those builds.
namespace ProtocolCombo {

// Populates combo with all three Protocol values, in enum-declaration
// order, using Protocol.h's own displayNameFor() for the label and the
// enum value itself (not the row index) as the item data — the pattern
// ConnectionDialog, SiteManagerDialog, and PreferencesDialog each
// hand-wrote identically, a real duplication found by code review: a
// fourth Protocol value would otherwise need the same addItem() line
// added by hand at three separate call sites, and missing one would
// silently leave that dialog's combo out of sync with the enum.
inline void populate(QComboBox *combo)
{
    combo->addItem(displayNameFor(Protocol::Sftp), QVariant::fromValue(int(Protocol::Sftp)));
    combo->addItem(displayNameFor(Protocol::Ftp),  QVariant::fromValue(int(Protocol::Ftp)));
    combo->addItem(displayNameFor(Protocol::Ftps), QVariant::fromValue(int(Protocol::Ftps)));
}

}
