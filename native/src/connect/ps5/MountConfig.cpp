#include "connect/ps5/MountConfig.h"

#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/FtpTransfer.h"
#include "connect/ps5/IniDocument.h"
#include "connect/ps5/KlogClient.h"
#include "connect/ps5/RemotePath.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <memory>

namespace fw::ps5 {

namespace {

// Python int(v, 0): decimal (no leading zeros), 0x / 0o / 0b prefixes, optional sign.
bool pythonIntBase0(const QString& in, qint64* out)
{
    QString s = in.trimmed();
    s.remove(u'_');
    bool neg = false;
    if (s.startsWith(u'-') || s.startsWith(u'+')) {
        neg = s.startsWith(u'-');
        s.remove(0, 1);
    }
    bool ok = false;
    qint64 v = 0;
    const QString low = s.toLower();
    if (low.startsWith(QLatin1String("0x")))
        v = s.mid(2).toLongLong(&ok, 16);
    else if (low.startsWith(QLatin1String("0o")))
        v = s.mid(2).toLongLong(&ok, 8);
    else if (low.startsWith(QLatin1String("0b")))
        v = s.mid(2).toLongLong(&ok, 2);
    else if (!s.isEmpty() && (s == u"0" || !s.startsWith(u'0'))) {
        for (QChar c : s)
            if (!c.isDigit())
                return false;
        v = s.toLongLong(&ok, 10);
    }
    if (!ok)
        return false;
    *out = neg ? -v : v;
    return true;
}

// Decimal whole number, sign allowed (ShadowMount+ uses strtol on base 10).
bool decimalInt(const QString& in, qint64* out)
{
    static const QRegularExpression re(QStringLiteral("^[+-]?\\d+$"));
    const QString s = in.trimmed();
    if (!re.match(s).hasMatch())
        return false;
    bool ok = false;
    *out = s.toLongLong(&ok);
    return ok;
}

QStringList listLines(const QString& text)
{
    QStringList out;
    for (const QString& raw : QString(text).replace(u'\r', u'\n').split(u'\n')) {
        const QString t = raw.trimmed();
        if (!t.isEmpty() && !t.startsWith(u'#'))
            out << t;
    }
    return out;
}

// parse_bool_ini: 1/0, true/false, yes/no, on/off (any case).
std::optional<bool> parseBool(const QString& v, bool allowRoRw = false)
{
    const QString s = v.trimmed().toLower();
    if (s == u"1" || s == u"true" || s == u"yes" || s == u"on" || (allowRoRw && s == u"ro"))
        return true;
    if (s == u"0" || s == u"false" || s == u"no" || s == u"off" || (allowRoRw && s == u"rw"))
        return false;
    return std::nullopt;
}

bool isPowerOfTwoSector(qint64 n)
{
    return n >= 512 && n <= 1048576 && (n & (n - 1)) == 0;
}

bool isTitleId(const QString& t)
{
    // PPSA/CUSA or a homebrew prefix (four letters) + five digits, any case.
    static const QRegularExpression re(QStringLiteral("^[A-Za-z]{4}\\d{5}$"));
    return re.match(t.trimmed()).hasMatch();
}

QString kStp(const char* s) { return QString::fromLatin1(s); }

SmpKey key(const char* name, const char* section, const char* v2, SmpKeyType type, const char* def, const char* group,
           const char* label, const char* help)
{
    SmpKey k;
    k.key = kStp(name);
    k.section = kStp(section);
    k.v2Key = kStp(v2);
    k.type = type;
    k.defaultValue = QString::fromUtf8(def);
    k.group = kStp(group);
    k.label = QString::fromUtf8(label);
    k.help = QString::fromUtf8(help);
    return k;
}

std::vector<SmpKey> buildSmpKeys()
{
    using T = SmpKeyType;
    std::vector<SmpKey> v;
    auto add = [&](SmpKey k) -> SmpKey& {
        v.push_back(std::move(k));
        return v.back();
    };
    // General
    add(key("debug", "general", "debug", T::Bool, "1", "General", "Debug log",
            "Writes debug.log and the kernel-log lines that Safe to unplug waits for. Keep it on."));
    add(key("quiet_mode", "general", "quiet_mode", T::Bool, "0", "General", "Quiet mode", "Shows fewer notifications on the console."));
    {
        auto& k = add(key("library_sync_notification", "general", "library_sync_notification", T::Bool, "1", "General",
                          "Library sync notification", "Notifies when the game library was synchronised."));
        k.v2Only = true;
    }
    {
        auto& k = add(key("language", "general", "language", T::Text, "auto", "General", "Language",
                          "auto, or a locale such as en-US, de-DE or fr-FR."));
        k.aliases = {QStringLiteral("lang"), QStringLiteral("locale")};
        k.since = QStringLiteral("1.7");
    }
    // Scanning
    {
        auto& k = add(key("scan_depth", "scan", "depth", T::Int, "1", "Scanning", "Scan depth", "How many folder levels below each scan path are searched."));
        k.minimum = 1;
        k.maximum = 2;
    }
    {
        auto& k = add(key("scan_interval_seconds", "scan", "interval_seconds", T::Int, "15", "Scanning", "Scan interval",
                          "Seconds between scans for new games."));
        k.minimum = 1;
        k.maximum = 3600;
        k.aliases = {QStringLiteral("scan_interval_sec")};
    }
    {
        auto& k = add(key("stability_wait_seconds", "scan", "stability_wait_seconds", T::Int, "10", "Scanning",
                          "Stability wait", "Seconds a new game must stay unchanged before it is mounted."));
        k.minimum = 0;
        k.maximum = 3600;
        k.aliases = {QStringLiteral("stability_wait_sec")};
    }
    {
        auto& k = add(key("scanpath", "scan", "path", T::PathList, "", "Scanning", "Scan paths",
                          "One folder per line. Any entry replaces the built-in list."));
        k.repeatable = true;
        k.maxItems = 256;
    }
    {
        auto& k = add(key("recursive_scan", "", "", T::Bool, "", "Scanning", "Recursive scan (deprecated)",
                          "Deprecated: 1 forces depth 2 and shows a notification. Use Scan depth."));
        k.advanced = true;
    }
    // Mounting
    {
        auto& k = add(key("mount_read_only", "mount", "mode", T::Bool, "1", "Mounting", "Mount read-only", "Mounts game images read-only."));
        k.aliases = {QStringLiteral("read_only")};
    }
    add(key("force_mount", "mount", "force", T::Bool, "0", "Mounting", "Force mount", "Mounts images even when a check fails."));
    {
        auto& k = add(key("persistent_image_mounts", "mount", "persistent_images", T::Bool, "0", "Mounting",
                          "Keep images mounted", "Keeps images mounted after the game exits (USB images still unmount for rest mode)."));
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("exfat_backend", "mount", "exfat_backend", T::Enum, "lvd", "Mounting", "exFAT backend",
                          "lvd is needed for the exFAT fast path."));
        k.choices = {QStringLiteral("lvd"), QStringLiteral("md")};
    }
    {
        auto& k = add(key("ufs_backend", "mount", "ufs_backend", T::Enum, "lvd", "Mounting", "UFS backend", "Backend for .ffpkg images."));
        k.choices = {QStringLiteral("lvd"), QStringLiteral("md")};
    }
    {
        auto& k = add(key("nested_pfs_index_cache", "mount", "nested_pfs_index_cache", T::Bool, "0", "Mounting",
                          "Nested PFS index cache", "Caches nested PFS indexes (older GDDR5 option)."));
        k.aliases = {QStringLiteral("legacy_gddr5_cache")};
        k.advanced = true;
        k.since = QStringLiteral("1.7");
    }
    // Library
    add(key("app_install_all", "library", "install_all", T::Bool, "0", "Library", "Register every app",
            "1.7 registers each title on FW 12+ by itself; rarely needed."));
    {
        auto& k = add(key("auto_remove_missing_games", "library", "remove_missing", T::Bool, "0", "Library",
                          "Remove missing games", "Removes games whose image or folder is gone."));
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("auto_remove_games_with_dlc", "library", "remove_with_dlc", T::Bool, "0", "Library",
                          "Also remove games with DLC", "Lets the removal above include games that have DLC."));
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("auto_remove_missing_delay_seconds", "library", "remove_delay_seconds", T::Int, "300", "Library",
                          "Removal delay", "Seconds a game must stay missing before it is removed."));
        k.minimum = 1;
        k.maximum = 86400;
        k.aliases = {QStringLiteral("auto_remove_missing_delay_sec")};
        k.since = QStringLiteral("1.7");
    }
    // Fakelib
    add(key("backport_fakelib", "fakelib", "enabled", T::Bool, "1", "Fakelib", "Backport fakelib", "Uses a game's own fakelib folder."));
    {
        auto& k = add(key("fakelib_default_mode", "fakelib", "default_mode", T::Enum, "full", "Fakelib", "Default fakelib mode",
                          "full, emulators (AMPR/PlayGo libraries only) or disabled."));
        k.choices = {QStringLiteral("full"), QStringLiteral("emulators"), QStringLiteral("disabled")};
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("global_fakelib", "fakelib", "global_enabled", T::Bool, "1", "Fakelib", "Global fakelib",
                          "Uses the shared overlay folder for every game."));
        k.aliases = {QStringLiteral("global_fakelib_enabled")};
    }
    add(key("global_fakelib_path", "fakelib", "global_path", T::Path, "/data/shadowmount/fakelib", "Fakelib",
            "Global fakelib folder", "Folder with the shared library overlays."));
    {
        auto& k = add(key("global_fakelib_priority", "fakelib", "global_priority", T::Enum, "game", "Fakelib",
                          "Global fakelib priority", "Which overlay wins when a game has its own: game or global."));
        k.choices = {QStringLiteral("game"), QStringLiteral("global")};
    }
    // Emulators
    {
        auto& k = add(key("update_emulators", "emulators", "update_enabled", T::Bool, "1", "Emulators", "Update emulators",
                          "Installs emulator updates found in the emulators folder."));
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("emulators_path", "emulators", "path", T::Path, "/data/shadowmount/emus", "Emulators",
                          "Emulators folder", "Where emulator update files are read from."));
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("auto_update_ampr", "emulators", "auto_update_ampr", T::Bool, "0", "Emulators",
                          "Update the AMPR emulator", "Downloads the latest libSceAmpr.sprx 30 s after start, then every 4 h."));
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("ampr_update_url", "emulators", "ampr_update_url", T::Url,
                          "https://github.com/drakmor/ampr_emu/releases/latest/download/libSceAmpr.sprx", "Emulators",
                          "AMPR update URL", "http or https address of libSceAmpr.sprx."));
        k.advanced = true;
        k.since = QStringLiteral("1.7");
    }
    // Fan
    {
        auto& k = add(key("fan_target_temperature", "fan", "target_temperature", T::Temperature, "system", "Fan",
                          "Fan target temperature", "system, or 50–91 °C."));
        k.minimum = 50;
        k.maximum = 91;
        k.since = QStringLiteral("1.7");
    }
    // API
    {
        auto& k = add(key("api_enabled", "api", "enabled", T::Bool, "1", "API", "HTTP API and web UI",
                          "Off also skips the Shell UI launcher install."));
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("api_bind_address", "api", "bind_address", T::Address, "0.0.0.0", "API", "API address",
                          "0.0.0.0 for the whole network, 127.0.0.1 for the console only."));
        k.aliases = {QStringLiteral("api_bind_adress")};
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("api_port", "api", "port", T::Int, "10101", "API", "API port", "TCP port of the API and web UI."));
        k.minimum = 1;
        k.maximum = 65535;
        k.since = QStringLiteral("1.7");
    }
    // Sector sizes
    const char* sectors[][5] = {
        {"lvd_exfat_sector_size", "lvd_exfat", "512", "LVD exFAT sector", "Keep 512 (the exFAT fast path needs it)."},
        {"lvd_ufs_sector_size", "lvd_ufs", "4096", "LVD UFS sector", "Power of two, 512–1048576."},
        {"lvd_pfs_sector_size", "lvd_pfs", "4096", "LVD PFS sector", "Power of two, 512–1048576 (4096 since 1.7)."},
        {"md_exfat_sector_size", "md_exfat", "512", "MD exFAT sector", "Power of two, 512–1048576."},
        {"md_ufs_sector_size", "md_ufs", "4096", "MD UFS sector", "Power of two, 512–1048576."},
    };
    for (const auto& s : sectors) {
        auto& k = add(key(s[0], "sectors", s[1], T::PowerOfTwo, s[2], "Sectors", s[3], s[4]));
        k.minimum = 512;
        k.maximum = 1048576;
        k.advanced = true;
    }
    // Rules (flat repeatable keys; profiles.ini in v2)
    {
        auto& k = add(key("fakelib_mode", "", "", T::FakelibRules, "", "Rules", "Per-game fakelib mode",
                          "One TITLEID:mode per line (default, full, emulators or disabled)."));
        k.repeatable = true;
        k.maxItems = 128;
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("fakelib_exclude", "", "", T::TitleIdList, "", "Rules", "No fakelib for",
                          "One title ID per line; those games get no fakelib."));
        k.repeatable = true;
        k.maxItems = 128;
        k.since = QStringLiteral("1.7");
    }
    {
        auto& k = add(key("global_fakelib_exclude", "", "", T::TitleIdList, "", "Rules", "No global fakelib for",
                          "One title ID per line; those games ignore the global overlays."));
        k.repeatable = true;
        k.aliases = {QStringLiteral("global_fakelib_exclude_title")};
        k.maxItems = 128;
    }
    {
        auto& k = add(key("image_ro", "", "", T::ImageNames, "", "Rules", "Read-only images", "One image file name per line."));
        k.repeatable = true;
        k.maxItems = 128;
    }
    {
        auto& k = add(key("image_rw", "", "", T::ImageNames, "", "Rules", "Read-write images", "One image file name per line."));
        k.repeatable = true;
        k.maxItems = 128;
    }
    {
        auto& k = add(key("image_sector", "", "", T::ImageSectors, "", "Rules", "Image sector sizes",
                          "One name:size per line, e.g. game.ffpkg:4096. Never 65536 for an .exfat image."));
        k.repeatable = true;
        k.maxItems = 128;
    }
    return v;
}

