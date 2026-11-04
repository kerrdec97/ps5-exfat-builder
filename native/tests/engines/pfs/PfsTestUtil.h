#pragma once

// Shared helpers for the PFS tests: the deterministic source-tree generator
// used by native/tests/data/pfs/make_golden.py (SplitMix64; must stay
// byte-identical to the Python version), spec loading and hashing.

#include "support/TestSupport.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdint>

namespace pfstest {

struct SplitMix64 {
    quint64 state;
    explicit SplitMix64(quint64 seed) : state(seed) {}
    quint64 next()
    {
        state += 0x9E3779B97F4A7C15ULL;
        quint64 z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

inline QByteArray genText(SplitMix64& rng, qint64 n)
{
    static const char* words[8] = {"alpha ", "beta ", "gamma ", "delta ", "lorem ", "ipsum ", "PS5 ", "\n"};
    QByteArray out;
    out.reserve(qsizetype(n + 8));
    while (out.size() < n)
        out.append(words[rng.next() % 8]);
    out.truncate(qsizetype(n));
    return out;
}

inline QByteArray genRandom(SplitMix64& rng, qint64 n)
{
    QByteArray out(qsizetype((n + 7) / 8 * 8), Qt::Uninitialized);
    for (qint64 i = 0; i < out.size(); i += 8) {
        const quint64 v = rng.next();
        for (int b = 0; b < 8; ++b)
            out[qsizetype(i + b)] = char((v >> (8 * b)) & 0xFF);
    }
    out.truncate(qsizetype(n));
    return out;
}

inline QByteArray genContent(const QString& kind, qint64 size, quint64 seed)
{
    SplitMix64 rng(seed);
    if (kind == u"text")
        return genText(rng, size);
    if (kind == u"random")
        return genRandom(rng, size);
    if (kind == u"zeros")
        return QByteArray(qsizetype(size), '\0');
    if (kind == u"mixed") {
        QByteArray out;
        for (qint64 c = 0; c * 65536 < size; ++c) {
            const qint64 n = std::min<qint64>(65536, size - c * 65536);
            out += (c % 2 == 0) ? genText(rng, n) : genRandom(rng, n);
        }
        return out;
    }
    if (kind == u"sparse") {
        QByteArray out(qsizetype(size), '\0');
        for (qint64 pos = 0; pos < size; pos += 4096)
            out[qsizetype(pos)] = char(rng.next() & 0xFF);
        return out;
    }
    qFatal("unknown generator kind");
    return {};
}

struct TreeItem {
    QString path;
    QString kind;
    qint64 size = 0;
    quint64 seed = 0;
};

inline std::vector<TreeItem> expandTree(const QJsonArray& tree)
{
    std::vector<TreeItem> out;
    for (const auto& v : tree) {
        const QJsonObject e = v.toObject();
        if (e.value(QStringLiteral("generate")).toString() == u"many") {
            const int count = e.value(QStringLiteral("count")).toInt();
            for (int i = 0; i < count; ++i) {
                TreeItem t;
                t.path = QStringLiteral("%1/%2%3%4")
                             .arg(e.value(QStringLiteral("dir")).toString(), e.value(QStringLiteral("prefix")).toString())
                             .arg(i, 3, 10, QLatin1Char('0'))
                             .arg(e.value(QStringLiteral("suffix")).toString());
                t.kind = e.value(QStringLiteral("kind")).toString();
                t.size = e.value(QStringLiteral("size")).toInteger() + (i % 7) * 13;
                t.seed = quint64(e.value(QStringLiteral("seed")).toInteger() + i);
                out.push_back(t);
            }
            continue;
        }
        TreeItem t;
        t.path = e.value(QStringLiteral("path")).toString();
        t.kind = e.value(QStringLiteral("kind")).toString();
        t.size = e.value(QStringLiteral("size")).toInteger();
        t.seed = quint64(e.value(QStringLiteral("seed")).toInteger());
        out.push_back(t);
    }
    return out;
}

inline bool setMtime(const QString& path, qint64 secs)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    return f.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime);
}

inline bool writeFile(const QString& path, const QByteArray& data, qint64 mtime)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size())
            return false;
    }
    return mtime < 0 || setMtime(path, mtime);
}

inline bool makeTree(const QString& root, const QJsonArray& tree, qint64 mtime)
{
    QDir(root).removeRecursively();
    QDir().mkpath(root);
    for (const TreeItem& t : expandTree(tree)) {
        QString rel = t.path;
        if (t.kind == u"dir") {
            while (rel.endsWith(u'/'))
                rel.chop(1);
            if (!QDir().mkpath(root + u'/' + rel))
                return false;
            continue;
        }
        if (!writeFile(root + u'/' + rel, genContent(t.kind, t.size, t.seed), mtime))
            return false;
    }
    return true;
}

inline QJsonObject loadJson(const QString& name)
{
    QFile f(fwtest::dataDir() + QStringLiteral("/pfs/") + name);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

inline QString sha256(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

inline QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace pfstest
