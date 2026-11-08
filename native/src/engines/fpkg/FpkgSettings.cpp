#include "engines/fpkg/FpkgSettings.h"

#include "engines/general/Module.h"

#include <QThread>

#include <algorithm>

namespace fw::fpkg {

QString outputConflictName(OutputConflict c)
{
    switch (c) {
    case OutputConflict::KeepBoth: return QStringLiteral("keepBoth");
    case OutputConflict::Overwrite: return QStringLiteral("overwrite");
    case OutputConflict::Fail: return QStringLiteral("fail");
    }
    return QStringLiteral("keepBoth");
}

std::optional<OutputConflict> parseOutputConflict(const QString& s)
{
    if (s == u"keepBoth") return OutputConflict::KeepBoth;
    if (s == u"overwrite") return OutputConflict::Overwrite;
    if (s == u"fail") return OutputConflict::Fail;
    return std::nullopt;
}

QString updateOutputName(UpdateOutput u)
{
    switch (u) {
    case UpdateOutput::Both: return QStringLiteral("both");
    case UpdateOutput::UpdateOnly: return QStringLiteral("update");
    case UpdateOutput::FullOnly: return QStringLiteral("full");
    }
    return QStringLiteral("both");
}

std::optional<UpdateOutput> parseUpdateOutput(const QString& s)
{
    if (s.isEmpty() || s == u"both") return UpdateOutput::Both;
    if (s == u"update") return UpdateOutput::UpdateOnly;
    if (s == u"full") return UpdateOutput::FullOnly;
    return std::nullopt;
}

QString passcodeProblem(const QString& passcode)
{
    if (passcode.size() != 32)
        return QStringLiteral("the passcode must be exactly 32 characters (it has %1).").arg(passcode.size());
    for (const QChar c : passcode)
        if (c.unicode() < 0x20 || c.unicode() > 0x7E)
            return QStringLiteral("the passcode may only contain printable ASCII characters.");
    return {};
}

FpkgSettings FpkgSettings::from(const SettingsValues& v)
{
    FpkgSettings s;
    s.pubToolsPath = v.string(keys::kPubToolsPath).trimmed();
    s.dds2pngPath = v.string(keys::kDds2pngPath).trimmed();

    const QString level = v.string(keys::kCompressionLevel);
    bool ok = false;
    const int n = level.toInt(&ok);
    if (ok && n >= -4 && n <= 9)
        s.compressionLevel = n;
    const QString pc = v.string(keys::kPasscode);
    s.passcode = pc.isEmpty() ? kDefaultPasscode : pc;
    s.keepIntermediate = v.boolean(keys::kKeepIntermediate);
    s.prescan = v.boolean(keys::kPrescan);
    s.outputConflict = parseOutputConflict(v.string(keys::kOutputConflict)).value_or(OutputConflict::KeepBoth);

    s.clearVersionFileUri = v.boolean(keys::kClearVersionFileUri);
    s.clearAttribute3 = v.boolean(keys::kClearAttribute3);
    s.lowerRequiredFirmware = v.boolean(keys::kLowerRequiredFirmware);
    s.autoSizeClass = v.boolean(keys::kAutoSizeClass);

    s.keepEmptyFolders = v.boolean(keys::kKeepEmptyFolders);
    s.keepDlcEmulator = v.boolean(keys::kKeepDlcEmulator);
    s.removeAmprLeftovers = v.boolean(keys::kRemoveAmprLeftovers);
    s.removeDumpLeftovers = v.boolean(keys::kRemoveDumpLeftovers);
    s.repairSelf = v.boolean(keys::kRepairSelf);
    s.repairUcp = v.boolean(keys::kRepairUcp);
    s.recoverPngFromDds = v.boolean(keys::kRecoverPngFromDds);

    s.playGoFallback = v.boolean(keys::kPlayGoFallback);
    s.playGoChunkCount = int(std::clamp<qint64>(v.integer(keys::kPlayGoChunkCount), 1, 255));
    s.fillEmptyChunks = v.boolean(keys::kFillEmptyChunks);
    s.useSourcePlayGoTable = v.boolean(keys::kUseSourcePlayGoTable);
    s.playGoAllLanguages = v.boolean(keys::kPlayGoAllLanguages);

    const QString pv = v.string(keys::kPostVerify);
    s.postVerify = pv == u"headerOnly" ? PostVerify::HeaderOnly
                 : pv == u"toolFormat" ? PostVerify::ToolFormat
                                       : PostVerify::ToolFull;
    const QVariant verifyAfter = v.value(general::keys::kVerifyAfterBuild);
    if (verifyAfter.isValid() && !verifyAfter.toBool())
        s.postVerify = PostVerify::HeaderOnly;
    s.computeSha256 = v.boolean(keys::kComputeSha256);

    const int cores = std::max(1, QThread::idealThreadCount());
    s.workerThreads = general::workerThreads(v);
    s.cpuCoreLimit = s.workerThreads >= cores ? 0 : s.workerThreads;
    s.priorityMode = general::performanceMode(v);
    s.ioBufferBytes = general::ioBufferBytes(v);
    s.freeSpaceMarginBytes = general::freeSpaceMarginBytes(v);
    s.defaultOutputDir = v.string(general::keys::kDefaultOutputDir).trimmed();
    s.userTempFolder = v.string(general::keys::kTempFolder).trimmed();
    return s;
}

namespace {

SettingDef boolSetting(const QString& key, const QString& section, const QString& group, const QString& label,
                       const QString& description, bool def, int order, bool perJob = true)
{
    SettingDef d;
    d.key = key;
    d.section = section;
    d.group = group;
    d.label = label;
    d.description = description;
    d.type = SettingType::Bool;
    d.defaultValue = def;
    d.perJob = perJob;
    d.order = order;
    return d;
}

} // namespace

void registerFpkgSettings(SettingsSchema& s)
{
    const QString fpkg = QStringLiteral("FPKG");
    const QString tools = QStringLiteral("Tools");
    const QString playgo = QStringLiteral("PlayGo");
    const QString verification = QStringLiteral("Verification");
    int order = 100;

    // ---- Tools -----------------------------------------------------------------
    {
        SettingDef d;
        d.key = keys::kPubToolsPath;
        d.section = tools;
        d.group = QStringLiteral("Sony Publishing Tools");
        d.label = QStringLiteral("prospero-pub-cmd.exe");
        d.description = QStringLiteral(
            "Your own copy of the Publishing Tools command-line tool. Choose the .exe, its folder, or a toolkit folder "
            "that contains toolchain\\ or bin\\. The app never includes or downloads Sony software. The "
            "SCE_PROSPERO_PUBTOOLS environment variable is used when this is empty.");
        d.type = SettingType::FilePath;
        d.defaultValue = QString();
        d.fileFilter = QStringLiteral("Publishing Tools (prospero-pub-cmd.exe);;Programs (*.exe)");
        d.dependencyId = QStringLiteral("sony.pubcmd");
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kDds2pngPath;
        d.section = tools;
        d.group = QStringLiteral("Sony Publishing Tools");
        d.label = QStringLiteral("prospero-dds2png.exe");
        d.description = QStringLiteral(
            "Converter used to rebuild missing or wrongly formatted sce_sys PNG images from their .dds copies. "
            "Empty: prospero-dds2png.exe in the toolkit folder, if it is there.");
        d.type = SettingType::FilePath;
        d.defaultValue = QString();
        d.fileFilter = QStringLiteral("prospero-dds2png.exe;;Programs (*.exe)");
        d.dependencyId = QStringLiteral("sony.dds2png");
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }

    // ---- FPKG · Packing --------------------------------------------------------
    order = 0;
    {
        SettingDef d;
        d.key = keys::kCompressionLevel;
        d.section = fpkg;
        d.group = QStringLiteral("Packing");
        d.label = QStringLiteral("Compression level");
        d.description = QStringLiteral(
            "Passed to img_create as --compression_level. Higher levels make smaller packages and take longer. "
            "Tool default passes nothing, which the Publishing Tools treat as level 7.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("default");
        d.choices = {{QStringLiteral("default"), QStringLiteral("Tool default (7)"), {}},
                     {QStringLiteral("9"), QStringLiteral("9 (maximum)"), {}},
                     {QStringLiteral("8"), QStringLiteral("8"), {}},
                     {QStringLiteral("7"), QStringLiteral("7 (smallest, explicit)"), {}},
                     {QStringLiteral("6"), QStringLiteral("6"), {}},
                     {QStringLiteral("5"), QStringLiteral("5"), {}},
                     {QStringLiteral("4"), QStringLiteral("4 (standard)"), {}},
                     {QStringLiteral("3"), QStringLiteral("3"), {}},
                     {QStringLiteral("2"), QStringLiteral("2 (fast)"), {}},
                     {QStringLiteral("1"), QStringLiteral("1"), {}},
                     {QStringLiteral("0"), QStringLiteral("0"), {}},
                     {QStringLiteral("-1"), QStringLiteral("-1"), {}},
                     {QStringLiteral("-2"), QStringLiteral("-2"), {}},
                     {QStringLiteral("-3"), QStringLiteral("-3"), {}},
                     {QStringLiteral("-4"), QStringLiteral("-4 (fastest)"), {}}};
        d.perJob = true;
        d.dependencyId = QStringLiteral("sony.pubcmd");
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kPasscode;
        d.section = fpkg;
        d.group = QStringLiteral("Packing");
        d.label = QStringLiteral("Package passcode");
        d.description = QStringLiteral(
            "32 printable ASCII characters written to the project and used by img_verify and img_extract. Debug "
            "packages normally use 32 zeros; change it only if you know the package needs another passcode.");
        d.type = SettingType::String;
        d.defaultValue = kDefaultPasscode;
        d.validator = [](const QVariant& v) { return passcodeProblem(v.toString()); };
        d.perJob = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }
    s.add(boolSetting(keys::kKeepIntermediate, fpkg, QStringLiteral("Packing"),
                      QStringLiteral("Keep the project files"),
                      QStringLiteral("Keeps <name>.gp5, <name>.playgo-scenario.json and .gp5-assets\\<name>\\ next to the "
                                     "package after a successful build. They are always kept after a failure. The "
                                     "build logs are always kept."),
                      false, order++));
    s.add(boolSetting(keys::kPrescan, fpkg, QStringLiteral("Packing"),
                      QStringLiteral("Pre-read source files before packing"),
                      QStringLiteral("Opens every game file once, in parallel, before img_create starts. On Windows this "
                                     "lets antivirus scanning happen in parallel instead of one file at a time inside "
                                     "the Publishing Tools. It changes nothing in the package."),
                      true, order++));
    {
        SettingDef d;
        d.key = keys::kOutputConflict;
        d.section = fpkg;
        d.group = QStringLiteral("Output");
        d.label = QStringLiteral("When the package already exists");
        d.description = QStringLiteral(
            "Keep both renames the existing file to \"name (1).pkg\" and saves the new package under the normal name. "
            "Overwrite replaces it only after the new package passed its checks; the old file is restored if that "
            "fails. Stop fails the job before packing starts.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("keepBoth");
        d.choices = {{QStringLiteral("keepBoth"), QStringLiteral("Keep both"), {}},
                     {QStringLiteral("overwrite"), QStringLiteral("Overwrite"), {}},
                     {QStringLiteral("fail"), QStringLiteral("Stop with an error"), {}}};
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }

    // ---- FPKG · param.json -------------------------------------------------------
    const QString paramGroup = QStringLiteral("param.json");
    s.add(boolSetting(keys::kClearVersionFileUri, fpkg, paramGroup, QStringLiteral("Clear versionFileUri"),
                      QStringLiteral("Empties versionFileUri in the packaged param.json so the console does not look "
                                     "for official updates of this title."),
                      true, order++));
    s.add(boolSetting(keys::kClearAttribute3, fpkg, paramGroup, QStringLiteral("Clear attribute3"),
                      QStringLiteral("Sets attribute3 to 0. This also removes the 120 Hz, VRR and PS5 Pro Enhanced "
                                     "flags, so leave it off unless a game needs it."),
                      false, order++));
    s.add(boolSetting(keys::kLowerRequiredFirmware, fpkg, paramGroup,
                      QStringLiteral("Lower the required firmware to the game's SDK"),
                      QStringLiteral("When requiredSystemSoftwareVersion is higher than the firmware of the game's "
                                     "sdkVersion, it is lowered to that firmware. The Publishing Tools may rewrite "
                                     "this field again."),
                      true, order++));
    s.add(boolSetting(keys::kAutoSizeClass, fpkg, paramGroup, QStringLiteral("Choose the package size class (attributePub, SDK Fix rule)"),
                      QStringLiteral("For full packages, sets attributePub from the estimated package size and lowers "
                                     "kernel.addcontMountLevel when the size requires it (the log warns, because "
                                     "separately installed DLC may then not mount). Updates keep their values."),
                      true, order++));

    // ---- FPKG · Files ------------------------------------------------------------
    const QString filesGroup = QStringLiteral("Files");
    s.add(boolSetting(keys::kKeepEmptyFolders, fpkg, filesGroup, QStringLiteral("Keep empty folders"),
                      QStringLiteral("Declares every empty folder of the dump (outside sce_sys) in the package. DLC "
                                     "emulator mount folders are always declared."),
                      true, order++));
    s.add(boolSetting(keys::kKeepDlcEmulator, fpkg, filesGroup, QStringLiteral("Keep the DLC emulator"),
                      QStringLiteral("Keeps dlc_emu.ini and its fakelib modules and declares its mount folders. Off "
                                     "leaves dlc_emu.ini and the libSceAppContent, libSceNpEntitlementAccess and "
                                     "libSceGameUpdate modules in fakelib out of the package."),
                      true, order++));
    s.add(boolSetting(keys::kRemoveAmprLeftovers, fpkg, filesGroup, QStringLiteral("Leave out AMPR emulator leftovers"),
                      QStringLiteral("Leaves ampr_emu.index and fakelib\\ampr_emu.index out of the package. "
                                     "fakelib\\libSceAmpr.sprx is always left out."),
                      true, order++));
    s.add(boolSetting(keys::kRemoveDumpLeftovers, fpkg, filesGroup, QStringLiteral("Leave out dump leftovers"),
                      QStringLiteral("Leaves the _DUBLEX_ folder and Unreal Engine \"Saved\" folders (Engine\\Saved, "
                                     "<project>\\Saved next to Content\\Paks) out of the package."),
                      true, order++));
    s.add(boolSetting(keys::kRepairSelf, fpkg, filesGroup, QStringLiteral("Fix PS4-format executables"),
                      QStringLiteral("Executables that start with the PS4 SELF signature get the PS5 signature in a copy "
                                     "used for the package. The dump itself is not changed."),
                      true, order++));
    s.add(boolSetting(keys::kRepairUcp, fpkg, filesGroup, QStringLiteral("Fix trophy and UDS archive digests"),
                      QStringLiteral("Checks sce_sys\\trophy2 and sce_sys\\uds .ucp archives and packages a corrected "
                                     "copy when their stored SHA-1 digest is stale. A damaged archive stops the build "
                                     "with its name. Off by default: the format is not confirmed on real dumps."),
                      false, order++));
    {
        SettingDef d = boolSetting(keys::kRecoverPngFromDds, fpkg, filesGroup,
                                   QStringLiteral("Rebuild sce_sys images from .dds"),
                                   QStringLiteral("When an sce_sys PNG is missing or has the wrong colour format "
                                                  "(icon0/pic0/pic1 must be RGB, pic2 RGBA), it is rebuilt from the "
                                                  ".dds copy with prospero-dds2png.exe."),
                                   true, order++);
        d.dependencyId = QStringLiteral("sony.dds2png");
        s.add(d);
    }

    // ---- PlayGo --------------------------------------------------------------------
    order = 100;
    s.add(boolSetting(keys::kPlayGoFallback, playgo, QStringLiteral("FPKG packages"),
                      QStringLiteral("Build a PlayGo layout when the dump has none"),
                      QStringLiteral("Without a usable playgo-chunk.dat: on, the package gets the chunk count below, one "
                                     "small data file per language and the dump's own scenarios; off, a single chunk "
                                     "and a single scenario."),
                      true, order++));
    {
        SettingDef d;
        d.key = keys::kPlayGoChunkCount;
        d.section = playgo;
        d.group = QStringLiteral("FPKG packages");
        d.label = QStringLiteral("PlayGo chunks");
        d.description = QStringLiteral("Number of chunks of the generated layout. Every chunk is installed before the "
                                       "game starts and every scenario lists every chunk.");
        d.type = SettingType::Int;
        d.defaultValue = 100;
        d.minimum = 1;
        d.maximum = 255;
        d.unit = QStringLiteral("chunks");
        d.enabledWhenKey = keys::kPlayGoFallback;
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    s.add(boolSetting(keys::kPlayGoAllLanguages, playgo, QStringLiteral("FPKG packages"),
                      QStringLiteral("All 31 PlayGo languages"),
                      QStringLiteral("Declares every PlayGo language in the package (one chunk each, at least 32 "
                                     "chunks) instead of only the game's own list, like the ppr-pkg Companion's "
                                     "option. Used when the package builds its own PlayGo layout."),
                      false, order++));
    s.add(boolSetting(keys::kFillEmptyChunks, playgo, QStringLiteral("FPKG packages"),
                      QStringLiteral("Fill empty chunks"),
                      QStringLiteral("Adds a 1 MiB uncompressed placeholder to every chunk that would hold no data. An "
                                     "empty chunk counts as not installed and the console can stall at "
                                     "\"Installing... 0 GB\"."),
                      true, order++));
    {
        SettingDef d = boolSetting(keys::kUseSourcePlayGoTable, playgo, QStringLiteral("FPKG packages"),
                                   QStringLiteral("Reuse the dump's PlayGo chunk table"),
                                   QStringLiteral("Keeps the chunks, languages and scenarios of the dump's "
                                                  "playgo-chunk.dat, and each file's chunk from playgo-ficm.dat and "
                                                  "playgo-hash-table.dat. Off by default: the table format and the "
                                                  "path hash are not confirmed on real dumps."),
                                   false, order++);
        d.advanced = true;
        s.add(d);
    }

    // ---- Verification ----------------------------------------------------------------
    order = 100;
    {
        SettingDef d;
        d.key = keys::kPostVerify;
        d.section = verification;
        d.group = QStringLiteral("FPKG packages");
        d.label = QStringLiteral("Check new packages with img_verify");
        d.description = QStringLiteral(
            "The package header and the plaintext debug marker are always checked. Format and integrity also runs "
            "img_verify with its integrity check, which reads the whole package; Format only skips that read.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("toolFull");
        d.choices = {{QStringLiteral("toolFull"), QStringLiteral("Format and integrity"), {}},
                     {QStringLiteral("toolFormat"), QStringLiteral("Format only"), {}},
                     {QStringLiteral("headerOnly"), QStringLiteral("Header check only"), {}}};
        d.enabledWhenKey = general::keys::kVerifyAfterBuild;
        d.dependencyId = QStringLiteral("sony.pubcmd");
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    s.add(boolSetting(keys::kComputeSha256, verification, QStringLiteral("FPKG packages"),
                      QStringLiteral("Compute the SHA-256 of new packages"),
                      QStringLiteral("Reads the finished package once more and records its SHA-256 in the log and the "
                                     "job details."),
                      false, order++));

    // ---- Legacy (Python app) keys ------------------------------------------------------
    s.addLegacyKey(QStringLiteral("fpkg_pubtools_path"), keys::kPubToolsPath);
    s.addLegacyKey(QStringLiteral("fpkg_compression"), keys::kCompressionLevel,
                   [](const QVariant& v) -> std::optional<QVariant> {
                       const QString t = v.toString().trimmed();
                       if (t.isEmpty() || t.startsWith(QStringLiteral("Tool default"), Qt::CaseInsensitive))
                           return QVariant(QStringLiteral("default"));
                       // "Fast (2)", "Standard (4)", "Smallest (7)", "Maximum (9)" or a bare number.
                       const qsizetype open = t.lastIndexOf(u'(');
                       const QString num = open >= 0 ? t.mid(open + 1).chopped(t.endsWith(u')') ? 1 : 0) : t;
                       bool ok = false;
                       const int n = num.trimmed().toInt(&ok);
                       if (!ok || n < -4 || n > 9)
                           return std::nullopt;
                       return QVariant(QString::number(n));
                   });
    s.addLegacyKey(QStringLiteral("fpkg_chunk_count"), keys::kPlayGoChunkCount);
    s.addLegacyKey(QStringLiteral("fpkg_tool_verify"), keys::kPostVerify,
                   [](const QVariant& v) -> std::optional<QVariant> {
                       return QVariant(v.toBool() ? QStringLiteral("toolFull") : QStringLiteral("headerOnly"));
                   });
    s.addLegacyKey(QStringLiteral("fpkg_keep_project"), keys::kKeepIntermediate);
}

} // namespace fw::fpkg