// MicroMount (archived; config.ini.example of PSBrew/MicroMount 0.0.3).
const QList<std::pair<QString, QString>>& microMountDefaults()
{
    static const QList<std::pair<QString, QString>> d = {
        {QStringLiteral("target_directory"), QStringLiteral("/data/homebrew")},
        {QStringLiteral("scan_depth"), QStringLiteral("1")},
        {QStringLiteral("scan_interval_seconds"), QStringLiteral("30")},
        {QStringLiteral("debug"), QStringLiteral("1")},
        {QStringLiteral("lvd_image_type"), QStringLiteral("0")},
        {QStringLiteral("lvd_sector_size"), QStringLiteral("65536")},
        {QStringLiteral("lvd_secondary_unit"), QStringLiteral("65536")},
        {QStringLiteral("lvd_raw_flags"), QStringLiteral("0x9")},
        {QStringLiteral("pfs_fstype"), QStringLiteral("pfs")},
        {QStringLiteral("pfs_mkeymode"), QStringLiteral("AC")},
        {QStringLiteral("pfs_budgetid"), QStringLiteral("system")},
        {QStringLiteral("pfs_sigverify"), QStringLiteral("0")},
        {QStringLiteral("pfs_playgo"), QStringLiteral("0")},
        {QStringLiteral("pfs_disc"), QStringLiteral("0")},
        {QStringLiteral("pfs_use_ekpfs"), QStringLiteral("1")},
        {QStringLiteral("pfs_read_only"), QStringLiteral("1")},
        {QStringLiteral("pfs_force"), QStringLiteral("0")},
    };
    return d;
}

