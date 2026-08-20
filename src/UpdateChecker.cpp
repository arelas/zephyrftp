#include "UpdateChecker.h"
#include "AppSettings.h"
#include "backends/ProxyConfig.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVersionNumber>
#include <QUrl>

namespace {
// The full list, NOT /releases/latest — confirmed directly against the
// real API, not assumed: /releases/latest deliberately excludes
// prereleases, and every release this project has ever published is
// flagged prerelease (it matches the project's actual beta status), so
// that endpoint 404s, always, for this repo specifically. The list is
// newest-first; the first non-draft entry is the real latest release.
const char kReleasesUrl[] = "https://api.github.com/repos/arelas/zephyrftp/releases";
}

UpdateChecker::UpdateChecker(AppSettings *settings, QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
{
    const ProxyConfig proxy = settings->resolvedProxyConfig();
    if (proxy.type != ProxyType::None) {
        const QNetworkProxy::ProxyType qtType =
            proxy.type == ProxyType::Socks5 ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy;
        m_manager->setProxy(QNetworkProxy(qtType, proxy.host, quint16(proxy.port),
                                           proxy.username, proxy.password));
    }
}

void UpdateChecker::check()
{
    QNetworkRequest request{QUrl(QString::fromLatin1(kReleasesUrl))};
    // GitHub's REST API rejects a request with no identifiable
    // User-Agent outright (403) — same requirement the marketing site's
    // own latest.php already documents and satisfies server-side.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                       QStringLiteral("zephyrftp-app/") + QStringLiteral(APP_VERSION));
    request.setRawHeader("Accept", "application/vnd.github+json");

    QNetworkReply *reply = m_manager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit failed(reply->errorString());
            return;
        }

        const QJsonArray list = QJsonDocument::fromJson(reply->readAll()).array();
        QJsonObject obj;
        for (const QJsonValue &entry : list) {
            const QJsonObject candidate = entry.toObject();
            if (!candidate.value(QStringLiteral("draft")).toBool(false)) {
                obj = candidate;
                break;
            }
        }
        const QString tag = obj.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty()) {
            emit failed(tr("Unexpected response from GitHub."));
            return;
        }

        QString latest = tag;
        if (latest.startsWith(QLatin1Char('v')) || latest.startsWith(QLatin1Char('V')))
            latest.remove(0, 1);

        const QVersionNumber latestVersion = QVersionNumber::fromString(latest);
        const QVersionNumber currentVersion = QVersionNumber::fromString(QStringLiteral(APP_VERSION));
        const bool updateAvailable = QVersionNumber::compare(latestVersion, currentVersion) > 0;

        // Fallback only — GitHub's API always populates html_url on a
        // real release object, so this realistically never fires. NOT
        // .../releases/latest: see kReleasesUrl's own comment above for
        // why that specific URL is broken for this repo — .../releases
        // (the list) is the one fallback that's actually reliable here.
        const QString releaseUrl = obj.value(QStringLiteral("html_url")).toString();
        emit checked(updateAvailable, latest,
                     releaseUrl.isEmpty()
                         ? QStringLiteral("https://github.com/arelas/zephyrftp/releases")
                         : releaseUrl);
    });
}
