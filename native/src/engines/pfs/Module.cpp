// PFS / FFPFSC engine registration: settings, legacy settings keys and jobs.
// The engine is a native port of Lazy_MkPFS (Nazky), itself derived from
// PSBrew/MkPFS; GPL-3.0-or-later.

#include "engines/pfs/Module.h"

#include "engines/general/Module.h"
#include "engines/pfs/AmprJobs.h"
#include "engines/pfs/PfsJobs.h"

#include <QThread>

#include <algorithm>
#include <cmath>

namespace fw::pfs {

namespace {

const QString kSection = QStringLiteral("PFS");

std::optional<QVariant> clampInt(const QVariant& v, int lo, int hi)
{
    bool ok = false;
    const int i = v.toInt(&ok);
    if (!ok)
        return std::nullopt;
    return QVariant(qint64(std::clamp(i, lo, hi)));
}

} // namespace

int threadsFrom(const SettingsValues& s)
{
    const qint64 own = s.integer(keys::kThreads);
    // More threads than cores only cost memory (three 2 MiB tasks in flight
    // per thread), so the module's own number is capped like the app-wide one.
    if (own > 0)
        return int(std::clamp<qint64>(own, 1, std::max(1, QThread::idealThreadCount())));
    return general::workerThreads(s);
}

BuildOptions buildOptionsFrom(const SettingsValues& s, qint64 time)
{
    BuildOptions o;
    o.layout.blockSize = s.string(keys::kBlockSize).toInt();
    if (o.layout.blockSize <= 0)
        o.layout.blockSize = 0x10000;
    o.layout.caseInsensitive = s.boolean(keys::kCaseInsensitive);
    o.layout.version = s.string(keys::kImageVersion) == u"ps4" ? kVersionPs4 : kVersionPs5;
    o.layout.time = time;
    o.level = int(std::clamp<qint64>(s.integer(keys::kCompressionLevel), 1, 9));
    o.threads = threadsFrom(s);
    o.container.compress = true;
    o.container.minCompressMiB = s.real(keys::kContainerMinCompressMiB);
    o.container.blockGain = int(s.integer(keys::kContainerBlockGain));
    o.container.minFileGain = 0;
    o.container.skipExecutables = false;
    o.files.compress = s.boolean(keys::kCompressFiles);
    o.files.minCompressMiB = s.real(keys::kFileMinCompressMiB);
    o.files.blockGain = int(s.integer(keys::kFileBlockGain));
    o.files.minFileGain = int(s.integer(keys::kFileMinGain));
    o.files.skipExecutables = s.boolean(keys::kSkipExecutables);
    return o;
}

PlanOptions planOptionsFrom(const SettingsValues& s, qint64 time)
{
    PlanOptions p;
    p.excludes = s.stringList(keys::kExcludes);
    p.keepEmptyFolders = s.boolean(keys::kKeepEmptyFolders);
    const QString ampr = s.string(keys::kAmprIndex);
    p.ampr = ampr == u"off" ? AmprMode::Off : ampr == u"refresh" ? AmprMode::Refresh : AmprMode::Missing;
    p.time = time;
    return p;
}

std::optional<VerifyDepth> verifyDepthFrom(const SettingsValues& s)
{
    // One app-wide switch (Settings › Verification) for every image format.
    if (!s.boolean(general::keys::kVerifyAfterBuild))
        return std::nullopt;
    return s.string(general::keys::kVerifyMode) == u"structure" ? VerifyDepth::Structure : VerifyDepth::Full;
}

qint64 freeSpaceMarginFrom(const SettingsValues& s)
{
    // App-wide reserve declared by the general module (ARCHITECTURE.md, Job rules).
    return general::freeSpaceMarginBytes(s);
}

void registerModule(ModuleContext& ctx)
{
    auto& s = ctx.settings;
    int order = 0;

    // ---- Compression ---------------------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kCompressionLevel;
        d.section = kSection;
        d.group = QStringLiteral("Compression");
        d.label = QStringLiteral("Compression level");
        d.description = QStringLiteral(
            "zlib level for compressed PFS (.ffpfsc and per-file compression). 9 gives the smallest images and "
            "is the slowest; 6 is balanced; 1 is fastest with larger output. Every level mounts the same way.");
        d.type = SettingType::Int;
        d.defaultValue = 9;
        d.minimum = 1;
        d.maximum = 9;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kThreads;
        d.section = kSection;
        d.group = QStringLiteral("Compression");
        d.label = QStringLiteral("Compression threads");
        d.description = QStringLiteral(
            "Threads that compress PFS blocks. 0 uses \"CPU threads per job\" from Performance. The image is "
            "identical whatever the number of threads.");
        d.type = SettingType::Int;
        d.defaultValue = 0;
        d.minimum = 0;
        d.maximum = 256;
        d.unit = QStringLiteral("threads");
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kContainerMinCompressMiB;
        d.section = kSection;
        d.group = QStringLiteral("Compression");
        d.label = QStringLiteral("Smallest image to compress");
        d.description = QStringLiteral(
            "Sources smaller than this are stored uncompressed inside the .ffpfsc. Game images are far larger, so "
            "this rarely matters.");
        d.type = SettingType::Double;
        d.defaultValue = 1.0;
        d.minimum = 0;
        d.maximum = 4096;
        d.unit = QStringLiteral("MiB");
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kContainerBlockGain;
        d.section = kSection;
        d.group = QStringLiteral("Compression");
        d.label = QStringLiteral("Minimum block saving");
        d.description = QStringLiteral(
            "A 64 KiB block of a .ffpfsc is stored compressed only if that saves at least this share of it; "
            "otherwise it is stored as is, which is faster to read.");
        d.type = SettingType::Int;
        d.defaultValue = 1;
        d.minimum = 0;
        d.maximum = 100;
        d.unit = QStringLiteral("%");
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Folder images (.ffpfs) ------------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kCompressFiles;
        d.section = kSection;
        d.group = QStringLiteral("Folder images (.ffpfs)");
        d.label = QStringLiteral("Compress files inside .ffpfs images");
        d.description = QStringLiteral(
            "Compresses each large file of an .ffpfs on its own. Compressed files grow to a whole number of "
            "blocks with zero bytes, which some games do not accept; leave this off unless you know the game "
            "works with it. .ffpfsc images are always compressed.");
        d.type = SettingType::Bool;
        d.defaultValue = false;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kFileMinCompressMiB;
        d.section = kSection;
        d.group = QStringLiteral("Folder images (.ffpfs)");
        d.label = QStringLiteral("Smallest file to compress");
        d.description = QStringLiteral("Files smaller than this are stored uncompressed.");
        d.type = SettingType::Double;
        d.defaultValue = 5.0;
        d.minimum = 0;
        d.maximum = 4096;
        d.unit = QStringLiteral("MiB");
        d.enabledWhenKey = keys::kCompressFiles;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kFileBlockGain;
        d.section = kSection;
        d.group = QStringLiteral("Folder images (.ffpfs)");
        d.label = QStringLiteral("Minimum block saving");
        d.description = QStringLiteral("A 64 KiB block of a file is stored compressed only if that saves at least "
                                       "this share of it.");
        d.type = SettingType::Int;
        d.defaultValue = 5;
        d.minimum = 0;
        d.maximum = 100;
        d.unit = QStringLiteral("%");
        d.enabledWhenKey = keys::kCompressFiles;
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kFileMinGain;
        d.section = kSection;
        d.group = QStringLiteral("Folder images (.ffpfs)");
        d.label = QStringLiteral("Minimum file saving");
        d.description = QStringLiteral("A file is stored compressed only if the whole file shrinks by at least this "
                                       "much; 0 keeps every file that shrinks at all.");
        d.type = SettingType::Int;
        d.defaultValue = 0;
        d.minimum = 0;
        d.maximum = 100;
        d.unit = QStringLiteral("%");
        d.enabledWhenKey = keys::kCompressFiles;
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kSkipExecutables;
        d.section = kSection;
        d.group = QStringLiteral("Folder images (.ffpfs)");
        d.label = QStringLiteral("Never compress executables and system files");
        d.description = QStringLiteral(
            "Keeps eboot*.bin, .prx/.sprx modules, sce_sys and sce_module, .json, .txt and .png files "
            "uncompressed so the console can load them directly.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.enabledWhenKey = keys::kCompressFiles;
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Image format ------------------------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kBlockSize;
        d.section = kSection;
        d.group = QStringLiteral("Image format");
        d.label = QStringLiteral("Block size");
        d.description = QStringLiteral(
            "PFS filesystem block size. 64 KiB is what the PS5 uses; smaller blocks waste less space on many small "
            "files. Compression always works on 64 KiB units.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("65536");
        d.choices = {{QStringLiteral("4096"), QStringLiteral("4 KiB"), {}},
                     {QStringLiteral("8192"), QStringLiteral("8 KiB"), {}},
                     {QStringLiteral("16384"), QStringLiteral("16 KiB"), {}},
                     {QStringLiteral("32768"), QStringLiteral("32 KiB"), {}},
                     {QStringLiteral("65536"), QStringLiteral("64 KiB (PS5 default)"), {}}};
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kCaseInsensitive;
        d.section = kSection;
        d.group = QStringLiteral("Image format");
        d.label = QStringLiteral("Case-insensitive file names");
        d.description = QStringLiteral(
            "Marks the image as case-insensitive, so the console finds DATA.PAK when the game asks for data.pak. "
            "Game images need this.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kImageVersion;
        d.section = kSection;
        d.group = QStringLiteral("Image format");
        d.label = QStringLiteral("PFS version");
        d.description = QStringLiteral("Version written in the image header. Use PS5 for PS5 games.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("ps5");
        d.choices = {{QStringLiteral("ps5"), QStringLiteral("PS5"), {}}, {QStringLiteral("ps4"), QStringLiteral("PS4"), {}}};
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Source folder ------------------------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kExcludes;
        d.section = kSection;
        d.group = QStringLiteral("Source folder");
        d.label = QStringLiteral("Leave out");
        d.description = QStringLiteral(
            "Files and folders not packed. A pattern without a slash matches a name anywhere (*.ffpkg); one with "
            "a slash matches a path from the dump's top folder (sce_sys/backup). Verification skips them too. By "
            "default leftover images from earlier builds are left out.");
        d.type = SettingType::StringList;
        d.defaultValue = QStringList{QStringLiteral("*.exfat"), QStringLiteral("*.ffpkg"), QStringLiteral("*.ffpfs"),
                                     QStringLiteral("*.ffpfsc")};
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kKeepEmptyFolders;
        d.section = kSection;
        d.group = QStringLiteral("Source folder");
        d.label = QStringLiteral("Keep empty folders");
        d.description = QStringLiteral(
            "PFS only stores folders that contain files. This adds an empty .pfskeep file to each empty folder so "
            "it survives; some games (often Unreal Engine) need them. The dump itself is not changed.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kAmprIndex;
        d.section = kSection;
        d.group = QStringLiteral("Source folder");
        d.label = QStringLiteral("AMPR file index");
        d.description = QStringLiteral(
            "When the dump has fakelib/libSceAmpr.sprx, its file resolver needs ampr_emu.index at the top folder. "
            "The index is added to the image only; the dump is not changed.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("missing");
        d.choices = {{QStringLiteral("missing"), QStringLiteral("Add when the dump has none"),
                      QStringLiteral("What Lazy_MkPFS does.")},
                     {QStringLiteral("refresh"), QStringLiteral("Always rebuild it"),
                      QStringLiteral("Replaces the dump's own index in the image (legacy Build page behaviour).")},
                     {QStringLiteral("off"), QStringLiteral("Never add one"), {}}};
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    // ---- After converting ----------------------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kDeleteSourceAfterConvert;
        d.section = kSection;
        d.group = QStringLiteral("After converting");
        d.label = QStringLiteral("Delete the source image after converting");
        d.description = QStringLiteral(
            "After an .exfat or .ffpkg is converted to .ffpfsc, deletes it — only when full verification passed. "
            "Folders are never deleted.");
        d.type = SettingType::Bool;
        d.defaultValue = false;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    // ---- legacy Python settings -------------------------------------------------------------
    s.addLegacyKey(QStringLiteral("pfs_compression_level"), keys::kCompressionLevel,
                   [](const QVariant& v) { return clampInt(v, 1, 9); });
    s.addLegacyKey(QStringLiteral("pfs_lazy_cpu_count"), keys::kThreads,
                   [](const QVariant& v) { return clampInt(v, 0, 256); });
    s.addLegacyKey(QStringLiteral("pfs_delete_source_after_convert"), keys::kDeleteSourceAfterConvert);

    registerJobs(ctx);
    registerAmprJobs(ctx);
}

} // namespace fw::pfs
