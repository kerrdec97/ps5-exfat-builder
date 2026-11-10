#include "engines/fpkg/PackageInspect.h"

#include "core/FileUtil.h"
#include "engines/fpkg/ParamJson.h"

#include <QFile>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace fw::fpkg {

namespace {

constexpr qint64 kFirstWindow = 64LL * 1024 * 1024;
constexpr qint64 kMaxWindow = 1024LL * 1024 * 1024;
constexpr qint64 kBlock = 8LL * 1024 * 1024;
constexpr int kMaxObject = 65536;
// Work limits of the param.json search, in bytes looked at. A real package
// costs a few hundred KiB; the limits only end a crafted tail (runs of '{'
// before every "contentId") that would otherwise keep a thread busy for days.
constexpr qint64 kParamSearchBudget = 256LL * 1024 * 1024; // per file
constexpr qint64 kParamCandidateBudget = 1024 * 1024;      // per "contentId"
constexpr int kParseCost = 4;                               // a parsed byte against a scanned one

// End of the JSON object that starts at data[start] ('{'), or -1 within `limit`
// bytes. `scanned` receives the number of bytes looked at.
qint64 objectEnd(const QByteArray& data, qint64 start, qint64 limit, qint64* scanned)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    const qint64 end = std::min<qint64>(data.size(), start + limit);
    for (qint64 i = start; i < end; ++i) {
        const char c = data[i];
        if (inString) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']') {
            if (--depth == 0) {
                *scanned = i - start + 1;
                return c == '}' ? i : -1;
            }
        } else if (c == '\0') {
            *scanned = i - start + 1;
            return -1;
        }
    }
    *scanned = std::max<qint64>(0, end - start);
    return -1;
}

// Scans the last 1 GiB of the file backwards in blocks. `findCandidates(block,
// from, to)` returns offsets in [from, to) to try, `accept(block, offset)`
// returns the bytes or nullopt. Without `pick` the accepted candidate with the
// highest offset wins (the Companion grows a window from 64 MiB to 1 GiB and
// keeps the last match in it: same result). With `pick`, every candidate up
// to `neighbourhood` bytes below the first one is collected and `pick`
// chooses among them (update images hold several param.json copies).
// `giveUp` (optional) ends the search early with what was found so far.
using Finder = std::function<QList<qint64>(const QByteArray&, qint64, qint64)>;
using Acceptor = std::function<std::optional<QByteArray>(const QByteArray&, qint64)>;
using Picker = std::function<QByteArray(const std::vector<QByteArray>&)>;

Result<std::optional<QByteArray>> scanFromEnd(const QString& path, const CancellationToken& token, qint64 before,
                                              qint64 after, const Finder& findCandidates, const Acceptor& accept,
                                              const Picker& pick = {}, qint64 neighbourhood = 0,
                                              const std::function<bool()>& giveUp = {})
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(path), f.errorString());
    const qint64 len = f.size();
    const qint64 floor = std::max<qint64>(0, len - kMaxWindow);
    std::vector<QByteArray> hits;
    qint64 firstHit = -1;
    qint64 end = len;
    bool stopped = false;
    while (end > floor && !stopped) {
        if (!token.checkpoint())
            return Error::cancelled();
        if (firstHit >= 0 && end < firstHit - neighbourhood)
            break;
        const qint64 start = std::max(floor, end - kBlock);
        const qint64 readFrom = std::max<qint64>(0, start - before);
        const qint64 readTo = std::min(len, end + after);
        if (!f.seek(readFrom))
            return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(path), f.errorString());
        const QByteArray block = f.read(readTo - readFrom);
        if (block.size() != readTo - readFrom)
            return Error(ErrorCode::IoError, QStringLiteral("Package changed or was truncated during metadata inspection."));
        QList<qint64> cands = findCandidates(block, start - readFrom, end - readFrom);
        std::sort(cands.begin(), cands.end(), std::greater<>());
        for (qint64 off : cands) {
            if (firstHit >= 0 && readFrom + off < firstHit - neighbourhood)
                break;
            // Per candidate: one block can hold hundreds of thousands of them.
            if (token.isCancelled())
                return Error::cancelled();
            if (giveUp && giveUp()) {
                stopped = true;
                break;
            }
            if (auto hit = accept(block, off)) {
                if (!pick)
                    return std::optional<QByteArray>(*hit);
                if (firstHit < 0)
                    firstHit = readFrom + off;
                if (std::find(hits.begin(), hits.end(), *hit) == hits.end())
                    hits.push_back(*hit);
            }
        }
        end = start;
    }
    if (hits.empty())
        return std::optional<QByteArray>();
    return std::optional<QByteArray>(pick(hits));
}

