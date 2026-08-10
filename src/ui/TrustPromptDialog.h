#pragma once

#include <QMessageBox>
#include <QWidget>
#include <QString>

// Shared helper for the two trust-on-first-use prompts (HostKeyVerifier,
// CertificateVerifier) — kept header-only since it's a one-shot, not
// because a .cpp would be wrong. Unifies only the part that was
// genuinely identical between the two (the QMessageBox
// warning-vs-question dispatch, the Yes/No buttons, the fail-safe "No"
// default, the bool return) — a real duplication found by code review.
// Each caller still builds its own title/body text, which is where the
// two genuinely differ (CertificateVerifier's extra `details` field).
namespace TrustPromptDialog {

// isMismatch=true uses QMessageBox::warning() (a stronger visual weight
// for "this changed since last time, possible MITM"); isMismatch=false
// uses QMessageBox::question() (the milder "haven't seen this before"
// case). Both default to No — fail safe if the user closes the dialog
// without an explicit choice.
inline bool confirm(QWidget *parent, const QString &title, const QString &text, bool isMismatch)
{
    const auto reply = isMismatch
        ? QMessageBox::warning(parent, title, text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        : QMessageBox::question(parent, title, text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return reply == QMessageBox::Yes;
}

}
