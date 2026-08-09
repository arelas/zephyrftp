// libsecret pulls in glib/gio headers that declare a struct member
// literally named `signals` — collides with Qt's `signals:` macro the
// instant any Qt header (even just <QString>, transitively) has already
// been parsed. Including it first, before "CredentialStore.h" activates
// that macro, sidesteps the collision entirely rather than fighting it
// with QT_NO_KEYWORDS.
#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#else
#include <libsecret/secret.h>
#endif

#include "CredentialStore.h"

namespace {

// A stable, namespaced key so this app's entries are identifiable and
// don't collide with anything else using the same store — matters more
// on Windows, where Credential Manager is a single flat namespace
// shared by every app on the system, than on libsecret, which already
// scopes by schema+attributes, but used consistently either way.
QString targetName(const QString &siteId)
{
    return QStringLiteral("ZephyrFTP/site/%1").arg(siteId);
}

#ifndef _WIN32
// SECRET_SCHEMA_NONE (not SECRET_SCHEMA_DONT_MATCH_NAME) — this schema
// is exclusively this app's own, so there's no reason to relax
// matching. site_id is the one attribute that identifies which saved
// site a given secret belongs to.
const SecretSchema *credentialSchema()
{
    static const SecretSchema schema = {
        "org.zephyrftp.SavedSitePassword", SECRET_SCHEMA_NONE,
        {
            {"site_id", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SecretSchemaAttributeType(0)},
        }
    };
    return &schema;
}
#endif

}

namespace CredentialStore {

#ifdef _WIN32

bool save(const QString &siteId, const QString &secret)
{
    const std::wstring target = targetName(siteId).toStdWString();
    const QByteArray blob = secret.toUtf8();

    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(blob.constData()));
    // LOCAL_MACHINE (not SESSION) — the whole point is surviving an app
    // restart, which a session-scoped credential wouldn't reliably do
    // across a real logoff/login, only within one login session.
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    return CredWriteW(&cred, 0) != FALSE;
}

bool load(const QString &siteId, QString *secret)
{
    const std::wstring target = targetName(siteId).toStdWString();
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred))
        return false;

    *secret = QString::fromUtf8(reinterpret_cast<const char *>(cred->CredentialBlob),
                                 static_cast<int>(cred->CredentialBlobSize));
    CredFree(cred);
    return true;
}

bool remove(const QString &siteId)
{
    const std::wstring target = targetName(siteId).toStdWString();
    return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) != FALSE;
}

bool hasSecret(const QString &siteId)
{
    // Delegates to load() rather than its own CredReadW/CredFree pair —
    // matches both CredentialStore.h's documented contract ("implemented
    // on top of load() on both platforms") and the Linux implementation
    // below, which already does this. Code review found these had drifted
    // apart: a future fix to load()'s behavior (error handling, encoding)
    // wouldn't otherwise have applied here too.
    QString discard;
    return load(siteId, &discard);
}

#else

bool save(const QString &siteId, const QString &secret)
{
    GError *error = nullptr;
    // SECRET_COLLECTION_DEFAULT: whichever collection the user's own
    // secret service (GNOME Keyring, KWallet's Secret Service
    // compatibility layer, etc.) treats as default — not this app's
    // call to make.
    // secret specifically (unlike siteId, always one of this app's own
    // ASCII UUIDs, and the label, this app's own ASCII text) can be
    // arbitrary user-typed text — encoded here as UTF-8 explicitly,
    // matching load()'s QString::fromUtf8() decode below. qPrintable()
    // would use the LOCAL 8-bit encoding instead, which is identical to
    // UTF-8 on the common case (a UTF-8 locale) but silently corrupts a
    // non-ASCII password/passphrase on any system that isn't — a real
    // bug found by code review, not exercised by any test in an
    // environment that's always been UTF-8.
    const QByteArray secretUtf8 = secret.toUtf8();
    const bool ok = secret_password_store_sync(
        credentialSchema(), SECRET_COLLECTION_DEFAULT,
        qPrintable(QStringLiteral("ZephyrFTP: saved site %1").arg(siteId)),
        secretUtf8.constData(), nullptr, &error,
        "site_id", qPrintable(siteId), nullptr);

    if (error) {
        g_error_free(error);
        return false;
    }
    return ok;
}

bool load(const QString &siteId, QString *secret)
{
    GError *error = nullptr;
    gchar *result = secret_password_lookup_sync(
        credentialSchema(), nullptr, &error,
        "site_id", qPrintable(siteId), nullptr);

    if (error) {
        g_error_free(error);
        return false;
    }
    if (!result)
        return false;

    *secret = QString::fromUtf8(result);
    secret_password_free(result);
    return true;
}

bool remove(const QString &siteId)
{
    GError *error = nullptr;
    const bool removed = secret_password_clear_sync(
        credentialSchema(), nullptr, &error,
        "site_id", qPrintable(siteId), nullptr);

    if (error) {
        g_error_free(error);
        return false;
    }
    return removed;
}

bool hasSecret(const QString &siteId)
{
    QString discard;
    return load(siteId, &discard);
}

#endif

}