QList<qint64> occurrences(const QByteArray& block, const QByteArray& needle, qint64 from, qint64 to)
{
    QList<qint64> out;
    qint64 i = block.indexOf(needle, from);
    while (i >= 0 && i < to) {
        out << i;
        i = block.indexOf(needle, i + 1);
    }
    return out;
}

} // namespace

QString packageKindName(PackageKind kind)
{
    switch (kind) {
    case PackageKind::Ps5: return QStringLiteral("ps5");
    case PackageKind::Ps5Delta: return QStringLiteral("ps5-delta");
    case PackageKind::Ps4: return QStringLiteral("ps4");
    case PackageKind::Unknown: break;
    }
    return QStringLiteral("unknown");
}

bool isStrictContentId(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Z]{2}[0-9]{4}-(?:PPSA|CUSA)[0-9]{5}_[0-9]{2}-[A-Z0-9_]{16}$"));
    return re.match(id).hasMatch();
}

QString titleIdFromContentId(const QString& contentId)
{
    return isStrictContentId(contentId) ? contentId.mid(7, 9) : QString();
}

Result<std::optional<QByteArray>> findParamJsonInPackage(const QString& path, const CancellationToken& token)
{
    static const QByteArray needle("\"contentId\"");
    const auto budget = std::make_shared<qint64>(kParamSearchBudget);
    return scanFromEnd(
        path, token, kMaxObject, kMaxObject,
        [](const QByteArray& b, qint64 from, qint64 to) { return occurrences(b, needle, from, to); },
        [budget](const QByteArray& b, qint64 at) -> std::optional<QByteArray> {
            // Nearest '{' first, up to 64 KiB back.
            const qint64 lowest = std::max<qint64>(0, at - kMaxObject);
            qint64 spent = 0;
            qint64 i = at;
            std::optional<QByteArray> found;
            for (; i >= lowest && spent < kParamCandidateBudget; --i) {
                if (b[i] != '{')
                    continue;
                qint64 scanned = 0;
                const qint64 e = objectEnd(b, i, kMaxObject, &scanned);
                spent += scanned;
                if (e < 0 || e < at)
                    continue;
                const QByteArray slice = b.mid(i, e - i + 1);
                spent += slice.size() * kParseCost;
                auto doc = json::parse(slice, {false, false, false, 64});
                if (!doc.ok() || !doc->isObject())
                    continue;
                if (doc->contains(QStringLiteral("contentId")) && doc->contains(QStringLiteral("contentVersion"))) {
                    found = slice;
                    break;
                }
            }
            *budget -= spent + (at - std::max(i, lowest));
            return found;
        },
        // Update images also carry the base's param.json (origin/target copies):
        // the newest contentVersion is the package's own.
        [](const std::vector<QByteArray>& hits) {
            QByteArray best;
            QString bestVersion;
            for (const QByteArray& h : hits) {
                auto doc = json::parse(h, {false, false, false, 64});
                const QString v = doc.ok() ? doc->value(QStringLiteral("contentVersion")).toString() : QString();
                if (best.isEmpty() || v > bestVersion) {
                    best = h;
                    bestVersion = v;
                }
            }
            return best;
        },
        1024 * 1024, [budget] { return *budget <= 0; });
}

