#pragma once

// Read-only audio report (docs/BACKPORT_PARITY.md matrix #40, §2.18). A port
// of this project's own legacy scanner (ui/audio_channel_scan.py and the
// confirmed profiles of ui/audio_profiles.py, owner's code). It never writes,
// never patches and never proposes a patch: detection is not a fix, and the
// patch profiles need console evidence first.
//
// What it looks for in the raw bytes of eboot.bin (or every game executable):
//  * the channel-config cluster from a console-confirmed working/broken pair:
//    "c7 45 94 00 <v>", "c7 03 <v> 00 00 00" and "6a <v> 58" within a small
//    window, v = 0x0C (12 channels, unpatched) or 0x08 (8, patched); a lone
//    fragment or a lone 0x0C never counts;
//  * twin branches: an 8-channel and a 12-channel carrier of the same
//    encoding within 48 bytes (report only);
//  * audio API names (sceAudioOut2PortCreate, sceAudioOut2, sceAudioOutOpen):
//    corroboration, not proof;
//  * loose leads: 3 or more 0x0C bytes within 16 bytes (investigation only);
//  * the two console-confirmed profiles (DBSparklingZero PPSA15210
//    02.000.013, SpiderMan2 PPSA03016 01.004.003): their exact bytes at their
//    recorded file offsets, unpatched or patched.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::backport {

struct AudioCluster {
    qint64 anchor = 0;               // offset of "c7 45 94 00"
    qint64 siteA = 0, siteB = 0, siteC = 0; // offsets of the channel bytes
    int channels = 12;
};

struct AudioTwinBranch {
    QString encoding;                // DIRECT_IMM8, IMM_SHIFTED_8, DIRECT_IMM8_MEM
    qint64 twelveOffset = 0, eightOffset = 0, twelveValueOffset = 0;
    int gap = 0;
    QByteArray region;               // a few bytes around both carriers
};

struct AudioLead {
    qint64 start = 0, end = 0;
    int count = 0;
};

struct AudioProfileSite {
    qint64 offset = 0;
    QString state;                   // unpatched, patched, absent
    QString encoding, structure, role;
};

struct AudioProfileMatch {
    QString id, title, titleId, appVersion, fixSummary;
    std::vector<AudioProfileSite> sites;
    bool allPresent = false;
    bool anyUnpatched = false;
    QStringList doNotTouch;
};

enum class AudioClusterStatus { Missing, PresentUnpatched, AlreadyPatched, Mixed };
enum class AudioTier { NoMatch, WeakLead, TwinBranch, ConfirmedProfile };
QString audioStatusName(AudioClusterStatus s);
QString audioTierName(AudioTier t);

inline constexpr int kAudioTwinCap = 100;
inline constexpr int kAudioLeadCap = 200;

struct AudioFileReport {
    QString rel;
    qint64 size = 0;
    QString format;                  // ELF, SELF, other
    bool readable = false;
    QString skipReason;
    std::vector<AudioCluster> unpatched, patched;
    AudioClusterStatus status = AudioClusterStatus::Missing;
    std::vector<AudioTwinBranch> twins; // at most kAudioTwinCap
    QStringList apiStrings;
    std::vector<AudioLead> leads;    // at most kAudioLeadCap
    std::vector<AudioProfileMatch> profiles;
    AudioTier tier = AudioTier::NoMatch;
};

struct AudioReport {
    QString game;
    std::vector<AudioFileReport> files;
    AudioTier tier = AudioTier::NoMatch;
    int unpatchedClusters = 0, patchedClusters = 0, twinBranches = 0, looseLeads = 0;
    bool saveDataModulePresent = false; // fakelib/libSceSaveData.native.sprx (informational)
};

// Scans one file's bytes (no I/O).
AudioFileReport scanAudioBytes(const QByteArray& data, const QString& rel);

// eboot.bin, or every game executable when `allExecutables`. Read-only.
Result<AudioReport> scanGameAudio(const QString& game, bool allExecutables, const CancellationToken& token);

// Report sections for the run report (summary, files, clusters, twinBranches,
// profiles, notes).
QJsonObject audioReportJson(const AudioReport& report);

} // namespace fw::backport
