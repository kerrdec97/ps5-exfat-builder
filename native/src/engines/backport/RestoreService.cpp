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
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

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

QString findOriginalsZip(const QString& game, const QString& originalsDir)
{
    if (originalsDir.isEmpty() || !QFileInfo(originalsDir).isDir())
        return {};
    const GameMetadata meta = readGameMetadata(game);
    const QString tid = manifestTitleId(meta, game);
    auto m = readManifest(manifestPathFor(originalsDir, tid));
    if (m.ok() && m.value()) {
        const QString z = QDir::fromNativeSeparators(m.value()->originalsZipAbs());
        if (!z.isEmpty() && QFileInfo(z).isFile())
            return z;
    }
    QString id = meta.titleId;
    if (id.isEmpty()) {
        static const QRegularExpression re(QStringLiteral("((?:PPSA|CUSA|PPSB)\\d+)"), QRegularExpression::CaseInsensitiveOption);
        const auto mm = re.match(QFileInfo(game).fileName());
        if (mm.hasMatch())
            id = mm.captured(1).toUpper();
    }
    if (id.isEmpty())
        return {};
    QString best;
    QDateTime bestTime;
    const QFileInfoList zips = QDir(originalsDir).entryInfoList({QStringLiteral("*.zip")}, QDir::Files);
    for (const QFileInfo& fi : zips) {
        const QString n = fi.fileName();
        if (!n.contains(QStringLiteral("original files"), Qt::CaseInsensitive) || !n.contains(id, Qt::CaseInsensitive))
            continue;
        if (best.isEmpty() || fi.lastModified() > bestTime) {
            best = fi.absoluteFilePath();
            bestTime = fi.lastModified();
        }
    }
    return best;
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

} // namespace

Result<RestoreReport> restoreOriginals(const RestoreOptions& opt, const CancellationToken& token, ProgressReporter* progress)
{
    RestoreReport rep;
    if (opt.game.isEmpty() || !QFileInfo(opt.game).isDir())
        return makeError(Code::NotAGame, QStringLiteral("The game folder %1 does not exist.").arg(QDir::toNativeSeparators(opt.game)));
    const QString game = QDir::cleanPath(QFileInfo(opt.game).absoluteFilePath());
    rep.zip = opt.zip.isEmpty() ? findOriginalsZip(game, opt.originalsDir) : opt.zip;
    if (rep.zip.isEmpty() || !QFileInfo(rep.zip).isFile())
        return makeError(Code::RestoreFailed,
                         rep.zip.isEmpty() ? QStringLiteral("No originals ZIP was found for %1. Choose the ZIP to restore from.")
                                                 .arg(QDir::toNativeSeparators(game))
                                           : QStringLiteral("The originals ZIP %1 does not exist.").arg(QDir::toNativeSeparators(rep.zip)));
    if (isSameOrInside(rep.zip, game))
        return makeError(Code::RestoreFailed, QStringLiteral("The originals ZIP must not be inside the game folder."));

    // Manifest: next to the chosen originals folder, else next to the ZIP.
    const GameMetadata meta = readGameMetadata(game);
    const QString tid = manifestTitleId(meta, game);
    std::optional<Manifest> manifest;
    for (const QString& dir : {opt.originalsDir, QFileInfo(rep.zip).absolutePath()}) {
        if (dir.isEmpty())
            continue;
        const QString p = manifestPathFor(dir, tid);
        auto m = readManifest(p);
        if (!m.ok()) {
            rep.warnings << m.error().message;
            continue;
        }
        if (m.value()) {
            manifest = m.value();
            rep.manifestPath = p;
            break;
        }
    }

    zip::Reader reader;
    if (Status st = reader.open(rep.zip); !st.ok())
        return makeError(Code::RestoreFailed, QStringLiteral("The originals ZIP cannot be read: %1").arg(st.error().message),
                         st.error().detail);

    // Hash checks against the manifest (D6 / S36).
    QMap<QString, QString> recorded; // relKey → sha256
    if (manifest) {
        const auto files = manifest->originalsFiles();
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            recorded.insert(relKey(it.key()), it.value().toLower());
        const QString recordedZip = QDir::fromNativeSeparators(manifest->originalsZipAbs());
        const bool sameZip = !recordedZip.isEmpty() && isSameOrInside(rep.zip, recordedZip) && isSameOrInside(recordedZip, rep.zip);
        if (sameZip) {
            auto h = sha256File(rep.zip, token);
            if (!h.ok())
                return h.takeError();
            if (!manifest->originalsZipSha256().isEmpty() && h.value() != manifest->originalsZipSha256().toLower())
                return makeError(Code::ManifestMismatch,
                                 QStringLiteral("The originals ZIP %1 changed since it was recorded, so it was not used. "
                                                "Nothing was changed.")
                                     .arg(QDir::toNativeSeparators(rep.zip)));
        } else {
            rep.warnings << QStringLiteral("%1 is not the originals ZIP recorded for this game; its files are checked "
                                           "against the recorded hashes.")
                                .arg(QFileInfo(rep.zip).fileName());
        }
    } else {
        rep.warnings << QStringLiteral("No backport manifest was found for this game: the ZIP is restored without hash "
                                       "checks, and files the backport added (such as fakelib/) are not removed.");
    }

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

    // When another ZIP than the recorded one is used, its members must hold
    // the recorded originals (checked while writing, below).
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
        if (recorded.contains(relKey(rel)) && recorded.value(relKey(rel)) != got) {
            QFile::remove(tmp);
            rep.errors << QStringLiteral("%1 in the ZIP is not the recorded original; it was not restored.").arg(rel);
            continue;
        }
        if (hashOf(target) == got) {
            QFile::remove(tmp);
            ++rep.unchanged;
        } else if (Status rs = replaceFileAtomic(tmp, target); !rs.ok()) {
            QFile::remove(tmp);
            rep.errors << QStringLiteral("%1: %2").arg(rel, rs.error().message);
            continue;
        } else {
            ++rep.restored;
        }
        if (progress)
            progress->advanceFiles(1);
    }

    // Files the backport added (D6).
    if (manifest) {
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
            if (QFile::remove(target)) {
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
    }
    return rep;
}

} // namespace fw::backport