Result<std::optional<QByteArray>> findPlayGoChunkDatInPackage(const QString& path, const CancellationToken& token)
{
    static const QByteArray magic("plgx");
    constexpr qint64 kMaxTable = 4 * 1024 * 1024;
    return scanFromEnd(
        path, token, 0, kMaxTable,
        [](const QByteArray& b, qint64 from, qint64 to) { return occurrences(b, magic, from, to); },
        [](const QByteArray& b, qint64 at) -> std::optional<QByteArray> {
            if (at + 256 > b.size())
                return std::nullopt;
            const auto* p = reinterpret_cast<const uchar*>(b.constData() + at);
            if (qFromLittleEndian<quint16>(p + 4) != 0x1000 || qFromLittleEndian<quint16>(p + 6) != 0)
                return std::nullopt;
            const quint32 total = qFromLittleEndian<quint32>(p + 16);
            if (total < 256 || total > quint32(kMaxTable) || at + qint64(total) > b.size())
                return std::nullopt;
            const int chunks = qFromLittleEndian<quint16>(p + 10);
            const int scenarios = qFromLittleEndian<quint16>(p + 14);
            if (chunks < 1 || chunks > 255 || scenarios < 1 || scenarios > 5)
                return std::nullopt;
            return b.mid(at, total);
        });
}

Result<PackageIdentity> inspectPackage(const QString& path, const CancellationToken& token)
{
    PackageIdentity id;
    {
        QFile f(toLongPath(path));
        if (!f.open(QIODevice::ReadOnly))
            return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(path), f.errorString());
        const QByteArray head = f.read(80);
        if (head.size() < 80)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is too short to be a package.").arg(path));
        if (head.startsWith(QByteArray("\x7F" "FIH", 4)))
            id.kind = PackageKind::Ps5;
        else if (head.startsWith(QByteArray("\x7F" "LIH", 4)))
            id.kind = PackageKind::Ps5Delta;
        else if (head.startsWith(QByteArray("\x7F" "CNT", 4)))
            id.kind = PackageKind::Ps4;
        if (id.kind == PackageKind::Ps5) {
            id.digest = head.mid(0x30, 32);
            id.digestHex = QString::fromLatin1(id.digest.toHex()).toUpper();
        }
    }
    auto found = findParamJsonInPackage(path, token);
    if (!found.ok())
        return found.takeError();
    if (found->has_value()) {
        auto doc = parseParamJson(**found);
        if (doc.ok() && doc->isObject()) {
            id.hasParam = true;
            id.param = doc.value();
            const auto str = [&](const QString& k) {
                const json::Value& v = id.param.value(k);
                return v.isString() ? v.toString() : QString();
            };
            id.contentId = str(QStringLiteral("contentId"));
            const json::Value& tid = id.param.value(QStringLiteral("titleId"));
            id.titleId = tid.isString() ? tid.toString() : (tid.isNull() ? titleIdFromContentId(id.contentId) : QString());
            id.version = str(QStringLiteral("contentVersion"));
            id.title = paramTitle(id.param);
        }
    }
    return id;
}

Result<qint64> countOccurrences(const QString& path, const QByteArray& needle, const CancellationToken& token)
{
    if (needle.isEmpty())
        return qint64(0);
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(path), f.errorString());
    qint64 count = 0;
    QByteArray carry;
    while (true) {
        if (!token.checkpoint())
            return Error::cancelled();
        const QByteArray chunk = f.read(kBlock);
        if (chunk.isEmpty()) {
            if (f.error() != QFileDevice::NoError || !f.atEnd())
                return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(path), f.errorString());
            break;
        }
        const QByteArray buf = carry + chunk;
        for (qint64 i = buf.indexOf(needle); i >= 0; i = buf.indexOf(needle, i + 1))
            ++count;
        carry = buf.right(needle.size() - 1);
    }
    return count;
}

} // namespace fw::fpkg
