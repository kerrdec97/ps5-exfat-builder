#include "connect/ps5/Autoloader.h"

#include "connect/ps5/Module.h"
#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "core/Settings.h"

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

// "<file>.bak", then ".bak.2", ".bak.3"...: an older backup is never overwritten.
QString freeBackupName(const QString& file)
{
    QString b = file + QStringLiteral(".bak");
    for (int n = 2; QFileInfo::exists(b) && n < 10000; ++n)
        b = file + QStringLiteral(".bak.%1").arg(n);
    return b;
}

Status needFolder(const QString& folder)
{
    if (folder.trimmed().isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the ps5_autoloader folder first."));
    if (!QFileInfo(folder).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(folder)));
    return success();
}

// A payload name as one plain file name (no folders, no "..").
QString payloadNameProblem(const QString& name)
{
    if (const QString why = localNameProblem(name); !why.isEmpty())
        return why;
    if (!isPayloadName(name))
        return QStringLiteral("Only .elf, .bin and .js payloads can be autoloaded.");
    return {};
}

} // namespace

QString folderFromSettings(const SettingsValues& settings)
{
    return settings.string(keys::kAutoloaderFolder).trimmed();
}

QString defaultHeader()
{
    // Verbatim from the legacy _al_load (exfat_builder.py).
    return QStringLiteral(
        "# ps5_autoloader\n"
        "# autoload config file\n"
        "# --------------------------------------------------------------------------------\n"
        "# The loader looks for ps5_autoloader/autoload.txt in this order (highest priority first):\n"
        "# 1) USB drives\n"
        "# 2) /data directory\n"
        "# 3) savedata directory\n"
        "# Only the first autoload.txt found will be used.\n"
        "#\n"
        "# Usage:\n"
        "# - Put one filename per line (e.g., payload.elf or script.js).\n"
        "# - Supported payload types: .elf, .bin, .js\n"
        "# - Lines starting with '!' are sleep commands (example: !1000 sleeps for 1000 ms).\n"
        "#\n"
        "# Notes:\n"
        "# - The kernel exploit will start automatically - do NOT include it here!\n"
        "# - You can use custom elf loader by putting it here and adding\n"
        "#   elfldr.elf (must be that filename!) line before other ELFs.\n"
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
    return low.endsWith(QStringLiteral(".elf")) || low.endsWith(QStringLiteral(".bin")) || low.endsWith(QStringLiteral(".js"));
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
        l.text = lines[i].trimmed();
        if (l.text.isEmpty()) {
            l.kind = LineKind::Blank;
        } else if (l.text.startsWith(u'#')) {
            l.kind = LineKind::Comment;
        } else if (l.text.startsWith(u'!')) {
            const QString ms = l.text.mid(1).trimmed();
            bool ok = false;
            const qint64 v = ms.toLongLong(&ok);
            if (isDigits(ms) && ok && v <= std::numeric_limits<int>::max()) {
                l.kind = LineKind::Sleep;
                l.sleepMs = int(v);
            } else {
                l.kind = LineKind::Invalid;
                l.problem = QStringLiteral("Line %1: \"%2\" is not a sleep. Write ! and the milliseconds, e.g. !1000.").arg(l.number).arg(l.text);
            }
        } else if (isPayloadName(l.text)) {
            l.kind = LineKind::Payload;
            l.payload = l.text;
        } else {
            l.kind = LineKind::Invalid;
            l.problem = QStringLiteral("Line %1: \"%2\" is not an .elf, .bin or .js file. Start comments with #.").arg(l.number).arg(l.text);
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
        switch (l.kind) {
        case LineKind::Invalid:
            v.errors << l.problem;
            break;
        case LineKind::Sleep:
            ++v.sleeps;
            if (l.sleepMs == 0)
                v.warnings << QStringLiteral("Line %1: !0 does not wait at all.").arg(l.number);
            else if (l.sleepMs > 60000)
                v.warnings << QStringLiteral("Line %1: waits %2 s; the editor's usual range is 0.1–60 s.").arg(l.number).arg(l.sleepMs / 1000);
            break;
        case LineKind::Payload: {
            ++v.payloads;
            const bool isLoader = l.payload == kElfLoader;
            if (!isLoader && l.payload.compare(kElfLoader, Qt::CaseInsensitive) == 0)
                v.warnings << QStringLiteral("Line %1: the custom ELF loader must be named exactly elfldr.elf.").arg(l.number);
            if (isLoader && firstPayloadLine != 0)
                v.warnings << QStringLiteral("Line %1: elfldr.elf must come before the other payloads (line %2 is loaded first).").arg(l.number).arg(firstPayloadLine);
            if (firstPayloadLine == 0)
                firstPayloadLine = l.number;
            if (l.payload.contains(u'/') || l.payload.contains(u'\\'))
                v.warnings << QStringLiteral("Line %1: \"%2\" is a path; the autoloader loads files from its own folder.").arg(l.number).arg(l.payload);
            else if (checkFiles && !folderFiles.contains(l.payload)) {
                const auto it = std::find_if(folderFiles.begin(), folderFiles.end(),
                                             [&](const QString& f) { return f.compare(l.payload, Qt::CaseInsensitive) == 0; });
                if (it != folderFiles.end())
                    v.warnings << QStringLiteral("Line %1: \"%2\" differs in case from the file %3.").arg(l.number).arg(l.payload, *it);
                else
                    v.warnings << QStringLiteral("Line %1: %2 is not in the folder.").arg(l.number).arg(l.payload);
            }
            break;
        }
        case LineKind::Blank:
        case LineKind::Comment:
            break;
        }
    }
    if (v.payloads == 0 && v.errors.isEmpty())
        v.warnings << QStringLiteral("autoload.txt lists no payloads yet.");
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
        const QString stripped = line.trimmed();
        if (stripped == fileName) {
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
        const QString bak = freeBackupName(r.path);
        if (!QFile::copy(r.path, bak))
            return Error(ErrorCode::IoError, QStringLiteral("Could not back up the current autoload.txt, so it was not changed."));
        r.backupPath = bak;
    }
    FW_TRY(writeFileAtomic(r.path, bytes));
    r.changed = true;
    return r;
}

Result<QString> addPayload(const QString& folder, const QString& sourceFile, bool overwrite, const CancellationToken& token)
{
    FW_TRY(needFolder(folder));
    const QFileInfo src(sourceFile);
    if (!src.isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("The file %1 does not exist.").arg(QDir::toNativeSeparators(sourceFile)));
    const QString name = src.fileName();
    if (const QString why = payloadNameProblem(name); !why.isEmpty())
        return Error(ErrorCode::InvalidArgument, why);
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
    if (const QString why = payloadNameProblem(fileName); !why.isEmpty())
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
