#include "engines/gameinfo/JsonText.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringDecoder>

#include <algorithm>

namespace fw::gameinfo::json {

namespace {

class Scanner {
public:
    explicit Scanner(const QByteArray& t) : s(t) {}

    const QByteArray& s;
    qsizetype pos = 0;
    QString error;

    bool atEnd() const { return pos >= s.size(); }
    char peek() const { return atEnd() ? '\0' : s.at(pos); }

    void skipWs()
    {
        while (!atEnd()) {
            const char c = s.at(pos);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos;
            else
                break;
        }
    }

    bool fail(const QString& why)
    {
        if (error.isEmpty())
            error = QStringLiteral("%1 at byte %2").arg(why).arg(pos);
        return false;
    }

    // Parses a string starting at '"'; stores the decoded value.
    bool string(QString* out)
    {
        if (peek() != '"')
            return fail(QStringLiteral("expected a string"));
        const qsizetype begin = pos;
        ++pos;
        while (!atEnd()) {
            const char c = s.at(pos);
            if (c == '\\') {
                pos += 2;
                continue;
            }
            if (c == '"') {
                ++pos;
                if (out) {
                    // Let Qt decode escapes: wrap the literal in an array.
                    const QByteArray lit = "[" + s.mid(begin, pos - begin) + "]";
                    const QJsonDocument d = QJsonDocument::fromJson(lit);
                    *out = d.isArray() ? d.array().at(0).toString() : QString();
                }
                return true;
            }
            ++pos;
        }
        return fail(QStringLiteral("unterminated string"));
    }

    bool literal()
    {
        const qsizetype begin = pos;
        while (!atEnd()) {
            const char c = s.at(pos);
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
                break;
            ++pos;
        }
        return pos > begin || fail(QStringLiteral("expected a value"));
    }

    bool value(int depth)
    {
        if (depth > 256)
            return fail(QStringLiteral("nesting too deep"));
        skipWs();
        switch (peek()) {
        case '"': return string(nullptr);
        case '{': return object(depth, nullptr);
        case '[': return array(depth);
        case '\0': return fail(QStringLiteral("unexpected end"));
        default: return literal();
        }
    }

    bool array(int depth)
    {
        ++pos; // '['
        skipWs();
        if (peek() == ']') {
            ++pos;
            return true;
        }
        while (true) {
            if (!value(depth + 1))
                return false;
            skipWs();
            if (peek() == ',') {
                ++pos;
                continue;
            }
            if (peek() == ']') {
                ++pos;
                return true;
            }
            return fail(QStringLiteral("expected , or ]"));
        }
    }