QString flatHeader()
{
    return QStringLiteral("# ShadowMount+ config, written by exFAT Image Builder.\n"
                          "# Only settings that differ from the defaults are listed.\n");
}

QString v2Header()
{
    return QStringLiteral("[config]\nversion = 2\n");
}

// Flat value -> v2 text.
QString toV2Value(const SmpKey& k, const QString& flat)
{
    if (k.type != SmpKeyType::Bool)
        return flat;
    const auto b = parseBool(flat, k.key == u"mount_read_only");
    if (!b)
        return flat;
    if (k.key == u"mount_read_only")
        return *b ? QStringLiteral("ro") : QStringLiteral("rw");
    return *b ? QStringLiteral("on") : QStringLiteral("off");
}

// File text (flat or v2) -> the editor's value: booleans as 1/0.
QString normalizeValue(const SmpKey& k, const QString& text)
{
    if (k.type != SmpKeyType::Bool)
        return text;
    const auto b = parseBool(text, k.key == u"mount_read_only");
    return b ? (*b ? QStringLiteral("1") : QStringLiteral("0")) : text;
}

QString titleUpper(const QString& t)
{
    return t.trimmed().toUpper();
}

// Rule values -> per-section settings of profiles.ini.
struct ProfileRules {
    QMap<QString, QString> fakelib;    // TITLEID -> mode
    QSet<QString> ignoreGlobal;        // TITLEID
    QMap<QString, QString> mountMode;  // file -> ro|rw
    QMap<QString, QString> sectorSize; // file -> size
};

ProfileRules rulesFrom(const MountConfigValues& v)
{
    ProfileRules r;
    for (const QString& line : listLines(v.value(QStringLiteral("fakelib_mode")))) {
        const qsizetype c = line.indexOf(u':');
        if (c > 0)
            r.fakelib.insert(titleUpper(line.left(c)), line.mid(c + 1).trimmed().toLower());
    }
    for (const QString& t : listLines(v.value(QStringLiteral("fakelib_exclude"))))
        r.fakelib.insert(titleUpper(t), QStringLiteral("disabled"));
    for (const QString& t : listLines(v.value(QStringLiteral("global_fakelib_exclude"))))
        r.ignoreGlobal.insert(titleUpper(t));
    for (const QString& f : listLines(v.value(QStringLiteral("image_ro"))))
        r.mountMode.insert(remoteFileName(f), QStringLiteral("ro"));
    for (const QString& f : listLines(v.value(QStringLiteral("image_rw"))))
        r.mountMode.insert(remoteFileName(f), QStringLiteral("rw"));
    for (const QString& line : listLines(v.value(QStringLiteral("image_sector")))) {
        const qsizetype c = line.lastIndexOf(u':');
        if (c > 0)
            r.sectorSize.insert(remoteFileName(line.left(c).trimmed()), line.mid(c + 1).trimmed());
    }
    return r;
}

QString sectionTail(const QString& section, const QString& prefix)
{
    return section.startsWith(prefix, Qt::CaseInsensitive) ? section.mid(prefix.size()) : QString();
}

// Sets or removes one profile setting in every matching section.
void applyProfileSetting(IniDocument& doc, const QString& prefix, const QString& settingKey,
                         const QMap<QString, QString>& wanted, bool caseInsensitiveIds)
{
    // Remove the setting where it is no longer wanted.
    for (const QString& s : doc.sections()) {
        const QString id = sectionTail(s, prefix);
        if (id.isEmpty() || !doc.contains(s, settingKey))
            continue;
        const bool keep = caseInsensitiveIds ? std::any_of(wanted.keyBegin(), wanted.keyEnd(), [&](const QString& w) {
            return w.compare(id, Qt::CaseInsensitive) == 0;
        })
                                             : wanted.contains(id);
        if (!keep)
            doc.remove(s, settingKey);
    }
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
        // Reuse an existing section whatever its case.
        QString section = prefix + it.key();
        for (const QString& s : doc.sections())
            if (sectionTail(s, prefix).compare(it.key(), caseInsensitiveIds ? Qt::CaseInsensitive : Qt::CaseSensitive) == 0)
                section = s;
        doc.set(section, settingKey, it.value());
    }
}

void dropEmptyProfileSections(IniDocument& doc)
{
    for (const QString& s : doc.sections())
        if ((s.startsWith(QLatin1String("game:"), Qt::CaseInsensitive) || s.startsWith(QLatin1String("image:"), Qt::CaseInsensitive))
            && doc.keys(s).isEmpty())
            doc.removeSection(s);
}

QString applyProfiles(const QString& original, const MountConfigValues& before, const MountConfigValues& changes)
{
    MountConfigValues after = before;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it)
        after.insert(it.key(), it.value());
    IniDocument doc(original.isEmpty() ? v2Header() : original, IniDocument::Quoting::V2);
    if (!doc.contains(QStringLiteral("config"), QStringLiteral("version")))
        doc.set(QStringLiteral("config"), QStringLiteral("version"), QStringLiteral("2"));
    const ProfileRules r = rulesFrom(after);
    const QString game = QStringLiteral("game:"), image = QStringLiteral("image:");
    if (changes.contains(QStringLiteral("fakelib_mode")) || changes.contains(QStringLiteral("fakelib_exclude")))
        applyProfileSetting(doc, game, QStringLiteral("fakelib"), r.fakelib, true);
    if (changes.contains(QStringLiteral("global_fakelib_exclude"))) {
        QMap<QString, QString> on;
        for (const QString& t : r.ignoreGlobal)
            on.insert(t, QStringLiteral("on"));
        applyProfileSetting(doc, game, QStringLiteral("ignore_global_fakelib"), on, true);
    }
    if (changes.contains(QStringLiteral("image_ro")) || changes.contains(QStringLiteral("image_rw")))
        applyProfileSetting(doc, image, QStringLiteral("mount_mode"), r.mountMode, false);
    if (changes.contains(QStringLiteral("image_sector")))
        applyProfileSetting(doc, image, QStringLiteral("sector_size"), r.sectorSize, false);
    dropEmptyProfileSections(doc);
    return doc.text();
}

MountConfigValues parseProfiles(const QString& text)
{
    MountConfigValues out;
    if (text.trimmed().isEmpty())
        return out;
    IniDocument doc(text, IniDocument::Quoting::V2);
    QStringList modes, ignore, ro, rw, sector;
    for (const QString& s : doc.sections()) {
        if (const QString t = sectionTail(s, QStringLiteral("game:")); !t.isEmpty()) {
            const QString mode = doc.value(s, QStringLiteral("fakelib")).trimmed();
            if (!mode.isEmpty() && mode.compare(QLatin1String("default"), Qt::CaseInsensitive) != 0)
                modes << QStringLiteral("%1:%2").arg(titleUpper(t), mode.toLower());
            if (parseBool(doc.value(s, QStringLiteral("ignore_global_fakelib"))).value_or(false))
                ignore << titleUpper(t);
        } else if (const QString f = sectionTail(s, QStringLiteral("image:")); !f.isEmpty()) {
            const QString mm = doc.value(s, QStringLiteral("mount_mode")).trimmed().toLower();
            if (mm == u"ro")
                ro << f;
            else if (mm == u"rw")
                rw << f;
            const QString size = doc.value(s, QStringLiteral("sector_size")).trimmed();
            if (!size.isEmpty() && size.compare(QLatin1String("default"), Qt::CaseInsensitive) != 0)
                sector << QStringLiteral("%1:%2").arg(f, size);
        }
    }
    auto put = [&](const char* k, const QStringList& l) {
        if (!l.isEmpty())
            out.insert(QLatin1String(k), l.join(u'\n'));
    };
    put("fakelib_mode", modes);
    put("global_fakelib_exclude", ignore);
    put("image_ro", ro);
    put("image_rw", rw);
    put("image_sector", sector);
    return out;
}

} // namespace

