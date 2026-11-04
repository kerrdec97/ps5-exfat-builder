#include "engines/backport/RestoreService.h"

#include "core/FileUtil.h"
#include "core/Zip.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/Common.h"
#include "engines/backport/GameMetadata.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <vector>

namespace fw::backport {

std::optional<QString> safeMemberTarget(const QString& root, const QString& entryName)
{
    const QString rel = zip::Reader::safeRelativePath(entryName);
    if (rel.isEmpty())
        return std::nullopt;
    const QString target = joinPath(QDir::cleanPath(root), rel);
    if (!isSameOrInside(target, root) || QDir::cleanPath(target) == QDir::cleanPath(root))
        return std::nullopt;
    return rel;
}

namespace {

QString hashOf(const QString& path)
{
    if (!QFileInfo(path).isFile())
        return {};
    auto h = sha256File(path, CancellationToken());
    return h.ok() ? h.value() : QString();
}

void removeEmptyParents(const QString& root, const QString& rel)
{
    QString dir = QFileInfo(rel).path();
    while (!dir.isEmpty() && dir != u'.') {
        if (!QDir().rmdir(joinPath(root, dir)))
            break;
        dir = QFileInfo(dir).path();
    }
}

QString recordedZip(const Manifest& m)
{
    QString z = m.originalsZipAbs();
    z.replace(u'\\', u'/');
    return QDir::fromNativeSeparators(z);
}

QString native(const QString& p)
{
    return QDir::toNativeSeparators(p);
}

} // namespace

QString findOriginalsZip(const QString& game, const QString& originalsDir)
{
    if (originalsDir.isEmpty() || !QFileInfo(originalsDir).isDir())
        return {};
    const QString abs = QDir::cleanPath(QFileInfo(game).absoluteFilePath());
    const ManifestSet records = findManifests(originalsDir, manifestTitleId(readGameMetadata(abs), abs), abs);
    if (!records.own)
        return {};
    const QString z = recordedZip(*records.own);
    return !z.isEmpty() && QFileInfo(z).isFile() ? z : QString();
}

Result<RestoreReport> restoreOriginals(const RestoreOptions& opt, const CancellationToken& token, ProgressReporter* progress)
{
    RestoreReport rep;
    if (opt.game.isEmpty() || !QFileInfo(opt.game).isDir())
        return makeError(Code::NotAGame, QStringLiteral("The game folder %1 does not exist.").arg(native(opt.game)));
    const QString game = QDir::cleanPath(QFileInfo(opt.game).absoluteFilePath());
    const QString tid = manifestTitleId(readGameMetadata(game), game);
    const bool chosenZip = !opt.zip.isEmpty();

    // Backport records of this title: in the originals folder, and next to a
    // chosen ZIP. This folder's own record, and the others.
    std::optional<Manifest> own;
    std::vector<Manifest> others;
    QStringList dirs;
    if (!opt.originalsDir.isEmpty())
        dirs << opt.originalsDir;
    if (chosenZip)
        dirs << QFileInfo(opt.zip).absolutePath();
    for (int i = 0; i < dirs.size(); ++i) {
        if (!QFileInfo(dirs[i]).isDir() || (i > 0 && samePath(dirs[i], dirs[0])))
            continue;
        ManifestSet set = findManifests(dirs[i], tid, game);
        for (const QString& d : std::as_const(set.damaged))
            rep.warnings << QStringLiteral("%1 It is ignored.").arg(d);
        if (!own && set.own)
            own = set.own;
        else if (set.own)
            others.push_back(*set.own);
        for (Manifest& m : set.others)
            others.push_back(std::move(m));
    }

    // Which ZIP, and which record goes with it.
    std::optional<Manifest> manifest;
    bool belongs = false;
    if (!chosenZip) {
        if (!own)
            return makeError(Code::RestoreFailed,
                             QStringLiteral("No originals ZIP is recorded for %1%2. Choose the ZIP to restore from.")
                                 .arg(native(game),
                                      opt.originalsDir.isEmpty() ? QString()
                                                                 : QStringLiteral(" in %1").arg(native(opt.originalsDir))));
        rep.zip = recordedZip(*own);
        if (rep.zip.isEmpty() || !QFileInfo(rep.zip).isFile())
            return makeError(Code::RestoreFailed,
                             QStringLiteral("The originals ZIP recorded for this folder (%1) does not exist. Choose the "
                                            "ZIP to restore from.")
                                 .arg(native(rep.zip)));
        manifest = own;
        belongs = true;
    } else {
        rep.zip = QDir::cleanPath(QFileInfo(opt.zip).absoluteFilePath());
        if (!QFileInfo(rep.zip).isFile())
            return makeError(Code::RestoreFailed, QStringLiteral("The originals ZIP %1 does not exist.").arg(native(rep.zip)));
        auto zipHash = sha256File(rep.zip, token);
        if (!zipHash.ok())
            return zipHash.takeError();
        const auto names = [&](const Manifest& m) {
            return samePath(recordedZip(m), rep.zip)
                || (!m.originalsZipSha256().isEmpty() && m.originalsZipSha256().toLower() == zipHash.value());
        };
        if (own && names(*own)) {
            manifest = own;
            belongs = true;
        } else {
            for (const Manifest& m : others)
                if (names(m)) {
                    manifest = m;
                    belongs = false;
                    rep.warnings << QStringLiteral("%1 holds the originals recorded for %2. Only files that hold what "
                                                   "that backport wrote are replaced; that folder's record is not "
                                                   "changed.")
                                        .arg(QFileInfo(rep.zip).fileName(),
                                             native(m.doc.value(QStringLiteral("game_folder")).toString()));
                    break;
                }
            if (!manifest && own) {
                manifest = own;
                belongs = true;
                rep.warnings << QStringLiteral("%1 is not the originals ZIP recorded for this folder; its files are "
                                               "checked against the recorded hashes.")
                                    .arg(QFileInfo(rep.zip).fileName());
            }
        }
    }
    if (isSameOrInside(rep.zip, game))
        return makeError(Code::RestoreFailed, QStringLiteral("The originals ZIP must not be inside the game folder."));
    rep.ownRecord = manifest && belongs;
    if (manifest)
        rep.manifestPath = manifest->path;

    zip::Reader reader;
    if (Status st = reader.open(rep.zip); !st.ok())
        return makeError(Code::RestoreFailed, QStringLiteral("The originals ZIP cannot be read: %1").arg(st.error().message),
                         st.error().detail);

    // Zip slip: refuse the whole restore before writing anything (S20).
    std::vector<std::pair<const zip::Entry*, QString>> work;
    for (const zip::Entry& e : reader.entries()) {
        if (e.isDirectory())
            continue;
        const auto rel = safeMemberTarget(game, e.name);
        if (!rel) {
            rep.refused << QStringLiteral("%1: refused, it would be written outside the game folder").arg(e.name);
            continue;
        }
        work.emplace_back(&e, *rel);
    }
    if (!rep.refused.isEmpty())
        return rep; // nothing written

    if (!manifest) {
        if (!opt.allowUnrecorded)
            return makeError(Code::ManifestMismatch,
                             QStringLiteral("No backport record names %1, so the files in %2 can't be checked before "
                                            "they are overwritten. Nothing was changed. Restore without a record only "
                                            "if you are sure this ZIP holds this folder's originals.")
                                 .arg(QFileInfo(rep.zip).fileName(), native(game)));
        rep.warnings << QStringLiteral("No backport record names this ZIP: it is restored without hash checks, and files "
                                       "the backport added (such as fakelib/) are not removed.");
    }

    // The ZIP the record names must be the recorded one, byte for byte.
    QMap<QString, QString> original;            // relKey → recorded original sha256
    QMap<QString, QSet<QString>> written;       // relKey → hashes the backport recorded
    if (manifest) {
        const auto files = manifest->originalsFiles();
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            original.insert(relKey(it.key()), it.value().toLower());
        written = manifest->patchedHashes();
        if (samePath(recordedZip(*manifest), rep.zip) && !manifest->originalsZipSha256().isEmpty()) {
            auto h = sha256File(rep.zip, token);
            if (!h.ok())
                return h.takeError();
            if (h.value() != manifest->originalsZipSha256().toLower())
                return makeError(Code::ManifestMismatch,
                                 QStringLiteral("The originals ZIP %1 changed since it was recorded, so it was not used. "
                                                "Nothing was changed.")
                                     .arg(native(rep.zip)));
        }
    }

    std::sort(work.begin(), work.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    QSet<QString> zipKeys;
    for (const auto& w : work)
        zipKeys.insert(relKey(w.second));
    for (const auto& [entry, rel] : work) {
        if (!token.checkpoint())
            return makeError(Code::Cancelled,
                             QStringLiteral("Cancelled after %1 of %2 files. Run the restore again to finish it.")
                                 .arg(rep.restored + rep.unchanged)
                                 .arg(work.size()));
        if (progress)
            progress->setCurrentItem(rel);
        const QString target = joinPath(game, rel);
        const QString key = relKey(rel);
        const QString current = hashOf(target);
        // Decide before writing anything for this file.
        if (manifest && current.isEmpty() && !belongs) {
            rep.skipped << QStringLiteral("%1 is missing in this folder and the record belongs to another folder; it "
                                          "was not created.")
                               .arg(rel);
            continue;
        }
        const QString tmp = target + QStringLiteral(".fwpart");
        QDir().mkpath(QFileInfo(target).absolutePath());
        QCryptographicHash h(QCryptographicHash::Sha256);
        Status st;
        {
            QFile out(toLongPath(tmp));
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                rep.errors << QStringLiteral("%1: %2").arg(rel, out.errorString());
                continue;
            }
            st = reader.read(
                *entry,
                [&](const QByteArray& chunk) -> Status {
                    h.addData(chunk);
                    if (out.write(chunk) != chunk.size())
                        return Error(ErrorCode::IoError, out.errorString());
                    return success();
                },
                token);
            if (st.ok())
                st = flushToDisk(out, QFileInfo(target).fileName()); // on the drive before it replaces anything
            out.close();
        }
        if (!st.ok()) {
            QFile::remove(tmp);
            if (st.error().isCancelled())
                return makeError(Code::Cancelled, QStringLiteral("Cancelled during %1. Run the restore again to finish it.").arg(rel));
            rep.errors << QStringLiteral("%1: %2").arg(rel, st.error().message);
            continue;
        }
        const QString got = QString::fromLatin1(h.result().toHex());
        if (original.contains(key) && original.value(key) != got) {
            QFile::remove(tmp);
            rep.errors << QStringLiteral("%1 in the ZIP is not the recorded original; it was not restored.").arg(rel);
            continue;
        }
        if (current == got) {
            QFile::remove(tmp);
            ++rep.unchanged;
            if (progress)
                progress->advanceFiles(1);
            continue;
        }
        // Only replace what the backport wrote (or recreate a missing file
        // from this folder's own record).
        const bool allowed = !manifest || (current.isEmpty() ? belongs : written.value(key).contains(current));
        if (!allowed) {
            QFile::remove(tmp);
            rep.skipped << QStringLiteral("%1 holds content this backport did not record (neither the recorded original "
                                          "nor the backported file); it was left as it is.")
                               .arg(rel);
            continue;
        }
        if (Status rs = replaceFileAtomic(tmp, target); !rs.ok()) {
            QFile::remove(tmp);
            rep.errors << QStringLiteral("%1: %2").arg(rel, rs.error().message);
            continue;
        }
        ++rep.restored;
        if (progress)
            progress->advanceFiles(1);
    }

    // Files the backport added (D6), only with this folder's own record.
    if (manifest && belongs) {
        QMap<QString, QString> added; // rel → sha256 written by the backport
        if (manifest->schemaVersion() >= 2 && !manifest->addedFiles().isEmpty()) {
            added = manifest->addedFileHashes();
        } else {
            const auto post = manifest->postHashes();
            for (auto it = post.cbegin(); it != post.cend(); ++it) {
                const QString k = relKey(it.key());
                if ((k.startsWith(QStringLiteral("fakelib/")) || k.contains(QStringLiteral("/fakelib/"))) && !zipKeys.contains(k))
                    added.insert(it.key(), it.value().toLower());
            }
        }
        for (auto it = added.cbegin(); it != added.cend(); ++it) {
            if (zipKeys.contains(relKey(it.key())))
                continue; // it was an original after all
            const auto rel = safeMemberTarget(game, it.key());
            if (!rel)
                continue;
            const QString target = joinPath(game, *rel);
            if (!QFileInfo(target).isFile())
                continue;
            if (!it.value().isEmpty() && hashOf(target) != it.value()) {
                rep.keptAdded << *rel;
                continue;
            }
            if (removeFileForce(target)) {
                rep.removedAdded << *rel;
                removeEmptyParents(game, *rel);
            } else {
                rep.errors << QStringLiteral("%1 (added by the backport) could not be removed.").arg(*rel);
            }
        }
        if (!rep.keptAdded.isEmpty())
            rep.warnings << QStringLiteral("Kept files the backport added because they changed since: %1.")
                                .arg(rep.keptAdded.join(QStringLiteral(", ")));
        if (rep.ok()) {
            recordRestore(*manifest, rep.zip);
            if (writeManifest(*manifest).ok())
                rep.manifestUpdated = true;
            else
                rep.warnings << QStringLiteral("The backport manifest could not be updated.");
        }
    } else if (manifest) {
        rep.warnings << QStringLiteral("Files a backport added to this folder (such as fakelib/) were not removed: the "
                                       "record used belongs to another folder.");
    }
    return rep;
}

} // namespace fw::backport
