#pragma once

// ShadowMount+ fakelib manager (legacy ui/tab_shadowmount.py "Fakelib Manager",
// _fakelib_*): the global library-overlay folder on the console.
//
//   /data/shadowmount/fakelib            enabled overlays (.prx / .sprx)
//   /data/shadowmount/fakelib.disabled   disabled overlays (moved there by rename)
//
// The disabled folder used to be /data/shadowmount/fakelib/disabled. ShadowMount+
// 1.7 copies the global folder recursively into every title's cache, so that
// subfolder showed up as common/lib/disabled/ in every game (research §1.2-10);
// overlays still there are listed (legacyFolder) and moveLegacyDisabled()
// moves them out. libkernel.sprx is refused: ShadowMount+ deletes it from
// writable fakelib folders. Do not run BackPork next to the ShadowMount+
// fakelib (1.7 stops it at startup).
//
// Rules kept from the legacy app:
//  * only .prx/.sprx files are listed; folders and the temporary names of a
//    safe replace (".uploading", ".replacing.bak") are skipped;
//  * a local file is refused when it is a folder, eboot.bin, inside an sce_sys
//    folder, an image (.exfat .ffpkg .ffpfs .ffpfsc) or not .prx/.sprx;
//  * the user confirms when its first 4 bytes are not an ELF (7F 45 4C 46) or
//    SELF (4F 15 3D 1D / 54 14 F5 EE) header, or the file cannot be read;
//  * uploads use the safe swap (FtpClient::uploadReplace): the old overlay is
//    only replaced once the new one is completely on the console;
//  * enable/disable = RNFR/RNTO between the two folders.
// New: names from the console go through remoteNameProblem(); enabling or
// disabling never overwrites an overlay of the same name in the other folder.

#include "connect/ps5/FtpClient.h"
#include "core/Cancellation.h"
#include "core/Result.h"

#include <QDateTime>
#include <QString>

#include <vector>

namespace fw::ps5::fakelib {

inline const QString kFolder = QStringLiteral("/data/shadowmount/fakelib");
inline const QString kDisabledFolder = QStringLiteral("/data/shadowmount/fakelib.disabled");
inline const QString kLegacyDisabledFolder = QStringLiteral("/data/shadowmount/fakelib/disabled");
inline constexpr qint64 kMaxOverlayBytes = 256LL * 1024 * 1024; // uploads are held in memory

struct Overlay {
    QString name;
    bool enabled = true;
    qint64 size = -1; // -1 = unknown
    QDateTime modified;
    bool legacyFolder = false; // disabled, still in the old fakelib/disabled subfolder
    QString folder() const { return enabled ? kFolder : legacyFolder ? kLegacyDisabledFolder : kDisabledFolder; }
    QString path() const;
};

// True for a name the manager lists (.prx/.sprx, not a temporary of a safe replace).
bool isListedName(const QString& name);

struct Listing {
    std::vector<Overlay> overlays; // sorted by name (case-insensitive), enabled first on a tie
    QStringList skipped;           // names from the console that cannot be used safely
    bool folderExists = true;      // false: /data/shadowmount/fakelib is not there yet
    int enabledCount() const;
    int legacyCount() const; // overlays still in fakelib/disabled
};

// Lists both folders. A missing folder (fresh console) is an empty list.
Result<Listing> list(FtpClient& client, const CancellationToken& token = {});

enum class Verdict { Ok, Confirm, Reject };
struct LocalCheck {
    Verdict verdict = Verdict::Ok;
    QString message; // Confirm: the question for the user; Reject: why
};
// The legacy _fakelib_validate().
LocalCheck checkLocalFile(const QString& localPath);

// Uploads a local .prx/.sprx into /data/shadowmount/fakelib under its own name
// (creates the folder). `confirmed` must be true for a Verdict::Confirm file.
Status add(FtpClient& client, const QString& localPath, bool confirmed, const CancellationToken& token = {});
// Replaces `target` (in its current folder, under its current name).
Status replace(FtpClient& client, const Overlay& target, const QString& localPath, bool confirmed,
               const CancellationToken& token = {});
Status remove(FtpClient& client, const Overlay& target);
// Moves `target` to the other folder (creates fakelib.disabled when needed).
Status setEnabled(FtpClient& client, const Overlay& target, bool enabled);
// Moves every overlay of the old fakelib/disabled subfolder to
// fakelib.disabled (an existing one of the same name is kept, that one is
// reported) and removes the old folder when it is empty. Returns how many moved.
Result<int> moveLegacyDisabled(FtpClient& client, const CancellationToken& token = {});

} // namespace fw::ps5::fakelib
