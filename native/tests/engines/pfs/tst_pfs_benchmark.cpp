// PFS throughput benchmark: the native builder with one thread and with every
// core on the same input. Logs MB/s (source bytes per second) and checks that
// both produce the same bytes. Size, level and repeats: FW_PFS_BENCH_MIB
// (default 24), FW_PFS_BENCH_LEVEL (default 9, the app's default),
// FW_PFS_BENCH_REPEAT (default 1; the best run is kept, which evens out a busy
// machine).
//
// With FW_LAZY_MKPFS_DIR naming a checkout of Lazy_MkPFS (it is not part of
// this repository; the references were made with commit 78bdd00) and a
// python3 that has the "cryptography" package, the same input is also built
// with Lazy_MkPFS, timed and compared byte for byte. Without them that part
// is left out and the log says why.

#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsSource.h"

#include "engines/pfs/PfsTestUtil.h"
#include "support/TestSupport.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <optional>

using namespace fw;
using namespace fw::pfs;

namespace {

constexpr qint64 kTime = 1700000000;

qint64 envInt(const char* name, qint64 fallback)
{
    bool ok = false;
    const qint64 v = qEnvironmentVariable(name).toLongLong(&ok);
    return ok && v > 0 ? v : fallback;
}

double mbps(qint64 bytes, double seconds) { return seconds > 0 ? double(bytes) / (1024.0 * 1024.0) / seconds : 0; }

// Writes `size` bytes of game-like data (alternating compressible / incompressible
// 64 KiB blocks) in 16 MiB pieces.
bool writeMixed(const QString& path, qint64 size, quint64 seed)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const qint64 piece = 16 * 1024 * 1024;
    for (qint64 off = 0, i = 0; off < size; off += piece, ++i) {
        const QByteArray b = pfstest::genContent(QStringLiteral("mixed"), std::min(piece, size - off), seed + quint64(i));
        if (f.write(b) != b.size())
            return false;
    }
    return true;
}

// The Lazy_MkPFS comparison: the interpreter to run it with, or why it cannot
// run here. Python is only looked for (and started) when the reference
// sources were named, so a plain test run starts no outside program.
struct Lazy {
    QString python;
    QString whyNot;
};

Lazy findLazy()
{
    Lazy lazy;
    const QString dir = qEnvironmentVariable("FW_LAZY_MKPFS_DIR");
    if (dir.isEmpty()) {
        lazy.whyNot = QStringLiteral("the Lazy_MkPFS sources are not part of this repository and FW_LAZY_MKPFS_DIR does not name a copy");
        return lazy;
    }
    if (!QFileInfo(dir + QStringLiteral("/main_file.py")).isFile()) {
        lazy.whyNot = QStringLiteral("FW_LAZY_MKPFS_DIR (%1) holds no main_file.py").arg(dir);
        return lazy;
    }
    for (const char* name : {"python3", "python"}) {
        const QString p = QStandardPaths::findExecutable(QLatin1String(name));
        if (p.isEmpty())
            continue;
        QProcess probe;
        probe.start(p, {QStringLiteral("-I"), QStringLiteral("-c"), QStringLiteral("import cryptography, zlib")});
        if (probe.waitForFinished(30000) && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0) {
            lazy.python = p;
            return lazy;
        }
    }
    lazy.whyNot = QStringLiteral("no python3 with the cryptography package was found");
    return lazy;
}

// Seconds Lazy_MkPFS took, or nothing with the reason in `whyNot`.
std::optional<double> runLazy(const QString& python, const QString& mode, const QString& src, const QString& out, int level,
                              int cpu, QString* whyNot)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(python, {QStringLiteral("-I"), fwtest::dataDir() + QStringLiteral("/pfs/bench_lazy.py"), mode, src, out,
                     QString::number(level), QString::number(cpu), QString::number(kTime)});
    if (!p.waitForFinished(60 * 60 * 1000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        *whyNot = QStringLiteral("Lazy_MkPFS failed: ") + QString::fromUtf8(p.readAll().right(2000));
        return std::nullopt;
    }
    const QString text = QString::fromUtf8(p.readAll());
    const qsizetype at = text.lastIndexOf(QStringLiteral("seconds="));
    if (at < 0) {
        *whyNot = QStringLiteral("bench_lazy.py printed no time: ") + text.right(2000);
        return std::nullopt;
    }
    return text.mid(at + 8).trimmed().section(u'\n', 0, 0).toDouble();
}

struct NativeRun {
    QString error; // empty when the image was written
    double seconds = 0;
    QString sha;
    BuildStats stats;
};

NativeRun runNative(const BuildInput& in, int threads, int level, const QString& out)
{
    BuildOptions o;
    o.layout.time = kTime;
    o.level = level;
    o.threads = threads;
    QElapsedTimer t;
    t.start();
    auto st = writeImage(in, OutputFormat::Container, o, out, CancellationToken());
    NativeRun r;
    r.seconds = double(t.nsecsElapsed()) / 1e9;
    if (!st.ok()) {
        r.error = st.error().toLogString();
        return r;
    }
    r.stats = *st;
    r.sha = pfstest::sha256(out);
    if (r.sha.isEmpty())
        r.error = QStringLiteral("the image could not be read back: ") + out;
    return r;
}

} // namespace

