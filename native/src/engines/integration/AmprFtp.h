#pragma once

// AMPR index over FTP (legacy Advanced › AMPR "Generate From Connected PS5",
// ui/tab_advanced.py _generate_from_ps5 + ui/build_ampr_index.py
// build_index_ftp; inventory §3.7).
//
//   ampr.index.ftp {remoteFolder, host?, port?}
//       1. Checks that the console folder is a game's app0 folder (eboot.bin or
//          sce_sys directly inside; legacy _probe_app0) before any recursive
//          walk, so a parent such as /data/etaHEN/games is never indexed whole.
//       2. Walks it over one FTP connection (RemoteTree.h: names only through
//          remoteChild(), MLSD times, MDTM where the listing has none).
//       3. Builds ampr_emu.index with the native writer and the tool's row
//          rules (AmprIndex.h) — the same bytes as from the same local folder.
//       4. Uploads it the upstream way: STOR ampr_emu.index.tmp, move an
//          existing ampr_emu.index to ampr_emu.index.bak (an older .bak is
//          replaced), rename the new file in, and put the old one back if that
//          fails. The legacy app deleted the old index first.
//       5. Reads it back and compares the bytes.
// Cancel: a half-written ampr_emu.index.tmp is removed over a fresh connection.

#include "connect/ps5/FtpClient.h"
#include "connect/ps5/RemoteTree.h"
#include "engines/pfs/AmprIndex.h"
#include "jobs/Module.h"

namespace fw::integration {

inline const QString kAmprFtpJob = QStringLiteral("ampr.index.ftp");

// Why `entries` (the folder's own listing) is not a game's app0 folder; empty
// when it has eboot.bin or sce_sys.
QString app0Problem(const std::vector<ps5::FtpEntry>& entries, const QString& folder);

struct AmprConsoleScan {
    ps5::RemoteTree tree;
    std::vector<pfs::AmprFile> files;
    int approximateTimes = 0; // files whose time is not to the second (no MLSD/MDTM)
};
// Walks the folder (exact sizes and times) and converts it for the index.
Result<AmprConsoleScan> scanConsoleApp0(ps5::FtpClient& client, const QString& remoteFolder,
                                        const CancellationToken& token = {}, const ps5::RemoteWalkProgress& progress = {});

struct AmprUpload {
    QString remotePath;  // <folder>/ampr_emu.index
    QString backupPath;  // <folder>/ampr_emu.index.bak, empty when there was no index
};
// Step 4 above.
Result<AmprUpload> uploadAmprIndex(ps5::FtpClient& client, const QString& remoteFolder, const QByteArray& index,
                                   const CancellationToken& token = {});

void registerAmprFtpJob(ModuleContext& ctx);

} // namespace fw::integration
