#include "engines/general/Module.h"

#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <algorithm>

namespace fw::general {

qint64 ioBufferBytes(const SettingsValues& settings)
{
    return std::clamp<qint64>(settings.integer(keys::kIoBufferMiB), 1, 64) * 1024 * 1024;
}

qint64 freeSpaceMarginBytes(const SettingsValues& settings)
{
    return std::max<qint64>(0, settings.integer(keys::kFreeSpaceMarginMiB)) * 1024 * 1024;
}

QString tempParentDir(const SettingsValues& settings)
{
    const QString custom = settings.string(keys::kTempFolder).trimmed();
    if (!custom.isEmpty() && QFileInfo(custom).isDir())
        return custom;
    return {};
}

int workerThreads(const SettingsValues& settings)
{
    const int cores = std::max(1, QThread::idealThreadCount());
    const qint64 v = settings.integer(keys::kWorkerThreads);
    if (v <= 0)
        return cores;
    return int(std::clamp<qint64>(v, 1, cores));
}

static QString dirMustExistIfSet(const QVariant& v)
{
    const QString s = v.toString().trimmed();
    if (s.isEmpty())
        return {};
    if (!QFileInfo(s).isDir())
        return QStringLiteral("the folder does not exist.");
    return {};
}

void registerModule(ModuleContext& ctx)
{
    auto& s = ctx.settings;
    int order = 0;

    // ---- General --------------------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kConfirmOnClose;
        d.section = QStringLiteral("General");
        d.group = QStringLiteral("Behaviour");
        d.label = QStringLiteral("Ask before closing while jobs run");
        d.description = QStringLiteral("Closing stops running jobs and their tools. Their partial output is removed and they can be retried after restart.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kNotifyOnFinish;
        d.section = QStringLiteral("General");
        d.group = QStringLiteral("Behaviour");
        d.label = QStringLiteral("Notify when the queue finishes");
        d.description = QStringLiteral("Shows a Windows notification when the last job finishes while the window is not in front.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kRestoreQueue;
        d.section = QStringLiteral("General");
        d.group = QStringLiteral("Behaviour");
        d.label = QStringLiteral("Restore the queue on start");
        d.description = QStringLiteral("Jobs that were waiting when the app closed come back, held until you start the queue. Jobs that were running come back as Interrupted.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kLastSourceDir;
        d.section = QStringLiteral("General");
        d.group = QStringLiteral("Behaviour");
        d.label = QStringLiteral("Last source folder");
        d.description = QStringLiteral("Where folder pickers open. Updated automatically.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Performance ------------------------------------------------------------
    order = 0;
    {
        SettingDef d;
        d.key = keys::kMaxConcurrentJobs;
        d.section = QStringLiteral("Performance");
        d.group = QStringLiteral("Queue");
        d.label = QStringLiteral("Jobs at the same time");
        d.description = QStringLiteral("How many queue jobs run in parallel. Image builds are limited by the disk, so 1 is usually fastest unless sources and outputs are on different drives.");
        d.type = SettingType::Int;
        d.defaultValue = 1;
        d.minimum = 1;
        d.maximum = 8;
        d.unit = QStringLiteral("jobs");
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kWorkerThreads;
        d.section = QStringLiteral("Performance");
        d.group = QStringLiteral("Processing");
        d.label = QStringLiteral("CPU threads per job");
        d.description = QStringLiteral("Threads used for compression, hashing and verification. 0 uses every logical core. Lower it to keep the computer responsive while building.");
        d.type = SettingType::Int;
        d.defaultValue = 0;
        d.minimum = 0;
        d.maximum = 256;
        d.unit = QStringLiteral("threads");
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kIoBufferMiB;
        d.section = QStringLiteral("Performance");
        d.group = QStringLiteral("Processing");
        d.label = QStringLiteral("I/O buffer size");
        d.description = QStringLiteral("Size of each read/write buffer. Larger buffers help on hard disks and network drives; 4 MiB suits most SSDs.");
        d.type = SettingType::Int;
        d.defaultValue = 4;
        d.minimum = 1;
        d.maximum = 64;
        d.unit = QStringLiteral("MiB");
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kKeepAwake;
        d.section = QStringLiteral("Performance");
        d.group = QStringLiteral("Processing");
        d.label = QStringLiteral("Keep the computer awake while building");
        d.description = QStringLiteral("Prevents sleep (not the screen saver) while any job runs.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Storage ----------------------------------------------------------------
    order = 0;
    {
        SettingDef d;
        d.key = keys::kDefaultOutputDir;
        d.section = QStringLiteral("Storage");
        d.group = QStringLiteral("Folders");
        d.label = QStringLiteral("Default output folder");
        d.description = QStringLiteral("Where new images and packages go when a job does not name a folder. Empty: next to the source folder.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.validator = dirMustExistIfSet;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kTempFolder;
        d.section = QStringLiteral("Storage");
        d.group = QStringLiteral("Folders");
        d.label = QStringLiteral("Temporary folder");
        d.description = QStringLiteral("Where jobs keep intermediate files. Empty: the app's own folder in your local app data. Choose a fast drive with free space.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.validator = dirMustExistIfSet;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kFreeSpaceMarginMiB;
        d.section = QStringLiteral("Storage");
        d.group = QStringLiteral("Safety");
        d.label = QStringLiteral("Free-space reserve");
        d.description = QStringLiteral("Jobs refuse to start unless the output drive keeps at least this much free after the build.");
        d.type = SettingType::Int;
        d.defaultValue = 512;
        d.minimum = 0;
        d.maximum = 65536;
        d.unit = QStringLiteral("MiB");
        d.order = order++;
        s.add(d);
    }

    // ---- Verification -----------------------------------------------------------
    order = 0;
    {
        SettingDef d;
        d.key = keys::kVerifyAfterBuild;
        d.section = QStringLiteral("Verification");
        d.group = QStringLiteral("After building");
        d.label = QStringLiteral("Verify every output");
        d.description = QStringLiteral("Reads each finished image or package back and compares it with the source. A job is only reported as done after verification passes.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kVerifyMode;
        d.section = QStringLiteral("Verification");
        d.group = QStringLiteral("After building");
        d.label = QStringLiteral("Verification depth");
        d.description = QStringLiteral("Full compares every byte. Structure checks the file list, sizes and the image's own checksums, and is much faster on large games.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("full");
        d.choices = {{QStringLiteral("full"), QStringLiteral("Full (every byte)"), {}},
                     {QStringLiteral("structure"), QStringLiteral("Structure (names, sizes, checksums)"), {}}};
        d.enabledWhenKey = keys::kVerifyAfterBuild;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Logging ------------------------------------------------------------------
    order = 0;
    {
        SettingDef d;
        d.key = keys::kLogLevel;
        d.section = QStringLiteral("Logging");
        d.group = QStringLiteral("Log files");
        d.label = QStringLiteral("Detail level");
        d.description = QStringLiteral("Debug adds tool command lines and timings; use it when reporting a problem.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("info");
        d.choices = {{QStringLiteral("debug"), QStringLiteral("Debug"), {}},
                     {QStringLiteral("info"), QStringLiteral("Info"), {}},
                     {QStringLiteral("warning"), QStringLiteral("Warnings and errors"), {}}};
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kLogKeepFiles;
        d.section = QStringLiteral("Logging");
        d.group = QStringLiteral("Log files");
        d.label = QStringLiteral("Old log files to keep");
        d.description = QStringLiteral("Logs rotate when they reach the size below.");
        d.type = SettingType::Int;
        d.defaultValue = 5;
        d.minimum = 1;
        d.maximum = 50;
        d.unit = QStringLiteral("files");
        d.restartRequired = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kLogMaxMiB;
        d.section = QStringLiteral("Logging");
        d.group = QStringLiteral("Log files");
        d.label = QStringLiteral("Maximum log file size");
        d.type = SettingType::Int;
        d.defaultValue = 8;
        d.minimum = 1;
        d.maximum = 512;
        d.unit = QStringLiteral("MiB");
        d.restartRequired = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Advanced -----------------------------------------------------------------
    order = 0;
    {
        SettingDef d;
        d.key = keys::kKeepJobTemp;
        d.section = QStringLiteral("Advanced");
        d.group = QStringLiteral("Troubleshooting");
        d.label = QStringLiteral("Keep job temporary folders");
        d.description = QStringLiteral("Leaves each job's temporary folder in place for inspection. Folders older than a day are removed at the next start.");
        d.type = SettingType::Bool;
        d.defaultValue = false;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    // Legacy (Python app) settings that map onto these.
    s.addLegacyKey(QStringLiteral("output_dir"), keys::kDefaultOutputDir);
}

} // namespace fw::general
