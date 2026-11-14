#include "engines/exfat/ExfatEdit.h"

#include "core/FileUtil.h"
#include "engines/exfat/ExfatReader.h"
#include "engines/exfat/PlatformFs.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QThread>

#include <algorithm>
#include <cstring>

namespace fw::exfat {

// ---- operations -------------------------------------------------------------------

Result<std::vector<EditOp>> parseEditOps(const QJsonArray& arr)
{
    if (arr.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose at least one change to make to the image."));
    std::vector<EditOp> ops;
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        const QString op = o.value(QStringLiteral("op")).toString().trimmed().toLower();
        EditOp e;
        if (op == u"add")
            e.kind = EditOp::Kind::Add;
        else if (op == u"replace")
            e.kind = EditOp::Kind::Replace;
        else if (op == u"delete")
            e.kind = EditOp::Kind::Delete;
        else if (op == u"mkdir")
            e.kind = EditOp::Kind::Mkdir;
        else
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("Unknown image change \"%1\". Use add, replace, delete or mkdir.").arg(op));
        e.imagePath = o.value(QStringLiteral("imagePath")).toString();
        e.localPath = o.value(QStringLiteral("localPath")).toString();
        if (e.imagePath.trimmed().isEmpty() && e.kind != EditOp::Kind::Add)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Every change needs a path inside the image."));
        if ((e.kind == EditOp::Kind::Add || e.kind == EditOp::Kind::Replace) && e.localPath.trimmed().isEmpty())
            return Error(ErrorCode::InvalidArgument,
                         e.imagePath.trimmed().isEmpty()
                             ? QStringLiteral("Adding to the image needs a file or folder on this computer.")
                             : QStringLiteral("Adding or replacing %1 needs a file or folder on this computer.").arg(e.imagePath));
        ops.push_back(std::move(e));
    }
    return ops;
}

QJsonArray editOpsToJson(const std::vector<EditOp>& ops)
{
    QJsonArray arr;
    for (const EditOp& e : ops) {
        QJsonObject o;
        static const char* names[] = {"add", "replace", "delete", "mkdir"};
        o.insert(QStringLiteral("op"), QLatin1String(names[int(e.kind)]));
        o.insert(QStringLiteral("imagePath"), e.imagePath);
        if (!e.localPath.isEmpty())
            o.insert(QStringLiteral("localPath"), e.localPath);
        arr.append(o);
    }
    return arr;
}

QString EditResult::summary() const
{
    if (!changed)
        return QStringLiteral("No changes were needed");
    QStringList parts;
    if (added)
        parts << (added == 1 ? QStringLiteral("1 file added") : QStringLiteral("%1 files added").arg(formatCount(added)));
    if (foldersAdded)
        parts << (foldersAdded == 1 ? QStringLiteral("1 folder created")
                                    : QStringLiteral("%1 folders created").arg(formatCount(foldersAdded)));
    if (replaced)
        parts << (replaced == 1 ? QStringLiteral("1 file replaced")
                                : QStringLiteral("%1 files replaced").arg(formatCount(replaced)));
    if (deleted)
        parts << (deleted == 1 ? QStringLiteral("1 item deleted") : QStringLiteral("%1 items deleted").arg(formatCount(deleted)));
    if (parts.isEmpty())
        parts << QStringLiteral("No changes");
    QString how;
    if (inPlace)
        how = QStringLiteral("edited in place");
    else if (newVolumeBytes > oldVolumeBytes)
        how = QStringLiteral("rebuilt; the image grew from %1 to %2").arg(formatBytes(oldVolumeBytes), formatBytes(newVolumeBytes));
    else
        how = QStringLiteral("rebuilt");
    return parts.join(QStringLiteral(", ")) + QStringLiteral(" (") + how + u')';
}

QString editJournalPath(const QString& image)
{
    return image + QStringLiteral(".edit-journal");
}

// ---- undo journal ---------------------------------------------------------------------
//
// "FWEXJNL1", u64 image size, u32 volume serial, u32 record count, records
// (u64 offset, u32 length, bytes), u64 FNV-1a checksum of the records. The
// size and serial tie the journal to the image it was made for (in-place
// edits never change them; every build writes a new serial). Written to
// "<journal>.fw.part" (a PartFile; up to 5.0.2 "<journal>.part"), flushed,
// then renamed: a journal under its final name is always complete, and the
// image is only modified after that rename.