// ---- names and paths -------------------------------------------------------------------------

QString mountToolName(MountTool tool)
{
    return tool == MountTool::ShadowMountPlus ? QStringLiteral("ShadowMountPlus") : QStringLiteral("MicroMount");
}

QString mountRemoteDir(MountTool tool)
{
    return tool == MountTool::ShadowMountPlus ? QStringLiteral("/data/shadowmount") : QStringLiteral("/data/micromount");
}

QString mountConfigRemotePath(MountTool tool)
{
    return mountRemoteDir(tool) + QStringLiteral("/config.ini");
}

QString mountDebugLogRemotePath(MountTool tool)
{
    return mountRemoteDir(tool) + QStringLiteral("/debug.log");
}

QString mountPreviousDebugLogRemotePath(MountTool tool)
{
    return mountRemoteDir(tool) + QStringLiteral("/debug.log.1");
}

QString smpProfilesRemotePath()
{
    return QStringLiteral("/data/shadowmount/profiles.ini");
}

// ---- formats and versions ---------------------------------------------------------------------

QString smpConfigFormatName(SmpConfigFormat f)
{
    switch (f) {
    case SmpConfigFormat::Flat16: return QStringLiteral("1.6 (flat)");
    case SmpConfigFormat::Flat17: return QStringLiteral("1.7 (flat)");
    case SmpConfigFormat::V2: return QStringLiteral("v2 (sections)");
    }
    return {};
}

bool isSmpV2Config(const QString& text)
{
    if (!text.contains(QLatin1String("[config"), Qt::CaseInsensitive))
        return false;
    const IniDocument doc(text, IniDocument::Quoting::V2);
    return doc.value(QStringLiteral("config"), QStringLiteral("version")).trimmed() == u"2";
}

QString smpVersionFromLog(const QString& log)
{
    static const QRegularExpression re(QStringLiteral("ShadowMount\\+ v(\\d[^\\s]*?)\\.? exFAT"));
    QString last;
    auto it = re.globalMatch(log);
    while (it.hasNext())
        last = it.next().captured(1);
    return last;
}

namespace {

struct SmpVersionParts {
    QList<int> numbers; // major, minor, then any numbers after the stage word
    int stage = 3;      // test 0, beta 1, rc 2, release 3
};

SmpVersionParts versionParts(const QString& v)
{
    SmpVersionParts p;
    const QString low = v.trimmed().toLower();
    static const QRegularExpression num(QStringLiteral("\\d+"));
    auto it = num.globalMatch(low);
    while (it.hasNext())
        p.numbers << it.next().captured(0).toInt();
    if (low.contains(QLatin1String("test")))
        p.stage = 0;
    else if (low.contains(QLatin1String("beta")))
        p.stage = 1;
    else if (low.contains(QLatin1String("rc")))
        p.stage = 2;
    return p;
}

} // namespace

int compareSmpVersions(const QString& a, const QString& b)
{
    const SmpVersionParts x = versionParts(a), y = versionParts(b);
    for (int i = 0; i < 2; ++i) {
        const int xa = i < x.numbers.size() ? x.numbers[i] : 0, yb = i < y.numbers.size() ? y.numbers[i] : 0;
        if (xa != yb)
            return xa < yb ? -1 : 1;
    }
    if (x.stage != y.stage)
        return x.stage < y.stage ? -1 : 1;
    for (int i = 2; i < std::max(x.numbers.size(), y.numbers.size()); ++i) {
        const int xa = i < x.numbers.size() ? x.numbers[i] : 0, yb = i < y.numbers.size() ? y.numbers[i] : 0;
        if (xa != yb)
            return xa < yb ? -1 : 1;
    }
    return 0;
}

bool smpVersionAtLeast(const QString& version, const QString& minimum)
{
    if (version.trimmed().isEmpty())
        return false;
    // "1.7" as a minimum means the 1.7 line, betas included.
    static const QRegularExpression plain(QStringLiteral("^\\d+\\.\\d+$"));
    if (plain.match(minimum.trimmed()).hasMatch()) {
        const SmpVersionParts v = versionParts(version), m = versionParts(minimum);
        const int vmaj = v.numbers.value(0), vmin = v.numbers.value(1), mmaj = m.numbers.value(0), mmin = m.numbers.value(1);
        return vmaj != mmaj ? vmaj > mmaj : vmin >= mmin;
    }
    return compareSmpVersions(version, minimum) >= 0;
}

SmpConfigFormat detectSmpConfigFormat(const QString& configText, const QString& smpVersion)
{
    if (isSmpV2Config(configText))
        return SmpConfigFormat::V2;
    if (smpVersionAtLeast(smpVersion, QStringLiteral("1.7")))
        return SmpConfigFormat::Flat17;
    return SmpConfigFormat::Flat16;
}

// ---- schema ------------------------------------------------------------------------------------

const std::vector<SmpKey>& smpKeys()
{
    static const std::vector<SmpKey> keys = buildSmpKeys();
    return keys;
}

const SmpKey* findSmpKey(const QString& keyOrAlias)
{
    const QString k = keyOrAlias.trimmed();
    for (const SmpKey& d : smpKeys())
        if (d.key.compare(k, Qt::CaseInsensitive) == 0 || d.aliases.contains(k, Qt::CaseInsensitive))
            return &d;
    return nullptr;
}

QString canonicalSmpKey(const QString& keyOrAlias)
{
    const SmpKey* d = findSmpKey(keyOrAlias);
    return d ? d->key : keyOrAlias.trimmed();
}

bool isObsoleteSmpKey(const QString& key)
{
    const QString k = key.trimmed().toLower();
    return k.startsWith(QLatin1String("kstuff_")) || k.startsWith(QLatin1String("legacy_mount_"));
}

bool isSmpProfileKey(const QString& key)
{
    static const QStringList k = {QStringLiteral("fakelib_mode"), QStringLiteral("fakelib_exclude"), QStringLiteral("global_fakelib_exclude"),
                                  QStringLiteral("image_ro"), QStringLiteral("image_rw"), QStringLiteral("image_sector")};
    return k.contains(canonicalSmpKey(key));
}

MountConfigValues mountConfigDefaults(MountTool tool)
{
    MountConfigValues out;
    if (tool == MountTool::ShadowMountPlus) {
        for (const SmpKey& k : smpKeys())
            if (!k.defaultValue.isEmpty() && !k.v2Only)
                out.insert(k.key, k.defaultValue);
        return out;
    }
    for (const auto& [k, v] : microMountDefaults())
        out.insert(k, v);
    return out;
}

QStringList mountConfigKeys(MountTool tool)
{
    QStringList keys;
    if (tool == MountTool::ShadowMountPlus) {
        for (const SmpKey& k : smpKeys())
            keys << k.key;
    } else {
        for (const auto& [k, v] : microMountDefaults())
            keys << k;
        keys << QStringLiteral("scanpath");
    }
    keys.sort();
    return keys;
}

// ---- parse ------------------------------------------------------------------------------------

