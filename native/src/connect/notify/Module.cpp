#include "connect/notify/Module.h"

#include <QRegularExpression>
#include <QUrl>

namespace fw::notify {

QString discordWebhookProblem(const QString& text)
{
    const QString s = text.trimmed();
    if (s.isEmpty())
        return {};
    const QUrl url(s, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0)
        return QStringLiteral("enter the webhook address Discord shows (it starts with https://discord.com/api/webhooks/).");
    static const QRegularExpression host(QStringLiteral("^(?:(?:canary|ptb)\\.)?discord(?:app)?\\.com$"),
                                         QRegularExpression::CaseInsensitiveOption);
    if (!host.match(url.host()).hasMatch() || !url.userInfo().isEmpty() || (url.port() != -1 && url.port() != 443))
        return QStringLiteral("only Discord webhook addresses (discord.com or discordapp.com) are accepted.");
    static const QRegularExpression path(QStringLiteral("^/api/(?:v\\d+/)?webhooks/\\d+/[A-Za-z0-9_\\-]+/?$"));
    if (!path.match(url.path()).hasMatch())
        return QStringLiteral("the address must look like https://discord.com/api/webhooks/<number>/<token>.");
    return {};
}

void registerModule(ModuleContext& ctx)
{
    SettingDef d;
    d.key = keys::kDiscordWebhookUrl;
    d.section = QStringLiteral("General");
    d.group = QStringLiteral("When the queue finishes");
    d.label = QStringLiteral("Discord webhook");
    d.description = QStringLiteral("Posts a short summary (how many jobs finished or failed, and their names) to this "
                                   "Discord channel webhook when the queue finishes. Folders are never included, only "
                                   "file names. Leave empty to turn it off. In Discord: channel settings › Integrations "
                                   "› Webhooks › Copy webhook URL.");
    d.type = SettingType::String;
    d.defaultValue = QString();
    d.advanced = true;
    d.order = 100; // after the module-owned settings of the General section
    d.validator = [](const QVariant& v) { return discordWebhookProblem(v.toString()); };
    ctx.settings.add(d);

    // Legacy hidden key (read at start-up, no control wrote it). Empty means off.
    ctx.settings.addLegacyKey(QStringLiteral("discord_webhook"), keys::kDiscordWebhookUrl,
                              [](const QVariant& v) -> std::optional<QVariant> {
                                  const QString s = v.toString().trimmed();
                                  if (s.isEmpty())
                                      return std::nullopt;
                                  return QVariant(s);
                              });
}

} // namespace fw::notify