namespace {

constexpr char kJournalMagic[9] = "FWEXJNL1";
constexpr qint64 kJournalPiece = 64 * kMiB;

struct Range {
    qint64 offset;
    qint64 length;
};

quint64 fnv(quint64 h, const char* p, qint64 n)
{
    for (qint64 i = 0; i < n; ++i) {
        h ^= uchar(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}
constexpr quint64 kFnvBasis = 14695981039346656037ULL;
constexpr int kJournalHeader = 24;

// VolumeSerialNumber of the image's main boot sector.
quint32 imageSerial(QFile& f)
{
    uchar b[4] = {};
    if (!f.seek(100) || f.read(reinterpret_cast<char*>(b), 4) != 4)
        return 0;
    return get32(b);
}

Status readExact(QFile& f, char* p, qint64 n, const QString& what)
{
    qint64 got = 0;
    while (got < n) {
        const qint64 r = f.read(p + got, n - got);
        if (r <= 0)
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(what), f.errorString());
        got += r;
    }
    return success();
}

Status writeExact(QFile& f, const char* p, qint64 n, const QString& what)
{
    qint64 done = 0;
    while (done < n) {
        const qint64 w = f.write(p + done, n - done);
        if (w <= 0)
            return fileError(QStringLiteral("Could not write %1.").arg(what), f);
        done += w;
    }
    return success();
}

// A journal whose edit was verified but which could not be deleted or emptied
// then is disowned by this file next to it (see retireJournal).
QString journalDoneMarker(const QString& journal)
{
    return journal + QStringLiteral(".done");
}

// Where a journal may be while it is written: its PartFile working name, and
// "<journal>.part", where versions up to 5.0.2 wrote it.
QStringList partJournalsOf(const QString& journal)
{
    return {PartFile::partPathFor(journal), journal + QStringLiteral(".part")};
}

// Copies everything the edit will overwrite. Nothing in the image has changed
// yet, so a cancel simply stops (the .part journal is removed). `identity`
// receives the bytes that tell this journal from any other.
Status writeJournal(const QString& image, const QString& journal, const std::vector<Range>& ranges,
                    const CancellationToken& token, ProgressReporter* progress, QByteArray* identity)
{
    std::vector<Range> pieces;
    for (const Range& r : ranges)
        for (qint64 off = 0; off < r.length; off += kJournalPiece)
            pieces.push_back({r.offset + off, std::min(kJournalPiece, r.length - off)});

    QFile in(toLongPath(image));
    if (!in.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read the image."), in.errorString());
    PartFile part(journal);
    // Recovery reads the journal's working file, and an older version's
    // "<journal>.part": one still there (it could not be removed when the
    // edit started) is not this edit's to replace or to leave beside it.
    for (const QString& left : partJournalsOf(journal))
        if (QFileInfo::exists(left))
            return Error(ErrorCode::IoError,
                         QStringLiteral("Could not remove %1, left by an earlier edit. Close any program using it, then try again.")
                             .arg(QDir::toNativeSeparators(left)));
    QFile out(toLongPath(part.partPath()));
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return fileError(QStringLiteral("Could not create the undo journal next to the image."), out);
    const QString what = QStringLiteral("the undo journal");
    uchar head[kJournalHeader];
    std::memcpy(head, kJournalMagic, 8);
    put64(head + 8, quint64(in.size()));
    put32(head + 16, imageSerial(in));
    put32(head + 20, quint32(pieces.size()));
    FW_TRY(writeExact(out, reinterpret_cast<const char*>(head), kJournalHeader, what));
    quint64 sum = kFnvBasis;
    std::vector<char> buf(size_t(4 * kMiB));
    for (const Range& p : pieces) {
        uchar rec[12];
        put64(rec, quint64(p.offset));
        put32(rec + 8, quint32(p.length));
        FW_TRY(writeExact(out, reinterpret_cast<const char*>(rec), 12, what));
        sum = fnv(sum, reinterpret_cast<const char*>(rec), 12);
        if (!in.seek(p.offset))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read the image."), in.errorString());
        for (qint64 done = 0; done < p.length;) {
            if (!token.checkpoint())
                return Error::cancelled();
            const qint64 n = std::min<qint64>(qint64(buf.size()), p.length - done);
            FW_TRY(readExact(in, buf.data(), n, QStringLiteral("the image")));
            FW_TRY(writeExact(out, buf.data(), n, what));
            sum = fnv(sum, buf.data(), n);
            done += n;
            if (progress)
                progress->advance(n);
        }
    }
    uchar tail[8];
    put64(tail, sum);
    FW_TRY(writeExact(out, reinterpret_cast<const char*>(tail), 8, what));
    FW_TRY(syncFile(out, what));
    out.close();
    if (identity)
        *identity = QByteArray(reinterpret_cast<const char*>(head), kJournalHeader)
                    + QByteArray(reinterpret_cast<const char*>(tail), 8);
    return part.commit(false);
}

// The edit is verified, so its journal must never be applied. Deleting it can
// fail while another program (a virus scanner, an indexer) holds the file:
// the delete is retried, then the journal is emptied, then disowned through
// a marker file. Recovery recognises both. False: none of that worked.
bool retireJournal(const QString& journal, const QByteArray& identity)
{
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (QFile::remove(journal) || !QFileInfo::exists(journal))
            return true;
        QThread::msleep(50);
    }
    {
        QFile f(toLongPath(journal));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.close();
            if (QFileInfo(journal).size() == 0)
                return true;
        }
    }
    QFile marker(toLongPath(journalDoneMarker(journal)));
    return !identity.isEmpty() && marker.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && marker.write(identity) == identity.size() && marker.flush();
}

// True for a journal that retireJournal() emptied or disowned.
bool journalIsRetired(const QString& journal)
{
    QFile j(toLongPath(journal));
    if (!j.open(QIODevice::ReadOnly))
        return false;
    if (j.size() == 0)
        return true;
    QFile marker(toLongPath(journalDoneMarker(journal)));
    if (!marker.open(QIODevice::ReadOnly))
        return false;
    const QByteArray identity = marker.read(kJournalHeader + 8 + 1);
    if (identity.size() != kJournalHeader + 8 || j.size() < identity.size())
        return false;
    QByteArray own = j.read(kJournalHeader);
    if (!j.seek(j.size() - 8))
        return false;
    own += j.read(8);
    return own == identity;
}

// `writingBack` (optional) becomes true once the journal has been checked and
// its bytes start going back into the image.
Status rollbackJournal(const QString& image, const QString& journal, bool* writingBack = nullptr)
{
    const QString what = QStringLiteral("the undo journal");
    QFile j(toLongPath(journal));
    if (!j.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open the undo journal %1.").arg(QDir::toNativeSeparators(journal)),
                     j.errorString());
    uchar head[kJournalHeader];
    FW_TRY(readExact(j, reinterpret_cast<char*>(head), kJournalHeader, what));
    if (std::memcmp(head, kJournalMagic, 8) != 0)
        return Error(ErrorCode::CorruptData, QStringLiteral("The undo journal %1 is not valid.").arg(QDir::toNativeSeparators(journal)));
    const qint64 imageSize = qint64(get64(head + 8));
    const quint32 serial = get32(head + 16);
    const quint32 count = get32(head + 20);
    {
        QFile probe(toLongPath(image));
        if (!probe.open(QIODevice::ReadOnly) || probe.size() != imageSize || imageSerial(probe) != serial)
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("The undo journal %1 does not belong to this image (the image was replaced "
                                        "since). Delete the journal; the image itself is not affected.")
                             .arg(QDir::toNativeSeparators(journal)));
    }
    std::vector<char> buf(size_t(4 * kMiB));
    // Pass 1: the checksum must match before anything is written back.
    quint64 sum = kFnvBasis;
    for (quint32 i = 0; i < count; ++i) {
        uchar rec[12];
        FW_TRY(readExact(j, reinterpret_cast<char*>(rec), 12, what));
        sum = fnv(sum, reinterpret_cast<const char*>(rec), 12);
        const qint64 len = get32(rec + 8);
        if (qint64(get64(rec)) < 0 || qint64(get64(rec)) > imageSize - len)
            return Error(ErrorCode::CorruptData, QStringLiteral("The undo journal %1 is damaged; the image was not changed back.")
                                                     .arg(QDir::toNativeSeparators(journal)));
        for (qint64 done = 0; done < len;) {
            const qint64 n = std::min<qint64>(qint64(buf.size()), len - done);
            FW_TRY(readExact(j, buf.data(), n, what));
            sum = fnv(sum, buf.data(), n);
            done += n;
        }
    }
    uchar tail[8];
    FW_TRY(readExact(j, reinterpret_cast<char*>(tail), 8, what));
    if (get64(tail) != sum)
        return Error(ErrorCode::CorruptData, QStringLiteral("The undo journal %1 is damaged; the image was not changed back.")
                                                 .arg(QDir::toNativeSeparators(journal)));
    // Pass 2: write the saved bytes back.
    if (writingBack)
        *writingBack = true;
    QFile img(toLongPath(image));
    if (!img.open(QIODevice::ReadWrite))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open the image to undo the edit."), img.errorString());
    if (!j.seek(kJournalHeader))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(what), j.errorString());
    for (quint32 i = 0; i < count; ++i) {
        uchar rec[12];
        FW_TRY(readExact(j, reinterpret_cast<char*>(rec), 12, what));
        const qint64 off = qint64(get64(rec));
        const qint64 len = get32(rec + 8);
        if (!img.seek(off))
            return fileError(QStringLiteral("Could not undo the edit."), img);
        for (qint64 done = 0; done < len;) {
            const qint64 n = std::min<qint64>(qint64(buf.size()), len - done);
            FW_TRY(readExact(j, buf.data(), n, what));
            FW_TRY(writeExact(img, buf.data(), n, QStringLiteral("the image")));
            done += n;
        }
    }
    FW_TRY(syncFile(img, QStringLiteral("the image")));
    img.close();
    j.close();
    if (!QFile::remove(journal))
        return Error(ErrorCode::IoError, QStringLiteral("The image was restored, but the undo journal %1 could not be removed.")
                                             .arg(QDir::toNativeSeparators(journal)));
    return success();
}