MountConfigValues parseMountConfig(MountTool tool, const QString& configText, const QString& profilesText)
{
    MountConfigValues out;
    if (tool == MountTool::ShadowMountPlus && isSmpV2Config(configText)) {
        const IniDocument doc(configText, IniDocument::Quoting::V2);
        for (const QString& s : doc.sections()) {
            if (s.isEmpty() || s.compare(QLatin1String("config"), Qt::CaseInsensitive) == 0
                || s.startsWith(QLatin1String("smb:"), Qt::CaseInsensitive))
                continue;
            for (const QString& k : doc.keys(s)) {
                const auto it = std::find_if(smpKeys().begin(), smpKeys().end(), [&](const SmpKey& d) {
                    return d.section.compare(s, Qt::CaseInsensitive) == 0 && d.v2Key.compare(k, Qt::CaseInsensitive) == 0;
                });
                if (it == smpKeys().end()) {
                    out.insert(s + u'.' + k, doc.value(s, k));
                    continue;
                }
                out.insert(it->key, it->repeatable ? doc.values(s, k).join(u'\n') : normalizeValue(*it, doc.value(s, k)));
            }
        }
        const MountConfigValues rules = parseProfiles(profilesText);
        for (auto it = rules.cbegin(); it != rules.cend(); ++it)
            out.insert(it.key(), it.value());
        return out;
    }
    QMap<QString, QStringList> multi;
    if (tool == MountTool::MicroMount) {
        // MicroMount's own reader (legacy ui/tab_micromount.py): whole-line
        // comments only; scan_paths is a ","/";" separated list.
        for (const QString& raw : configText.split(u'\n')) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(u'#') || line.startsWith(u';'))
                continue;
            const qsizetype eq = line.indexOf(u'=');
            const QString k = eq > 0 ? line.left(eq).trimmed() : QString();
            const QString v = line.mid(eq + 1).trimmed();
            if (k.isEmpty())
                continue;
            if (k == u"scanpath") {
                multi[k] << v;
            } else if (k == u"scan_paths") {
                static const QRegularExpression sep(QStringLiteral("[,;]"));
                for (const QString& p : v.split(sep))
                    if (!p.trimmed().isEmpty())
                        multi[QStringLiteral("scanpath")] << p.trimmed();
            } else {
                out.insert(k, v);
            }
        }
        for (auto it = multi.cbegin(); it != multi.cend(); ++it)
            out.insert(it.key(), it.value().join(u'\n'));
        return out;
    }
    const IniDocument doc(configText, IniDocument::Quoting::None);
    for (const QString& rawKey : doc.keys(QString())) {
        const SmpKey* d = tool == MountTool::ShadowMountPlus ? findSmpKey(rawKey) : nullptr;
        const QString k = d ? d->key : rawKey;
        const QStringList vals = doc.values(QString(), rawKey);
        const bool repeat = d ? d->repeatable : (rawKey == u"scanpath" || vals.size() > 1);
        if (repeat) {
            multi[k] << vals;
            continue;
        }
        if (vals.isEmpty() || vals.back().isEmpty())
            continue;
        out.insert(k, d ? normalizeValue(*d, vals.back()) : vals.back());
    }
    for (auto it = multi.cbegin(); it != multi.cend(); ++it) {
        QStringList clean;
        for (const QString& v : it.value())
            if (!v.trimmed().isEmpty())
                clean << v.trimmed();
        if (!clean.isEmpty())
            out.insert(it.key(), clean.join(u'\n'));
    }
    return out;
}

MountConfigValues changedMountValues(const MountConfigValues& before, const MountConfigValues& after)
{
    auto norm = [](const QString& v) { return listLines(v).join(u'\n'); };
    MountConfigValues out;
    QStringList keys = before.keys();
    for (const QString& k : after.keys())
        if (!keys.contains(k))
            keys << k;
    for (const QString& k : keys)
        if (norm(before.value(k)) != norm(after.value(k)))
            out.insert(k, after.value(k));
    return out;
}

// ---- edit / render ---------------------------------------------------------------------------

MountConfigFiles applyMountConfigChanges(MountTool tool, const QString& config, const QString& profiles,
                                         const MountConfigValues& changes, SmpConfigFormat format)
{
    MountConfigFiles out;
    out.config = config;
    out.profiles = profiles;
    if (tool == MountTool::MicroMount) {
        if (config.isEmpty()) {
            MountConfigValues merged = mountConfigDefaults(tool);
            for (auto it = changes.cbegin(); it != changes.cend(); ++it)
                merged.insert(it.key(), it.value());
            out.config = renderMountConfig(tool, merged);
        } else {
            IniDocument doc(config);
            for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
                if (it.key() == u"scanpath") {
                    doc.remove(QString(), QStringLiteral("scan_paths"));
                    doc.setList(QString(), QStringLiteral("scanpath"), listLines(it.value()));
                } else if (it.value().trimmed().isEmpty()) {
                    doc.remove(QString(), it.key());
                } else {
                    doc.set(QString(), it.key(), it.value().trimmed());
                }
            }
            out.config = doc.text();
        }
        out.configChanged = out.config != config;
        return out;
    }

    const bool v2 = format == SmpConfigFormat::V2;
    const MountConfigValues before = parseMountConfig(tool, config, profiles);
    IniDocument doc(config.isEmpty() ? (v2 ? v2Header() : flatHeader()) : config,
                    v2 ? IniDocument::Quoting::V2 : IniDocument::Quoting::None);
    if (v2 && !doc.contains(QStringLiteral("config"), QStringLiteral("version")))
        doc.set(QStringLiteral("config"), QStringLiteral("version"), QStringLiteral("2"));
    if (format != SmpConfigFormat::Flat16)
        for (const QString& s : doc.sections())
            for (const QString& k : doc.keys(s))
                if (isObsoleteSmpKey(k))
                    doc.remove(s, k);
    MountConfigValues profileChanges;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        const QString value = it.value().trimmed();
        const SmpKey* d = findSmpKey(it.key());
        if (v2 && d && isSmpProfileKey(d->key)) {
            profileChanges.insert(d->key, it.value());
            continue;
        }
        if (!d) {
            // Unknown key, written as it came: "section.key" in v2, plain in flat.
            QString section, k = it.key();
            if (v2) {
                const qsizetype dot = k.indexOf(u'.');
                if (dot <= 0)
                    continue;
                section = k.left(dot);
                k = k.mid(dot + 1);
            }
            if (value.isEmpty())
                doc.remove(section, k);
            else
                doc.set(section, k, value);
            continue;
        }
        if (v2 ? d->section.isEmpty() : d->v2Only)
            continue; // the key has no place in this format
        const QString section = v2 ? d->section : QString();
        QStringList names = v2 ? QStringList{d->v2Key} : QStringList{d->key} + d->aliases;
        QString present;
        for (const QString& n : names)
            if (doc.contains(section, n)) {
                present = n;
                break;
            }
        if (value.isEmpty()) {
            for (const QString& n : names)
                doc.remove(section, n);
            continue;
        }
        if (present.isEmpty() && !d->repeatable && value == d->defaultValue)
            continue; // never pin a default
        const QString name = present.isEmpty() ? names.front() : present;
        for (const QString& n : names)
            if (n.compare(name, Qt::CaseInsensitive) != 0)
                doc.remove(section, n); // no alias duplicates
        if (d->repeatable)
            doc.setList(section, name, listLines(value));
        else
            doc.set(section, name, v2 ? toV2Value(*d, value) : value);
    }
    out.config = doc.text();
    out.configChanged = out.config != config;
    if (v2 && !profileChanges.isEmpty()) {
        out.profiles = applyProfiles(profiles, before, profileChanges);
        out.profilesChanged = out.profiles != profiles;
    }
    return out;
}

