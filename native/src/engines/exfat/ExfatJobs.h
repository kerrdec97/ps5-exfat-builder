#pragma once

// Queue jobs of the exFAT module:
//   image.exfat.build   {source, output, label?, excludes?}   Scanning -> Writing image -> Verifying
//   image.exfat.verify  {image, source, excludes?}            Reading image -> Comparing
//   image.exfat.extract {image, output}                       Reading image -> Extracting -> Verifying
//   image.exfat.edit    {image, operations[]}                 Planning -> Writing -> Verifying

#include "core/Settings.h"
#include "engines/exfat/ExfatEdit.h"
#include "engines/exfat/ExfatWriter.h"
#include "engines/exfat/SourceScan.h"
#include "engines/pfs/AmprIndex.h"
#include "jobs/Job.h"

namespace fw::exfat {

void registerJobKinds(JobFactory& jobs);

// Every exFAT option of a settings snapshot, resolved.
struct ExfatSettings {
    WriterOptions writer;
    ScanRules rules;
    pfs::AmprMode ampr = pfs::AmprMode::Auto; // exfat.amprIndex
    bool requireEboot = true;
    bool overwrite = true;
    QString labelPolicy;     // fixed | titleId | none
    QString label;
    bool deleteSource = false;
    bool verify = true;      // verification.verifyAfterBuild
    bool fullVerify = true;  // verification.mode == full
    int threads = 1;         // performance.workerThreads
    qint64 marginBytes = 0;  // storage.freeSpaceMarginMiB
    bool extractParallel = false;
    int extractThreads = 1;
    EditStrategy editStrategy = EditStrategy::Auto;
};
ExfatSettings readSettings(const SettingsValues& s);

// The title ID of a game folder: sce_sys/param.json "titleId", else a
// CUSA/PPSA/PPLH id in the folder or output name. Empty when none.
QString detectTitleId(const QString& sourceDir, const QString& outputPath);

// `output` may be a file or an existing folder (the image is then named
// after the source folder).
QString resolveBuildOutput(const QString& source, const QString& output);

} // namespace fw::exfat
