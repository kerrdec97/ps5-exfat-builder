#include "engines/backport/BackportSettings.h"

#include "core/Log.h"
#include "engines/backport/RightSprx.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QFileInfo>

namespace fw::backport {

namespace {

QString dirMustExistIfSet(const QVariant& v)
{
    const QString s = v.toString().trimmed();
    if (s.isEmpty())
        return {};
    if (QFileInfo(s).isFile())
        return QStringLiteral("this is a file; choose a folder.");
    if (!QFileInfo(s).isDir())
        return QStringLiteral("the folder does not exist.");
    return {};
}

void warnLegacy(const QString& what)
{
    Logger::instance().log(LogLevel::Warning, QStringLiteral("backport"), what);
}

} // namespace

void registerBackportSettings(SettingsSchema& s)
{
    const QString kBackport = QStringLiteral("Backport");
    int order = 0;

    // ---- Backport: folders ----------------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kOriginalsZipFolder;
        d.section = kBackport;
        d.group = QStringLiteral("Folders");
        d.label = QStringLiteral("Originals ZIP folder");
        d.description = QStringLiteral("Where the originals ZIP is saved before anything in a game changes, with the "
                                       "BackportBackup manifests. Required to start a backport. Must be outside every "
                                       "game folder and not inside the backport ZIP folder (the same folder is fine).");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.validator = dirMustExistIfSet;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kBackportZipFolder;
        d.section = kBackport;
        d.group = QStringLiteral("Folders");
        d.label = QStringLiteral("Backport ZIP folder");
        d.description = QStringLiteral("Where \"FW <N>xx <game> backport files.zip\" is saved. Empty: no backport ZIP "
                                       "(then Apply to game folders must be on, or the run would produce nothing).");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.validator = dirMustExistIfSet;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kFakelibFolder;
        d.section = kBackport;
        d.group = QStringLiteral("Folders");
        d.label = QStringLiteral("Fakelib folder");
        d.description = QStringLiteral("A folder with the fakelib .sprx/.prx libraries directly in it (no sub-folders, "
                                       "each at least 1 KiB, ELF/SELF only, outside the game). They are copied to "
                                       "fakelib/ of the game and of every sub-folder with its own eboot.bin. Empty: "
                                       "no fakelib is added; a dump's own fakelib/ is carried over unchanged.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.validator = dirMustExistIfSet;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    {
        SettingDef d;
        d.key = keys::kAmprPlaygoFolder;
        d.section = kBackport;
        d.group = QStringLiteral("Folders");
        d.label = QStringLiteral("AMPR / PlayGo libraries");
        d.description = QStringLiteral("A folder with your own libSceAmpr.sprx and/or libScePlayGo.sprx (for example a "
                                       "build of ampr_emu). They take the place of the same-named libraries in fakelib/ "
                                       "(from the fakelib folder or the dump). Their SHA-256 is pinned at the first "
                                       "backport; a changed file is refused until you pin the folder again on the "
                                       "Backport page. Nothing is bundled or downloaded. Empty: not used.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.validator = dirMustExistIfSet;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    {
        SettingDef d;
        d.key = keys::kStandardRightSprx;
        d.section = kBackport;
        d.group = QStringLiteral("Folders");
        d.label = QStringLiteral("Standard right.sprx");
        d.description = QStringLiteral("Some dumps carry a sce_sys/about/right.sprx that cannot be converted (still "
                                       "encrypted; for example MGS4, 007 First Light, Planet of Lana). Pick your own copy "
                                       "of the standard Sony right.sprx (the one most dumps carry, SHA-256 81544cbb...) and "
                                       "it takes that file's place, converted like every executable; the dump's copy is "
                                       "kept in the originals ZIP. Nothing is bundled. Empty: such a dump is refused.");
        d.type = SettingType::FilePath;
        d.defaultValue = QString();
        d.validator = [](const QVariant& v) -> QString {
            const QString p = v.toString().trimmed();
            return p.isEmpty() ? QString() : standardRightSprxProblem(p);
        };
        d.order = order++;
        s.add(d);
    }

    // ---- Backport: target -----------------------------------------------------------
    // In the Backport section (not "Firmware and SDK") so that the Backport page,
    // which lists the Backport section, shows them; before, Target SDK appeared
    // on the FPKG page instead.
    {
        SettingDef d;
        d.key = keys::kTargetSdk;
        d.section = kBackport;
        d.group = QStringLiteral("Target");
        d.label = QStringLiteral("Target SDK");
        d.description = QStringLiteral("The SDK written into every executable. An SDK N build only loads on firmware "
                                       "N.00 and later. SDK 4 is the tested baseline; SDK 5 fixed black screens for "
                                       "some titles on newer firmware; other targets are untested on hardware. It can't "
                                       "be higher than the game's own SDK.");
        d.type = SettingType::Enum;
        d.defaultValue = QString::number(kDefaultTargetSdk);
        for (const SdkPair& p : sdkPairs())
            d.choices << SettingChoice{QString::number(p.n), firmwareLabel(p.n), {}};
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kProFix;
        d.section = kBackport;
        d.group = QStringLiteral("Target");
        d.label = QStringLiteral("PS5 Pro fix");
        d.description = QStringLiteral("For a black screen or GPU hang on a PS5 Pro. Turns the guarded shader-assert "
                                       "traps in eboot.bin into no-ops (the fix confirmed on Assassin's Creed Black Flag "
                                       "Resynced; not confirmed on other games). The ZIP is named \"... Pro fix backport "
                                       "files.zip\". The Backport GUI's AGCDriver hook is not included.");
        d.type = SettingType::Bool;
        d.defaultValue = false;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kNidPatchMode;
        d.section = kBackport;
        d.group = QStringLiteral("Target");
        d.label = QStringLiteral("libc import rename");
        d.description = QStringLiteral("Automatic renames one libc import for targets up to SDK 6 and renames it back "
                                       "for targets above 6, as the earlier engine did. Off leaves imports unchanged.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("auto");
        d.choices = {{QStringLiteral("auto"), QStringLiteral("Automatic (SDK 6 and lower)"), {}},
                     {QStringLiteral("off"), QStringLiteral("Off"), {}}};
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Backport: run --------------------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kApplyToGameFolder;
        d.section = kBackport;
        d.group = QStringLiteral("Run");
        d.label = QStringLiteral("Apply to game folders");
        d.description = QStringLiteral("Copy the backported files into the game folder after the originals ZIP has been "
                                       "written and verified, with a journal that rolls back an interrupted copy. Off: "
                                       "only the ZIPs are made and the game folder is not changed.");
        d.type = SettingType::Bool;
        d.defaultValue = true; // owner decision (D5); PS5 Backport GUI defaults to off
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kNotes;
        d.section = kBackport;
        d.group = QStringLiteral("Run");
        d.label = QStringLiteral("Notes");
        d.description = QStringLiteral("Free text stored in the game's backport manifest, for example which console "
                                       "and firmware you tested on.");
        d.type = SettingType::String;
        d.defaultValue = QString();
        d.perJob = true;
        d.validator = [](const QVariant& v) -> QString {
            return v.toString().size() > kMaxNotesLength
                ? QStringLiteral("at most %1 characters.").arg(kMaxNotesLength)
                : QString();
        };
        d.order = order++;
        s.add(d);
    }

    // ---- Backport: game library -------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kScanFolders;
        d.section = kBackport;
        d.group = QStringLiteral("Game library");
        d.label = QStringLiteral("Folders to scan for games");
        d.description = QStringLiteral("Folders searched for game folders (a folder with eboot.bin at its root). The "
                                       "ZIP folders are never scanned. Missing folders are skipped with a warning.");
        d.type = SettingType::StringList;
        d.defaultValue = QStringList();
        d.validator = [](const QVariant& v) -> QString {
            for (const QString& f : v.toStringList())
                if (f.trimmed().isEmpty())
                    return QStringLiteral("a folder entry is empty.");
            return {};
        };
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kScanDepth;
        d.section = kBackport;
        d.group = QStringLiteral("Game library");
        d.label = QStringLiteral("Sub-folder depth");
        d.description = QStringLiteral("How many folder levels below each scanned folder are searched for games.");
        d.type = SettingType::Int;
        d.defaultValue = 6;
        d.minimum = 1;
        d.maximum = 12;
        d.unit = QStringLiteral("levels");
        d.order = order++;
        s.add(d);
    }

    // ---- Backport: safety (advanced) ---------------------------------------------
    {
        SettingDef d;
        d.key = keys::kEncryptedSelfPolicy;
        d.section = kBackport;
        d.group = QStringLiteral("Safety");
        d.label = QStringLiteral("Encrypted or compressed executables");
        d.description = QStringLiteral("What to do when a SELF in the game is still encrypted or compressed (for "
                                       "example sce_sys/about/right.sprx). Refuse stops before anything changes. Leave "
                                       "unchanged copies such files as they are and marks the run Partial; whether the "
                                       "console accepts that is unverified.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("refuse");
        d.choices = {{QStringLiteral("refuse"), QStringLiteral("Refuse the backport"), {}},
                     {QStringLiteral("leaveUnchanged"), QStringLiteral("Leave them unchanged (experimental)"), {}}};
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kVerifyBuild;
        d.section = kBackport;
        d.group = QStringLiteral("Safety");
        d.label = QStringLiteral("Verify the build");
        d.description = QStringLiteral("Re-reads every signed executable before anything is saved (structure, round "
                                       "trip to the patched ELF, SDK in every param). Turn off only to diagnose a "
                                       "problem. The ZIPs are always read back.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kKeepStagingOnFailure;
        d.section = kBackport;
        d.group = QStringLiteral("Safety");
        d.label = QStringLiteral("Keep the staging folder when a run fails");
        d.description = QStringLiteral("Keeps the private folder with the snapshots and patched files of a failed run "
                                       "for diagnosis. It is removed at the next start once it is a day old.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kJournalRetentionDays;
        d.section = kBackport;
        d.group = QStringLiteral("Safety");
        d.label = QStringLiteral("Keep apply journals for");
        d.description = QStringLiteral("Finished apply journals are deleted after this many days. Journals of an "
                                       "interrupted apply are kept until it is rolled back.");
        d.type = SettingType::Int;
        d.defaultValue = 30;
        d.minimum = 1;
        d.maximum = 365;
        d.unit = QStringLiteral("days");
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }

    // ---- Legacy (Python app) keys -------------------------------------------------
    s.addLegacyKey(QStringLiteral("abp_zip_orig_dest"), keys::kOriginalsZipFolder);
    s.addLegacyKey(QStringLiteral("abp_zip_pat_dest"), keys::kBackportZipFolder);
    const auto sdkConverter = [](const QVariant& v) -> std::optional<QVariant> {
        bool ok = false;
        const int n = v.toString().trimmed().toInt(&ok);
        if (!ok || !sdkPair(n)) {
            warnLegacy(QStringLiteral("Legacy target SDK %1 is not a supported pair (1-%2); not imported.")
                           .arg(v.toString())
                           .arg(kMaxTargetSdk));
            return std::nullopt;
        }
        if (n > kMaxLegacyTargetSdk) {
            // The old app wrote pair 10 for "11": importing 11 would silently
            // raise the build's firmware floor to FW 11.00.
            warnLegacy(QStringLiteral("Legacy target SDK %1 was written as SDK %2 by the old app; not imported. Choose "
                                      "the target again in Settings.")
                           .arg(n)
                           .arg(kMaxLegacyTargetSdk));
            return std::nullopt;
        }
        return QVariant(QString::number(n));
    };
    s.addLegacyKey(QStringLiteral("abp_target_sdk"), keys::kTargetSdk, sdkConverter);
    s.addLegacyKey(QStringLiteral("abp_last_sdk"), keys::kTargetSdk, sdkConverter);
    s.addLegacyKey(QStringLiteral("abp_fakelib"), keys::kFakelibFolder, [](const QVariant& v) -> std::optional<QVariant> {
        const QString p = v.toString().trimmed();
        if (!p.isEmpty() && QFileInfo(p).isFile()) {
            warnLegacy(QStringLiteral("Legacy fakelib setting %1 is a file, not a folder; not imported.").arg(p));
            return std::nullopt;
        }
        return QVariant(p);
    });
    s.addLegacyKey(QStringLiteral("abp_notes"), keys::kNotes, [](const QVariant& v) -> std::optional<QVariant> {
        return QVariant(v.toString().left(kMaxNotesLength));
    });
    // bp_folders (list) and bp_scan_folder (single folder) both feed the scan
    // folder list: merged by a whole-file importer instead of a per-key map.
    s.addLegacyImporter([](const QJsonObject& legacy) {
        QStringList folders;
        for (const QJsonValue& v : legacy.value(QStringLiteral("bp_folders")).toArray())
            if (!v.toString().trimmed().isEmpty() && !folders.contains(v.toString().trimmed(), Qt::CaseInsensitive))
                folders << v.toString().trimmed();
        const QString single = legacy.value(QStringLiteral("bp_scan_folder")).toString().trimmed();
        if (!single.isEmpty() && !folders.contains(single, Qt::CaseInsensitive))
            folders << single;
        QVariantMap out;
        if (!folders.isEmpty())
            out.insert(keys::kScanFolders, folders);
        return out;
    });
}

BackportOptions optionsFromSettings(const SettingsValues& v, const QString& game)
{
    BackportOptions o;
    o.game = game;
    o.originalsDir = v.string(keys::kOriginalsZipFolder).trimmed();
    o.backportDir = v.string(keys::kBackportZipFolder).trimmed();
    o.targetSdk = v.string(keys::kTargetSdk).toInt();
    o.fakelibDir = v.string(keys::kFakelibFolder).trimmed();
    o.providerDir = v.string(keys::kAmprPlaygoFolder).trimmed();
    o.standardRightSprx = v.string(keys::kStandardRightSprx).trimmed();
    o.applyToGame = v.boolean(keys::kApplyToGameFolder);
    o.nidMode = v.string(keys::kNidPatchMode) == u"off" ? NidMode::Off : NidMode::Auto;
    o.encryptedPolicy = v.string(keys::kEncryptedSelfPolicy) == u"leaveUnchanged" ? EncryptedPolicy::LeaveUnchanged
                                                                                   : EncryptedPolicy::Refuse;
    o.verifyBuild = v.boolean(keys::kVerifyBuild);
    o.proFix = v.boolean(keys::kProFix);
    o.keepStagingOnFailure = v.boolean(keys::kKeepStagingOnFailure);
    o.journalRetentionDays = int(v.integer(keys::kJournalRetentionDays));
    o.notes = v.string(keys::kNotes).left(kMaxNotesLength);
    const QVariant margin = v.value(keys::kFreeSpaceMarginMiB);
    if (margin.isValid())
        o.freeSpaceMarginBytes = margin.toLongLong() * 1024 * 1024;
    return o;
}

} // namespace fw::backport