class TestPfsBenchmark : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_appData.isValid() && m_work.isValid());
        fwtest::isolateAppData(m_appData);
        m_mib = envInt("FW_PFS_BENCH_MIB", 24);
        m_level = int(envInt("FW_PFS_BENCH_LEVEL", 9));
        m_repeat = int(envInt("FW_PFS_BENCH_REPEAT", 1));
        m_cores = std::max(1, QThread::idealThreadCount());
        m_lazy = findLazy();
    }

    void benchmark_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("image to ffpfsc") << QStringLiteral("file");
        QTest::newRow("folder to ffpfsc") << QStringLiteral("folder");
    }

    void benchmark()
    {
        QFETCH(QString, mode);
        const qint64 bytes = m_mib * 1024 * 1024;
        BuildInput in;
        QString src;
        if (mode == u"file") {
            src = m_work.filePath(QStringLiteral("PPSA00001.exfat"));
            QVERIFY(writeMixed(src, bytes, 1000));
            in.isFolder = false;
            in.file = src;
        } else {
            src = m_work.filePath(QStringLiteral("game"));
            QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), pfstest::genContent(QStringLiteral("random"), 300000, 1), -1));
            QVERIFY(pfstest::writeFile(src + QStringLiteral("/sce_sys/param.json"), pfstest::genContent(QStringLiteral("text"), 2000, 2), -1));
            for (int i = 0; i < 8; ++i)
                QVERIFY(writeMixed(src + QStringLiteral("/data/pack%1.bin").arg(i), bytes / 8, quint64(2000 + i * 100)));
            PlanOptions po;
            po.time = kTime;
            auto plan = planFolder(src, po, CancellationToken());
            QVERIFY(plan.ok());
            in.plan = std::move(plan).value();
        }

        const NativeRun one = runNative(in, 1, m_level, m_work.filePath(QStringLiteral("n1.ffpfsc")));
        QVERIFY2(one.error.isEmpty(), qPrintable(one.error));
        const QString pyOut = m_work.filePath(QStringLiteral("lazy.ffpfsc"));
        NativeRun many;
        std::optional<double> py;
        QString noLazy = m_lazy.whyNot;
        for (int r = 0; r < m_repeat; ++r) {
            const NativeRun n = runNative(in, m_cores, m_level, m_work.filePath(QStringLiteral("nn.ffpfsc")));
            QVERIFY2(n.error.isEmpty(), qPrintable(n.error));
            if (r == 0 || n.seconds < many.seconds)
                many = n;
            if (m_lazy.python.isEmpty())
                continue;
            const auto p = runLazy(m_lazy.python, mode, src, pyOut, m_level, m_cores, &noLazy);
            // The reference was asked for by name: when it cannot run, that is a failure, not a quiet pass.
            QVERIFY2(p.has_value(), qPrintable(noLazy));
            if (!py || *p < *py)
                py = p;
        }
        QCOMPARE(one.sha, many.sha);
        qInfo("writer thread with %d threads: read %.2f s, waiting %.2f s, write %.2f s", m_cores,
              many.stats.readSeconds, many.stats.waitSeconds, many.stats.writeSeconds);
        QString line = QStringLiteral("PFS benchmark, %1, %2 MiB, level %3: native 1 thread %4 MB/s (%5 s), native %6 threads "
                                      "%7 MB/s (%8 s, best of %9)")
                           .arg(mode == u"file" ? QStringLiteral("image -> .ffpfsc") : QStringLiteral("folder -> .ffpfsc"))
                           .arg(m_mib)
                           .arg(m_level)
                           .arg(mbps(bytes, one.seconds), 0, 'f', 1)
                           .arg(one.seconds, 0, 'f', 2)
                           .arg(m_cores)
                           .arg(mbps(bytes, many.seconds), 0, 'f', 1)
                           .arg(many.seconds, 0, 'f', 2)
                           .arg(m_repeat);

        if (py) {
            line += QStringLiteral("; Lazy_MkPFS --cpu-count %1: %2 MB/s (%3 s)")
                        .arg(m_cores)
                        .arg(mbps(bytes, *py), 0, 'f', 1)
                        .arg(*py, 0, 'f', 2);
            if (bytes < 256LL * 1024 * 1024)
                line += QStringLiteral(" [Lazy compresses one file on one core below 256 MiB]");
            const QString pySha = pfstest::sha256(pyOut);
            line += pySha == many.sha ? QStringLiteral("; outputs identical") : QStringLiteral("; OUTPUTS DIFFER");
            qInfo("%s", qPrintable(line));
            QCOMPARE(pySha, many.sha);
        } else {
            qInfo("%s", qPrintable(line));
            qInfo("Lazy_MkPFS comparison left out: %s", qPrintable(noLazy));
        }
    }

private:
    QTemporaryDir m_appData;
    QTemporaryDir m_work;
    Lazy m_lazy;
    qint64 m_mib = 24;
    int m_level = 9;
    int m_repeat = 1;
    int m_cores = 1;
};

QTEST_GUILESS_MAIN(TestPfsBenchmark)
#include "tst_pfs_benchmark.moc"
