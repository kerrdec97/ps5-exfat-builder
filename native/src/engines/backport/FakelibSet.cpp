#include "engines/backport/FakelibSet.h"

#include "core/FileUtil.h"
#include "engines/backport/Common.h"
#include "engines/backport/ExecutableScanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <algorithm>

namespace fw::backport {

qint64 FakelibSource::totalBytes() const
{
    qint64 n = 0;
    for (const FakelibLib& l : libs)
        n += l.size;
    return n;
}

const FakelibLib* FakelibSource::find(const QString& name) const
{
    for (const FakelibLib& l : libs)
        if (l.name.compare(name, Qt::CaseInsensitive) == 0)
            return &l;
    return nullptr;
}

QString fakelibStateName(FakelibState s)
{
    switch (s) {
    case FakelibState::None: return QStringLiteral("none");
    case FakelibState::Same: return QStringLiteral("same");
    case FakelibState::Other: return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

namespace {

bool isLibName(const QString& lowName)
{
    return lowName.endsWith(QStringLiteral(".sprx")) || lowName.endsWith(QStringLiteral(".prx"));
}

} // namespace

FakelibSource listFakelibSource(const QString& dir, const CancellationToken& token)
{
    FakelibSource s;
    s.dir = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    const QFileInfoList files = QDir(s.dir).entryInfoList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo& fi : files) {
        const QString name = fi.fileName();
        if (isOsMetadataFile(name) || !isLibName(name.toLower())) {
            s.ignored << name;
            continue;
        }
        FakelibLib l;
        l.name = name;
        l.path = fi.absoluteFilePath();
        l.size = fi.size();
        auto h = sha256File(l.path, token);
        if (h.ok())
            l.sha256 = h.value();
        s.libs.push_back(l);
    }
    return s;
}

QStringList fakelibProblems(const QString& dir, const QString& game, FakelibSource* out, const CancellationToken& token)
{
    const QFileInfo di(dir);
    if (dir.trimmed().isEmpty() || !di.isDir())
        return {QStringLiteral("The fakelib folder does not exist: %1").arg(QDir::toNativeSeparators(dir))};
    if (!game.isEmpty() && (isSameOrInside(dir, game) || isSameOrInside(game, dir)))
        return {QStringLiteral("The fakelib folder must be outside the game folder (pick a separate copy of the "
                               "fakelib set).")};

    QStringList problems;
    int libs = 0;
    const QString root = QDir::cleanPath(di.absoluteFilePath());
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    QStringList found;
    while (it.hasNext())
        found << it.next();
    found.sort();
    const QDir base(root);
    for (const QString& path : std::as_const(found)) {
        const QString rel = base.relativeFilePath(path);
        const QStringList parts = rel.split(u'/');
        bool inMetadataDir = false;
        for (int i = 0; i + 1 < parts.size(); ++i)
            if (isOsMetadataDir(parts[i]))
                inMetadataDir = true;
        const QString name = parts.back();
        if (inMetadataDir || isOsMetadataFile(name) || !isLibName(name.toLower()))
            continue;
        if (parts.size() > 1) {
            problems << QStringLiteral("%1 is in a sub-folder. Libraries must sit directly in the chosen folder: they "
                                       "are copied to fakelib/<same name> and are only looked up directly in "
                                       "fakelib/. If you picked the folder above the set, pick the set itself.")
                            .arg(QDir::toNativeSeparators(rel));
            continue;
        }
        const qint64 size = QFileInfo(path).size();
        if (size < kMinFakelibBytes) {
            problems << QStringLiteral("%1 is only %2 bytes: a placeholder or an incomplete download, not a library.")
                            .arg(rel)
                            .arg(size);
            continue;
        }
        auto head = readHead(path, 4);
        if (!head.ok()) {
            problems << QStringLiteral("%1 cannot be read: %2").arg(rel, head.error().detail);
            continue;
        }
        if (!hasExecutableMagic(head.value())) {
            problems << QStringLiteral("%1 is not an ELF/SELF library.").arg(rel);
            continue;
        }
        ++libs;
    }
    if (libs == 0 && problems.isEmpty())
        problems << QStringLiteral("The fakelib folder contains no .sprx/.prx libraries.");
    if (problems.isEmpty() && out)
        *out = listFakelibSource(root, token);
    return problems;
}

FakelibState fakelibState(const QString& game, const FakelibSource* source, const CancellationToken& token)
{
    const QString dest = QDir(game).filePath(QStringLiteral("fakelib"));
    if (!QFileInfo(dest).isDir())
        return FakelibState::None;
    QDirIterator it(dest, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    if (!it.hasNext())
        return FakelibState::None;
    if (!source || source->libs.empty())
        return FakelibState::Other;
    for (const FakelibLib& l : source->libs) {
        // Case-insensitive lookup so the result is the same on every OS.
        QString match;
        const QFileInfoList present = QDir(dest).entryInfoList(QDir::Files | QDir::Hidden | QDir::System);
        for (const QFileInfo& fi : present)
            if (fi.fileName().compare(l.name, Qt::CaseInsensitive) == 0)
                match = fi.absoluteFilePath();
        if (match.isEmpty() || QFileInfo(match).size() != l.size)
            return FakelibState::Other;
        auto h = sha256File(match, token);
        if (!h.ok() || h.value() != l.sha256)
            return FakelibState::Other;
    }
    return FakelibState::Same;
}

QStringList fakelibExtras(const QString& game, const FakelibSource& source)
{
    QStringList out;
    const QString dest = QDir(game).filePath(QStringLiteral("fakelib"));
    QDirIterator it(dest, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    const QDir base(dest);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString rel = base.relativeFilePath(path);
        if (isOsMetadataFile(QFileInfo(path).fileName()))
            continue;
        if (rel.contains(u'/') || !source.find(rel))
            out << QStringLiteral("fakelib/") + rel;
    }
    out.sort();
    return out;
}

} // namespace fw::backport
