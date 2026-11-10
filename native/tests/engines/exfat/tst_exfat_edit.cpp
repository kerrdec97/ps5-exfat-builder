// Editing images: delete and fitting replacements in place (undo journal),
// adds and bigger replacements by rebuilding, growth when the image is full,
// cancellation and failed verification leaving the original byte-identical,
// and recovery of an interrupted in-place edit.

#include "ExfatJobEnv.h"
#include "ExfatTestUtil.h"

#include "engines/exfat/ExfatEdit.h"
#include "engines/exfat/ExfatVerify.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace fw;
using namespace fw::exfat;
using namespace exfattest;

class TestExfatEdit : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    JobEnv m_env;
    QString m_src;
    QString m_master; // pristine image of m_src
    int m_counter = 0;

    QString path(const QString& rel) const { return m_tmp.filePath(rel); }
    static QString err(const Error& e) { return e.toLogString(); }

    // A fresh copy of the master image.
    QString freshImage()
    {
        const QString p = path(QStringLiteral("edit-%1.exfat").arg(++m_counter));
        QFile::remove(p);
        if (!QFile::copy(m_master, p))
            return {};
        QFile(p).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return p;
    }

    static EditOp op(EditOp::Kind k, const QString& imagePath, const QString& local = {})
    {
        EditOp o;
        o.kind = k;
        o.imagePath = imagePath;
        o.localPath = local;
        return o;
    }

    static EditOptions options(bool full = true)
    {
        EditOptions o;
        o.verify.full = full;
        o.verify.threads = 4;
        o.writer.spare = SpareMode::Fixed;
        o.writer.spareBytes = 4 * kMiB;
        o.writer.syncToDisk = false;
        return o;
    }

    void expectIntact(const QString& image)
    {
        const Fsck f = runFsck(image);
        if (f.available)
            QVERIFY2(f.exitCode == 0 && f.output.contains(QStringLiteral("clean")), qPrintable(f.output));
        auto r = ExfatReader::open(image);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        CancellationToken token;
        auto t = r.value()->readTree(token);
        QVERIFY(t.ok());
        QVERIFY2(t->problems.isEmpty(), qPrintable(t->problems.join(u'\n')));
        QVERIFY2(t->warnings.isEmpty(), qPrintable(t->warnings.join(u'\n'))); // no lost clusters
    }

    ImageTree treeOf(const QString& image)
    {
        CancellationToken token;
        return ExfatReader::open(image).value()->readTree(token).value();
    }

    QByteArray contentOf(const QString& image, const QString& file)
    {
        auto r = ExfatReader::open(image);
        CancellationToken token;
        auto t = r.value()->readTree(token);
        const int i = t->find(file);
        if (i < 0)
            return QByteArray("<missing>");
        const ImageEntry& e = t->entries[size_t(i)];
        ImageFileStream s(image, r.value()->geometry());
        if (!s.open().ok())
            return {};
        QByteArray b(qsizetype(e.size), '\0');
        auto n = s.read(e, 0, b.data(), e.size);
        return n.ok() ? b : QByteArray();
    }

    // The image holds exactly `folder` (contents compared byte for byte).
    void expectMatchesFolder(const QString& image, const QString& folder)
    {
        CancellationToken token;
        VerifyOptions vo;
        vo.full = true;
        auto v = verifyImageAgainstFolder(image, folder, {}, vo, token);
        QVERIFY(v.ok());
        QVERIFY2(v->passed(), qPrintable(v->summary()));
    }

    // Manual cross-checks with other exFAT drivers: FW_EXFAT_KEEP_EDIT=<dir>.
    void keep(const QString& image, const QString& expected, const QString& name)
    {
        const QByteArray dir = qgetenv("FW_EXFAT_KEEP_EDIT");
        if (dir.isEmpty())
            return;
        const QString d = QString::fromLocal8Bit(dir) + u'/' + name;
        QDir(d).removeRecursively();
        QDir().mkpath(d);
        QFile::copy(image, d + QStringLiteral("/image.exfat"));
        QDir().rename(expected, d + QStringLiteral("/expected"));
    }

    // A copy of the source tree to model the expected result of an edit.
    QString mirror(const QString& name)
    {
        const QString dst = path(name);
        QDir(dst).removeRecursively();
        QDirIterator it(m_src, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
        QDir().mkpath(dst);
        while (it.hasNext()) {
            it.next();
            const QString rel = QDir(m_src).relativeFilePath(it.filePath());
            if (it.fileInfo().isDir())
                QDir().mkpath(dst + u'/' + rel);
            else
                QFile::copy(it.filePath(), dst + u'/' + rel);
        }
        return dst;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
        m_src = path(QStringLiteral("src"));
        makeSampleTree(m_src, 60);
        QVERIFY(writeRandom(m_src + QStringLiteral("/data/large.bin"), 24 * kMiB, 11));
        m_master = path(QStringLiteral("master.exfat"));
        WriterOptions o;
        o.spare = SpareMode::Fixed;
        o.spareBytes = 2 * kMiB; // nearly full: adds beyond 2 MiB must grow the image
        o.clusterBytes = 32768;
        QVERIFY(build(m_src, m_master, o).ok());
        expectIntact(m_master);
    }

    void parseOperations()
    {
        auto ops = parseEditOps(QJsonArray{
            QJsonObject{{QStringLiteral("op"), QStringLiteral("add")}, {QStringLiteral("imagePath"), QStringLiteral("a/b.bin")}, {QStringLiteral("localPath"), QStringLiteral("/x")}},
            QJsonObject{{QStringLiteral("op"), QStringLiteral("DELETE")}, {QStringLiteral("imagePath"), QStringLiteral("c")}}});
        QVERIFY(ops.ok());
        QCOMPARE(ops->size(), size_t(2));
        QCOMPARE(ops->at(1).kind, EditOp::Kind::Delete);
        QCOMPARE(parseEditOps(editOpsToJson(ops.value())).value().size(), size_t(2));
        QVERIFY(!parseEditOps({}).ok());
        QVERIFY(!parseEditOps(QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("rename")}}}).ok());
        QVERIFY(!parseEditOps(QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("replace")}, {QStringLiteral("imagePath"), QStringLiteral("a")}}}).ok());
    }

    void deleteIsDoneInPlace()
    {
        const QString img = freshImage();
        const qint64 size = QFileInfo(img).size();
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("data/medium.bin")),
                                 op(EditOp::Kind::Delete, QStringLiteral("MANY")), // case-insensitive, a folder tree
                                 op(EditOp::Kind::Delete, QStringLiteral("empty folder"))},
                           options(), token);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QVERIFY(r->inPlace);
        QCOMPARE(r->deleted, qint64(3));
        QVERIFY(r->verification.passed());
        QCOMPARE(QFileInfo(img).size(), size);
        QVERIFY(!QFileInfo::exists(editJournalPath(img)));
        expectIntact(img);
        const QString expected = mirror(QStringLiteral("exp-delete"));
        QVERIFY(QFile::remove(expected + QStringLiteral("/data/medium.bin")));
        QVERIFY(QDir(expected + QStringLiteral("/many")).removeRecursively());
        QVERIFY(QDir(expected).rmdir(QStringLiteral("empty folder")));
        expectMatchesFolder(img, expected);
        keep(img, expected, QStringLiteral("delete"));
    }

    void fittingReplacementIsDoneInPlace()
    {
        const QString img = freshImage();
        const qint64 size = QFileInfo(img).size();
        const QString smaller = path(QStringLiteral("smaller.bin"));
        QVERIFY(writeRandom(smaller, 2 * kMiB + 77, 21)); // replaces 24 MiB
        const QString empty = path(QStringLiteral("empty.bin"));
        QVERIFY(writeBytes(empty, {}));
        const QString same = path(QStringLiteral("same.bin"));
        QVERIFY(writeRandom(same, 700 * 1024, 22)); // same size as medium.bin
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Replace, QStringLiteral("data/large.bin"), smaller),
                                 op(EditOp::Kind::Replace, QStringLiteral("eboot.bin"), empty),
                                 op(EditOp::Kind::Replace, QStringLiteral("data/medium.bin"), same)},
                           options(), token);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QVERIFY(r->inPlace);
        QCOMPARE(r->replaced, qint64(3));
        QVERIFY2(r->verification.passed(), qPrintable(r->verification.summary()));
        QCOMPARE(QFileInfo(img).size(), size);
        expectIntact(img); // the freed tail clusters are free in the bitmap
        QCOMPARE(contentOf(img, QStringLiteral("data/large.bin")), readAllOf(smaller));
        QCOMPARE(contentOf(img, QStringLiteral("eboot.bin")), QByteArray());
        QCOMPARE(contentOf(img, QStringLiteral("data/medium.bin")), readAllOf(same));
        QCOMPARE(contentOf(img, QStringLiteral("data/big.bin")), readAllOf(m_src + QStringLiteral("/data/big.bin")));
        const QString expected = mirror(QStringLiteral("exp-replace"));
        QVERIFY(writeBytes(expected + QStringLiteral("/data/large.bin"), readAllOf(smaller)));
        QVERIFY(writeBytes(expected + QStringLiteral("/eboot.bin"), {}));
        QVERIFY(writeBytes(expected + QStringLiteral("/data/medium.bin"), readAllOf(same)));
        expectMatchesFolder(img, expected);
        keep(img, expected, QStringLiteral("replace"));
        // The entry took the replacement's modification time.
        const ImageTree t = treeOf(img);
        const ImageEntry& e = t.entries[size_t(t.find(QStringLiteral("data/large.bin")))];
        QVERIFY(qAbs(e.modified.msecsTo(QFileInfo(smaller).lastModified())) < 10);
    }

    void addsAndBiggerReplacementsRebuildAndGrow()
    {
        const QString img = freshImage();
        const ImageTree before = treeOf(img);
        auto reader = ExfatReader::open(img);
        const qint64 oldSize = reader.value()->geometry().volumeBytes();
        const quint32 serial = reader.value()->volume().boot.serial;
        reader.value().reset();

        const QString bigger = path(QStringLiteral("bigger.bin"));
        QVERIFY(writeRandom(bigger, 3 * kMiB, 31)); // eboot.bin was 15 bytes: needs more clusters
        const QString newFile = path(QStringLiteral("new data.bin"));
        QVERIFY(writeRandom(newFile, 20 * kMiB, 32)); // more than the 2 MiB free
        const QString folder = path(QStringLiteral("patch folder"));
        writeBytes(folder + QStringLiteral("/sub/one.txt"), "one");
        writeBytes(folder + QStringLiteral("/two.txt"), "two");
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Replace, QStringLiteral("eboot.bin"), bigger),
                                 op(EditOp::Kind::Add, QStringLiteral("added/deep/new data.bin"), newFile),
                                 op(EditOp::Kind::Add, QStringLiteral("sce_sys"), folder), // into an existing folder
                                 op(EditOp::Kind::Mkdir, QStringLiteral("brand new/folder")),
                                 op(EditOp::Kind::Delete, QStringLiteral("data/big.bin"))},
                           options(), token);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QVERIFY(!r->inPlace);
        QVERIFY2(r->verification.passed(), qPrintable(r->verification.summary()));
        QCOMPARE(r->added, qint64(3));
        QCOMPARE(r->replaced, qint64(1));
        QCOMPARE(r->deleted, qint64(1));
        QCOMPARE(r->oldVolumeBytes, oldSize);
        QVERIFY2(r->newVolumeBytes > oldSize, qPrintable(r->summary()));
        QVERIFY(r->summary().contains(QStringLiteral("grew")));
        QCOMPARE(QFileInfo(img).size(), r->newVolumeBytes);
        QVERIFY(!QFileInfo::exists(img + QStringLiteral(".part")));
        QVERIFY(!QFileInfo::exists(img + QStringLiteral(".old")));
        expectIntact(img);

        auto after = ExfatReader::open(img);
        QVERIFY(after.ok());
        QCOMPARE(after.value()->volume().label, QStringLiteral("OSFIMG"));
        QCOMPARE(after.value()->volume().boot.serial, serial);
        QCOMPARE(after.value()->geometry().clusterBytes(), 32768);
        after.value().reset();

        const QString expected = mirror(QStringLiteral("exp-rebuild"));
        QVERIFY(QFile::remove(expected + QStringLiteral("/eboot.bin")));
        QVERIFY(QFile::copy(bigger, expected + QStringLiteral("/eboot.bin")));
        writeBytes(expected + QStringLiteral("/added/deep/new data.bin"), readAllOf(newFile));
        writeBytes(expected + QStringLiteral("/sce_sys/patch folder/sub/one.txt"), "one");
        writeBytes(expected + QStringLiteral("/sce_sys/patch folder/two.txt"), "two");
        QDir().mkpath(expected + QStringLiteral("/brand new/folder"));
        QVERIFY(QFile::remove(expected + QStringLiteral("/data/big.bin")));
        expectMatchesFolder(img, expected);
        keep(img, expected, QStringLiteral("rebuild"));
        // Unchanged entries keep their times.
        const ImageTree t = treeOf(img);
        const ImageEntry& a = before.entries[size_t(before.find(QStringLiteral("data/medium.bin")))];
        const ImageEntry& b = t.entries[size_t(t.find(QStringLiteral("data/medium.bin")))];
        QCOMPARE(b.modified, a.modified);
        QCOMPARE(b.attributes, a.attributes);
    }

    // A rebuilt edit whose original is open in another program cannot replace
    // it; the finished, verified image is kept next to it.
    void rebuiltImageIsKeptWhenTheOriginalCannotBeReplaced()
    {
#ifndef Q_OS_WIN
        QSKIP("An open file can be replaced on this platform.");
#endif
        const QString img = freshImage();
        const QByteArray original = readAllOf(img);
        EditOptions o = options(false);
        o.strategy = EditStrategy::Rebuild;
        CancellationToken token;
        QFile holder(img);
        QVERIFY(holder.open(QIODevice::ReadOnly)); // opened without delete sharing
        auto r = editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("data/medium.bin"))}, o, token);
        holder.close();
        QVERIFY(!r.ok());
        const QFileInfo fi(img);
        const QString kept = fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(" (new).exfat"));
        QVERIFY2(r.error().message.contains(QStringLiteral("edited image was built and verified")), qPrintable(err(r.error())));
        QVERIFY2(r.error().message.contains(QDir::toNativeSeparators(kept)), qPrintable(err(r.error())));
        QCOMPARE(readAllOf(img), original);
        QVERIFY(!QFileInfo::exists(img + QStringLiteral(".part")));
        expectIntact(kept);
        QVERIFY(treeOf(kept).find(QStringLiteral("data/medium.bin")) < 0);
        QVERIFY(treeOf(kept).find(QStringLiteral("data/big.bin")) > 0);
    }

    void alwaysRebuildStrategy()
    {
        const QString img = freshImage();
        EditOptions o = options(false);
        o.strategy = EditStrategy::Rebuild;
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("data/medium.bin"))}, o, token);
        QVERIFY(r.ok());
        QVERIFY(!r->inPlace);
        QVERIFY(r->verification.passed());
        QCOMPARE(r->newVolumeBytes, r->oldVolumeBytes); // still fits: same size
        expectIntact(img);
        QVERIFY(treeOf(img).find(QStringLiteral("data/medium.bin")) < 0);
    }

    void invalidOperationsChangeNothing()
    {
        const QString img = freshImage();
        const QByteArray hash = fileHash(img);
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("data/medium.bin")),
                                 op(EditOp::Kind::Delete, QStringLiteral("no/such/file"))},
                           options(), token);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::NotFound);
        r = editImage(img, {op(EditOp::Kind::Add, QStringLiteral("eboot.bin"), m_src + QStringLiteral("/eboot.bin"))}, options(), token);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::AlreadyExists);
        r = editImage(img, {op(EditOp::Kind::Replace, QStringLiteral("data"), m_src + QStringLiteral("/eboot.bin"))}, options(), token);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::InvalidArgument);
        r = editImage(img, {op(EditOp::Kind::Add, QStringLiteral("bad:name.txt"), m_src + QStringLiteral("/eboot.bin"))}, options(), token);
        QVERIFY(!r.ok());
        r = editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("../x"))}, options(), token);
        QVERIFY(!r.ok());
        QCOMPARE(fileHash(img), hash);
    }

    void cancellationLeavesTheImageByteIdentical_data()
    {
        QTest::addColumn<bool>("inPlace");
        QTest::newRow("in place") << true;
        QTest::newRow("rebuild") << false;
    }
    void cancellationLeavesTheImageByteIdentical()
    {
        QFETCH(bool, inPlace);
        const QString img = freshImage();
        const QByteArray hash = fileHash(img);
        const QString local = path(QStringLiteral("cancel-%1.bin").arg(inPlace));
        QVERIFY(writeRandom(local, inPlace ? 20 * kMiB : 40 * kMiB, 41));
        const EditOp e = inPlace ? op(EditOp::Kind::Replace, QStringLiteral("data/large.bin"), local)
                                 : op(EditOp::Kind::Add, QStringLiteral("cancel.bin"), local);
        CancellationSource cancel;
        bool sawWriting = false;
        // After the undo journal: the cancel must roll back image writes.
        ProgressReporter progress([&](const ProgressSnapshot& s) {
            if (s.done > 4 * kMiB && s.phaseIndex == 1 && s.currentItem != QStringLiteral("Undo journal")) {
                sawWriting = true;
                cancel.cancel();
            }
        }, 0);
        progress.setPhases({QStringLiteral("Planning"), QStringLiteral("Writing"), QStringLiteral("Verifying")});
        EditOptions o = options();
        o.onPhase = [&](int phase, qint64 total, qint64 files, bool bytes) { progress.beginPhase(phase, total, files, bytes); };
        auto r = editImage(img, {e}, o, cancel.token(), &progress);
        QVERIFY(sawWriting);
        QVERIFY(!r.ok());
        QVERIFY2(r.error().isCancelled(), qPrintable(err(r.error())));
        QCOMPARE(fileHash(img), hash);
        QVERIFY(!QFileInfo::exists(editJournalPath(img)));
        QVERIFY(!QFileInfo::exists(img + QStringLiteral(".part")));
    }

    // The journal of a big in-place replacement is as large as the new file:
    // writing it reports progress and stops at Cancel, before the image is touched.
    void journalWritingShowsProgressAndCanBeCancelled()
    {
        const QString img = freshImage();
        const QByteArray hash = fileHash(img);
        const QString local = path(QStringLiteral("journal-cancel.bin"));
        QVERIFY(writeRandom(local, 20 * kMiB, 43));
        CancellationSource cancel;
        qint64 journalBytesSeen = 0;
        ProgressReporter progress([&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 1 && s.currentItem == QStringLiteral("Undo journal") && s.done > 4 * kMiB) {
                journalBytesSeen = s.done;
                QVERIFY(s.total >= 40 * kMiB); // journal and new data
                cancel.cancel();
            }
        }, 0);
        progress.setPhases({QStringLiteral("Planning"), QStringLiteral("Writing"), QStringLiteral("Verifying")});
        EditOptions o = options();
        o.onPhase = [&](int phase, qint64 total, qint64 files, bool bytes) { progress.beginPhase(phase, total, files, bytes); };
        auto r = editImage(img, {op(EditOp::Kind::Replace, QStringLiteral("data/large.bin"), local)}, o, cancel.token(), &progress);
        QVERIFY(journalBytesSeen > 4 * kMiB);
        QVERIFY2(journalBytesSeen < 20 * kMiB, "the cancel was not noticed while the journal was written");
        QVERIFY(!r.ok());
        QVERIFY2(r.error().isCancelled(), qPrintable(err(r.error())));
        QCOMPARE(fileHash(img), hash);
        QVERIFY(!QFileInfo::exists(editJournalPath(img)));
        QVERIFY(!QFileInfo::exists(editJournalPath(img) + QStringLiteral(".part")));
    }

    // After a verified edit the journal is deleted. When that fails because
    // another program holds the file, the journal is emptied, or (held
    // exclusively) disowned by a marker file: a later verify, extract or edit
    // must not undo the good edit.
    void verifiedEditSurvivesAJournalThatCannotBeDeleted_data()
    {
        QTest::addColumn<bool>("exclusive");
        QTest::newRow("held open: emptied") << false;
#ifdef Q_OS_WIN
        QTest::newRow("held exclusively: disowned") << true;
#endif
    }
    void verifiedEditSurvivesAJournalThatCannotBeDeleted()
    {
        QFETCH(bool, exclusive);
        const QString img = freshImage();
        const QString journal = editJournalPath(img);
        const QString marker = journal + QStringLiteral(".done");
        QFile holder;
#ifdef Q_OS_WIN
        HANDLE lock = INVALID_HANDLE_VALUE;
#endif
        EditOptions o = options();
        o.testBeforeJournalRemoval = [&](const QString& j) {
            if (!exclusive) {
                holder.setFileName(j);
                QVERIFY(holder.open(QIODevice::ReadOnly));
                return;
            }
#ifdef Q_OS_WIN
            lock = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(j).utf16()), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            QVERIFY(lock != INVALID_HANDLE_VALUE);
#endif
        };
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("data/medium.bin"))}, o, token);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QVERIFY(r->inPlace);
        QVERIFY(r->verification.passed());
        QVERIFY2(r->warnings.isEmpty(), qPrintable(r->warnings.join(u'\n')));
        const QByteArray edited = fileHash(img);
