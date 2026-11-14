#include "connect/ps5/Autoloader.h"

#include "connect/ps5/Module.h"
#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "core/Settings.h"
#include "core/Tr.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <limits>

namespace fw::ps5::autoloader {

namespace {

bool isDigits(const QString& s)
{
    if (s.isEmpty())
        return false;
    for (const QChar c : s)
        if (c < u'0' || c > u'9')
            return false;
    return true;
}

// The backups save() made of `file`, oldest first: "<file>.fw.bak" is number
// 1, "<file>.fw.bak.N" number N. ".fw" is a name only this app uses: a plain
// "autoload.txt.bak" (or ".bak.2", ".bak.old"...) may be the user's own copy,
// or one an older version made, and is never listed, so never removed.
QList<QPair<int, QString>> backupsOf(const QString& file)
{
    const QFileInfo fi(file);
    const QString stem = fi.fileName() + QStringLiteral(".fw.bak");
    QList<QPair<int, QString>> out;
    for (const QFileInfo& b : fi.absoluteDir().entryInfoList({stem, stem + QStringLiteral(".*")}, QDir::Files)) {
        if (b.isSymLink())
            continue;
        const QString tail = b.fileName().mid(stem.size());
        if (tail.isEmpty())
            out.append({1, b.absoluteFilePath()});
        else if (tail.size() > 1 && tail.size() < 9 && tail.startsWith(u'.') && isDigits(tail.mid(1)) && tail.mid(1).toInt() >= 2)
            out.append({tail.mid(1).toInt(), b.absoluteFilePath()});
    }
    std::sort(out.begin(), out.end());
    return out;
}

// "<file>.fw.bak", then ".fw.bak.2", ".fw.bak.3"...: the number after the
// newest backup, so the numbers stay in the order the backups were made in
// and an existing one is never overwritten.
QString nextBackupName(const QString& file)
{
    const auto existing = backupsOf(file);
    QString name = existing.isEmpty() ? file + QStringLiteral(".fw.bak")
                                      : file + QStringLiteral(".fw.bak.%1").arg(existing.last().first + 1);
    // Not a backup by the rules above (a folder, a link) but the name is taken.
    for (int n = existing.isEmpty() ? 2 : existing.last().first + 2; QFileInfo(name).exists() || QFileInfo(name).isSymLink(); ++n)
        name = file + QStringLiteral(".fw.bak.%1").arg(n);
    return name;
}

// Every changed save adds a backup to a folder the user copies to a USB
// stick: the newest `keep` stay, the older ones go.
void removeOldBackups(const QString& file, int keep)
{
    auto existing = backupsOf(file);
    while (existing.size() > keep)
        QFile::remove(existing.takeFirst().second);
}

Status needFolder(const QString& folder)
{
    if (folder.trimmed().isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the ps5_autoloader folder first."));
    if (!QFileInfo(folder).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(folder)));
    return success();
}

bool isScriptName(const QString& name)
{
    return name.toLower().endsWith(QStringLiteral(".js"));
}

// A file the editor manages as one plain file name (no folders, no ".."):
// payloads, and (only to delete them) old .js files.
QString fileNameProblem(const QString& name, bool allowScripts)
{
    if (const QString why = localNameProblem(name); !why.isEmpty())
        return why;
    if (!isPayloadName(name) && !(allowScripts && isScriptName(name)))
        return QStringLiteral("Only .elf and .bin payloads can be autoloaded.");
    return {};
}

// The part of a line before its comment, with trailing spaces and tabs removed
// (ps5-unified-autoloader: '#' anywhere starts a comment; leading spaces stay).
QString lineContent(const QString& raw, bool* hadComment = nullptr)
{
    QString s = raw;
    while (s.endsWith(u'\r') || s.endsWith(u'\n'))
        s.chop(1);
    const qsizetype hash = s.indexOf(u'#');
    if (hadComment)
        *hadComment = hash >= 0;
    if (hash >= 0)
        s.truncate(hash);
    while (s.endsWith(u' ') || s.endsWith(u'\t'))
        s.chop(1);
    return s;
}

} // namespace

QString folderFromSettings(const SettingsValues& settings)
{
    return settings.string(keys::kAutoloaderFolder).trimmed();
}

QString defaultHeader()
{
    // The rules of itsPLK/ps5-unified-autoloader 0.1.7 (Y2JB, BD-JB, Lua,
    // WebKit and yarpe autoloaders; research §9).
    return QStringLiteral(
        "# ps5_autoloader\n"
        "# autoload config file\n"
        "# --------------------------------------------------------------------------------\n"
        "# The loader uses the first autoload.txt it finds, in this order:\n"
        "# 1) /mnt/usbN/ps5_autoloader_<TITLEID>/  (USB, one entry app, e.g. PPSA01650)\n"
        "# 2) /data/ps5_autoloader_<TITLEID>/\n"
        "# 3) /mnt/usbN/ps5_autoloader/\n"
        "# 4) /data/ps5_autoloader/\n"
        "#\n"
        "# Usage:\n"
        "# - One payload per line: a file in this folder, or an absolute path (/data/...).\n"
        "# - Payload types: .elf and .bin (.js files are not run by the autoloader).\n"
        "# - !1000 waits 1000 ms. # starts a comment, also after a file name.\n"
        "# - @sync: after a clean run from USB, copy this folder to /data and remove it from USB.\n"
        "#\n"
        "# Notes:\n"
        "# - The kernel exploit starts by itself - do NOT include it here!\n"
        "# - To send payloads from a PC, put elfldr.elf (exactly that name) in this\n"
        "#   folder, list it first and follow it with !4000.\n"
        "# --------------------------------------------------------------------------------\n");
}

QString normalize(const QString& text)
{
    QString s = text;
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    s.replace(u'\r', u'\n');
    static const QRegularExpression runs(QStringLiteral("\n{3,}"));
    s.replace(runs, QStringLiteral("\n\n"));
    while (s.endsWith(u'\n'))
        s.chop(1);
    return s + u'\n';
}

bool isPayloadName(const QString& name)
{
    const QString low = name.toLower();
    return low.endsWith(QStringLiteral(".elf")) || low.endsWith(QStringLiteral(".bin"));
}

std::vector<Line> parse(const QString& text)
{
    std::vector<Line> out;
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(u'\r', u'\n');
    const QStringList lines = t.split(u'\n');
    for (int i = 0; i < lines.size(); ++i) {
        // A trailing newline does not start another line.
        if (i == lines.size() - 1 && lines[i].isEmpty() && i > 0)
            break;
        Line l;
        l.number = i + 1;
        bool hadComment = false;
        const QString content = lineContent(lines[i], &hadComment);
        l.text = content.trimmed();
        if (l.text.isEmpty()) {
            l.kind = hadComment ? LineKind::Comment : LineKind::Blank;
            out.push_back(l);
            continue;
        }
        if (content.front().isSpace())
            l.warning = FW_TR("Line %1: starts with spaces; the autoloader keeps them, so \"%2\" would not be found.")
                            .arg(l.number)
                            .arg(l.text);
        if (l.text.startsWith(u'!')) {
            const QString ms = l.text.mid(1).trimmed();
            bool ok = false;
            const qint64 v = ms.toLongLong(&ok);
            if (isDigits(ms) && ok && v <= std::numeric_limits<int>::max()) {
                l.kind = LineKind::Sleep;
                l.sleepMs = int(v);
            } else {
                l.kind = LineKind::Invalid;
                l.problem = FW_TR("Line %1: \"%2\" is not a sleep. Write ! and the milliseconds, e.g. !1000.").arg(l.number).arg(l.text);
            }
        } else if (l.text.startsWith(u'@')) {
            l.kind = LineKind::Directive;
            l.directive = l.text.mid(1).trimmed().toLower();
            if (l.directive != u"sync")
                l.warning = FW_TR("Line %1: %2 is not a directive the autoloader knows (only @sync).").arg(l.number).arg(l.text);
        } else if (isPayloadName(l.text) || isScriptName(l.text)) {
            l.kind = LineKind::Payload;
            l.payload = l.text;
            l.absolute = l.text.startsWith(u'/');
            if (isScriptName(l.text))
                l.warning = FW_TR("Line %1: %2 is a .js file; the autoloader sends every line to elfldr, so it would fail. "
                                           "Run .js payloads through Y2JB's JS server (port 50000).")
                                .arg(l.number)
                                .arg(l.text);
        } else {
            l.kind = LineKind::Invalid;
            l.problem = (hadComment ? FW_TR("Line %1: \"%2\" is not an .elf or .bin file. Everything after # is a comment, so file names cannot contain #.")
                                    : FW_TR("Line %1: \"%2\" is not an .elf or .bin file."))
                            .arg(l.number)
                            .arg(l.text);
        }
        out.push_back(l);
    }
    return out;
}

Validation validate(const QString& text, const QStringList& folderFiles, bool checkFiles)
{
    Validation v;
    int firstPayloadLine = 0;
    for (const Line& l : parse(text)) {
        if (!l.warning.isEmpty())
            v.warnings << l.warning;
        switch (l.kind) {
        case LineKind::Invalid:
            v.errors << l.problem;
            break;
        case LineKind::Sleep:
            ++v.sleeps;
            if (l.sleepMs == 0)
                v.warnings << FW_TR("Line %1: !0 does not wait at all.").arg(l.number);
            else if (l.sleepMs > 60000)
                v.warnings << FW_TR("Line %1: waits %2 s; the editor's usual range is 0.1–60 s.").arg(l.number).arg(l.sleepMs / 1000);
            break;
        case LineKind::Directive:
            break;
        case LineKind::Payload: {
            ++v.payloads;
            const QString name = remoteFileName(l.payload);
            const bool isLoader = name == kElfLoader;
            if (!isLoader && name.compare(kElfLoader, Qt::CaseInsensitive) == 0)
                v.warnings << FW_TR("Line %1: the custom ELF loader must be named exactly elfldr.elf.").arg(l.number);
            if (isLoader && firstPayloadLine != 0)
                v.warnings << FW_TR("Line %1: elfldr.elf must come before the other payloads (line %2 is loaded first).").arg(l.number).arg(firstPayloadLine);
            if (firstPayloadLine == 0)
                firstPayloadLine = l.number;
            if (l.payload.contains(u'\\'))
                v.warnings << FW_TR("Line %1: \"%2\" uses \\; paths on the PS5 use /.").arg(l.number).arg(l.payload);
            else if (checkFiles && !l.absolute && !l.payload.contains(u'/') && !folderFiles.contains(l.payload)) {
                const auto it = std::find_if(folderFiles.begin(), folderFiles.end(),
                                             [&](const QString& f) { return f.compare(l.payload, Qt::CaseInsensitive) == 0; });
                if (it != folderFiles.end())
                    v.warnings << FW_TR("Line %1: \"%2\" differs in case from the file %3.").arg(l.number).arg(l.payload, *it);
                else
                    v.warnings << FW_TR("Line %1: %2 is not in the folder.").arg(l.number).arg(l.payload);
            }
            break;
        }
        case LineKind::Blank:
        case LineKind::Comment:
            break;
        }
    }
    if (v.payloads == 0 && v.errors.isEmpty())
        v.warnings << FW_TR("autoload.txt lists no payloads yet.");
    return v;
}

QStringList payloadFiles(const QString& folder)
{
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        return {};
    QStringList out;
    for (const QFileInfo& fi : QDir(folder).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot))
        if (isPayloadName(fi.fileName()))
            out << fi.fileName();
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) {
        const int c = a.compare(b, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a < b;
    });
    return out;
}