// ---- the edited tree ---------------------------------------------------------------------

struct MNode {
    QString name;
    bool isDir = false;
    qint64 size = 0;
    quint16 attributes = 0;
    QDateTime created, modified, accessed;
    int imageEntry = -1;   // entry of the original image; -1 = new
    QString localPath;     // data comes from this file on disk
    int parent = -1;
    std::vector<int> children;
};

class Model {
public:
    Model(const ImageTree& t, const UpcaseTable& up) : m_up(up)
    {
        nodes.resize(t.entries.size());
        for (size_t i = 0; i < t.entries.size(); ++i) {
            const ImageEntry& e = t.entries[i];
            MNode& n = nodes[i];
            n.name = e.name;
            n.isDir = e.isDir;
            n.size = e.size;
            n.attributes = e.attributes;
            n.created = e.created;
            n.modified = e.modified;
            n.accessed = e.accessed;
            n.imageEntry = int(i);
            n.parent = e.parent;
            n.children = e.children;
        }
    }

    int child(int dir, const QString& name) const
    {
        const QString key = m_up.upcase(name);
        for (int c : nodes[size_t(dir)].children)
            if (m_up.upcase(nodes[size_t(c)].name) == key)
                return c;
        return -1;
    }
    int find(const QStringList& parts) const
    {
        int cur = 0;
        for (const QString& p : parts) {
            if (!nodes[size_t(cur)].isDir)
                return -1;
            cur = child(cur, p);
            if (cur < 0)
                return -1;
        }
        return cur;
    }
    int add(int parent, MNode n)
    {
        n.parent = parent;
        const int idx = int(nodes.size());
        nodes.push_back(std::move(n));
        nodes[size_t(parent)].children.push_back(idx);
        return idx;
    }
    void detach(int idx)
    {
        auto& kids = nodes[size_t(nodes[size_t(idx)].parent)].children;
        kids.erase(std::remove(kids.begin(), kids.end(), idx), kids.end());
        nodes[size_t(idx)].parent = -2;
    }
    // Creates missing folders along `parts`; returns the last one.
    Result<int> ensureDirs(const QStringList& parts, qint64& created)
    {
        int cur = 0;
        QString shown;
        for (const QString& p : parts) {
            shown = shown.isEmpty() ? p : shown + u'/' + p;
            const int c = child(cur, p);
            if (c >= 0) {
                if (!nodes[size_t(c)].isDir)
                    return Error(ErrorCode::InvalidArgument,
                                 QStringLiteral("%1 is a file in the image, not a folder.").arg(shown));
                cur = c;
                continue;
            }
            if (const QString why = checkFileName(p); !why.isEmpty())
                return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 cannot be created: %2.").arg(shown, why));
            MNode d;
            d.name = p;
            d.isDir = true;
            d.attributes = kAttrDirectory;
            d.created = d.modified = d.accessed = QDateTime::currentDateTime();
            cur = add(cur, std::move(d));
            ++created;
        }
        return cur;
    }

    std::vector<MNode> nodes;

private:
    const UpcaseTable& m_up;
};

QStringList splitImagePath(const QString& path, QString& why)
{
    QString s = path;
    s.replace(u'\\', u'/');
    const QStringList parts = s.split(u'/', Qt::SkipEmptyParts);
    for (const QString& p : parts)
        if (p == u"." || p == u"..") {
            why = QStringLiteral("\".\" and \"..\" cannot be used in a path inside the image");
            return {};
        }
    return parts;
}

MNode nodeFromLocal(const QString& name, const QFileInfo& fi, const QString& abs)
{
    MNode n;
    n.name = name;
    n.isDir = fi.isDir();
    n.size = n.isDir ? 0 : fi.size();
    n.attributes = sourceAttributes(abs, n.isDir, fi.isWritable());
    n.modified = fi.lastModified();
    n.created = fi.birthTime().isValid() ? fi.birthTime() : n.modified;
    n.accessed = fi.lastRead().isValid() ? fi.lastRead() : n.modified;
    if (!n.isDir)
        n.localPath = abs;
    return n;
}