#ifdef Q_OS_WIN
        QVERIFY(QFileInfo::exists(journal)); // it really could not be deleted
        if (exclusive) {
            QVERIFY(QFileInfo(journal).size() > 0);
            QVERIFY(QFileInfo::exists(marker));
            // Still held (it cannot even be read): nothing is undone and the
            // journal stays disowned.
            auto held = recoverInterruptedEdit(img);
            QVERIFY(!held.ok() || !held.value());
            QCOMPARE(fileHash(img), edited);
            QVERIFY(QFileInfo::exists(marker));
            // Another edit meanwhile is refused before anything changes, and
            // leaves the marker in place.
            auto again = editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("eboot.bin"))}, options(), token);
            QVERIFY(!again.ok());
            QCOMPARE(fileHash(img), edited);
            QVERIFY(QFileInfo::exists(marker));
            CloseHandle(lock);
        } else {
            QCOMPARE(QFileInfo(journal).size(), qint64(0));
        }
#endif
        holder.close();
        auto rec = recoverInterruptedEdit(img);
        QVERIFY2(rec.ok(), qPrintable(rec.ok() ? QString() : err(rec.error())));
        QVERIFY(!rec.value());
        QCOMPARE(fileHash(img), edited);
        QVERIFY(!QFileInfo::exists(journal));
        QVERIFY(!QFileInfo::exists(marker));
        QVERIFY(treeOf(img).find(QStringLiteral("data/medium.bin")) < 0);
        expectIntact(img);

        // A marker left behind never disowns the journal of a later edit.
        QVERIFY(writeBytes(marker, QByteArray(32, 'x')));
        EditOptions crash = options();
        crash.testCrashAfterApply = true;
        QVERIFY(!editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("eboot.bin"))}, crash, token).ok());
        QVERIFY(!QFileInfo::exists(marker));
        auto undone = recoverInterruptedEdit(img);
        QVERIFY(undone.ok());
        QVERIFY(undone.value());
        QCOMPARE(fileHash(img), edited);
    }

    // Operations that cancel each other out write nothing and claim nothing.
    void operationsThatCancelOutAreNotReportedAsAnEdit()
    {
        const QString img = freshImage();
        const QByteArray hash = fileHash(img);
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Mkdir, QStringLiteral("made-then-gone")),
                                 op(EditOp::Kind::Delete, QStringLiteral("made-then-gone"))},
                           options(), token);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QVERIFY(!r->changed);
        QCOMPARE(r->summary(), QStringLiteral("No changes were needed"));
        QCOMPARE(fileHash(img), hash);

        const QJsonObject p{
            {QStringLiteral("image"), img},
            {QStringLiteral("operations"),
             QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("mkdir")}, {QStringLiteral("imagePath"), QStringLiteral("gone")}},
                        QJsonObject{{QStringLiteral("op"), QStringLiteral("delete")}, {QStringLiteral("imagePath"), QStringLiteral("gone")}}}}};
        auto job = m_env.run(kJobEdit, p);
        QVERIFY2(job.ok(), qPrintable(job.ok() ? QString() : err(job.error())));
        QCOMPARE(job->verification, Verification::NotRun);
        QCOMPARE(job->summary, QStringLiteral("No changes were needed"));
        QCOMPARE(fileHash(img), hash);
    }

    // What the exclusion rules leave out of an added folder is reported.
    void addingAFolderReportsWhatWasLeftOut()
    {
        const QString img = freshImage();
        const QString folder = path(QStringLiteral("add-with-rules"));
        QVERIFY(writeBytes(folder + QStringLiteral("/keep.txt"), "keep"));
        QVERIFY(writeBytes(folder + QStringLiteral("/skip.tmp"), "skip"));
        EditOptions o = options();
        o.rules.excludePatterns << QStringLiteral("*.tmp");
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Add, QStringLiteral("extra"), folder)}, o, token);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QVERIFY2(r->warnings.join(u'\n').contains(QStringLiteral("1 item in")), qPrintable(r->warnings.join(u'\n')));
        QVERIFY(r->warnings.join(u'\n').contains(QStringLiteral("left out")));
        const ImageTree t = treeOf(img);
        QVERIFY(t.find(QStringLiteral("extra/keep.txt")) > 0);
        QVERIFY(t.find(QStringLiteral("extra/skip.tmp")) < 0);
    }

    void failedVerificationKeepsTheOriginal_data()
    {
        QTest::addColumn<bool>("inPlace");
        QTest::newRow("in place") << true;
        QTest::newRow("rebuild") << false;
    }
    void failedVerificationKeepsTheOriginal()
    {
        QFETCH(bool, inPlace);
        const QString img = freshImage();
        const QByteArray hash = fileHash(img);
        const QString local = path(QStringLiteral("changing-%1.bin").arg(inPlace));
        QVERIFY(writeRandom(local, 1 * kMiB, 51));
        // 1 MiB fits large.bin's clusters (in place); a new file needs a rebuild.
        const EditOp e = inPlace ? op(EditOp::Kind::Replace, QStringLiteral("data/large.bin"), local)
                                 : op(EditOp::Kind::Add, QStringLiteral("changing.bin"), local);
        EditOptions o = options();
        // The local file changes (same size) after it was written into the
        // image but before verification compares it.
        o.onPhase = [&](int phase, qint64, qint64, bool) {
            if (phase == 2)
                flipByte(local, 12345);
        };
        CancellationToken token;
        auto r = editImage(img, {e}, o, token);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->inPlace, inPlace);
        QVERIFY(!r->verification.passed());
        QVERIFY(r->restored);
        QVERIFY(r->verification.summary().contains(QStringLiteral("differ")));
        QCOMPARE(fileHash(img), hash);
        QVERIFY(!QFileInfo::exists(editJournalPath(img)));
        QVERIFY(!QFileInfo::exists(img + QStringLiteral(".part")));
    }

    // A replacement that cannot be read halfway through the apply rolls the
    // image back byte for byte, even when the files written before it were
    // small (nothing may be left in a write buffer to land after the undo).
    void failedInPlaceApplyRestoresEveryByte()
    {
        for (int variant = 0; variant < 2; ++variant) {
            const QString img = freshImage();
            const QByteArray hash = fileHash(img);
            const quint16 flagsBefore = volumeFlagsOf(img);
            const QString small1 = path(QStringLiteral("small1-%1.bin").arg(variant));
            const QString small2 = path(QStringLiteral("small2-%1.bin").arg(variant));
            const QString vanishing = path(QStringLiteral("vanishing-%1.bin").arg(variant));
            QVERIFY(writeBytes(small1, QByteArray(99, 'a')));
            QVERIFY(writeBytes(small2, QByteArray(10, 'b')));
            QVERIFY(writeRandom(vanishing, 300 * 1024, 81));
            // Either order, so that small writes happen before the failure in one of them.
            const QString firstTarget = variant == 0 ? QStringLiteral("data/medium.bin") : QStringLiteral("data/large.bin");
            const QString lastTarget = variant == 0 ? QStringLiteral("data/large.bin") : QStringLiteral("data/medium.bin");
            EditOptions o = options();
            o.writer.openRetries = 0;
            o.writer.retryWaitMs = 0;
            o.onPhase = [&](int phase, qint64, qint64, bool) {
                if (phase == 1)
                    QFile::remove(vanishing);
            };
            CancellationToken token;
            auto r = editImage(img, {op(EditOp::Kind::Replace, firstTarget, small1),
                                     op(EditOp::Kind::Replace, QStringLiteral("eboot.bin"), small2),
                                     op(EditOp::Kind::Replace, lastTarget, vanishing)},
                               o, token);
            QVERIFY(!r.ok());
            QCOMPARE(fileHash(img), hash);
            QCOMPARE(volumeFlagsOf(img), flagsBefore);
            QVERIFY(!QFileInfo::exists(editJournalPath(img)));
            expectIntact(img);
        }
    }

    void successfulInPlaceEditLeavesTheVolumeClean()
    {
        const QString img = freshImage();
        const QString local = path(QStringLiteral("clean.bin"));
        QVERIFY(writeBytes(local, QByteArray(1234, 'c')));
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Replace, QStringLiteral("data/medium.bin"), local)}, options(), token);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QVERIFY(r->inPlace);
        QVERIFY(r->verification.passed());
        QCOMPARE(volumeFlagsOf(img) & kVolumeDirty, 0);
        QVERIFY(treeOf(img).warnings.join(u' ').indexOf(QStringLiteral("VolumeDirty")) < 0);
    }

    static quint16 volumeFlagsOf(const QString& img)
    {
        const QByteArray b = readAllOf(img).left(512);
        return quint16(uchar(b[int(kVolumeFlagsOffset)]) | (uchar(b[int(kVolumeFlagsOffset) + 1]) << 8));
    }

    void interruptedInPlaceEditIsRolledBack()
    {
        const QString img = freshImage();
        const QByteArray hash = fileHash(img);
        const QString local = path(QStringLiteral("crash.bin"));
        QVERIFY(writeRandom(local, 5 * kMiB, 61));
        EditOptions o = options();
        o.testCrashAfterApply = true;
        CancellationToken token;
        auto r = editImage(img, {op(EditOp::Kind::Replace, QStringLiteral("data/large.bin"), local),
                                 op(EditOp::Kind::Delete, QStringLiteral("many/sub3"))},
                           o, token);
        QVERIFY(!r.ok()); // "crashed"
        QVERIFY(QFileInfo::exists(editJournalPath(img)));
        QVERIFY(fileHash(img) != hash);
        // A damaged journal is refused and the image is left alone.
        const QString journal = editJournalPath(img);
        const QByteArray saved = readAllOf(journal);
        flipByte(journal, saved.size() / 2);
        auto bad = recoverInterruptedEdit(img);
        QVERIFY(!bad.ok());
        QCOMPARE(bad.error().code, ErrorCode::CorruptData);
        QVERIFY(writeBytes(journal, saved));
        // The next edit (or an explicit recovery) undoes it first.
        auto rec = recoverInterruptedEdit(img);
        QVERIFY(rec.ok());
        QVERIFY(rec.value());
        QCOMPARE(fileHash(img), hash);
        QVERIFY(!QFileInfo::exists(journal));
        auto none = recoverInterruptedEdit(img);
        QVERIFY(none.ok());
        QVERIFY(!none.value());
        // Through editImage: a crashed edit, then a normal one.
        o.testCrashAfterApply = true;
        QVERIFY(!editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("eboot.bin"))}, o, token).ok());
        o.testCrashAfterApply = false;
        auto next = editImage(img, {op(EditOp::Kind::Delete, QStringLiteral("data/big.bin"))}, o, token);
        QVERIFY(next.ok());
        QVERIFY(next->recoveredJournal);
        const ImageTree t = treeOf(img);
        QVERIFY(t.find(QStringLiteral("eboot.bin")) > 0); // the crashed delete was undone
        QVERIFY(t.find(QStringLiteral("data/big.bin")) < 0);
        expectIntact(img);
    }

    void journalsBelongToOneImage()
    {
        const QString local = path(QStringLiteral("stale.bin"));
        QVERIFY(writeRandom(local, 3 * kMiB, 71));
        EditOptions crash = options();
        crash.testCrashAfterApply = true;
        CancellationToken token;

        // The verify job undoes an interrupted edit before reading the image.
        const QString a = freshImage();
        const QByteArray hashA = fileHash(a);
        QVERIFY(!editImage(a, {op(EditOp::Kind::Replace, QStringLiteral("data/large.bin"), local)}, crash, token).ok());
        QVERIFY(fileHash(a) != hashA);
        auto v = m_env.run(kJobVerify, {{QStringLiteral("image"), a}, {QStringLiteral("source"), m_src}});
        QVERIFY2(v.ok(), qPrintable(v.ok() ? QString() : err(v.error())));
        QCOMPARE(v->verification, Verification::Passed);
        QVERIFY(v->warnings.join(u' ').contains(QStringLiteral("undone")));
        QCOMPARE(fileHash(a), hashA);

        // Building a new image over it discards the old image's journal.
        const QString b = freshImage();
        QVERIFY(!editImage(b, {op(EditOp::Kind::Delete, QStringLiteral("eboot.bin"))}, crash, token).ok());
        QVERIFY(QFileInfo::exists(editJournalPath(b)));
        auto built = m_env.run(kJobBuild, {{QStringLiteral("source"), m_src}, {QStringLiteral("output"), b}});
        QVERIFY2(built.ok(), qPrintable(built.ok() ? QString() : err(built.error())));
        QVERIFY(!QFileInfo::exists(editJournalPath(b)));

        // A journal is never applied to a different image of the same size.
        const QString c = freshImage();
        QVERIFY(!editImage(c, {op(EditOp::Kind::Delete, QStringLiteral("eboot.bin"))}, crash, token).ok());
        const QByteArray journal = readAllOf(editJournalPath(c));
        {
            CancellationToken t;
            auto tree = scanSource(m_src, {}, t);
            QVERIFY(tree.ok());
            WriterOptions w;
            w.volumeBytes = QFileInfo(c).size();
            w.clusterBytes = 32768;
            w.label = QStringLiteral("OTHER");
            w.serial = 0x0BADF00D;
            w.syncToDisk = false;
            auto plan = planImage(tree.value(), w);
            QVERIFY(plan.ok());
            QVERIFY(writeImage(tree.value(), plan.value(), c, w, t).ok());
        }
        QVERIFY(writeBytes(editJournalPath(c), journal));
        const QByteArray hashC = fileHash(c);
        auto rec = recoverInterruptedEdit(c);
        QVERIFY(!rec.ok());
        QCOMPARE(rec.error().code, ErrorCode::CorruptData);
        QVERIFY2(rec.error().message.contains(QStringLiteral("does not belong")), qPrintable(rec.error().message));
        QCOMPARE(fileHash(c), hashC);
        QFile::remove(editJournalPath(c));
    }

    void editJobThroughTheFactory()
    {
        const QString img = freshImage();
        const QString local = path(QStringLiteral("job-add.txt"));
        writeBytes(local, "added by the job");
        const QJsonObject p{
            {QStringLiteral("image"), img},
            {QStringLiteral("operations"),
             QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("add")},
                                    {QStringLiteral("imagePath"), QStringLiteral("notes/job-add.txt")},
                                    {QStringLiteral("localPath"), local}},
                        QJsonObject{{QStringLiteral("op"), QStringLiteral("delete")},
                                    {QStringLiteral("imagePath"), QStringLiteral("data/empty.dat")}}}}};
        auto r = m_env.run(kJobEdit, p);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->verification, Verification::Passed);
        QCOMPARE(r->outputs, QStringList{img});
        QVERIFY2(r->summary.contains(QStringLiteral("1 file added")), qPrintable(r->summary));
        QCOMPARE(contentOf(img, QStringLiteral("notes/job-add.txt")), QByteArray("added by the job"));
        expectIntact(img);

        // In place through the job (delete only), and the rebuild strategy setting.
        auto r2 = m_env.run(kJobEdit, {{QStringLiteral("image"), img},
                                       {QStringLiteral("operations"), QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("delete")},
                                                                                              {QStringLiteral("imagePath"), QStringLiteral("notes")}}}}});
        QVERIFY(r2.ok());
        QVERIFY(r2->details.value(QStringLiteral("inPlace")).toBool());
        auto r3 = m_env.run(kJobEdit, {{QStringLiteral("image"), img},
                                       {QStringLiteral("operations"), QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("delete")},
                                                                                              {QStringLiteral("imagePath"), QStringLiteral("eboot.bin")}}}}},
                            {{keys::kEditStrategy, QStringLiteral("rebuild")}});
        QVERIFY(r3.ok());
        QVERIFY(!r3->details.value(QStringLiteral("inPlace")).toBool());
        expectIntact(img);

        // Validation: missing local file.
        auto bad = m_env.run(kJobEdit, {{QStringLiteral("image"), img},
                                        {QStringLiteral("operations"), QJsonArray{QJsonObject{{QStringLiteral("op"), QStringLiteral("add")},
                                                                                               {QStringLiteral("imagePath"), QStringLiteral("x")},
                                                                                               {QStringLiteral("localPath"), path(QStringLiteral("missing"))}}}}});
        QVERIFY(!bad.ok());
        QCOMPARE(bad.error().code, ErrorCode::NotFound);
    }
};

QTEST_GUILESS_MAIN(TestExfatEdit)
#include "tst_exfat_edit.moc"