QString renderMountConfig(MountTool tool, const MountConfigValues& data, SmpConfigFormat format)
{
    if (tool == MountTool::MicroMount) {
        // The legacy layout (ui/tab_micromount.py); MicroMount is archived upstream.
        QStringList lines;
        auto put = [&](const char* key) {
            const QString v = data.value(QLatin1String(key));
            if (!v.isEmpty())
                lines << QStringLiteral("%1=%2").arg(QLatin1String(key), v);
        };
        lines << QStringLiteral("# MicroMount runtime config") << QStringLiteral("# Generated by exFAT Image Builder")
              << QStringLiteral("# Place as: /data/micromount/config.ini") << QString();
        lines << QString::fromUtf8("# ── core ───────────────────────────────────────");
        for (const char* k : {"target_directory", "scan_depth", "scan_interval_seconds", "debug"})
            put(k);
        for (const QString& t : listLines(data.value(QStringLiteral("scanpath"))))
            lines << QStringLiteral("scanpath=%1").arg(t);
        lines << QString();
        lines << QString::fromUtf8("# ── LVD mount profile ──────────────────────────");
        for (const char* k : {"lvd_image_type", "lvd_sector_size", "lvd_secondary_unit", "lvd_raw_flags"})
            put(k);
        lines << QString();
        lines << QString::fromUtf8("# ── PFS mount profile ──────────────────────────");
        for (const char* k : {"pfs_fstype", "pfs_mkeymode", "pfs_budgetid", "pfs_sigverify", "pfs_playgo", "pfs_disc",
                              "pfs_use_ekpfs", "pfs_read_only", "pfs_force"})
            put(k);
        return lines.join(u'\n') + u'\n';
    }
    MountConfigValues changes;
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        if (isObsoleteSmpKey(it.key()) || it.value().trimmed().isEmpty())
            continue;
        const SmpKey* d = findSmpKey(it.key());
        if (d && !d->repeatable && it.value().trimmed() == d->defaultValue)
            continue;
        changes.insert(d ? d->key : it.key(), it.value());
    }
    return applyMountConfigChanges(tool, {}, {}, changes, format).config;
}

QString renderSmpProfiles(const MountConfigValues& values)
{
    MountConfigValues rules;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (isSmpProfileKey(it.key()))
            rules.insert(canonicalSmpKey(it.key()), it.value());
    for (const char* k : {"fakelib_mode", "fakelib_exclude", "global_fakelib_exclude", "image_ro", "image_rw", "image_sector"})
        if (!rules.contains(QLatin1String(k)))
            rules.insert(QLatin1String(k), QString());
    return applyProfiles({}, {}, rules);
}

// ---- validation -------------------------------------------------------------------------------

QStringList validateMountConfig(MountTool tool, const MountConfigValues& data, bool flatFormat)
{
    QStringList problems;
    if (flatFormat) {
        // The flat file cannot quote: the console would read "/mnt/usb0/Games #2" as "/mnt/usb0/Games".
        for (auto it = data.cbegin(); it != data.cend(); ++it)
            if (it.value().contains(u'#') || it.value().contains(u';'))
                problems << QStringLiteral("%1 cannot contain # or ; in this config format (the console reads the value up to that character). Got: %2")
                                .arg(it.key(), it.value().simplified());
    }
    if (tool == MountTool::MicroMount) {
        for (const char* k : {"scan_interval_seconds", "lvd_sector_size", "lvd_secondary_unit", "lvd_image_type"}) {
            const QString v = data.value(QLatin1String(k)).trimmed();
            if (v.isEmpty())
                continue;
            qint64 n = 0;
            if (!pythonIntBase0(v, &n) || n < 0)
                problems << QStringLiteral("%1 must be a non-negative integer. Got: %2").arg(QLatin1String(k), v);
        }
        return problems;
    }
    int titleRules = 0, imageRules = 0;
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        const SmpKey* d = findSmpKey(it.key());
        const QString v = it.value().trimmed();
        if (!d || v.isEmpty())
            continue;
        const QString& k = d->key;
        switch (d->type) {
        case SmpKeyType::Bool:
            if (!parseBool(v, k == u"mount_read_only"))
                problems << QStringLiteral("%1 must be 1 or 0 (also true/false, yes/no, on/off). Got: %2").arg(k, v);
            break;
        case SmpKeyType::Int: {
            qint64 n = 0;
            if (!decimalInt(v, &n) || n < d->minimum || n > d->maximum)
                problems << QStringLiteral("%1 must be a whole number in %2–%3. Got: %4").arg(k).arg(d->minimum).arg(d->maximum).arg(v);
            break;
        }
        case SmpKeyType::Enum:
            if (!d->choices.contains(v, Qt::CaseInsensitive))
                problems << QStringLiteral("%1 must be one of %2. Got: %3").arg(k, d->choices.join(QStringLiteral(", ")), v);
            break;
        case SmpKeyType::PowerOfTwo: {
            qint64 n = 0;
            if (!decimalInt(v, &n) || !isPowerOfTwoSector(n))
                problems << QStringLiteral("%1 must be a power of two from 512 to 1048576. Got: %2").arg(k, v);
            break;
        }
        case SmpKeyType::Path:
            if (!v.startsWith(u'/'))
                problems << QStringLiteral("%1 must be an absolute path starting with /. Got: %2").arg(k, v);
            break;
        case SmpKeyType::Url:
            if (!v.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) && !v.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
                problems << QStringLiteral("%1 must be an http or https address. Got: %2").arg(k, v);
            break;
        case SmpKeyType::Address: {
            QHostAddress a;
            if (!a.setAddress(v) || a.protocol() != QAbstractSocket::IPv4Protocol)
                problems << QStringLiteral("%1 must be an IPv4 address such as 0.0.0.0 or 127.0.0.1. Got: %2").arg(k, v);
            break;
        }
        case SmpKeyType::Temperature: {
            qint64 n = 0;
            if (v.compare(QLatin1String("system"), Qt::CaseInsensitive) != 0 && (!decimalInt(v, &n) || n < d->minimum || n > d->maximum))
                problems << QStringLiteral("%1 must be system or %2–%3 (°C). Got: %4").arg(k).arg(d->minimum).arg(d->maximum).arg(v);
            break;
        }
        case SmpKeyType::Text:
            if (k == u"language") {
                static const QRegularExpression loc(QStringLiteral("^(auto|[a-zA-Z]{2,3}([-_][a-zA-Z]{2,4})?)$"));
                if (!loc.match(v).hasMatch())
                    problems << QStringLiteral("language must be auto or a locale such as en-US. Got: %1").arg(v);
            }
            break;
        case SmpKeyType::PathList: {
            const QStringList l = listLines(v);
            for (const QString& p : l)
                if (!p.startsWith(u'/'))
                    problems << QStringLiteral("Scan paths must start with /. Got: %1").arg(p);
            if (d->maxItems > 0 && l.size() > d->maxItems)
                problems << QStringLiteral("At most %1 scan paths are allowed; there are %2.").arg(d->maxItems).arg(l.size());
            break;
        }
        case SmpKeyType::TitleIdList:
            for (const QString& t : listLines(v)) {
                ++titleRules;
                if (!isTitleId(t))
                    problems << QStringLiteral("%1: \"%2\" is not a title ID (four letters and five digits, e.g. PPSA01234).").arg(k, t);
            }
            break;
        case SmpKeyType::FakelibRules:
            for (const QString& t : listLines(v)) {
                ++titleRules;
                const qsizetype c = t.indexOf(u':');
                const QString mode = c > 0 ? t.mid(c + 1).trimmed().toLower() : QString();
                if (c <= 0 || !isTitleId(t.left(c))
                    || !QStringList{QStringLiteral("default"), QStringLiteral("full"), QStringLiteral("emulators"), QStringLiteral("disabled")}.contains(mode))
                    problems << QStringLiteral("fakelib_mode: expected TITLEID:mode (default, full, emulators or disabled), got: %1").arg(t);
            }
            break;
        case SmpKeyType::ImageNames:
            for (const QString& f : listLines(v)) {
                ++imageRules;
                if (f.contains(u'/') || f.contains(u'\\'))
                    problems << QStringLiteral("%1: use the image file name only, not a path. Got: %2").arg(k, f);
            }
            break;
        case SmpKeyType::ImageSectors:
            for (const QString& t : listLines(v)) {
                ++imageRules;
                const qsizetype c = t.lastIndexOf(u':');
                qint64 n = 0;
                const QString name = c > 0 ? t.left(c).trimmed() : QString();
                if (c <= 0 || !decimalInt(t.mid(c + 1), &n) || !isPowerOfTwoSector(n))
                    problems << QStringLiteral("image_sector: expected name:size with a power of two from 512 to 1048576, got: %1").arg(t);
                else if (name.contains(u'/') || name.contains(u'\\'))
                    problems << QStringLiteral("image_sector: use the image file name only, not a path. Got: %1").arg(t);
                else if (name.endsWith(QLatin1String(".exfat"), Qt::CaseInsensitive) && n == 65536)
                    problems << QStringLiteral("image_sector: never set 65536 for an exFAT image (the logical sector stays 512). Got: %1").arg(t);
            }
            break;
        }
    }
    if (titleRules > 128)
        problems << QStringLiteral("At most 128 per-game fakelib rules are allowed; there are %1.").arg(titleRules);
    if (imageRules > 128)
        problems << QStringLiteral("At most 128 per-image rules are allowed; there are %1.").arg(imageRules);
    return problems;
}