QString sleepLine(int ms)
{
    return QStringLiteral("!%1").arg(std::max(0, ms));
}

Insertion insertLineAfter(const QString& text, int cursor, const QString& line)
{
    if (text.isEmpty())
        return {line, int(line.size())};
    const int at = std::clamp(cursor, 0, int(text.size()));
    qsizetype end = text.indexOf(u'\n', at);
    if (end < 0)
        end = text.size();
    QString out = text;
    out.insert(end, u'\n' + line);
    return {out, int(end + 1 + line.size())};
}

QString removePayloadLines(const QString& text, const QString& fileName)
{
    const QStringList lines = text.split(u'\n');
    QStringList kept;
    bool skipSleeps = false;
    for (const QString& line : lines) {
        const QString stripped = lineContent(line).trimmed();
        if (!stripped.isEmpty() && stripped == fileName) {
            skipSleeps = true; // and the sleeps right after it
            continue;
        }
        if (skipSleeps && stripped.startsWith(u'!') && isDigits(stripped.mid(1)))
            continue;
        skipSleeps = false;
        kept << line;
    }
    return kept.join(u'\n');
}

Result<Loaded> load(const QString& folder)
{
    FW_TRY(needFolder(folder));
    const QString path = QDir(folder).filePath(kFileName);
    Loaded out;
    if (!QFileInfo(path).isFile()) {
        out.text = defaultHeader();
        return out;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(path)), f.errorString());
    out.text = normalize(QString::fromUtf8(f.readAll()));
    out.existed = true;
    return out;
}

