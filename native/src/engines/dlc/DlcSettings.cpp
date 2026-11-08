#include "engines/dlc/DlcSettings.h"

#include "engines/general/Module.h"

#include <QRegularExpression>
#include <QThread>

#include <algorithm>

namespace fw::dlc {

QString hex32Problem(const QString& value)
{
    static const QRegularExpression re(QStringLiteral("^[0-9A-Fa-f]{32}$"));
    if (re.match(value.trimmed()).hasMatch())
        return {};
    return QStringLiteral("must be exactly 32 hexadecimal characters (it has %1).").arg(value.trimmed().size());
}

DlcSettings DlcSettings::from(const SettingsValues& v)
{
    DlcSettings s;
    s.fpkgCliPath = v.string(keys::kFpkgCliPath).trimmed();
    s.outputFolder = v.string(keys::kOutputFolder).trimmed();
    const QString key = v.string(keys::kEntitlementKey).trimmed();
    s.entitlementKey = key.isEmpty() ? kZeroKey : key.toUpper();
    const QString pc = v.string(keys::kSourcePasscode).trimmed();
    s.sourcePasscode = pc.isEmpty() ? kZeroKey : pc;
    s.imageMode = v.string(keys::kImageMode) == u"native" ? QStringLiteral("native") : QStringLiteral("plaintext");
    s.compressionLevel = int(std::clamp<qint64>(v.value(keys::kCompressionLevel).isValid() ? v.integer(keys::kCompressionLevel) : 7, -4, 9));
    const QString b = v.string(keys::kBackend);
    s.backend = (b == u"builtin" || b == u"pubtools" || b == u"none") ? b : QStringLiteral("auto");
    s.pfsFormat = v.string(keys::kPfsFormat) == u"v3" ? QStringLiteral("v3") : QStringLiteral("v2");
    s.sdkOverride = int(std::clamp<qint64>(v.integer(keys::kSdkOverride), 0, 11));
    s.deterministic = v.value(keys::kDeterministic).isValid() ? v.boolean(keys::kDeterministic) : true;
    s.fullVerify = v.value(keys::kFullVerify).isValid() ? v.boolean(keys::kFullVerify) : true;
    s.onlineLookup = v.value(keys::kOnlineLookup).isValid() ? v.boolean(keys::kOnlineLookup) : true;
    s.saveReport = v.value(keys::kSaveReport).isValid() ? v.boolean(keys::kSaveReport) : true;
    const int cores = std::max(1, QThread::idealThreadCount());
    const int workers = general::workerThreads(v);
    s.threads = workers >= cores ? 0 : workers;
    s.priorityMode = general::performanceMode(v);
    s.freeSpaceMarginBytes = general::freeSpaceMarginBytes(v);
    return s;
}

void registerDlcSettings(SettingsSchema& s)
{
    const QString dlc = QStringLiteral("DLC");
    int order = 0;
    const auto add = [&](SettingDef d) {
        d.order = order++;
        s.add(std::move(d));
    };
    {
        SettingDef d;
        d.key = keys::kFpkgCliPath;
        d.section = QStringLiteral("Tools");
        d.group = QStringLiteral("DLC engine");
        d.label = QStringLiteral("fpkg-cli.exe");
        d.description = QStringLiteral(
            "PSVIETHOA's command-line front end of Drakmor's LibProsperoPkg engine, used to build PS5 DLC packages. "
            "Choose the .exe or its folder. Empty: the copy included with a private build, or the FW_FPKG_CLI "
            "environment variable.");
        d.type = SettingType::FilePath;
        d.defaultValue = QString();
        d.fileFilter = QStringLiteral("fpkg-cli (fpkg-cli.exe);;Programs (*.exe)");
        d.dependencyId = QStringLiteral("dlc.fpkgcli");
        d.order = 120;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kOutputFolder;
        d.section = dlc;
        d.group = QStringLiteral("DLC packages");
        d.label = QStringLiteral("Output folder");
        d.description = QStringLiteral("Where converted DLC packages and updated INIs are saved. Empty: a \"DLC "
                                       "packages\" folder next to the first input.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kEntitlementKey;
        d.section = dlc;
        d.group = QStringLiteral("DLC packages");
        d.label = QStringLiteral("Entitlement key");
        d.description = QStringLiteral("32 hexadecimal characters. Development entitlement records are generated for "
                                       "the target Content ID with this key. Default: 32 zeroes.");
        d.type = SettingType::String;
        d.defaultValue = kZeroKey;
        d.validator = [](const QVariant& v) { return hex32Problem(v.toString()); };
        d.perJob = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kSourcePasscode;
        d.section = dlc;
        d.group = QStringLiteral("DLC packages");
        d.label = QStringLiteral("PKG source passcode");
        d.description = QStringLiteral("32 hexadecimal characters used to extract imported DLC packages and for the "
                                       "rebuilt package. Default: 32 zeroes.");
        d.type = SettingType::String;
        d.defaultValue = kZeroKey;
        d.validator = [](const QVariant& v) { return hex32Problem(v.toString()); };
        d.perJob = true;
        d.advanced = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kOnlineLookup;
        d.section = dlc;
        d.group = QStringLiteral("DLC packages");
        d.label = QStringLiteral("Find regional releases online");
        d.description = QStringLiteral("When DLC is added, look up the game and its regional releases on the "
                                       "PlayStation Store, SerialStation and ProsperoPatches. Only identifiers are "
                                       "sent. Off: enter target identifiers manually.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kSaveReport;
        d.section = dlc;
        d.group = QStringLiteral("DLC packages");
        d.label = QStringLiteral("Save a results report");
        d.description = QStringLiteral("Writes DLC-results-<date>.json and .log into the output folder after each "
                                       "conversion or INI export.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.advanced = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kImageMode;
        d.section = dlc;
        d.group = QStringLiteral("Engine");
        d.label = QStringLiteral("Outer image mode");
        d.description = QStringLiteral("PLAINTEXT_NOAUTH (like game packages) or the engine's native encrypted outer "
                                       "layer.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("plaintext");
        d.choices = {{QStringLiteral("plaintext"), QStringLiteral("Plaintext (PLAINTEXT_NOAUTH)"), {}},
                     {QStringLiteral("native"), QStringLiteral("Native"), {}}};
        d.perJob = true;
        d.advanced = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kCompressionLevel;
        d.section = dlc;
        d.group = QStringLiteral("Engine");
        d.label = QStringLiteral("Kraken level");
        d.description = QStringLiteral("Compression level of the DLC data (-4 fastest ... 9 smallest).");
        d.type = SettingType::Int;
        d.defaultValue = 7;
        d.minimum = -4;
        d.maximum = 9;
        d.perJob = true;
        d.advanced = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kBackend;
        d.section = dlc;
        d.group = QStringLiteral("Engine");
        d.label = QStringLiteral("Kraken encoder");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("auto");
        d.description = QStringLiteral("Automatic uses the native Oodle encoder when libScePubTools.dll is available, "
                                       "else the built-in one (byte-identical).");
        d.choices = {{QStringLiteral("auto"), QStringLiteral("Automatic"), {}},
                     {QStringLiteral("builtin"), QStringLiteral("Built-in"), {}},
                     {QStringLiteral("pubtools"), QStringLiteral("Publishing Tools required"), {}},
                     {QStringLiteral("none"), QStringLiteral("Uncompressed"), {}}};
        d.perJob = true;
        d.advanced = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kPfsFormat;
        d.section = dlc;
        d.group = QStringLiteral("Engine");
        d.label = QStringLiteral("PFS compression format");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("v2");
        d.description = QStringLiteral("Version 2 is the engine's default; version 3 adds shuffles and region hints.");
        d.choices = {{QStringLiteral("v2"), QStringLiteral("Version 2"), {}}, {QStringLiteral("v3"), QStringLiteral("Version 3"), {}}};
        d.perJob = true;
        d.advanced = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kSdkOverride;
        d.section = dlc;
        d.group = QStringLiteral("Engine");
        d.label = QStringLiteral("SDK version override");
        d.description = QStringLiteral("0 keeps the DLC's own SDK version. 1-11 overrides the SDK major version (and the "
                                       "required firmware).");
        d.type = SettingType::Int;
        d.defaultValue = 0;
        d.minimum = 0;
        d.maximum = 11;
        d.perJob = true;
        d.advanced = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kDeterministic;
        d.section = dlc;
        d.group = QStringLiteral("Engine");
        d.label = QStringLiteral("Deterministic builds");
        d.description = QStringLiteral("The same input always gives the same package bytes.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.perJob = true;
        d.advanced = true;
        add(d);
    }
    {
        SettingDef d;
        d.key = keys::kFullVerify;
        d.section = dlc;
        d.group = QStringLiteral("Engine");
        d.label = QStringLiteral("Full structural verification");
        d.description = QStringLiteral("Decrypts and test-decompresses every file of each new package before it is "
                                       "saved. The quick content check always runs.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.perJob = true;
        add(d);
    }
}

} // namespace fw::dlc
