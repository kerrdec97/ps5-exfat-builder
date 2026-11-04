#include "engines/backport/ModuleCaseFix.h"

#include "engines/backport/Common.h"
#include "engines/backport/ExecutableScanner.h"

namespace fw::backport {

namespace {

bool isNameChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'
        || c == '-';
}

QString folderOf(const QString& rel)
{
    const qsizetype i = rel.lastIndexOf(u'/');
    return i < 0 ? QString() : rel.left(i);
}

QString fileOf(const QString& rel)
{
    return rel.section(u'/', -1);
}

} // namespace

QStringList requestedModulePaths(QByteArrayView elf)
{
    const QByteArrayView kLead("%s/Modules/");
    QStringList out;
    qsizetype from = 0;
    while (true) {
        const qsizetype at = elf.indexOf(kLead, from);
        if (at < 0)
            break;
        const qsizetype start = at + kLead.size();
        qsizetype end = start;
        while (end < elf.size() && isNameChar(elf[end]))
            ++end;
        // The regex "([A-Za-z0-9_.-]+\.prx)" takes the longest run that ends
        // in ".prx" with at least one character before it.
        const QByteArrayView run = elf.sliced(start, end - start);
        const qsizetype prx = run.lastIndexOf(QByteArrayView(".prx"));
        if (prx >= 1) {
            const QString name = QString::fromLatin1(run.first(prx + 4));
            const QString path = QStringLiteral("Media/Modules/") + name;
            if (!out.contains(path))
                out << path;
            from = start + prx + 4;
        } else {
            from = start;
        }
    }
    return out;
}

ModuleCasePlan planModuleCase(const ScanResult& scan, const QStringList& requested)
{
    ModuleCasePlan plan;
    for (const QString& want : requested) {
        const QString have = scan.resolve(want);
        if (have.isEmpty() || have == want)
            continue; // not in the dump, or already spelled as the game asks
        const QString fromFile = fileOf(have), wantFile = fileOf(want);
        const bool folderDiffers = folderOf(have) != folderOf(want);
        if (fromFile == wantFile) {
            plan.notes << QStringLiteral("%1: the game loads %2; only the folder's letter case differs, which is not "
                                         "changed.")
                              .arg(have, want);
            continue;
        }
        const ExecFile* e = scan.find(have);
        if (!e || !e->isProcessable()) {
            plan.notes << QStringLiteral("%1: the game loads it as %2, but it is not an executable this backport "
                                         "rewrites, so it is not renamed.")
                              .arg(have, wantFile);
            continue;
        }
        const QString folder = folderOf(have);
        // A case-sensitive file system can hold both spellings: then the
        // game's file exists already and nothing is renamed over it.
        if (onDiskName(joinPath(scan.root, folder), wantFile) == wantFile) {
            plan.notes << QStringLiteral("%1 and %2 both exist; neither is renamed.")
                              .arg(have, folder.isEmpty() ? wantFile : folder + u'/' + wantFile);
            continue;
        }
        plan.renames.push_back({have, folder.isEmpty() ? wantFile : folder + u'/' + wantFile, folderDiffers});
    }
    return plan;
}

} // namespace fw::backport
