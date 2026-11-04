#include "engines/backport/BackupArchive.h"

#include "core/FileUtil.h"
#include "core/Zip.h"
#include "engines/backport/Common.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace fw::backport {

QString originalsZipName(const QString& prefix)
{
    return prefix + QStringLiteral(" original files.zip");
}

QString backportZipName(const QString& prefix, int sdk)
{
    return QStringLiteral("FW %1xx %2 backport files.zip").arg(sdk).arg(prefix);
}

QString uniqueZipPath(const QString& path)
{
    const auto taken = [](const QString& p) {
        return QFileInfo::exists(p) || QFileInfo::exists(p + QStringLiteral(".part"))
            || QFileInfo::exists(p + QStringLiteral(".part.part"));
    };
    if (!taken(path))
        return path;
    const QFileInfo fi(path);
    const QString dir = fi.absolutePath();
    const QString suffix = fi.suffix().isEmpty() ? QString() : u'.' + fi.suffix();
    const QString stem = suffix.isEmpty() ? fi.fileName() : fi.fileName().chopped(suffix.size());
    for (int n = 2;; ++n) {
        const QString candidate = QDir(dir).filePath(QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(suffix));
        if (!taken(candidate))
            return candidate;
    }
}

QString zipDestinationProblem(const QString& dest, const QString& game, const QString& zipName)
{
    if (dest.trimmed().isEmpty())
        return QStringLiteral("No folder was chosen.");
    const QFileInfo di(dest);
    if (!di.isDir())
        return QStringLiteral("The folder does not exist: %1").arg(QDir::toNativeSeparators(dest));
    if (!game.isEmpty() && isSameOrInside(dest, game))
        return QStringLiteral("The folder is inside the game folder. The ZIP would be packed into the image and "
                              "mixed with the game files; choose a folder outside the game.");
    const QString full = QDir::toNativeSeparators(QDir(di.absoluteFilePath()).filePath(zipName));
    const int len = int(full.size()) + int(qstrlen(kWorstNameSuffix));
    if (len > kMaxWinPath)
        return QStringLiteral("The ZIP path would be about %1 characters; Windows only guarantees %2. Choose a "
                              "shorter folder.")
            .arg(len)
            .arg(kMaxWinPath);
    const QString probe = QDir(di.absoluteFilePath()).filePath(QStringLiteral(".exfat_builder_write_test.tmp"));
    if (QFileInfo::exists(probe))
        return QStringLiteral("The folder already holds %1; remove it and try again.")
            .arg(QStringLiteral(".exfat_builder_write_test.tmp"));
    QFile f(probe);
    if (!f.open(QIODevice::WriteOnly) || f.write("0", 1) != 1) {
        const QString why = f.errorString();
        f.close();
        QFile::remove(probe);
        return QStringLiteral("The folder cannot be written: %1").arg(why);
    }
    f.close();
    QFile::remove(probe);
    return {};
}

std::vector<ZipMember> normalizeMembers(std::vector<ZipMember> members)
{
    std::vector<ZipMember> out;
    QSet<QString> seen;
    for (ZipMember& m : members) {
        m.arcName = normalizeRel(m.arcName);
        const QString key = m.arcName.toLower();
        if (m.arcName.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const ZipMember& a, const ZipMember& b) { return a.arcName < b.arcName; });
    return out;
}

StagedZip::StagedZip(QString finalPath) : m_final(std::move(finalPath)) {}

StagedZip::~StagedZip()
{
    if (!m_committed)
        discard();
}

void StagedZip::discard()
{
    QFile::remove(partPath());
}

