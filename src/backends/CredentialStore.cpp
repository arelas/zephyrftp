// libsecret pulls in glib/gio headers that declare a struct member
// literally named `signals` — collides with Qt's `signals:` macro the
// instant any Qt header (even just <QString>, transitively) has already
// been parsed. Including it first, before "CredentialStore.h" activates
// that macro, sidesteps the collision entirely rather than fighting it
// with QT_NO_KEYWORDS.
#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <libsecret/secret.h>
#endif

#include "CredentialStore.h"

namespace {

#ifdef _WIN32
// A stable, namespaced key so this app's entries are identifiable and
// don't collide with anything else using the same store — Credential
// Manager is a single flat namespace shared by every app on the
// system, unlike libsecret, which already scopes by schema+attributes
// (only used on the Windows side; defining it outside this #ifdef
// left it unused, and so a -Wunused-function warning, on every
// non-Windows build — a real bug found by code review).
QString targetName(const QString &siteId)
{
    return QStringLiteral("ZephyrFTP/site/%1").arg(siteId);
}
#elif defined(__APPLE__)
// kSecAttrService fixed to this app's own name; kSecAttrAccount is the
// per-site key — the direct analog of Windows' namespaced
// targetName() and Linux's site_id schema attribute above. Every
// query starts from this same class+service+account triple; save()
// adds kSecValueData on top for the add/update calls, load()/remove()
// use it as-is.
CFMutableDictionaryRef baseQuery(const QString &siteId)
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, CFSTR("ZephyrFTP"));
    const CFStringRef account = siteId.toCFString();
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFRelease(account);
    return query;
}
#else
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

#elif defined(__APPLE__)

bool save(const QString &siteId, const QString &secret)
{
    CFMutableDictionaryRef query = baseQuery(siteId);
    const QByteArray secretUtf8 = secret.toUtf8();
    const CFDataRef secretData = CFDataCreate(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(secretUtf8.constData()), secretUtf8.size());
    CFDictionarySetValue(query, kSecValueData, secretData);

    OSStatus status = SecItemAdd(query, nullptr);
    if (status == errSecDuplicateItem) {
        // SecItemAdd() alone doesn't overwrite an existing item the
        // way Windows' CredWriteW() and libsecret's
        // secret_password_store_sync() both do unconditionally —
        // SecItemUpdate() is the explicit second step needed to match
        // this function's documented "overwrites any existing secret"
        // contract. The query for the update must NOT itself carry
        // kSecValueData (that belongs in the separate
        // attributes-to-update dictionary), hence a fresh baseQuery()
        // rather than reusing the one above.
        CFMutableDictionaryRef updateQuery = baseQuery(siteId);
        CFMutableDictionaryRef attributesToUpdate = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(attributesToUpdate, kSecValueData, secretData);
        status = SecItemUpdate(updateQuery, attributesToUpdate);
        CFRelease(attributesToUpdate);
        CFRelease(updateQuery);
    }

    CFRelease(secretData);
    CFRelease(query);
    return status == errSecSuccess;
}

bool load(const QString &siteId, QString *secret)
{
    CFMutableDictionaryRef query = baseQuery(siteId);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result)
        return false;

    const CFDataRef data = static_cast<CFDataRef>(result);
    *secret = QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                                 static_cast<int>(CFDataGetLength(data)));
    CFRelease(result);
    return true;
}

bool remove(const QString &siteId)
{
    CFMutableDictionaryRef query = baseQuery(siteId);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    return status == errSecSuccess;
}

bool hasSecret(const QString &siteId)
{
    // Delegates to load() rather than its own SecItemCopyMatching
    // call — matches both CredentialStore.h's documented contract and
    // the Windows/Linux implementations above, which already do this.
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
    // secret can be arbitrary user-typed text — encoded here as UTF-8
    // explicitly, matching load()'s QString::fromUtf8() decode below,
    // rather than via qPrintable() (QString::toLocal8Bit()). Verified
    // directly (a standalone probe, not assumed): under Qt6,
    // toLocal8Bit() unconditionally uses UTF-8 regardless of the
    // process's C-library locale — Qt6 dropped the old locale-dependent
    // "local 8-bit" behavior entirely — so qPrintable() and toUtf8()
    // are provably identical here on any platform/locale this app
    // actually runs under. An earlier round of code review flagged this
    // as fixing a live locale-corruption bug; that specific claim was
    // wrong, corrected here rather than left to stand uninvestigated
    // (see CLAUDE.md's "verify rather than assume"). Kept anyway,
    // demoted to a clarity/robustness improvement: expressing the
    // encoding explicitly doesn't depend on that Qt6 behavior staying
    // true forever, and costs nothing.
    //
    // site_id gets the same treatment for the same non-bug reason. It's
    // normally one of this app's own ASCII UUIDs (SiteManagerDialog
    // always generates it that way) — SavedSite's JSON loader does
    // accept whatever string a hand-edited or migrated sites.json
    // supplies with no format validation, so a non-ASCII id is at least
    // possible, but qPrintable()/toUtf8() would encode it identically
    // either way under Qt6, so there's no actual save/load mismatch
    // risk to guard against — this is consistency/clarity only.
    const QByteArray secretUtf8 = secret.toUtf8();
    const QByteArray siteIdUtf8 = siteId.toUtf8();
    const bool ok = secret_password_store_sync(
        credentialSchema(), SECRET_COLLECTION_DEFAULT,
        qPrintable(QStringLiteral("ZephyrFTP: saved site %1").arg(siteId)),
        secretUtf8.constData(), nullptr, &error,
        "site_id", siteIdUtf8.constData(), nullptr);

    if (error) {
        g_error_free(error);
        return false;
    }
    return ok;
}

bool load(const QString &siteId, QString *secret)
{
    GError *error = nullptr;
    const QByteArray siteIdUtf8 = siteId.toUtf8();
    gchar *result = secret_password_lookup_sync(
        credentialSchema(), nullptr, &error,
        "site_id", siteIdUtf8.constData(), nullptr);

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
    const QByteArray siteIdUtf8 = siteId.toUtf8();
    const bool removed = secret_password_clear_sync(
        credentialSchema(), nullptr, &error,
        "site_id", siteIdUtf8.constData(), nullptr);

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
