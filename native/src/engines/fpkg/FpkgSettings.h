#pragma once

// Settings of the FPKG engine and the immutable snapshot a job reads.
//
// Every key declared here is consumed by FpkgBuilder / the FPKG jobs. Options
// of the reference's built-in engine (LibProsperoPkg: Kraken, PFS v3, shuffle,
// block size, ...) are deliberately absent: this engine only drives the
// user's own Sony Publishing Tools.
//
// App-wide settings the engine also reads (declared by engines/general):
// performance.workerThreads (img_create CPU-core limit, pre-scan and DDS
// parallelism), storage.tempFolder (job temp and the SDK's TEMP/TMP),
// storage.defaultOutputFolder, storage.freeSpaceMarginMiB,
// performance.ioBufferMiB, performance.keepComputerAwake (applied by the
// queue) and verification.verifyAfterBuild.

#include "core/Settings.h"

#include <QString>

#include <optional>

namespace fw::fpkg {

namespace keys {
inline const QString kPubToolsPath = QStringLiteral("fpkg.pubToolsPath");
inline const QString kDds2pngPath = QStringLiteral("fpkg.dds2pngPath");
inline const QString kCompressionLevel = QStringLiteral("fpkg.compressionLevel");
inline const QString kPasscode = QStringLiteral("fpkg.passcode");
inline const QString kKeepIntermediate = QStringLiteral("fpkg.keepIntermediate");
inline const QString kPrescan = QStringLiteral("fpkg.prescan");
inline const QString kOutputConflict = QStringLiteral("fpkg.outputConflict");
inline const QString kClearVersionFileUri = QStringLiteral("fpkg.clearVersionFileUri");
inline const QString kClearAttribute3 = QStringLiteral("fpkg.clearAttribute3");
inline const QString kLowerRequiredFirmware = QStringLiteral("fpkg.lowerRequiredFirmware");
inline const QString kAutoSizeClass = QStringLiteral("fpkg.autoSizeClass");
inline const QString kKeepEmptyFolders = QStringLiteral("fpkg.keepEmptyFolders");
inline const QString kKeepDlcEmulator = QStringLiteral("fpkg.keepDlcEmulator");
inline const QString kRemoveAmprLeftovers = QStringLiteral("fpkg.removeAmprLeftovers");
inline const QString kRemoveDumpLeftovers = QStringLiteral("fpkg.removeDumpLeftovers");
inline const QString kRepairSelf = QStringLiteral("fpkg.repairSelf");
inline const QString kRepairUcp = QStringLiteral("fpkg.repairUcp");
inline const QString kRecoverPngFromDds = QStringLiteral("fpkg.recoverPngFromDds");
inline const QString kPlayGoFallback = QStringLiteral("fpkg.playGoFallback");
inline const QString kPlayGoChunkCount = QStringLiteral("fpkg.playGoChunkCount");
inline const QString kFillEmptyChunks = QStringLiteral("fpkg.fillEmptyChunks");
inline const QString kUseSourcePlayGoTable = QStringLiteral("fpkg.useSourcePlayGoTable");
inline const QString kPlayGoAllLanguages = QStringLiteral("fpkg.playGoAllLanguages");
inline const QString kPostVerify = QStringLiteral("fpkg.postVerify");
inline const QString kComputeSha256 = QStringLiteral("fpkg.computeSha256");
} // namespace keys

enum class OutputConflict { KeepBoth, Overwrite, Fail };
enum class UpdateOutput { Both, UpdateOnly, FullOnly };
enum class PostVerify { HeaderOnly, ToolFormat, ToolFull };

QString outputConflictName(OutputConflict c);   // "keepBoth" | "overwrite" | "fail"
std::optional<OutputConflict> parseOutputConflict(const QString& s);
QString updateOutputName(UpdateOutput u);        // "both" | "update" | "full"
std::optional<UpdateOutput> parseUpdateOutput(const QString& s);

inline const QString kDefaultPasscode = QString(32, QLatin1Char('0'));

// Empty string when acceptable, else why not (32 printable ASCII characters).
QString passcodeProblem(const QString& passcode);

struct FpkgSettings {
    // Tools
    QString pubToolsPath;
    QString dds2pngPath;

    // Packing
    std::optional<int> compressionLevel; // nullopt = not passed (tool default 7)
    QString passcode = kDefaultPasscode;
    bool keepIntermediate = false;
    bool prescan = true;
    OutputConflict outputConflict = OutputConflict::KeepBoth;

    // param.json
    bool clearVersionFileUri = true;
    bool clearAttribute3 = false;
    bool lowerRequiredFirmware = true;
    bool autoSizeClass = true;

    // Files
    bool keepEmptyFolders = true;
    bool keepDlcEmulator = true;
    bool removeAmprLeftovers = true;
    bool removeDumpLeftovers = true;
    bool repairSelf = true;
    bool repairUcp = false;
    bool recoverPngFromDds = true;

    // PlayGo
    bool playGoFallback = true;
    int playGoChunkCount = 100;
    bool fillEmptyChunks = true;
    bool useSourcePlayGoTable = false;
    bool playGoAllLanguages = false;
    QStringList playGoForcedLanguages; // not a setting: backport updates keep the base's languages

    // Verification
    PostVerify postVerify = PostVerify::ToolFull; // effective value (HeaderOnly when verifyAfterBuild is off)
    bool computeSha256 = false;

    // App-wide values (engines/general)
    int cpuCoreLimit = 0;          // effective workers (performance.workerThreads / mode); 0 = no limit
    int priorityMode = 2;          // performance.mode for Sony's publisher (applyProcessPriority)
    int workerThreads = 1;         // effective thread count (>= 1)
    qint64 ioBufferBytes = 4 * 1024 * 1024;
    qint64 freeSpaceMarginBytes = 512LL * 1024 * 1024;
    QString defaultOutputDir;      // storage.defaultOutputFolder
    QString userTempFolder;        // storage.tempFolder ("" = not set: image sources are staged on the output drive)

    static FpkgSettings from(const SettingsValues& values);
};

// Declares every fpkg.* setting and the legacy (Python app) key mapping.
void registerFpkgSettings(SettingsSchema& schema);

} // namespace fw::fpkg
