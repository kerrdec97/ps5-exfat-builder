#include "engines/backport/AudioScan.h"

#include "engines/backport/Common.h"
#include "engines/backport/ElfImage.h"
#include "engines/backport/ExecutableScanner.h"
#include "engines/backport/SelfReader.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cstring>

namespace fw::backport {

namespace {

constexpr quint8 kUnpatched = 0x0C; // 12 channels
constexpr quint8 kPatched = 0x08;   // 8 channels
constexpr int kClusterWindow = 24;  // fragments within this many bytes of fragment A
constexpr int kTwinWindow = 48;
constexpr int kLeadWindow = 16;
constexpr int kLeadMin = 3;
constexpr int kTwinsListed = 20;    // per file, in the report

QByteArray bytes(const char* hex, int value = -1, const char* tail = nullptr)
{
    QByteArray b = QByteArray::fromHex(hex);
    if (value >= 0)
        b.append(char(value));
    if (tail)
        b.append(QByteArray::fromHex(tail));
    return b;
}

std::vector<qint64> findAll(const QByteArray& data, const QByteArray& needle)
{
    std::vector<qint64> out;
    qsizetype i = 0;
    while (true) {
        const qsizetype j = data.indexOf(needle, i);
        if (j < 0)
            break;
        out.push_back(j);
        i = j + 1;
    }
    return out;
}

QString hexOff(qint64 v)
{
    return QStringLiteral("0x%1").arg(v, 0, 16).toUpper().replace(QStringLiteral("0X"), QStringLiteral("0x"));
}

std::vector<AudioCluster> findClusters(const QByteArray& data, quint8 v)
{
    const QByteArray fa = bytes("c7459400", v);
    const QByteArray fb = bytes("c703", v, "000000");
    const QByteArray fc = bytes("6a", v, "58");
    std::vector<AudioCluster> out;
    for (qint64 a : findAll(data, fa)) {
        const QByteArray window = data.mid(a, kClusterWindow + fb.size());
        const qsizetype b = window.indexOf(fb);
        const qsizetype c = window.indexOf(fc);
        if (b < 0 || c < 0)
            continue;
        AudioCluster cl;
        cl.anchor = a;
        cl.siteA = a + 4;
        cl.siteB = a + b + 2;
        cl.siteC = a + c + 1;
        cl.channels = v == kUnpatched ? 12 : 8;
        out.push_back(cl);
    }
    return out;
}

struct Carrier {
    const char* prefix;
    const char* suffix;
    const char* encoding;
};

std::vector<AudioTwinBranch> findTwins(const QByteArray& data)
{
    static const Carrier carriers[] = {{"6a", "58", "DIRECT_IMM8"},
                                       {"b800", "0000", "IMM_SHIFTED_8"},
                                       {"c703", "000000", "DIRECT_IMM8_MEM"}};
    std::vector<AudioTwinBranch> out;
    for (const Carrier& c : carriers) {
        const QByteArray prefix = QByteArray::fromHex(c.prefix);
        const std::vector<qint64> twelves = findAll(data, bytes(c.prefix, kUnpatched, c.suffix));
        const std::vector<qint64> eights = findAll(data, bytes(c.prefix, kPatched, c.suffix));
        for (qint64 t : twelves) {
            if (int(out.size()) >= kAudioTwinCap)
                return out;
            auto it = std::lower_bound(eights.begin(), eights.end(), t - kTwinWindow);
            for (; it != eights.end() && *it <= t + kTwinWindow; ++it) {
                const qint64 gap = *it > t ? *it - t : t - *it;
                if (gap == 0)
                    continue;
                AudioTwinBranch tb;
                tb.encoding = QString::fromLatin1(c.encoding);
                tb.twelveOffset = t;
                tb.eightOffset = *it;
                tb.twelveValueOffset = t + prefix.size();
                tb.gap = int(gap);
                const qint64 lo = std::max<qint64>(0, std::min(t, *it) - 2);
                const qint64 hi = std::min<qint64>(data.size(), std::max(t, *it) + 8);
                tb.region = data.mid(lo, hi - lo);
                out.push_back(tb);
                break; // one twin per 12-carrier
            }
        }
    }
    return out;
}

std::vector<AudioLead> findLeads(const QByteArray& data)
{
    std::vector<AudioLead> out;
    const qint64 n = data.size();
    const char* d = data.constData();
    const auto next = [&](qint64 from) -> qint64 {
        if (from >= n)
            return -1;
        const void* m = std::memchr(d + from, kUnpatched, size_t(n - from));
        return m ? static_cast<const char*>(m) - d : -1;
    };
    qint64 p = next(0);
    while (p >= 0 && int(out.size()) < kAudioLeadCap) {
        int count = 0;
        qint64 last = p;
        for (qint64 q = p; q < std::min(n, p + kLeadWindow); ++q)
            if (quint8(d[q]) == kUnpatched) {
                ++count;
                last = q;
            }
        if (count >= kLeadMin) {
            out.push_back({p, last + 1, count});
            p = next(p + kLeadWindow);
        } else {
            p = next(p + 1);
        }
    }
    return out;
}

struct ProfileSiteDef {
    qint64 offset;
    const char* broken;
    const char* patched;
    const char* encoding;
    const char* structure;
    const char* role;
};

struct ProfileDef {
    const char* id;
    const char* title;
    const char* titleId;
    const char* appVersion;
    const char* fix;
    std::vector<ProfileSiteDef> sites;
    std::vector<qint64> doNotTouch;
};

// Console-confirmed profiles of the legacy app (ui/audio_profiles.py): a
// broken/working eboot diff plus a console result for ONE title and build.
const std::vector<ProfileDef>& profiles()
{
    static const std::vector<ProfileDef> p = {
        {"DBSparklingZero_PPSA15210_02.000.013",
         "DBSparklingZero",
         "PPSA15210",
         "02.000.013",
         "channel 12->8, twin-branch (push/mov cluster)",
         {{0x5292796, "c74594000c0000c7030c0000006a0c58", "c7459400080000c703080000006a0858", "DIRECT_IMM8",
           "TWIN_BRANCH", "twin-branch 12-cluster (mov/mov/push) -> 8"}},
         {0x5292784}},
        {"SpiderMan2_PPSA03016_01.004.003",
         "SpiderMan2",
         "PPSA03016",
         "01.004.003",
         "channel 12->8: twin-branch (mov eax,0xC00) + standalone imm32 low-byte",
         {{0x942493, "b8000c0000", "b800080000", "IMM_SHIFTED_8", "TWIN_BRANCH", "twin-branch mov eax,0xC00 (12<<8) -> 0x800"},
          {0x9422B5, "0cf1632d", "08f1632d", "IMM32_LOW_BYTE", "STANDALONE_IMM", "standalone imm32 low-byte 0x..0C -> 0x..08"}},
         {0x94248C}},
    };
    return p;
}

std::vector<AudioProfileMatch> matchProfiles(const QByteArray& data)
{
    std::vector<AudioProfileMatch> out;
    for (const ProfileDef& p : profiles()) {
        AudioProfileMatch m;
        m.id = QString::fromLatin1(p.id);
        m.title = QString::fromLatin1(p.title);
        m.titleId = QString::fromLatin1(p.titleId);
        m.appVersion = QString::fromLatin1(p.appVersion);
        m.fixSummary = QString::fromLatin1(p.fix);
        m.allPresent = true;
        int present = 0;
        for (const ProfileSiteDef& s : p.sites) {
            const QByteArray broken = QByteArray::fromHex(s.broken);
            const QByteArray patched = QByteArray::fromHex(s.patched);
            const QByteArray chunk = s.offset + broken.size() <= data.size() ? data.mid(s.offset, broken.size()) : QByteArray();
            AudioProfileSite site;
            site.offset = s.offset;
            site.encoding = QString::fromLatin1(s.encoding);
            site.structure = QString::fromLatin1(s.structure);
            site.role = QString::fromLatin1(s.role);
            if (!chunk.isEmpty() && chunk == broken) {
                site.state = QStringLiteral("unpatched");
                m.anyUnpatched = true;
                ++present;
            } else if (!chunk.isEmpty() && chunk == patched) {
                site.state = QStringLiteral("patched");
                ++present;
            } else {
                site.state = QStringLiteral("absent");
                m.allPresent = false;
            }
            m.sites.push_back(site);
        }
        for (qint64 o : p.doNotTouch)
            m.doNotTouch << hexOff(o);
        if (present > 0)
            out.push_back(m);
    }
    return out;
}

QString formatOf(const QByteArray& d)
{
    if (elf::hasElfMagic(d))
        return QStringLiteral("ELF");
    if (selfio::hasSelfMagic(d))
        return QStringLiteral("SELF");
    return QStringLiteral("other");
}

} // namespace

QString audioStatusName(AudioClusterStatus s)
{
    switch (s) {
    case AudioClusterStatus::Missing: return QStringLiteral("missing");
    case AudioClusterStatus::PresentUnpatched: return QStringLiteral("present, unpatched (12 channels)");
    case AudioClusterStatus::AlreadyPatched: return QStringLiteral("already patched (8 channels)");
    case AudioClusterStatus::Mixed: return QStringLiteral("mixed");
    }
    return QStringLiteral("missing");
}

QString audioTierName(AudioTier t)
{
    switch (t) {
    case AudioTier::NoMatch: return QStringLiteral("no match");
    case AudioTier::WeakLead: return QStringLiteral("weak lead");
    case AudioTier::TwinBranch: return QStringLiteral("twin-branch match");
    case AudioTier::ConfirmedProfile: return QStringLiteral("confirmed profile");
    }
    return QStringLiteral("no match");
}

AudioFileReport scanAudioBytes(const QByteArray& data, const QString& rel)
{
    AudioFileReport r;
    r.rel = rel;
    r.size = data.size();
    r.readable = true;
    r.format = formatOf(data);
    r.profiles = matchProfiles(data);
    r.unpatched = findClusters(data, kUnpatched);
    r.patched = findClusters(data, kPatched);
    if (!r.unpatched.empty() && r.patched.empty())
        r.status = AudioClusterStatus::PresentUnpatched;
    else if (r.unpatched.empty() && !r.patched.empty())
        r.status = AudioClusterStatus::AlreadyPatched;
    else if (!r.unpatched.empty())
        r.status = AudioClusterStatus::Mixed;
    else
        r.status = AudioClusterStatus::Missing;
    r.twins = findTwins(data);
    for (const char* name : {"sceAudioOut2PortCreate", "sceAudioOut2", "sceAudioOutOpen"})
        if (data.contains(name))
            r.apiStrings << QString::fromLatin1(name);
    r.leads = findLeads(data);
    if (!r.profiles.empty())
        r.tier = AudioTier::ConfirmedProfile;
    else if (!r.twins.empty())
        r.tier = AudioTier::TwinBranch;
    else if (!r.leads.empty())
        r.tier = AudioTier::WeakLead;
    else
        r.tier = AudioTier::NoMatch;
    return r;
}

Result<AudioReport> scanGameAudio(const QString& game, bool allExecutables, const CancellationToken& token)
{
    if (game.isEmpty() || !QFileInfo(game).isDir())
        return makeError(Code::NotAGame, QStringLiteral("The game folder %1 does not exist.").arg(QDir::toNativeSeparators(game)));
    AudioReport rep;
    rep.game = QDir::cleanPath(QFileInfo(game).absoluteFilePath());
    QStringList targets;
    if (allExecutables) {
        FW_TRY_ASSIGN(const ScanResult scan, scanGame(rep.game, token, ScanMode::Quick));
        for (const ExecFile& e : scan.executables)
            if (e.area == Area::Game)
                targets << e.rel;
    } else {
        if (!QFileInfo(joinPath(rep.game, QStringLiteral("eboot.bin"))).isFile())
            return makeError(Code::NotAGame, QStringLiteral("eboot.bin is not in %1. Pick the game's root folder.")
                                                 .arg(QDir::toNativeSeparators(rep.game)));
        targets << QStringLiteral("eboot.bin");
    }
    for (const QString& rel : std::as_const(targets)) {
        if (!token.checkpoint())
            return Error::cancelled();
        auto data = readFileBytes(joinPath(rep.game, rel));
        if (!data.ok()) {
            AudioFileReport f;
            f.rel = rel;
            f.size = QFileInfo(joinPath(rep.game, rel)).size();
            f.skipReason = data.error().message;
            rep.files.push_back(f);
            continue;
        }
        rep.files.push_back(scanAudioBytes(data.value(), rel));
    }
    bool profile = false;
    for (const AudioFileReport& f : rep.files) {
        rep.unpatchedClusters += int(f.unpatched.size());
        rep.patchedClusters += int(f.patched.size());
        rep.twinBranches += int(f.twins.size());
        rep.looseLeads += int(f.leads.size());
        profile = profile || !f.profiles.empty();
    }
    rep.tier = profile                ? AudioTier::ConfirmedProfile
        : rep.twinBranches > 0        ? AudioTier::TwinBranch
        : rep.looseLeads > 0          ? AudioTier::WeakLead
                                      : AudioTier::NoMatch;
    rep.saveDataModulePresent = QFileInfo(joinPath(rep.game, QStringLiteral("fakelib/libSceSaveData.native.sprx"))).isFile();
    return rep;
}

QJsonObject audioReportJson(const AudioReport& r)
{
    QJsonObject out;
    QJsonObject summary;
    summary.insert(QStringLiteral("tier"), audioTierName(r.tier));
    QString verdict;
    if (r.unpatchedClusters > 0)
        verdict = QStringLiteral("The confirmed 12-channel cluster is present and unpatched. Reported only: this tool "
                                 "does not patch audio, and a 12 -> 8 test needs console evidence for this title and build.");
    else if (r.patchedClusters > 0)
        verdict = QStringLiteral("The confirmed cluster is already the 8-channel version.");
    else
        verdict = QStringLiteral("The confirmed cluster is not present. Twin branches and loose leads are investigation "
                                 "leads only, not patch targets.");
    summary.insert(QStringLiteral("verdict"), verdict);
    summary.insert(QStringLiteral("unpatchedClusters"), r.unpatchedClusters);
    summary.insert(QStringLiteral("patchedClusters"), r.patchedClusters);
    summary.insert(QStringLiteral("twinBranches"), r.twinBranches);
    summary.insert(QStringLiteral("looseLeads"), r.looseLeads);
    QStringList confirmed;
    for (const AudioFileReport& f : r.files)
        for (const AudioProfileMatch& m : f.profiles)
            confirmed << QStringLiteral("%1 (%2)").arg(m.id, m.anyUnpatched ? QStringLiteral("unpatched") : QStringLiteral("patched"));
    summary.insert(QStringLiteral("confirmedProfiles"), QJsonArray::fromStringList(confirmed));
    summary.insert(QStringLiteral("saveDataModulePresent"), r.saveDataModulePresent);
    summary.insert(QStringLiteral("patch"), QStringLiteral("Not offered: this report is read-only."));
    out.insert(QStringLiteral("summary"), summary);

    QJsonArray files, clusters, twins, profs;
    for (const AudioFileReport& f : r.files) {
        QJsonObject o;
        o.insert(QStringLiteral("file"), f.rel);
        if (!f.readable) {
            o.insert(QStringLiteral("result"), QStringLiteral("skipped"));
            o.insert(QStringLiteral("why"), f.skipReason);
            files.append(o);
            continue;
        }
        o.insert(QStringLiteral("format"), f.format);
        o.insert(QStringLiteral("size"), double(f.size));
        o.insert(QStringLiteral("cluster"), audioStatusName(f.status));
        o.insert(QStringLiteral("tier"), audioTierName(f.tier));
        o.insert(QStringLiteral("twinBranches"), int(f.twins.size()) >= kAudioTwinCap
                                                     ? QStringLiteral("%1+").arg(kAudioTwinCap)
                                                     : QString::number(f.twins.size()));
        o.insert(QStringLiteral("looseLeads"), int(f.leads.size()) >= kAudioLeadCap ? QStringLiteral("%1+").arg(kAudioLeadCap)
                                                                                    : QString::number(f.leads.size()));
        o.insert(QStringLiteral("audioApi"), f.apiStrings.isEmpty() ? QStringLiteral("none") : f.apiStrings.join(QStringLiteral(", ")));
        files.append(o);
        const auto addClusters = [&](const std::vector<AudioCluster>& list, const QString& state) {
            for (const AudioCluster& c : list) {
                QJsonObject co;
                co.insert(QStringLiteral("file"), f.rel);
                co.insert(QStringLiteral("state"), state);
                co.insert(QStringLiteral("channels"), c.channels);
                co.insert(QStringLiteral("anchor"), hexOff(c.anchor));
                co.insert(QStringLiteral("valueOffsets"),
                          QStringLiteral("%1, %2, %3").arg(hexOff(c.siteA), hexOff(c.siteB), hexOff(c.siteC)));
                clusters.append(co);
            }
        };
        addClusters(f.unpatched, QStringLiteral("unpatched"));
        addClusters(f.patched, QStringLiteral("patched"));
        for (int i = 0; i < int(f.twins.size()) && i < kTwinsListed; ++i) {
            const AudioTwinBranch& t = f.twins[size_t(i)];
            QJsonObject to;
            to.insert(QStringLiteral("file"), f.rel);
            to.insert(QStringLiteral("encoding"), t.encoding);
            to.insert(QStringLiteral("twelveOffset"), hexOff(t.twelveOffset));
            to.insert(QStringLiteral("eightOffset"), hexOff(t.eightOffset));
            to.insert(QStringLiteral("gap"), t.gap);
            to.insert(QStringLiteral("regionHex"), QString::fromLatin1(t.region.toHex(' ')));
            twins.append(to);
        }
        for (const AudioProfileMatch& m : f.profiles) {
            QJsonObject po;
            po.insert(QStringLiteral("file"), f.rel);
            po.insert(QStringLiteral("profile"), m.id);
            po.insert(QStringLiteral("titleId"), m.titleId);
            po.insert(QStringLiteral("appVersion"), m.appVersion);
            po.insert(QStringLiteral("fix"), m.fixSummary);
            QStringList sites;
            for (const AudioProfileSite& s : m.sites)
                sites << QStringLiteral("%1 %2 (%3)").arg(hexOff(s.offset), s.state, s.role);
            po.insert(QStringLiteral("sites"), sites.join(QStringLiteral("; ")));
            po.insert(QStringLiteral("allSitesPresent"), m.allPresent);
            po.insert(QStringLiteral("doNotTouch"), m.doNotTouch.join(QStringLiteral(", ")));
            profs.append(po);
        }
    }
    out.insert(QStringLiteral("files"), files);
    out.insert(QStringLiteral("clusters"), clusters);
    out.insert(QStringLiteral("twinBranches"), twins);
    out.insert(QStringLiteral("profiles"), profs);
    out.insert(QStringLiteral("notes"),
               QJsonArray{QStringLiteral("Read-only: nothing was written to the game and no patch is offered."),
                          QStringLiteral("Offsets are raw file offsets of this build; they are not universal."),
                          QStringLiteral("Twin branches, audio API names and loose leads are investigation aids, not "
                                         "proof and not patch targets."),
                          QStringLiteral("The save-data module note is informational: it is present in a working "
                                         "release but not proven to be the audio fix.")});
    return out;
}

} // namespace fw::backport