    bool object(int depth, std::vector<Member>* members)
    {
        if (peek() != '{')
            return fail(QStringLiteral("expected an object"));
        ++pos;
        skipWs();
        if (peek() == '}') {
            ++pos;
            return true;
        }
        while (true) {
            skipWs();
            Member m;
            if (!string(members ? &m.key : nullptr))
                return false;
            skipWs();
            if (peek() != ':')
                return fail(QStringLiteral("expected :"));
            ++pos;
            skipWs();
            m.valueBegin = pos;
            const char c = peek();
            m.valueType = c == '{' || c == '[' || c == '"' ? c
                        : c == 't' || c == 'f' ? c
                        : c == 'n' ? 'z'
                                   : 'n';
            if (!value(depth + 1))
                return false;
            m.valueEnd = pos;
            if (members)
                members->push_back(std::move(m));
            skipWs();
            if (peek() == ',') {
                ++pos;
                continue;
            }
            if (peek() == '}') {
                ++pos;
                return true;
            }
            return fail(QStringLiteral("expected , or }"));
        }
    }
};

qsizetype bomLength(const QByteArray& t)
{
    return t.startsWith("\xEF\xBB\xBF") ? 3 : 0;
}

} // namespace

Result<std::vector<Member>> topLevelMembers(const QByteArray& text)
{
    Scanner sc(text);
    sc.pos = bomLength(text);
    sc.skipWs();
    std::vector<Member> out;
    if (!sc.object(0, &out))
        return Error(ErrorCode::CorruptData, QStringLiteral("The JSON text could not be read."), sc.error);
    return out;
}

Result<std::vector<Member>> membersAt(const QByteArray& text, const QStringList& path)
{
    auto top = topLevelMembers(text);
    if (!top.ok())
        return top.takeError();
    std::vector<Member> cur = std::move(top.value());
    for (const QString& key : path) {
        auto it = std::find_if(cur.begin(), cur.end(), [&](const Member& m) { return m.key == key; });
        if (it == cur.end() || it->valueType != '{')
            return Error(ErrorCode::NotFound, QStringLiteral("\"%1\" is not an object in this JSON.").arg(key));
        Scanner sc(text);
        sc.pos = it->valueBegin;
        std::vector<Member> next;
        if (!sc.object(0, &next))
            return Error(ErrorCode::CorruptData, QStringLiteral("The JSON text could not be read."), sc.error);
        cur = std::move(next);
    }
    return cur;
}

QByteArray replaceValues(const QByteArray& text, std::vector<std::pair<Member, QByteArray>> replacements)
{
    std::sort(replacements.begin(), replacements.end(),
              [](const auto& a, const auto& b) { return a.first.valueBegin > b.first.valueBegin; });
    QByteArray out = text;
    for (const auto& [m, raw] : replacements)
        out.replace(m.valueBegin, m.valueEnd - m.valueBegin, raw);
    return out;
}

QByteArray quote(const QString& s)
{
    QByteArray doc = QJsonDocument(QJsonArray{QJsonValue(s)}).toJson(QJsonDocument::Compact);
    // "[\"...\"]" -> "\"...\""
    return doc.mid(1, doc.size() - 2);
}

QString stringValue(const QByteArray& text, const Member& m)
{
    if (m.valueType != '"')
        return {};
    const QByteArray lit = "[" + text.mid(m.valueBegin, m.valueEnd - m.valueBegin) + "]";
    const QJsonDocument d = QJsonDocument::fromJson(lit);
    return d.isArray() ? d.array().at(0).toString() : QString();
}

QByteArray repair(const QByteArray& input, bool* changed)
{
    QByteArray t = input;
    // UTF-16 with BOM -> UTF-8.
    if (t.startsWith("\xFF\xFE") || t.startsWith("\xFE\xFF")) {
        auto dec = QStringDecoder(t.startsWith("\xFF\xFE") ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
        t = QString(dec(t.mid(2))).toUtf8();
    }
    if (t.startsWith("\xEF\xBB\xBF"))
        t.remove(0, 3);
    // errors='replace': round-trip through QString replaces invalid UTF-8.
    t = QString::fromUtf8(t).toUtf8();

    QByteArray out;
    out.reserve(t.size());
    bool inString = false;
    int depth = 0;
    bool started = false;
    for (qsizetype i = 0; i < t.size(); ++i) {
        const char c = t.at(i);
        if (inString) {
            out.append(c);
            if (c == '\\' && i + 1 < t.size()) {
                out.append(t.at(++i));
            } else if (c == '"') {
                inString = false;
                if (depth == 0)
                    break; // a top-level string value ends the document
            }
            continue;
        }
        if (c == '\0')
            continue;
        if (c == '/' && i + 1 < t.size() && t.at(i + 1) == '/') {
            while (i < t.size() && t.at(i) != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < t.size() && t.at(i + 1) == '*') {
            i += 2;
            while (i + 1 < t.size() && !(t.at(i) == '*' && t.at(i + 1) == '/'))
                ++i;
            ++i;
            continue;
        }
        if (c == '"') {
            inString = true;
            started = true;
            out.append(c);
            continue;
        }
        if (c == '}' || c == ']') {
            // Drop a trailing comma before the closing bracket.
            qsizetype j = out.size() - 1;
            while (j >= 0 && (out.at(j) == ' ' || out.at(j) == '\t' || out.at(j) == '\n' || out.at(j) == '\r'))
                --j;
            if (j >= 0 && out.at(j) == ',')
                out.remove(j, 1);
            out.append(c);
            --depth;
            if (depth <= 0 && started)
                break; // ignore anything after the top-level value
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
            started = true;
        }
        out.append(c);
    }
    if (changed) {
        QByteArray in = input;
        if (in.startsWith("\xEF\xBB\xBF"))
            in.remove(0, 3);
        *changed = (out.trimmed() != in.trimmed());
    }
    return out;
}

} // namespace fw::gameinfo::json