QStringList mountConfigWarnings(MountTool tool, const MountConfigValues& v)
{
    QStringList out;
    if (tool != MountTool::ShadowMountPlus)
        return out;
    if (parseBool(v.value(QStringLiteral("debug"), QStringLiteral("1"))) == false)
        out << QStringLiteral("debug is off: ShadowMount+ logs nothing, so Safe to unplug cannot confirm the cleanup.");
    if (const QString s = v.value(QStringLiteral("lvd_exfat_sector_size")).trimmed(); !s.isEmpty() && s != u"512")
        out << QStringLiteral("Keep lvd_exfat_sector_size at 512: the exFAT fast path needs it.");
    if (v.value(QStringLiteral("exfat_backend")).trimmed().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0)
        out << QStringLiteral("The exFAT fast path needs the lvd backend.");
    QStringList obsolete;
    for (auto it = v.cbegin(); it != v.cend(); ++it)
        if (isObsoleteSmpKey(it.key()))
            obsolete << it.key();
    if (!obsolete.isEmpty())
        out << QStringLiteral("%1 no longer exist in ShadowMount+ 1.7 and are removed on push to a 1.7 console.").arg(obsolete.join(QStringLiteral(", ")));
    return out;
}

// ---- FTP --------------------------------------------------------------------------------------

namespace {

// RETR of a small text file; "" with *exists = false when it is not there.
Result<QString> downloadText(FtpClient& c, const QString& remote, bool* exists, const CancellationToken& token)
{
    auto bytes = c.download(remote, token);
    if (!bytes.ok()) {
        if (bytes.error().code == ErrorCode::NotFound && c.isConnected()) {
            *exists = false;
            return QString();
        }
        return bytes.takeError();
    }
    *exists = true;
    return QString::fromUtf8(bytes.value());
}

Result<MountPushResult> pushRemoteText(FtpClient& c, const QString& dir, const QString& remote, const QString& text,
                                       const QString& backupDir, const CancellationToken& token,
                                       const std::optional<QString>& expected)
{
    MountPushResult out;
    if (expected) {
        bool exists = false;
        FW_TRY_ASSIGN(QString now, downloadText(c, remote, &exists, token));
        if (now != *expected)
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 changed on the console since it was loaded (the ShadowMount+ web UI or API may "
                                        "have saved it). Load it again, then make your changes.")
                             .arg(remote),
                         QStringLiteral("expected %1 bytes, found %2").arg(expected->toUtf8().size()).arg(now.toUtf8().size()));
    }
    (void)c.mkdirs(dir); // best effort, like the legacy MKD
    FW_TRY_ASSIGN(out.backupPath, backupRemoteFile(c, remote, backupDir, token));
    const QByteArray bytes = text.toUtf8();
    const Status swap = c.uploadReplace(remote, bytes, QStringLiteral(".new"), token);
    if (swap.ok()) {
        out.swapped = true;
        return out;
    }
    if (swap.error().code != ErrorCode::Unsupported || !c.isConnected())
        return swap.error();
    // Rename unsupported: direct STOR (legacy fallback); the backup is local.
    FW_TRY(c.upload(remote, bytes, token));
    (void)c.dele(remote + QStringLiteral(".new"));
    return out;
}

} // namespace

Result<QString> loadMountConfig(FtpClient& c, MountTool tool, const CancellationToken& token)
{
    auto bytes = c.download(mountConfigRemotePath(tool), token);
    if (!bytes.ok()) {
        if (bytes.error().code == ErrorCode::NotFound)
            return Error(ErrorCode::NotFound,
                         QStringLiteral("%1 does not exist on the PS5. This is normal if %2 has never been run. Edit "
                                        "the settings and push them to create it.")
                             .arg(mountConfigRemotePath(tool), mountToolName(tool)),
                         bytes.error().detail);
        return bytes.takeError();
    }
    return QString::fromUtf8(bytes.value());
}

Result<QString> fetchMountDebugLog(FtpClient& c, MountTool tool, const CancellationToken& token)
{
    FW_TRY_ASSIGN(QByteArray bytes, c.download(mountDebugLogRemotePath(tool), token));
    return QString::fromUtf8(bytes);
}

Result<SmpRemoteState> loadSmpState(FtpClient& c, const QString& apiVersion, const CancellationToken& token, qint64 maxLogBytes)
{
    SmpRemoteState s;
    FW_TRY_ASSIGN(s.config, downloadText(c, mountConfigRemotePath(MountTool::ShadowMountPlus), &s.configExists, token));
    FW_TRY_ASSIGN(s.profiles, downloadText(c, smpProfilesRemotePath(), &s.profilesExists, token));
    if (apiVersion.isEmpty()) {
        const QString log = mountDebugLogRemotePath(MountTool::ShadowMountPlus);
        if (auto size = c.size(log); size.ok() && size.value() <= maxLogBytes) {
            bool exists = false;
            if (auto text = downloadText(c, log, &exists, token); text.ok())
                s.logVersion = smpVersionFromLog(text.value());
            else if (text.error().isCancelled())
                return Error::cancelled();
        }
        if (!c.isConnected())
            return Error(ErrorCode::IoError, QStringLiteral("The PS5 closed the FTP connection."));
    }
    s.format = detectSmpConfigFormat(s.config, apiVersion.isEmpty() ? s.logVersion : apiVersion);
    return s;
}

Result<MountPushResult> pushMountConfig(FtpClient& c, MountTool tool, const QString& text, const QString& backupDir,
                                        const CancellationToken& token, const std::optional<QString>& expectedRemote)
{
    return pushRemoteText(c, mountRemoteDir(tool), mountConfigRemotePath(tool), text, backupDir, token, expectedRemote);
}

Result<MountPushResult> pushSmpProfiles(FtpClient& c, const QString& text, const QString& backupDir, const CancellationToken& token,
                                        const std::optional<QString>& expectedRemote)
{
    return pushRemoteText(c, mountRemoteDir(MountTool::ShadowMountPlus), smpProfilesRemotePath(), text, backupDir, token,
                          expectedRemote);
}

// ---- Safe to unplug -----------------------------------------------------------------------