Result<SaveResult> save(const QString& folder, const QString& text)
{
    FW_TRY(needFolder(folder));
    SaveResult r;
    r.path = QDir(folder).filePath(kFileName);
    const QByteArray bytes = normalize(text).toUtf8();
    if (QFileInfo(r.path).isFile()) {
        QFile old(r.path);
        if (!old.open(QIODevice::ReadOnly))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read the current autoload.txt, so it was not changed."), old.errorString());
        if (old.readAll() == bytes)
            return r; // nothing to write
        old.close();
        const QString bak = nextBackupName(r.path);
        if (!QFile::copy(r.path, bak))
            return Error(ErrorCode::IoError, QStringLiteral("Could not back up the current autoload.txt, so it was not changed."));
        r.backupPath = bak;
    }
    FW_TRY(writeFileAtomic(r.path, bytes));
    r.changed = true;
    // Only once the new file is in place: a failed save loses no backup.
    removeOldBackups(r.path, kBackupsKept);
    return r;
}

Result<QString> addPayload(const QString& folder, const QString& sourceFile, bool overwrite, const CancellationToken& token)
{
    FW_TRY(needFolder(folder));
    const QFileInfo src(sourceFile);
    if (!src.isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("The file %1 does not exist.").arg(QDir::toNativeSeparators(sourceFile)));
    const QString name = src.fileName();
    if (const QString why = fileNameProblem(name, false); !why.isEmpty())
        return Error(ErrorCode::InvalidArgument, isScriptName(name) ? why + QStringLiteral(" .js files are not run by the autoloader.") : why);
    const QString dst = QDir(folder).filePath(name);
    if (QFileInfo(dst).exists() && QFileInfo(dst).canonicalFilePath() == src.canonicalFilePath())
        return name; // already the folder's own file
    if (QFileInfo::exists(dst) && !overwrite)
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 is already in the folder.").arg(name));
    FW_TRY(copyFileStreaming(sourceFile, dst, token, nullptr, overwrite));
    return name;
}

Status deletePayload(const QString& folder, const QString& fileName, bool toRecycleBin, bool* trashed)
{
    FW_TRY(needFolder(folder));
    if (const QString why = fileNameProblem(fileName, true); !why.isEmpty())
        return Error(ErrorCode::InvalidArgument, why);
    const QString path = QDir(folder).filePath(fileName);
    if (!QFileInfo(path).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 is no longer in the folder.").arg(fileName));
    if (trashed)
        *trashed = false;
    if (toRecycleBin && QFile::moveToTrash(path)) {
        if (trashed)
            *trashed = true;
        return success();
    }
    if (!QFile::remove(path))
        return Error(ErrorCode::IoError, QStringLiteral("Could not delete %1.").arg(fileName));
    return success();
}

} // namespace fw::ps5::autoloader
