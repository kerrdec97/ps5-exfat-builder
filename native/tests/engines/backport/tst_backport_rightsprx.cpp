// Standard right.sprx substitute (docs/BACKPORT_PARITY.md matrix #35,
// RightSprx.h), ported from the owner's Backport engine (make_baseline.py,
// test_pssr2_release_fixes.py RightSprxSubstitute): a game-root
// sce_sys/about/right.sprx that cannot be converted takes the user's standard
// copy, converted the normal way; the dump's copy is kept in the originals
// ZIP and Restore puts it back. Sony's file is never used here: a synthetic
// SELF stands in for it, with its own hash passed through the test seam.

#include "BackportTestEnv.h"

#include "core/Zip.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/BackportPlanner.h"
#include "engines/backport/RestoreService.h"
#include "engines/backport/RightSprx.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SelfReader.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

const Issue* findIssue(const BackportPlan& p, Code c, Severity s)
{
    for (const Issue& i : p.issues)
        if (i.code == c && i.severity == s)
            return &i;
    return nullptr;
}

const Issue* infoMentioning(const BackportPlan& p, const QString& text)
{
    for (const Issue& i : p.issues)
        if (i.severity == Severity::Info && i.message.contains(text))
            return &i;
    return nullptr;
}

struct Standard {
    QString path;
    QString sha;
};

Standard writeStandard(const QString& dir)
{
    Standard s;
    s.path = dir + "/standard/right.sprx";
    const QByteArray bytes = selfOf(f2(99));
    writeBytes(s.path, bytes);
    s.sha = QString::fromLatin1(sha(bytes));
    return s;
}

} // namespace

class TstBackportRightSprx : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void pinAndPath()
    {
        QVERIFY(isRightSprx(QStringLiteral("sce_sys/about/right.sprx")));
        QVERIFY(isRightSprx(QStringLiteral("SCE_SYS/About/Right.SPRX")));
        QVERIFY(!isRightSprx(QStringLiteral("sub/sce_sys/about/right.sprx"))); // game root only
        QVERIFY(!isRightSprx(QStringLiteral("sce_sys/about/left.sprx")));
        QCOMPARE(BackportOptions().standardRightSprxSha256, QString::fromLatin1(kStandardRightSprxSha256));
        // Only the pinned file passes by default: a synthetic one does not.
        const Standard s = writeStandard(m_root.path());
        QVERIFY(standardRightSprxProblem(s.path).contains(QStringLiteral("is not the standard right.sprx")));
        QVERIFY(standardRightSprxProblem(s.path, s.sha).isEmpty());
        QVERIFY(!standardRightSprxProblem(m_root.filePath(QStringLiteral("missing.sprx"))).isEmpty());
        QVERIFY(!standardRightSprxProblem(QString()).isEmpty());
    }

    void withoutAStandardCopyTheRunIsRefusedWithAHint()
    {
        Env e(EncryptedRight);
        auto p = planBackport(e.options(), CancellationToken());
        QVERIFY(p.ok());
        QVERIFY(findIssue(p.value(), Code::SelfEncrypted, Severity::Refuse));
        QVERIFY(infoMentioning(p.value(), QStringLiteral("Standard right.sprx")));
        QVERIFY(p->rightSprxSubstitute.isEmpty());
    }

    void aWrongFileIsRefused()
    {
        Env e(EncryptedRight);
        const Standard s = writeStandard(e.tmp.path());
        auto o = e.options();
        o.standardRightSprx = s.path; // the default pin: not this synthetic file
        auto p = planBackport(o, CancellationToken());
        QVERIFY(p.ok());
        QVERIFY(findIssue(p.value(), Code::RightSprxInvalid, Severity::Refuse));
        QVERIFY(!p->canRun());
    }

    void theStandardCopyTakesItsPlaceAndRestoreBringsTheDumpsCopyBack()
    {
        Env e(EncryptedRight);
        const auto before = snapshotTree(e.game);
        const Standard s = writeStandard(e.tmp.path());
        auto o = e.options();
        o.standardRightSprx = s.path;
        o.standardRightSprxSha256 = s.sha;
        auto p = planBackport(o, CancellationToken());
        QVERIFY(p.ok());
        QCOMPARE(p->rightSprxSubstitute, QStringLiteral("sce_sys/about/right.sprx"));
        QVERIFY(p->canRun());
        QVERIFY(p->leaveUnchanged.isEmpty());

        auto r = runBackport(o, e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QCOMPARE(r->status, RunStatus::Succeeded); // not Partial: nothing was left encrypted
        // The game now has the standard copy, converted to SDK 4.
        auto u = selfio::unwrap(readBytes(e.game + "/sce_sys/about/right.sprx"));
        QVERIFY(u.ok());
        QCOMPARE(elfSdkMajor(u.value()).value_or(-1), 4);
        // The originals ZIP keeps the dump's own copy byte for byte.
        zip::Reader z;
        QVERIFY(z.open(r->originalsZip).ok());
        QVERIFY(z.find("sce_sys/about/right.sprx"));
        QCOMPARE(z.readAll(*z.find("sce_sys/about/right.sprx")).value(), before.value(QStringLiteral("sce_sys/about/right.sprx")));
        // The job details and the run report say what happened.
        bool flagged = false;
        for (const QJsonValue& f : r->details.value(QStringLiteral("files")).toArray())
            if (f.toObject().value(QStringLiteral("file")).toString() == QLatin1String(kRightSprxRel))
                flagged = f.toObject().value(QStringLiteral("substitutedStandard")).toBool();
        QVERIFY(flagged);
        QFile report(r->reportJson);
        QVERIFY(report.open(QIODevice::ReadOnly));
        QVERIFY(report.readAll().contains("the standard right.sprx was converted"));
        // Restore puts the dump's copy back.
        auto rep = restoreOriginals({e.game, r->originalsZip, e.orig}, CancellationToken());
        QVERIFY2(rep.ok() && rep->ok(), qPrintable(rep.ok() ? rep->errors.join(u'|') : rep.error().toLogString()));
        QCOMPARE(snapshotTree(e.game), before);
    }

    void theStandardCopyChangingBeforeTheBuildFailsSafely()
    {
        Env e(EncryptedRight);
        const auto before = snapshotTree(e.game);
        const Standard s = writeStandard(e.tmp.path());
        auto o = e.options();
        o.standardRightSprx = s.path;
        o.standardRightSprxSha256 = s.sha;
        o.testHook = [&s](const QString& point, const QString&) -> Status {
            if (point == u"build.file")
                writeBytes(s.path, selfOf(f2(98))); // swapped after the preflight
            return success();
        };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::RightSprxInvalid);
        QCOMPARE(snapshotTree(e.game), before);
    }
};

QTEST_GUILESS_MAIN(TstBackportRightSprx)
#include "tst_backport_rightsprx.moc"