namespace {

qint64 monotonicMs()
{
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

bool isProblemLine(const QString& low)
{
    static const char* const markers[] = {
        "[shutdown] some image mounts or devices were not fully released",
        "[shutdown] image teardown skipped while title layers remain",
        "[kstuff] keeping automount disabled after incomplete teardown",
        "filesystem release pending",
        "filesystem released; device detach pending",
        "filesystem and device released; mount directory remains",
    };
    for (const char* m : markers)
        if (low.contains(QLatin1String(m)))
            return true;
    return false;
}

} // namespace

StopConfirmTracker::StopConfirmTracker(int settleMs) : m_settleMs(std::max(0, settleMs)), m_clockStart(monotonicMs()) {}

bool StopConfirmTracker::feed(const QString& line)
{
    return feed(line, monotonicMs() - m_clockStart);
}

bool StopConfirmTracker::poll()
{
    return poll(monotonicMs() - m_clockStart);
}

bool StopConfirmTracker::feed(const QString& line, qint64 nowMs)
{
    if (finished())
        return true;
    const QString low = line.toLower();
    if (low.contains(QLatin1String("shadowmountplus.elf calls exit"))) {
        m_exit = true;
        if (m_problems.isEmpty())
            m_confirmed = true;
        else
            m_incomplete = true;
        return true;
    }
    if (low.contains(QLatin1String("[shutdown] stop requested")) || low.contains(QLatin1String("[shutdown] requested by stop file")))
        m_ack = true;
    if (!m_ack)
        return false;
    if (isProblemLine(low))
        m_problems << line.trimmed();
    else if (low.contains(QLatin1String("unmount start")))
        ++m_started;
    else if (low.contains(QLatin1String("unmount complete")))
        ++m_completed;
    if (low.contains(QLatin1String("[shutdown] cleanup complete for"))) {
        m_cleanup = true;
        if (m_problems.isEmpty())
            m_confirmed = true;
        else
            m_incomplete = true;
        return true;
    }
    const bool balanced = m_problems.isEmpty() && m_started > 0 && m_completed >= m_started;
    if (!balanced)
        m_balancedSince = -1;
    else if (m_balancedSince < 0)
        m_balancedSince = nowMs;
    return poll(nowMs);
}

bool StopConfirmTracker::poll(qint64 nowMs)
{
    if (!finished() && m_problems.isEmpty() && m_balancedSince >= 0 && nowMs - m_balancedSince >= m_settleMs)
        m_confirmed = true;
    return finished();
}

QString stopOutcomeMessage(StopOutcome outcome)
{
    switch (outcome) {
    case StopOutcome::Confirmed:
        return QStringLiteral("ShadowMountPlus cleanup confirmed — safe to unplug. Re-send the payload after plugging "
                              "the drive back in to resume auto-mounting.");
    case StopOutcome::NoKlog:
        return QStringLiteral("STOP sent — cleanup NOT confirmed (neither the kernel log nor debug.log could be read). Do "
                              "not unplug yet; wait, or check the kernel log.");
    case StopOutcome::KlogLost:
        return QStringLiteral("STOP sent — cleanup NOT confirmed (the kernel log connection dropped). Do not unplug yet.");
    case StopOutcome::Timeout:
        return QStringLiteral("STOP sent — cleanup NOT confirmed in time. Do not unplug yet. With debug=0 in config.ini "
                              "ShadowMount+ logs nothing to confirm with.");
    case StopOutcome::Incomplete:
        return QStringLiteral("STOP sent — ShadowMount+ could not release every mount. Do not unplug yet: close the game, "
                              "then try again.");
    }
    return {};
}

Result<StopResult> stopShadowMount(const ConsoleEndpoint& ep, const CancellationToken& token,
                                   const std::function<void(const QString&)>& status, int confirmTimeoutMs,
                                   int klogConnectTimeoutMs, int settleMs, int logPollMs)
{
    auto say = [&](const QString& s) {
        if (status)
            status(s);
    };
    // 1. klog first, so no cleanup line races past us.
    say(QStringLiteral("Connecting to klog (%1:%2)...").arg(ep.host).arg(ep.klogPort));
    std::unique_ptr<KlogSession> klog;
    if (auto k = KlogSession::open(ep.host, ep.klogPort, klogConnectTimeoutMs, token); k.ok())
        klog = std::move(k.value());
    else if (k.error().isCancelled())
        return Error::cancelled();

    // 2. The STOP sentinel, always. Without klog, remember where debug.log ends.
    say(QStringLiteral("Sending STOP signal to PS5..."));
    const QString logPath = mountDebugLogRemotePath(MountTool::ShadowMountPlus);
    FtpClient c(ftpOptionsFor(ep));
    FW_TRY(c.open(ep.host, ep.ftpPort, token));
    qint64 logOffset = -1;
    if (!klog)
        if (auto sz = c.size(logPath); sz.ok())
            logOffset = sz.value();
    (void)c.mkdirs(mountRemoteDir(MountTool::ShadowMountPlus)); // best effort
    FW_TRY(c.upload(mountRemoteDir(MountTool::ShadowMountPlus) + QStringLiteral("/STOP"), QByteArray(), token));
    StopResult res;
    StopConfirmTracker tracker(settleMs);
    QElapsedTimer clock;
    clock.start();
    auto finish = [&](StopOutcome o) {
        // Problem lines without the final cleanup line are still the better answer.
        res.outcome = tracker.incomplete() || (!tracker.confirmed() && !tracker.problems().isEmpty()) ? StopOutcome::Incomplete : o;
        res.problems = tracker.problems();
        res.unmountsStarted = tracker.unmountsStarted();
        res.unmountsCompleted = tracker.unmountsCompleted();
        return res;
    };
    if (!klog) {
        if (logOffset < 0) {
            c.quit();
            return finish(StopOutcome::NoKlog);
        }
        // 3b. Follow debug.log over FTP (ShadowMount+ polls STOP every 3 s).
        res.source = QStringLiteral("debug.log");
        say(QStringLiteral("Kernel log not reachable; following %1...").arg(logPath));
        QByteArray carry;
        while (true) {
            if (tracker.poll(clock.elapsed()))
                break;
            if (clock.elapsed() >= confirmTimeoutMs) {
                c.quit();
                return finish(StopOutcome::Timeout);
            }
            if (!token.sleepFor(logPollMs))
                return Error::cancelled();
            if (!c.isConnected() && !c.open(ep.host, ep.ftpPort, token).ok())
                continue;
            QByteArray chunk;
            QBuffer buf(&chunk);
            buf.open(QIODevice::WriteOnly);
            Status st = c.retrieve(logPath, buf, logOffset, {}, token);
            if (!st.ok() && st.error().isCancelled())
                return Error::cancelled();
            if (!st.ok() && st.error().code == ErrorCode::Unsupported && c.isConnected()) {
                // No REST: the whole file, then skip what was there before.
                chunk.clear();
                buf.seek(0);
                st = c.retrieve(logPath, buf, 0, {}, token);
                chunk = chunk.mid(logOffset);
            }
            if (!st.ok())
                continue; // rotated or busy: try again
            logOffset += chunk.size();
            carry += chunk;
            const qsizetype nl = carry.lastIndexOf('\n');
            if (nl < 0)
                continue;
            for (const QByteArray& l : carry.left(nl).split('\n')) {
                const QString line = QString::fromUtf8(l).trimmed();
                if (line.isEmpty())
                    continue;
                res.klogLines << line;
                if (tracker.feed(line, clock.elapsed()))
                    break;
            }
            carry = carry.mid(nl + 1);
            if (tracker.finished())
                break;
        }
        c.quit();
        return finish(StopOutcome::Confirmed);
    }
    c.quit();
    // 3. Watch klog for the confirmation. Lines are read in short slices so the
    //    settle period can complete while klog is quiet.
    res.source = QStringLiteral("klog");
    say(QStringLiteral("Waiting for ShadowMountPlus confirmation..."));
    while (true) {
        if (tracker.poll(clock.elapsed()))
            return finish(StopOutcome::Confirmed);
        const qint64 left = confirmTimeoutMs - clock.elapsed();
        if (left <= 0)
            return finish(StopOutcome::Timeout);
        const auto r = klog->read(int(std::min<qint64>(left, 200)), token);
        if (r.cancelled)
            return Error::cancelled();
        for (const QString& line : r.lines) {
            res.klogLines << line;
            if (tracker.feed(line, clock.elapsed()))
                return finish(StopOutcome::Confirmed);
        }
        if (r.closed)
            return finish(StopOutcome::KlogLost);
    }
}

} // namespace fw::ps5