Status applyOp(Model& m, const EditOp& op, const ScanRules& rules, const CancellationToken& token, EditResult& res)
{
    QString why;
    const QStringList parts = splitImagePath(op.imagePath, why);
    if (!why.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1: %2.").arg(op.imagePath, why));
    const QString shown = parts.join(u'/');
    switch (op.kind) {
    case EditOp::Kind::Delete: {
        const int idx = m.find(parts);
        if (parts.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The root folder of an image cannot be deleted."));
        if (idx < 0)
            return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in the image, so it cannot be deleted.").arg(shown));
        m.detach(idx);
        ++res.deleted;
        return success();
    }
    case EditOp::Kind::Mkdir: {
        if (parts.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Name the folder to create inside the image."));
        FW_TRY_ASSIGN(int dir, m.ensureDirs(parts, res.foldersAdded));
        Q_UNUSED(dir);
        return success();
    }
    case EditOp::Kind::Replace: {
        const int idx = m.find(parts);
        if (parts.isEmpty() || idx < 0)
            return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in the image, so it cannot be replaced. Use Add instead.").arg(shown));
        if (m.nodes[size_t(idx)].isDir)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("%1 is a folder. Only files can be replaced; delete the folder and add the new one.").arg(shown));
        const QFileInfo fi(op.localPath);
        if (!fi.exists())
            return Error(ErrorCode::NotFound, QStringLiteral("The replacement file %1 does not exist.").arg(QDir::toNativeSeparators(op.localPath)));
        if (fi.isDir())
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("%1 is a folder; choose a file to replace %2 with.")
                             .arg(QDir::toNativeSeparators(op.localPath), shown));
        MNode n = nodeFromLocal(m.nodes[size_t(idx)].name, fi, QDir::cleanPath(fi.absoluteFilePath()));
        MNode& t = m.nodes[size_t(idx)];
        t.size = n.size;
        t.attributes = n.attributes;
        t.created = n.created;
        t.modified = n.modified;
        t.accessed = n.accessed;
        t.localPath = n.localPath;
        ++res.replaced;
        return success();
    }
    case EditOp::Kind::Add: {
        const QFileInfo fi(op.localPath);
        if (!fi.exists())
            return Error(ErrorCode::NotFound, QStringLiteral("%1 does not exist.").arg(QDir::toNativeSeparators(op.localPath)));
        QStringList target = parts;
        // "add <folder>/" or an existing folder as target: keep the local name.
        const int existing = m.find(parts);
        if (target.isEmpty() || (existing >= 0 && m.nodes[size_t(existing)].isDir))
            target << fi.fileName();
        const QString name = target.last();
        FW_TRY_ASSIGN(int parent, m.ensureDirs(target.mid(0, target.size() - 1), res.foldersAdded));
        const QString targetShown = target.join(u'/');
        if (m.child(parent, name) >= 0)
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 is already in the image. Use Replace for a file, or delete it first.").arg(targetShown));
        if (const QString bad = checkFileName(name); !bad.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 cannot be added: %2.").arg(targetShown, bad));
        const QString abs = QDir::cleanPath(fi.absoluteFilePath());
        if (!fi.isDir()) {
            m.add(parent, nodeFromLocal(name, fi, abs));
            ++res.added;
            return success();
        }
        auto scanned = scanSource(abs, rules, token);
        if (!scanned.ok())
            return scanned.takeError();
        const SourceTree& st = scanned.value();
        // What the scan skipped is not in the image afterwards: say so.
        res.warnings += st.warnings;
        if (st.excludedCount > 0)
            res.warnings << (st.excludedCount == 1
                                 ? QStringLiteral("1 item in %1 was left out (exclusion rules).")
                                 : QStringLiteral("%1 items in %2 were left out (exclusion rules).").arg(formatCount(st.excludedCount)))
                                .arg(QDir::toNativeSeparators(abs));
        std::vector<int> map(st.nodes.size(), -1);
        MNode root = nodeFromLocal(name, fi, abs);
        map[0] = m.add(parent, std::move(root));
        ++res.foldersAdded;
        for (size_t i = 1; i < st.nodes.size(); ++i) {
            const SourceNode& s = st.nodes[i];
            MNode n;
            n.name = s.name;
            n.isDir = s.isDir;
            n.size = s.size;
            n.attributes = s.attributes;
            n.created = s.created;
            n.modified = s.modified;
            n.accessed = s.accessed;
            if (!s.isDir)
                n.localPath = s.absolutePath;
            map[i] = m.add(map[size_t(s.parent)], std::move(n));
            if (s.isDir)
                ++res.foldersAdded;
            else
                ++res.added;
        }
        return success();
    }
    }
    return success();
}

struct Diff {
    std::vector<char> reachable;   // per model node
    std::vector<int> removedTop;   // original entries whose entry set is deleted
    std::vector<int> removedAll;   // original entries no longer in the tree
    std::vector<int> replaced;     // model nodes: original entries with new data
    std::vector<int> added;        // model nodes that are new
};

Diff diffModel(const Model& m, const ImageTree& tree)
{
    Diff d;
    d.reachable.assign(m.nodes.size(), 0);
    std::vector<int> stack{0};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        d.reachable[size_t(n)] = 1;
        for (int c : m.nodes[size_t(n)].children)
            stack.push_back(c);
    }
    for (int i = 1; i < int(m.nodes.size()); ++i) {
        const MNode& n = m.nodes[size_t(i)];
        if (i < int(tree.entries.size())) {
            if (!d.reachable[size_t(i)]) {
                d.removedAll.push_back(i);
                if (d.reachable[size_t(tree.entries[size_t(i)].parent)])
                    d.removedTop.push_back(i);
            } else if (!n.localPath.isEmpty()) {
                d.replaced.push_back(i);
            }
        } else if (d.reachable[size_t(i)]) {
            d.added.push_back(i);
        }
    }
    return d;
}

QString nodePath(const Model& m, int idx)
{
    QStringList parts;
    for (int n = idx; n > 0; n = m.nodes[size_t(n)].parent)
        parts.prepend(m.nodes[size_t(n)].name);
    return parts.join(u'/');
}

std::vector<ExpectedFile> expectedFromModel(const Model& m, const Diff& d, const ImageTree& tree, bool inPlace)
{
    std::vector<ExpectedFile> out;
    for (int i = 1; i < int(m.nodes.size()); ++i) {
        if (!d.reachable[size_t(i)])
            continue;
        const MNode& n = m.nodes[size_t(i)];
        ExpectedFile x;
        x.path = nodePath(m, i);
        x.isDir = n.isDir;
        x.size = n.size;
        if (!n.isDir) {
            if (!n.localPath.isEmpty())
                x.localPath = n.localPath;
            else if (inPlace)
                x.unchangedFirstCluster = tree.entries[size_t(n.imageEntry)].firstCluster;
            else
                x.imageEntry = &tree.entries[size_t(n.imageEntry)];
        }
        out.push_back(std::move(x));
    }
    return out;
}

qint64 dirEntryOffset(const ImageEntry& dir, qint64 index, const Geometry& g)
{
    qint64 byte = index * kEntrySize;
    for (const Extent& e : dir.extents) {
        const qint64 run = qint64(e.count) * g.clusterBytes();
        if (byte < run)
            return g.clusterOffset(e.cluster) + byte;
        byte -= run;
    }
    return -1;
}

class ImageNodeReader final : public NodeReader {
public:
    ImageNodeReader(ImageFileStream& s, const ImageEntry& e) : m_s(s), m_e(e) {}
    qint64 size() const override { return m_e.size; }
    qint64 read(char* b, qint64 n) override
    {
        auto r = m_s.read(m_e, m_pos, b, n);
        if (!r.ok()) {
            m_err = r.error().toLogString();
            return -1;
        }
        m_pos += r.value();
        return r.value();
    }
    QString errorString() const override { return m_err; }

private:
    ImageFileStream& m_s;
    const ImageEntry& m_e;
    qint64 m_pos = 0;
    QString m_err;
};

struct Patch {
    qint64 offset;
    QByteArray bytes;
};

Result<EditResult> undoAfter(const QString& image, const QString& journal, Error why)
{
    const Status rb = rollbackJournal(image, journal);
    if (!rb.ok())
        return Error(ErrorCode::IoError,
                     QStringLiteral("The edit stopped and could not be undone automatically. The original contents are "
                                    "saved in %1: keep that file and run the edit again to restore the image.")
                         .arg(QDir::toNativeSeparators(journal)),
                     rb.error().toLogString() + QStringLiteral(" / ") + why.toLogString());
    return why;
}

// Verifies the edited image. A cancel once the edit is written does not throw
// it away (finished work is kept as with verification turned off): when the
// verification had found nothing wrong yet, names, sizes and the structure are
// checked instead, from the image's metadata (seconds), with a token that the
// cancel does not stop but closing the app does, and that check decides;
// `cancelled` then says the edit is not verified. A problem found before the
// cancel fails as always. While the app is closing nothing is kept: the cancel
// stands (an edit in place is undone, a rebuilt image is not used).
Result<VerifyReport> verifyEdited(const QString& image, const std::vector<ExpectedFile>& expected, const VerifyOptions& vo,
                                  const CancellationToken& token, ProgressReporter* progress, const Log* log,
                                  bool& cancelled, const QString& original = {}, const Geometry* originalGeometry = nullptr)
{
    cancelled = false;
    auto report = verifyImage(image, expected, vo, token, progress, original, originalGeometry);
    if (report.ok() || !token.isCancelled() || token.isShutdown())
        return report;
    if (!token.keepFinishedWork())
        return Error::cancelled();
    if (log)
        log->info(QStringLiteral("Verification was cancelled; checking the structure, names and sizes of the edited image before "
                                 "it is kept."));
    VerifyOptions quick = vo;
    quick.full = false;
    quick.onCompareStart = {};
    auto checked = verifyImage(image, expected, quick, token.untilShutdown(), nullptr, original, originalGeometry);
    if (!checked.ok())
        return Error::cancelled(); // nothing could be checked: the cancel stands
    cancelled = checked->passed();
    if (cancelled && log)
        log->warn(QStringLiteral("Verification was cancelled, so the edited image is not verified (its structure, names and "
                                 "sizes were checked)."));
    return checked;
}

// ---- in place ----------------------------------------------------------------------------

Result<EditResult> editInPlace(const QString& image, ExfatReader& reader, const ImageTree& tree, const Model& m,
                               const Diff& d, const EditOptions& o, const CancellationToken& token,
                               ProgressReporter* progress, const Log* log, EditResult res)
{
    const Geometry& g = reader.geometry();
    const qint64 cb = g.clusterBytes();
    res.inPlace = true;
    res.newVolumeBytes = res.oldVolumeBytes;

    QFile ro(toLongPath(image));
    if (!ro.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open the image."), ro.errorString());
    const auto readAt = [&](qint64 off, qint64 n) -> Result<QByteArray> {
        if (!ro.seek(off))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read the image."), ro.errorString());
        QByteArray b = ro.read(n);
        if (b.size() != n)
            return Error(ErrorCode::IoError, QStringLiteral("Could not read the image."), ro.errorString());
        return b;
    };

    std::vector<Patch> patches;
    std::vector<Extent> freed;

    // Deleted entries: clear the InUse bit of every entry of their sets.
    for (int i : d.removedTop) {
        const ImageEntry& e = tree.entries[size_t(i)];
        const ImageEntry& parent = tree.entries[size_t(e.parent)];
        for (int k = 0; k < e.entryCount; ++k) {
            const qint64 off = dirEntryOffset(parent, e.entryIndex + k, g);
            FW_TRY_ASSIGN(QByteArray b, readAt(off, 1));
            b[0] = char(uchar(b[0]) & 0x7F);
            patches.push_back({off, b});
        }
    }
    for (int i : d.removedAll)
        for (const Extent& x : tree.entries[size_t(i)].extents)
            freed.push_back(x);

    // Replaced files: new data in the existing clusters, updated entry set.
    struct DataWrite {
        qint64 offset;
        qint64 length;
        QString localPath;
        QString shown;
    };
    std::vector<DataWrite> writes;
    qint64 dataBytes = 0;
    for (int i : d.replaced) {
        const ImageEntry& e = tree.entries[size_t(i)];
        const MNode& n = m.nodes[size_t(i)];
        const qint64 keep = ceilDiv(n.size, cb);
        if (!e.extents.empty() && e.allocatedClusters() > keep) {
            if (e.noFatChain)
                freed.push_back({quint32(e.firstCluster + keep), quint32(e.allocatedClusters() - keep)});
            else
                for (const Extent& x : e.extents) // only reached when the new size is 0
                    freed.push_back(x);
        }
        if (n.size > 0) {
            writes.push_back({g.clusterOffset(e.firstCluster), n.size, n.localPath, e.path});
            dataBytes += n.size;
        }
        const ImageEntry& parent = tree.entries[size_t(e.parent)];
        QByteArray set;
        std::vector<qint64> offs;
        for (int k = 0; k < e.entryCount; ++k) {
            offs.push_back(dirEntryOffset(parent, e.entryIndex + k, g));
            FW_TRY_ASSIGN(QByteArray b, readAt(offs.back(), kEntrySize));
            set += b;
        }
        auto* p = reinterpret_cast<uchar*>(set.data());
        put16(p + 4, quint16(n.attributes & kAttrMask & ~kAttrDirectory));
        const Timestamp c = encodeTimestamp(n.created), mo = encodeTimestamp(n.modified), a = encodeTimestamp(n.accessed);
        put32(p + 8, c.value);
        put32(p + 12, mo.value);
        put32(p + 16, a.value);
        p[20] = c.tenMs;
        p[21] = mo.tenMs;
        p[22] = c.utcOffset;
        p[23] = mo.utcOffset;
        p[24] = a.utcOffset;
        uchar* s = p + kEntrySize;
        s[1] = uchar(kFlagAllocationPossible | (n.size > 0 ? kFlagNoFatChain : 0));
        put64(s + 8, quint64(n.size));
        put32(s + 20, n.size > 0 ? e.firstCluster : 0);
        put64(s + 24, quint64(n.size));
        put16(p + 2, entrySetChecksum(p, e.entryCount));
        for (int k = 0; k < e.entryCount; ++k)
            patches.push_back({offs[size_t(k)], set.mid(k * kEntrySize, kEntrySize)});
    }

    // Allocation bitmap and PercentInUse.
    FW_TRY_ASSIGN(QByteArray bitmap, reader.readBitmap());
    QByteArray updated = bitmap;
    qint64 freedClusters = 0;
    for (const Extent& x : freed)
        for (quint32 k = 0; k < x.count; ++k) {
            const qint64 bit = qint64(x.cluster) + k - kFirstDataCluster;
            updated[qsizetype(bit / 8)] = char(uchar(updated[qsizetype(bit / 8)]) & ~(1 << (bit % 8)));
            ++freedClusters;
        }
    const auto bitmapOffset = [&](qint64 byteIndex) {
        for (const Extent& x : reader.volume().bitmapExtents) {
            const qint64 run = qint64(x.count) * cb;
            if (byteIndex < run)
                return g.clusterOffset(x.cluster) + byteIndex;
            byteIndex -= run;
        }
        return qint64(-1);
    };
    for (qsizetype i = 0; i < updated.size();) {
        if (updated[i] == bitmap[i]) {
            ++i;
            continue;
        }
        qsizetype j = i + 1;
        while (j < updated.size() && updated[j] != bitmap[j] && bitmapOffset(j) == bitmapOffset(j - 1) + 1)
            ++j;
        patches.push_back({bitmapOffset(i), updated.mid(i, j - i)});
        i = j;
    }
    const qint64 used = std::max<qint64>(0, tree.usedClusters - freedClusters);
    patches.push_back({112, QByteArray(1, char(quint8(used * 100 / std::max<quint32>(1, g.clusterCount))))});

    // VolumeFlags (boot sector bytes 106-107, outside the boot checksum): the
    // volume is marked dirty while the edit is applied, so a crash in the
    // middle leaves an image that the console or Windows does not trust.
    const quint16 volumeFlags = reader.volume().boot.volumeFlags;
    const auto flagBytes = [](quint16 v) {
        QByteArray b(2, '\0');
        b[0] = char(v & 0xFF);
        b[1] = char(v >> 8);
        return b;
    };

    // Undo journal: everything about to be overwritten.
    std::vector<Range> ranges;
    ranges.push_back({kVolumeFlagsOffset, 2});
    qint64 journalBytes = 64 + 2 + 12;
    for (const DataWrite& w : writes) {
        ranges.push_back({w.offset, w.length});
        journalBytes += w.length + 12;
    }
    for (const Patch& p : patches) {
        ranges.push_back({p.offset, p.bytes.size()});
        journalBytes += p.bytes.size() + 12;
    }
    qint64 journalData = 0;
    for (const Range& r : ranges)
        journalData += r.length;
    const QString journal = editJournalPath(image);
    FW_TRY(ensureFreeSpace(QFileInfo(image).absolutePath(), journalBytes, o.freeSpaceMarginBytes));
    // Replacement files must still be what was planned before anything changes.
    for (int i : d.replaced) {
        const MNode& n = m.nodes[size_t(i)];
        if (QFileInfo(n.localPath).size() != n.size)
            return Error(ErrorCode::IoError, QStringLiteral("%1 changed while the edit was being prepared. Try again.")
                                                 .arg(QDir::toNativeSeparators(n.localPath)));
    }
    if (!token.checkpoint())
        return Error::cancelled();
    if (log)
        log->info(QStringLiteral("Editing in place: %1 bytes of new data, %2 metadata patches, undo journal %3")
                      .arg(dataBytes).arg(patches.size()).arg(formatBytes(journalBytes)));
    // A journal still here was retired (anything else was rolled back when
    // the edit started) but could not be deleted then. It goes first: while
    // it exists its marker must stay, or recovery would apply it.
    if (QFileInfo::exists(journal) && !QFile::remove(journal))
        return Error(ErrorCode::IoError,
                     QStringLiteral("Could not remove the old undo journal %1. Close any program using it, then try again.")
                         .arg(QDir::toNativeSeparators(journal)));
    // A marker of an earlier journal must not disown the one written now.
    if (const QString marker = journalDoneMarker(journal); QFileInfo::exists(marker) && !QFile::remove(marker))
        return Error(ErrorCode::IoError,
                     QStringLiteral("Could not remove %1, left by an earlier edit. Delete it, then try again.")
                         .arg(QDir::toNativeSeparators(marker)));
    // The journal copies as many bytes as the edit writes, so it is part of
    // the writing phase: it shows progress and can be cancelled.
    if (o.onPhase)
        o.onPhase(1, journalData + dataBytes, qint64(writes.size()), true);
    if (progress)
        progress->setCurrentItem(QStringLiteral("Undo journal"));
    QByteArray journalId;
    FW_TRY(writeJournal(image, journal, ranges, token, progress, &journalId));
    ro.close();

    // Apply. From here on any failure or cancellation rolls back.
    {
        // Unbuffered: nothing may be left in a write buffer when a failure
        // rolls back through another handle (a later flush would put the new
        // bytes back on top of the restored image).
        QFile rw(toLongPath(image));
        const auto fail = [&](Error why) {
            rw.close();
            return undoAfter(image, journal, std::move(why));
        };
        if (!rw.open(QIODevice::ReadWrite | QIODevice::Unbuffered))
            return fail(Error(ErrorCode::IoError, QStringLiteral("Could not open the image for writing."), rw.errorString()));
        const auto writeFlags = [&](quint16 v) -> Status {
            if (!rw.seek(kVolumeFlagsOffset))
                return fileError(QStringLiteral("Could not write the image."), rw);
            const QByteArray b = flagBytes(v);
            FW_TRY(writeExact(rw, b.constData(), b.size(), QStringLiteral("the image")));
            return syncFile(rw, QStringLiteral("the image"));
        };
        // The dirty mark reaches the disk before any of the new data does.
        if (const Status st = writeFlags(quint16(volumeFlags | kVolumeDirty)); !st.ok())
            return fail(st.error());
        std::vector<char> buf(size_t(std::max<qint64>(64 * 1024, o.writer.bufferBytes)));
        for (const DataWrite& w : writes) {
            if (progress)
                progress->setCurrentItem(w.shown);
            auto in = openForRead(w.localPath, o.writer.openRetries, o.writer.retryWaitMs, token);
            if (!in.ok())
                return fail(in.takeError());
            if (in.value()->size() != w.length)
                return fail(Error(ErrorCode::IoError, QStringLiteral("%1 changed while the image was being edited.")
                                                      .arg(QDir::toNativeSeparators(w.localPath))));
            if (!rw.seek(w.offset))
                return fail(fileError(QStringLiteral("Could not write the image."), rw));
            for (qint64 done = 0; done < w.length;) {
                if (!token.checkpoint())
                    return fail(Error::cancelled());
                const qint64 n = std::min<qint64>(qint64(buf.size()), w.length - done);
                if (const Status st = readExact(*in.value(), buf.data(), n, QDir::toNativeSeparators(w.localPath)); !st.ok())
                    return fail(st.error());
                if (const Status st = writeExact(rw, buf.data(), n, QStringLiteral("the image")); !st.ok())
                    return fail(st.error());
                done += n;
                if (progress)
                    progress->advance(n);
            }
            if (progress)
                progress->advanceFiles(1);
        }
        for (const Patch& p : patches) {
            if (!rw.seek(p.offset))
                return fail(fileError(QStringLiteral("Could not write the image."), rw));
            if (const Status st = writeExact(rw, p.bytes.constData(), p.bytes.size(), QStringLiteral("the image")); !st.ok())
                return fail(st.error());
        }
        if (const Status st = syncFile(rw, QStringLiteral("the image")); !st.ok())
            return fail(st.error());
        // Everything is on disk: the volume is consistent again (the journal
        // stays until verification has passed).
        if (const Status st = writeFlags(volumeFlags); !st.ok())
            return fail(st.error());
    }
    if (o.testCrashAfterApply)
        return Error(ErrorCode::Internal, QStringLiteral("Simulated crash after applying the edit (test)."));
    if (!token.checkpoint())
        return undoAfter(image, journal, Error::cancelled());

    // Verify; keep the edit only if it passes.
    const auto expected = expectedFromModel(m, d, tree, true);
    VerifyOptions vo = o.verify;
    if (o.onPhase) {
        const auto onPhase = o.onPhase;
        vo.onCompareStart = [onPhase](qint64 total, qint64 files, bool bytes) { onPhase(2, total, files, bytes); };
    }
    bool cancelled = false;
    auto report = verifyEdited(image, expected, vo, token, progress, log, cancelled);
    if (!report.ok())
        return undoAfter(image, journal, report.takeError());
    res.verification = std::move(report).value();
    res.verificationCancelled = cancelled;
    if (!res.verification.passed()) {
        if (log)
            log->warn(QStringLiteral("The edited image did not verify; undoing the edit: %1").arg(res.verification.summary()));
        auto undone = undoAfter(image, journal, Error(ErrorCode::VerificationFailed, res.verification.summary()));
        if (undone.error().code != ErrorCode::VerificationFailed)
            return undone; // the rollback itself failed
        res.restored = true;
        return res;
    }
    if (o.testBeforeJournalRemoval)
        o.testBeforeJournalRemoval(journal);
    if (!retireJournal(journal, journalId))
        res.warnings << QStringLiteral("The undo journal %1 could not be removed. Delete it by hand before this image is "
                                       "edited, verified or extracted again; otherwise that step undoes this edit.")
                            .arg(QDir::toNativeSeparators(journal));
    return res;
}

// ---- rebuild -------------------------------------------------------------------------------

Result<EditResult> editRebuild(const QString& image, std::unique_ptr<ExfatReader> reader, const ImageTree& tree,
                               const Model& m, const Diff& d, const EditOptions& o, const CancellationToken& token,
                               ProgressReporter* progress, const Log* log, EditResult res)
{
    const Geometry geo = reader->geometry();
    const QString label = reader->volume().label;
    const quint32 serial = reader->volume().boot.serial;

    // The edited tree as a writer source, children in up-cased name order.
    SourceTree st;
    st.root = image;
    const UpcaseTable& up = UpcaseTable::recommended();
    std::vector<std::pair<int, int>> stack{{0, -1}}; // model node, parent source node
    while (!stack.empty()) {
        const auto [mi, parent] = stack.back();
        stack.pop_back();
        const MNode& n = m.nodes[size_t(mi)];
        SourceNode s;
        s.name = mi == 0 ? QString() : n.name;
        s.isDir = n.isDir;
        s.size = n.size;
        s.attributes = n.attributes;
        s.created = n.created;
        s.modified = n.modified;
        s.accessed = n.accessed;
        s.parent = parent;
        s.absolutePath = n.localPath;
        s.imageEntry = n.localPath.isEmpty() && !n.isDir ? n.imageEntry : -1;
        if (parent >= 0) {
            const QString& pr = st.nodes[size_t(parent)].relativePath;
            s.relativePath = pr.isEmpty() ? n.name : pr + u'/' + n.name;
            if (n.isDir)
                ++st.dirCount;
            else {
                ++st.fileCount;
                st.totalBytes += n.size;
            }
        }
        const int si = int(st.nodes.size());
        st.nodes.push_back(std::move(s));
        if (parent >= 0)
            st.nodes[size_t(parent)].children.push_back(si);
        std::vector<int> kids = n.children;
        std::sort(kids.begin(), kids.end(), [&](int a, int b) {
            const int c = up.upcase(m.nodes[size_t(a)].name).compare(up.upcase(m.nodes[size_t(b)].name));
            return c != 0 ? c < 0 : m.nodes[size_t(a)].name < m.nodes[size_t(b)].name;
        });
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.push_back({*it, si});
    }
    // Children were appended in push order; restore name order.
    for (SourceNode& s : st.nodes)
        std::sort(s.children.begin(), s.children.end(), [&](int a, int b) {
            const int c = up.upcase(st.nodes[size_t(a)].name).compare(up.upcase(st.nodes[size_t(b)].name));
            return c != 0 ? c < 0 : st.nodes[size_t(a)].name < st.nodes[size_t(b)].name;
        });

    WriterOptions w = o.writer;
    w.clusterBytes = int(geo.clusterBytes());
    w.sectorBytes = int(geo.sectorBytes());
    w.label = label;
    w.serial = serial;
    w.volumeBytes = geo.volumeBytes();
    auto plan = planImage(st, w);
    if (!plan.ok()) {
        w.volumeBytes = 0; // the edited contents no longer fit: grow
        plan = planImage(st, w);
        if (!plan.ok())
            return plan.takeError();
        if (log)
            log->info(QStringLiteral("The edited contents do not fit in %1; the image grows to %2 (%3).")
                          .arg(formatBytes(geo.volumeBytes()), formatBytes(plan->geo.volumeBytes()), plan->sizeChoice));
    }
    res.newVolumeBytes = plan->geo.volumeBytes();
    if (tooLargeForDrive(QFileInfo(image).absolutePath(), res.newVolumeBytes))
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("The edited image would be %1, and its drive uses FAT32, which cannot store files of "
                                    "4 GB or more. Move the image to an NTFS or exFAT drive first.")
                         .arg(formatBytes(res.newVolumeBytes)));
    // Made before the free-space check: it removes what an interrupted
    // rebuild of this image left, whose space the check must count as free.
    PartFile part(image);
    FW_TRY(ensureFreeSpace(QFileInfo(image).absolutePath(), res.newVolumeBytes, o.freeSpaceMarginBytes));
    if (!token.checkpoint())
        return Error::cancelled();

    ImageFileStream original(image, geo);
    FW_TRY(original.open());
    w.opener = [&](const SourceNode& n, const CancellationToken& t) -> Result<std::unique_ptr<NodeReader>> {
        if (n.imageEntry >= 0)
            return std::unique_ptr<NodeReader>(new ImageNodeReader(original, tree.entries[size_t(n.imageEntry)]));
        return openLocalNode(n, w, t);
    };
    if (o.onPhase)
        o.onPhase(1, st.totalBytes, st.fileCount, true);
    if (log)
        log->info(QStringLiteral("Rebuilding the image: %1, %2, %3")
                      .arg(counted(st.fileCount, "file", "files"), formatBytes(st.totalBytes), plan->sizeChoice));
    auto written = writeImage(st, plan.value(), part.partPath(), w, token, progress, log);
    original.close(); // Windows cannot rename a file that is still open
    if (!written.ok())
        return written.takeError();

    const auto expected = expectedFromModel(m, d, tree, false);
    VerifyOptions vo = o.verify;
    if (o.onPhase) {
        const auto onPhase = o.onPhase;
        vo.onCompareStart = [onPhase](qint64 total, qint64 files, bool bytes) { onPhase(2, total, files, bytes); };
    }
    bool cancelled = false;
    auto report = verifyEdited(part.partPath(), expected, vo, token, progress, log, cancelled, image, &geo);
    if (!report.ok())
        return report.takeError();
    res.verification = std::move(report).value();
    res.verificationCancelled = cancelled;
    if (!res.verification.passed()) {
        res.restored = true; // the original was never touched; the new image is discarded
        return res;
    }
    reader.reset(); // release the original before it is swapped (Windows)
    FW_TRY(commitOrKeep(part, true, cancelled ? QStringLiteral("The edited image was built")
                                              : QStringLiteral("The edited image was built and verified")));
    return res;
}

} // namespace

