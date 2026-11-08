#include "engines/dlc/Module.h"

#include "engines/dlc/DlcConvert.h"
#include "engines/dlc/DlcSettings.h"
#include "engines/dlc/FpkgCli.h"

namespace fw::dlc {

void registerModule(ModuleContext& ctx)
{
    registerDlcSettings(ctx.settings);
    ctx.dependencies.add(QStringLiteral("dlc.fpkgcli"), QStringLiteral("DLC engine (fpkg-cli · LibProsperoPkg)"),
                         QStringLiteral("Builds PS5 DLC packages on the DLC page."), probeFpkgCli);
    ctx.jobs.add(kJobConvert, QStringLiteral("Convert DLC"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        auto r = DlcConvertRequest::fromJson(p);
        if (!r.ok())
            return r.takeError();
        return std::unique_ptr<Job>(new DlcConvertJob(std::move(r).value()));
    });
}

} // namespace fw::dlc
