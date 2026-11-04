#pragma once

// Notifications that leave the PC when the queue finishes: the legacy app's
// hidden `discord_webhook` (POST of an embed after a run, `_discord_notify`),
// now a real setting. See DiscordWebhook.h.

#include "jobs/Module.h"

#include <QString>

namespace fw::notify {

// Registers general.discordWebhookUrl (section General, advanced) and maps the
// legacy discord_webhook key.
void registerModule(ModuleContext& ctx);

namespace keys {
// Empty = off. Read by DiscordNotifier when the queue finishes.
inline const QString kDiscordWebhookUrl = QStringLiteral("general.discordWebhookUrl");
} // namespace keys

// Empty when `url` is empty (off) or a Discord webhook address:
// https://discord.com/api/webhooks/<id>/<token> (also discordapp.com and the
// canary./ptb. hosts, an /api/v<n>/ path and a query). Otherwise the problem,
// for the Settings page.
QString discordWebhookProblem(const QString& url);

} // namespace fw::notify