Result<bool> recoverInterruptedEdit(const QString& image, const Log* log)
{
    const QString journal = editJournalPath(image);
    const QStringList partJournals = partJournalsOf(journal);
    for (const QString& partJournal : partJournals) {
        if (!QFileInfo::exists(partJournal))
            continue;
        // The journal is written and flushed under its .part name, then
        // renamed before the image is touched. After a crash the rename may not
        // have reached the disk although image writes did, so a complete .part
        // journal is applied too: it holds the original bytes, so writing them
        // back is harmless if the image was never changed. An incomplete or
        // foreign one means the image was not modified. Only one can be an
        // edit's (an edit starts only when neither name is taken).
        if (!QFileInfo::exists(journal)) {
            bool writingBack = false;
            const Status st = rollbackJournal(image, partJournal, &writingBack);
            if (st.ok()) {
                for (const QString& other : partJournals)
                    QFile::remove(other);
                if (log)
                    log->warn(QStringLiteral("An earlier edit of %1 was interrupted while it started; undone from %2.")
                                  .arg(QDir::toNativeSeparators(image), QDir::toNativeSeparators(partJournal)));
                return true;
            }
            if (writingBack)
                return st.error();
        }
        QFile::remove(partJournal);
    }
    const QString marker = journalDoneMarker(journal);
    if (!QFileInfo::exists(journal)) {
        QFile::remove(marker); // nothing left to disown
        return false;
    }
    if (journalIsRetired(journal)) {
        // Left by an edit that was verified: it only needs removing. The
        // marker goes last, so a journal that is still held stays disowned.
        if (QFile::remove(journal)) {
            QFile::remove(marker);
            if (log)
                log->info(QStringLiteral("Removed %1, left by an earlier edit that was completed.")
                              .arg(QDir::toNativeSeparators(journal)));
        }
        return false;
    }
    if (log)
        log->warn(QStringLiteral("An earlier edit of %1 was interrupted; undoing it from %2.")
                      .arg(QDir::toNativeSeparators(image), QDir::toNativeSeparators(journal)));
    FW_TRY(rollbackJournal(image, journal));
    return true;
}

