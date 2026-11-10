#include "engines/ufs2/Module.h"

#include "engines/ufs2/Ufs2Jobs.h"

#include <optional>

namespace fw::ufs2 {

FormatParams formatParams(const SettingsValues& settings, QStringList* notes)
{
    FormatParams p;
    p.blockSize = settings.string(keys::kBlockSize).toInt();
    p.fragmentSize = settings.string(keys::kFragmentSize).toInt();
    p.minFreePercent = int(settings.integer(keys::kMinFreePercent));
    p.bytesPerInode = settings.string(keys::kBytesPerInode).toInt();
    if (p.fragmentSize > p.blockSize) {
        if (notes)
            notes->append(QStringLiteral("The fragment size (%1) is larger than the block size; %2 is used for both.")
                              .arg(p.fragmentSize)
                              .arg(p.blockSize));
        p.fragmentSize = p.blockSize;
    }
    return p;
}

namespace {

// Legacy values were strings ("65536") or numbers; keep only values that
// UFS2Tool 4.0.0 accepted. The legacy UI also offered a 128 KiB block size,
// which UFS2Tool rejects ("Block size must be between 4096 and 65536"), so it
// is not imported.
SettingsSchema::LegacyConverter choiceOf(QStringList allowed)
{
    return [allowed](const QVariant& v) -> std::optional<QVariant> {
        const QString s = QString::number(v.toString().trimmed().toLongLong());
        if (allowed.contains(s))
            return QVariant(s);
        return std::nullopt;
    };
}

void addChoice(SettingDef& d, const QString& value, const QString& label, const QString& description = {})
{
    d.choices.append({value, label, description});
}

} // namespace

void registerModule(ModuleContext& ctx)
{
    auto& s = ctx.settings;
    int order = 100;
    const QString section = QStringLiteral("Build");
    const QString group = QStringLiteral("FFPKG images (UFS2)");
    {
        SettingDef d;
        d.key = keys::kBlockSize;
        d.section = section;
        d.group = group;
        d.label = QStringLiteral("Block size");
        d.description = QStringLiteral("UFS2 block size of new .ffpkg images (newfs -b). 64 KB is what PS5 loaders have been "
                                       "fed so far; change it only if a loader asks for something else.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("65536");
        addChoice(d, QStringLiteral("32768"), QStringLiteral("32 KB"));
        addChoice(d, QStringLiteral("65536"), QStringLiteral("64 KB (recommended)"));
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kFragmentSize;
        d.section = section;
        d.group = group;
        d.label = QStringLiteral("Fragment size");
        d.description = QStringLiteral("Smallest allocation unit (newfs -f). Smaller fragments waste less space on many small "
                                       "files. A fragment larger than the block size is reduced to the block size.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("65536");
        addChoice(d, QStringLiteral("32768"), QStringLiteral("32 KB"));
        addChoice(d, QStringLiteral("65536"), QStringLiteral("64 KB (recommended)"));
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kMinFreePercent;
        d.section = section;
        d.group = group;
        d.label = QStringLiteral("Reserved free space");
        d.description = QStringLiteral("Share of the image kept free for the system (newfs -m). 0 uses the whole image, which "
                                       "suits read-only game images; the size of the image does not change.");
        d.type = SettingType::Int;
        d.defaultValue = 0;
        d.minimum = 0;
        d.maximum = 99;
        d.unit = QStringLiteral("%");
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kBytesPerInode;
        d.section = section;
        d.group = group;
        d.label = QStringLiteral("Bytes per inode");
        d.description = QStringLiteral("Inode density (newfs -i). Lower values allow more files per gigabyte; the image grows "
                                       "automatically when a game has more files than the inodes allow.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("262144");
        addChoice(d, QStringLiteral("131072"), QStringLiteral("128 KB"));
        addChoice(d, QStringLiteral("262144"), QStringLiteral("256 KB (recommended)"));
        addChoice(d, QStringLiteral("524288"), QStringLiteral("512 KB"));
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kAmprIndex;
        d.section = section;
        d.group = QStringLiteral("FFPKG contents");
        d.label = QStringLiteral("AMPR file index");
        d.description = QStringLiteral("A game with fakelib/libSceAmpr.sprx needs ampr_emu.index, the list of its files, at "
                                       "its top folder. A new or rebuilt index goes into the image only; the dump folder is "
                                       "never changed.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("auto");
        addChoice(d, QStringLiteral("auto"), QStringLiteral("When the game uses the AMPR emulator"),
                  QStringLiteral("Adds the index when the dump has none and rebuilds one that no longer matches the game's "
                                 "files, as version 4 did. A game with asset packs keeps its own index."));
        addChoice(d, QStringLiteral("missing"), QStringLiteral("Add when the dump has none"),
                  QStringLiteral("An index the dump already has is copied as it is."));
        addChoice(d, QStringLiteral("refresh"), QStringLiteral("Always rebuild it"));
        addChoice(d, QStringLiteral("off"), QStringLiteral("Never add or change it"));
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kDeleteSourceAfterBuild;
        d.section = section;
        d.group = QStringLiteral("Danger zone");
        d.label = QStringLiteral("Delete the game folder after an FFPKG build");
        d.description = QStringLiteral("Removes the source folder once the image is saved, but only when full verification "
                                       "(every byte) passed and the folder contains eboot.bin. Otherwise the folder is kept.");
        d.type = SettingType::Bool;
        d.defaultValue = false;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    s.addLegacyKey(QStringLiteral("adv_ffpkg_block"), keys::kBlockSize,
                   choiceOf({QStringLiteral("32768"), QStringLiteral("65536")}));
    s.addLegacyKey(QStringLiteral("adv_ffpkg_frag"), keys::kFragmentSize,
                   choiceOf({QStringLiteral("32768"), QStringLiteral("65536")}));
    s.addLegacyKey(QStringLiteral("adv_ffpkg_minfree"), keys::kMinFreePercent, [](const QVariant& v) -> std::optional<QVariant> {
        bool ok = false;
        const int n = v.toString().trimmed().toInt(&ok);
        if (!ok || n < 0 || n > 99)
            return std::nullopt;
        return QVariant(n);
    });
    s.addLegacyKey(QStringLiteral("adv_ffpkg_inode"), keys::kBytesPerInode,
                   choiceOf({QStringLiteral("131072"), QStringLiteral("262144"), QStringLiteral("524288")}));
    s.addLegacyKey(QStringLiteral("adv_delete_source_ffpkg"), keys::kDeleteSourceAfterBuild);

    registerJobs(ctx.jobs);
}

} // namespace fw::ufs2
