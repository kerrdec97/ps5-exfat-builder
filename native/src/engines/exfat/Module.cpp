#include "engines/exfat/Module.h"

#include "engines/exfat/ExfatFormat.h"
#include "engines/exfat/ExfatJobs.h"
#include "engines/general/Module.h"

#include <algorithm>
#include <optional>

namespace fw::exfat {

QStringList defaultExcludePatterns()
{
    return {QStringLiteral("*.exfat"),      QStringLiteral("*.ffpkg"),     QStringLiteral("*.ffpfs"),
            QStringLiteral("*.ffpfsc"),     QStringLiteral("Thumbs.db"),   QStringLiteral("desktop.ini"),
            QStringLiteral(".DS_Store"),    QStringLiteral("System Volume Information/"),
            QStringLiteral("$RECYCLE.BIN/")};
}

namespace {

std::optional<QVariant> legacyInt(const QVariant& v, int lo, int hi)
{
    bool ok = false;
    const int n = v.toString().trimmed().toInt(&ok);
    if (!ok)
        return std::nullopt;
    return QVariant(std::clamp(n, lo, hi));
}

} // namespace

void registerModule(ModuleContext& ctx)
{
    auto& s = ctx.settings;
    int order = 100; // after the general Build settings

    const QString build = QStringLiteral("Build");
    const QString gImage = QStringLiteral("exFAT images");
    const QString gContents = QStringLiteral("exFAT contents");
    {
        SettingDef d;
        d.key = keys::kClusterSize;
        d.section = build;
        d.group = gImage;
        d.label = QStringLiteral("Cluster size");
        d.description = QStringLiteral(
            "Allocation unit of the image. Automatic uses 64 KB when the average file is at least 1 MB and 32 KB "
            "otherwise, like earlier versions of this app. PS5 loaders are tested with 32 KB and 64 KB.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("auto");
        d.choices = {{QStringLiteral("auto"), QStringLiteral("Automatic (32 or 64 KB)"), {}},
                     {QStringLiteral("16384"), QStringLiteral("16 KB"), {}},
                     {QStringLiteral("32768"), QStringLiteral("32 KB"), {}},
                     {QStringLiteral("65536"), QStringLiteral("64 KB"), {}},
                     {QStringLiteral("131072"), QStringLiteral("128 KB"), {}},
                     {QStringLiteral("262144"), QStringLiteral("256 KB"), {}},
                     {QStringLiteral("524288"), QStringLiteral("512 KB"), {}},
                     {QStringLiteral("1048576"), QStringLiteral("1 MB"), {}}};
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kSectorSize;
        d.section = build;
        d.group = gImage;
        d.label = QStringLiteral("Sector size");
        d.description = QStringLiteral(
            "Bytes per sector recorded in the image. PS5 loaders expect 512; 4096 only works if the loader is "
            "configured for it (for example lvd_exfat_sector_size in ShadowMount's config.ini).");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("512");
        d.choices = {{QStringLiteral("512"), QStringLiteral("512 bytes"), {}},
                     {QStringLiteral("4096"), QStringLiteral("4096 bytes"), {}}};
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kImageSizeGiB;
        d.section = build;
        d.group = gImage;
        d.label = QStringLiteral("Image size");
        d.description = QStringLiteral(
            "0 sizes each image automatically. Any other value makes every image exactly this size (in GB of "
            "1024 MB); a build fails if the game does not fit.");
        d.type = SettingType::Double;
        d.defaultValue = 0.0;
        d.minimum = 0;
        d.maximum = 131072;
        d.unit = QStringLiteral("GB");
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kSpareSpace;
        d.section = build;
        d.group = gImage;
        d.label = QStringLiteral("Free space in automatic images");
        d.description = QStringLiteral(
            "How much room an automatically sized image keeps free, for files you add later. Standard keeps 1 % "
            "of the image, at least 1 GB and at most 8 GB (as before). Fixed keeps exactly the amount below, "
            "for the smallest images.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("legacy");
        d.choices = {{QStringLiteral("legacy"), QStringLiteral("Standard (1 %, 1 to 8 GB)"), {}},
                     {QStringLiteral("fixed"), QStringLiteral("Fixed amount"), {}}};
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kSpareSpaceMiB;
        d.section = build;
        d.group = gImage;
        d.label = QStringLiteral("Fixed free space");
        d.description = QStringLiteral("Free space kept in automatically sized images when the option above is Fixed.");
        d.type = SettingType::Int;
        d.defaultValue = 256;
        d.minimum = 0;
        d.maximum = 1048576;
        d.unit = QStringLiteral("MiB");
        d.enabledWhenKey = keys::kSpareSpace;
        d.enabledWhenValue = QStringLiteral("fixed");
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kLabelPolicy;
        d.section = build;
        d.group = gImage;
        d.label = QStringLiteral("Volume label");
        d.description = QStringLiteral(
            "Name shown for the image when it is mounted. Fixed uses the text below (OSFIMG, as earlier versions). "
            "Title ID uses the game's ID (PPSA01234) and falls back to the text below.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("fixed");
        d.choices = {{QStringLiteral("fixed"), QStringLiteral("Fixed text"), {}},
                     {QStringLiteral("titleId"), QStringLiteral("Title ID"), {}},
                     {QStringLiteral("none"), QStringLiteral("No label"), {}}};
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kLabel;
        d.section = build;
        d.group = gImage;
        d.label = QStringLiteral("Label text");
        d.description = QStringLiteral("Up to 11 characters.");
        d.type = SettingType::String;
        d.defaultValue = QStringLiteral("OSFIMG");
        d.validator = [](const QVariant& v) { return checkVolumeLabel(v.toString()); };
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kRequireEboot;
        d.section = build;
        d.group = gContents;
        d.label = QStringLiteral("Require eboot.bin");
        d.description = QStringLiteral(
            "Refuse to build an image from a folder without eboot.bin at its top, which is almost always the "
            "wrong folder (one level too high or too low).");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kExcludePatterns;
        d.section = build;
        d.group = gContents;
        d.label = QStringLiteral("Leave out");
        d.description = QStringLiteral(
            "Files never copied into an image, at any depth. Use * and ? as wildcards, a / to match a path "
            "(sce_sys/*.bak), and a trailing / for folders (decrypted/). Verification ignores them too.");
        d.type = SettingType::StringList;
        d.defaultValue = defaultExcludePatterns();
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kExcludeHidden;
        d.section = build;
        d.group = gContents;
        d.label = QStringLiteral("Leave out hidden files");
        d.description = QStringLiteral("Skip files marked hidden in the source folder. Folders are always copied.");
        d.type = SettingType::Bool;
        d.defaultValue = false;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kOverwriteExisting;
        d.section = build;
        d.group = gImage;
        d.label = QStringLiteral("Replace an existing image");
        d.description = QStringLiteral(
            "When an image with the same name exists, replace it once the new one is built and verified. The old "
            "image is kept until then. Off: the build stops instead.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kAllocation;
        d.section = build;
        d.group = gImage;
        d.label = QStringLiteral("File allocation");
        d.description = QStringLiteral(
            "Contiguous marks every file as one unbroken run of clusters (NoFatChain), as Windows does for files "
            "copied onto a fresh volume. FAT chains also records each run in the FAT, for old readers that ignore "
            "that flag.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("contiguous");
        d.choices = {{QStringLiteral("contiguous"), QStringLiteral("Contiguous"), {}},
                     {QStringLiteral("fatChain"), QStringLiteral("FAT chains"), {}}};
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kOpenRetries;
        d.section = build;
        d.group = gContents;
        d.label = QStringLiteral("Retries for locked files");
        d.description = QStringLiteral(
            "How many more times a source file that cannot be opened (held by antivirus or Windows Search) is "
            "tried before the build stops.");
        d.type = SettingType::Int;
        d.defaultValue = 3;
        d.minimum = 0;
        d.maximum = 20;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kRetryWaitSeconds;
        d.section = build;
        d.group = gContents;
        d.label = QStringLiteral("Wait between retries");
        d.type = SettingType::Int;
        d.defaultValue = 3;
        d.minimum = 0;
        d.maximum = 60;
        d.unit = QStringLiteral("s");
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kEditStrategy;
        d.section = build;
        d.group = QStringLiteral("Editing exFAT images");
        d.label = QStringLiteral("How edits are written");
        d.description = QStringLiteral(
            "Automatic deletes files and replaces files that fit their old space directly in the image, with an "
            "undo copy of every changed byte. Other edits write a new image and swap it in after verification. "
            "Always rebuild writes a new image for every edit.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("auto");
        d.choices = {{QStringLiteral("auto"), QStringLiteral("Automatic"), {}},
                     {QStringLiteral("rebuild"), QStringLiteral("Always rebuild"), {}}};
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Storage --------------------------------------------------------------
    order = 100;
    {
        SettingDef d;
        d.key = keys::kExtractParallel;
        d.section = QStringLiteral("Storage");
        d.group = QStringLiteral("Extracting exFAT images");
        d.label = QStringLiteral("Write several files at once");
        d.description = QStringLiteral(
            "Faster for games with many small files on SSDs. Leave off for hard disks, where reading the image "
            "in order is faster.");
        d.type = SettingType::Bool;
        d.defaultValue = false;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kExtractThreads;
        d.section = QStringLiteral("Storage");
        d.group = QStringLiteral("Extracting exFAT images");
        d.label = QStringLiteral("Files at once");
        d.type = SettingType::Int;
        d.defaultValue = 32;
        d.minimum = 1;
        d.maximum = 128;
        d.unit = QStringLiteral("files");
        d.enabledWhenKey = keys::kExtractParallel;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Verification -------------------------------------------------------------
    order = 100;
    {
        SettingDef d;
        d.key = keys::kDeleteSourceAfterVerify;
        d.section = QStringLiteral("Verification");
        d.group = QStringLiteral("After a verified build");
        d.label = QStringLiteral("Delete the source folder after an exFAT build");
        d.description = QStringLiteral(
            "Deletes the game folder only after a full byte-for-byte verification passed and nothing in it was "
            "left out of the image. Structure-only or skipped verification never deletes anything.");
        d.type = SettingType::Bool;
        d.defaultValue = false;
        d.advanced = true;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    // ---- legacy (Python app) settings ---------------------------------------------------
    s.addLegacyKey(QStringLiteral("adv_cluster"), keys::kClusterSize, [](const QVariant& v) -> std::optional<QVariant> {
        const QString t = v.toString().trimmed();
        if (t.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0 || t.isEmpty())
            return QVariant(QStringLiteral("auto"));
        static const QStringList known = {QStringLiteral("16384"),  QStringLiteral("32768"),  QStringLiteral("65536"),
                                          QStringLiteral("131072"), QStringLiteral("262144"), QStringLiteral("524288"),
                                          QStringLiteral("1048576")};
        return known.contains(t) ? std::optional<QVariant>(t) : std::nullopt;
    });
    s.addLegacyKey(QStringLiteral("adv_sector"), keys::kSectorSize, [](const QVariant& v) -> std::optional<QVariant> {
        const QString t = v.toString().trimmed();
        return (t == u"512" || t == u"4096") ? std::optional<QVariant>(t) : std::nullopt;
    });
    s.addLegacyKey(QStringLiteral("adv_img_size_override"), keys::kImageSizeGiB,
                   [](const QVariant& v) -> std::optional<QVariant> {
                       const QString t = v.toString().trimmed();
                       if (t.isEmpty() || t.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0)
                           return QVariant(0.0);
                       bool ok = false;
                       const double gb = t.toDouble(&ok);
                       return (ok && gb >= 0) ? std::optional<QVariant>(gb) : std::nullopt;
                   });
    s.addLegacyKey(QStringLiteral("adv_excl_hidden"), keys::kExcludeHidden);
    s.addLegacyKey(QStringLiteral("adv_retries"), keys::kOpenRetries,
                   [](const QVariant& v) { return legacyInt(v, 0, 20); });
    s.addLegacyKey(QStringLiteral("adv_retry_wait"), keys::kRetryWaitSeconds,
                   [](const QVariant& v) { return legacyInt(v, 0, 60); });
    s.addLegacyKey(QStringLiteral("adv_delete_source"), keys::kDeleteSourceAfterVerify);
    s.addLegacyKey(QStringLiteral("exfat_extract_robocopy_mt"), keys::kExtractParallel);
    s.addLegacyKey(QStringLiteral("exfat_extract_mt_threads"), keys::kExtractThreads,
                   [](const QVariant& v) { return legacyInt(v, 1, 128); });


    registerJobKinds(ctx.jobs);
}

} // namespace fw::exfat