Result<EditResult> editImage(const QString& image, const std::vector<EditOp>& ops, const EditOptions& o,
                             const CancellationToken& token, ProgressReporter* progress, const Log* log)
{
    EditResult res;
    if (o.onPhase)
        o.onPhase(0, 0, 0, false);
    FW_TRY_ASSIGN(res.recoveredJournal, recoverInterruptedEdit(image, log));
    if (res.recoveredJournal)
        res.warnings << QStringLiteral("An earlier edit of this image had been interrupted and was undone first.");

    FW_TRY_ASSIGN(std::unique_ptr<ExfatReader> reader, ExfatReader::open(image));
    FW_TRY_ASSIGN(ImageTree tree, reader->readTree(token, true));
    FW_TRY(ExfatReader::requireIntact(tree));
    res.oldVolumeBytes = reader->geometry().volumeBytes();

    Model m(tree, reader->upcase());
    for (const EditOp& op : ops) {
        if (!token.checkpoint())
            return Error::cancelled();
        FW_TRY(applyOp(m, op, o.rules, token, res));
    }
    const Diff d = diffModel(m, tree);

    bool inPlace = o.strategy == EditStrategy::Auto && d.added.empty();
    const qint64 cb = reader->geometry().clusterBytes();
    for (int i : d.replaced) {
        const ImageEntry& e = tree.entries[size_t(i)];
        const qint64 size = m.nodes[size_t(i)].size;
        const bool fits = size == 0
            || (e.noFatChain && e.extents.size() == 1 && ceilDiv(size, cb) <= qint64(e.extents.front().count));
        if (!fits)
            inPlace = false;
    }
    if (log)
        log->info(QStringLiteral("Edit plan: %1 added, %2 replaced, %3 deleted -> %4")
                      .arg(d.added.size()).arg(d.replaced.size()).arg(d.removedTop.size())
                      .arg(inPlace ? QStringLiteral("in place") : QStringLiteral("rebuild")));
    if (d.added.empty() && d.replaced.empty() && d.removedTop.empty()) {
        res.changed = false;
        res.inPlace = true;
        res.newVolumeBytes = res.oldVolumeBytes;
        res.verification.full = o.verify.full;
        return res; // nothing changes (e.g. mkdir of an existing folder)
    }
    if (inPlace)
        return editInPlace(image, *reader, tree, m, d, o, token, progress, log, res);
    return editRebuild(image, std::move(reader), tree, m, d, o, token, progress, log, res);
}

} // namespace fw::exfat