Status StagedZip::write(const std::vector<ZipMember>& members, const CancellationToken& token,
                        ProgressReporter* progress, const ZipHook& hook)
{
    m_members = normalizeMembers(members);
    m_result = VerifiedZip{};
    m_written = m_verified = false;
    if (QFileInfo::exists(m_final))
        return makeError(Code::ZipWriteFailed,
                         QStringLiteral("%1 already exists; an earlier ZIP is never overwritten.")
                             .arg(QDir::toNativeSeparators(m_final)));
    zip::Writer w; // stages "<part>.part" and renames it to partPath() in finish()
    if (Status st = w.open(partPath(), false); !st.ok())
        return makeError(Code::ZipWriteFailed,
                         QStringLiteral("Could not create %1: %2").arg(QDir::toNativeSeparators(partPath()), st.error().message),
                         st.error().detail);
    for (const ZipMember& m : m_members) {
        if (!token.checkpoint())
            return Error::cancelled();
        if (hook)
            FW_TRY(hook(QStringLiteral("zip.member"), m.arcName));
        if (progress)
            progress->setCurrentItem(m.arcName);
        Status st = w.addFile(m.arcName, m.sourcePath, zip::Method::Deflate, token, progress);
        if (!st.ok()) {
            if (st.error().isCancelled())
                return st;
            return makeError(Code::ZipWriteFailed,
                             QStringLiteral("Could not add %1 to %2: %3")
                                 .arg(m.arcName, QFileInfo(m_final).fileName(), st.error().message),
                             st.error().detail);
        }
    }
    if (Status st = w.finish(); !st.ok())
        return makeError(Code::ZipWriteFailed,
                         QStringLiteral("Could not finish %1: %2").arg(QFileInfo(m_final).fileName(), st.error().message),
                         st.error().detail);
    m_written = true;
    return success();
}

Status StagedZip::verify(const CancellationToken& token, ProgressReporter* progress)
{
    if (!m_written)
        return makeError(Code::Internal, QStringLiteral("The ZIP was not written."));
    const QString name = QFileInfo(m_final).fileName();
    zip::Reader r;
    if (Status st = r.open(partPath()); !st.ok())
        return makeError(Code::ZipVerifyFailed, QStringLiteral("%1 could not be read back: %2").arg(name, st.error().message),
                         st.error().detail);
    int files = 0;
    for (const zip::Entry& e : r.entries())
        if (!e.isDirectory())
            ++files;
    if (files != int(m_members.size()))
        return makeError(Code::ZipVerifyFailed,
                         QStringLiteral("%1 holds %2 of %3 files.").arg(name).arg(files).arg(m_members.size()));
    VerifiedZip res;
    for (const ZipMember& m : m_members) {
        const zip::Entry* e = r.find(m.arcName);
        if (!e)
            return makeError(Code::ZipVerifyFailed, QStringLiteral("%1 is missing from %2.").arg(m.arcName, name));
        QCryptographicHash h(QCryptographicHash::Sha256);
        Status st = r.read(
            *e,
            [&h](const QByteArray& chunk) {
                h.addData(chunk);
                return success();
            },
            token, progress);
        if (!st.ok()) {
            if (st.error().isCancelled())
                return st;
            return makeError(Code::ZipVerifyFailed,
                             QStringLiteral("CRC check failed for %1 in %2: %3").arg(m.arcName, name, st.error().message),
                             st.error().detail);
        }
        const QString got = QString::fromLatin1(h.result().toHex());
        if (got != m.sha256.toLower())
            return makeError(Code::ZipVerifyFailed,
                             QStringLiteral("%1 in %2 does not match the file it was made from.").arg(m.arcName, name),
                             QStringLiteral("expected %1, read %2").arg(m.sha256, got));
        res.sha256.insert(m.arcName, got);
    }
    auto whole = sha256File(partPath(), token);
    if (!whole.ok())
        return whole.takeError();
    res.zipSha256 = whole.value();
    res.count = files;
    res.bytes = QFileInfo(partPath()).size();
    res.path = m_final;
    m_result = res;
    m_verified = true;
    return success();
}

Status StagedZip::commit(const ZipHook& hook)
{
    if (!m_verified)
        return makeError(Code::Internal, QStringLiteral("The ZIP was not verified."));
    if (hook)
        FW_TRY(hook(QStringLiteral("zip.commit"), m_final));
    if (QFileInfo::exists(m_final))
        return makeError(Code::ZipWriteFailed,
                         QStringLiteral("%1 already exists; an earlier ZIP is never overwritten.")
                             .arg(QDir::toNativeSeparators(m_final)));
    if (!QFile::rename(partPath(), m_final))
        return makeError(Code::ZipWriteFailed,
                         QStringLiteral("Could not rename %1 to its final name.").arg(QFileInfo(partPath()).fileName()));
    m_committed = true;
    return success();
}

} // namespace fw::backport
