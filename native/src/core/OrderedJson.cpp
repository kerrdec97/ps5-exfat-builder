#include "core/OrderedJson.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cmath>
#include <cstring>

namespace fw::json {

namespace {
const Value& nullValue()
{
    static const Value v;
    return v;
}
} // namespace

Value::Value(double d) : m_type(Type::Number)
{
    if (!std::isfinite(d)) {
        m_type = Type::Null;
        return;
    }
    // As Python writes a float ("2.0", "-0.0", "1e+16"), so round trips match.
    m_text = pythonFloatRepr(d);
}

Value Value::numberLiteral(const QString& literal)
{
    Value v;
    v.m_type = Type::Number;
    v.m_text = literal;
    return v;
}

Value Value::array()
{
    Value v;
    v.m_type = Type::Array;
    return v;
}

Value Value::object()
{
    Value v;
    v.m_type = Type::Object;
    return v;
}

qint64 Value::toInt(qint64 fallback) const
{
    if (m_type != Type::Number)
        return fallback;
    bool ok = false;
    const qint64 n = m_text.toLongLong(&ok);
    if (ok)
        return n;
    const double d = m_text.toDouble(&ok);
    // 1e300 is valid JSON; converting it to an integer is not defined.
    return ok && std::isfinite(d) && std::fabs(d) < 9.2e18 ? qint64(d) : fallback;
}

double Value::toDouble(double fallback) const
{
    if (m_type != Type::Number)
        return fallback;
    bool ok = false;
    const double d = m_text.toDouble(&ok);
    return ok ? d : fallback;
}

const Value& Value::at(int i) const
{
    return (i >= 0 && i < int(m_items.size())) ? m_items[size_t(i)] : nullValue();
}

Value& Value::at(int i)
{
    return m_items.at(size_t(i));
}

void Value::append(Value v)
{
    if (m_type == Type::Null)
        m_type = Type::Array;
    m_items.push_back(std::move(v));
}

void Value::removeAt(int i)
{
    if (i < 0 || i >= int(m_items.size()))
        return;
    m_items.erase(m_items.begin() + i);
    if (m_type == Type::Object) {
        m_keys.removeAt(i);
        m_index.clear(); // every later position moved
        indexKeys();
    }
}

namespace {
// Objects with fewer members are searched as a list. From here on a list
// search per member makes reading or writing an object quadratic: a crafted
// file with 200,000 members in one object would stall the thread reading it.
constexpr int kIndexedFrom = 32;
} // namespace

void Value::indexKeys()
{
    const int n = int(m_keys.size());
    if (!m_index.isEmpty())
        m_index.insert(m_keys.last(), n - 1); // complete before: only the new key is missing
    else if (n >= kIndexedFrom)
        for (int i = 0; i < n; ++i)
            m_index.insert(m_keys.at(i), i);
}

int Value::indexOf(const QString& key) const
{
    if (m_type != Type::Object)
        return -1;
    if (!m_index.isEmpty()) // complete whenever it is not empty
        return m_index.value(key, -1);
    return int(m_keys.indexOf(key));
}

const Value& Value::value(const QString& key) const
{
    const int i = indexOf(key);
    return i < 0 ? nullValue() : m_items[size_t(i)];
}

Value& Value::operator[](const QString& key)
{
    if (m_type == Type::Null)
        m_type = Type::Object;
    const int i = indexOf(key);
    if (i >= 0)
        return m_items[size_t(i)];
    m_keys.append(key);
    m_items.emplace_back();
    indexKeys();
    return m_items.back();
}

void Value::insert(const QString& key, Value v)
{
    (*this)[key] = std::move(v);
}

bool Value::remove(const QString& key)
{
    const int i = indexOf(key);
    if (i < 0)
        return false;
    removeAt(i);
    return true;
}

const Value& Value::path(const QString& dottedPath) const
{
    const Value* cur = this;
    for (const QString& part : dottedPath.split(u'.')) {
        if (!cur->isObject())
            return nullValue();
        const int i = cur->indexOf(part);
        if (i < 0)
            return nullValue();
        cur = &cur->m_items[size_t(i)];
    }
    return *cur;
}

bool Value::operator==(const Value& o) const
{
    if (m_type != o.m_type)
        return false;
    switch (m_type) {
    case Type::Null: return true;
    case Type::Bool: return m_bool == o.m_bool;
    case Type::Number: return m_text == o.m_text || toDouble() == o.toDouble();
    case Type::String: return m_text == o.m_text;
    case Type::Array: return m_items == o.m_items;
    case Type::Object: return m_keys == o.m_keys && m_items == o.m_items;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Parser

namespace {

class Parser {
public:
    Parser(const QByteArray& data, const ParseOptions& opt) : m_d(data.constData()), m_n(data.size()), m_opt(opt) {}

    Result<Value> run()
    {
        if (m_opt.allowBom && m_n >= 3 && std::memcmp(m_d, "\xEF\xBB\xBF", 3) == 0)
            m_i = 3;
        skipWs();
        Value v;
        if (!parseValue(v, 0))
            return fail();
        skipWs();
        if (m_i != m_n) {
            m_err = QStringLiteral("unexpected data after the JSON value");
            return fail();
        }
        return v;
    }

private:
    Error fail() const
    {
        // Line/column for the message.
        int line = 1, col = 1;
        for (qsizetype k = 0; k < m_i && k < m_n; ++k) {
            if (m_d[k] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        return Error(ErrorCode::CorruptData, QStringLiteral("The file is not valid JSON (line %1, column %2).").arg(line).arg(col),
                     m_err);
    }

    void skipWs()
    {
        while (m_i < m_n) {
            const char c = m_d[m_i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_i;
            } else if (m_opt.allowComments && c == '/' && m_i + 1 < m_n && m_d[m_i + 1] == '/') {
                while (m_i < m_n && m_d[m_i] != '\n')
                    ++m_i;
            } else if (m_opt.allowComments && c == '/' && m_i + 1 < m_n && m_d[m_i + 1] == '*') {
                m_i += 2;
                while (m_i + 1 < m_n && !(m_d[m_i] == '*' && m_d[m_i + 1] == '/'))
                    ++m_i;
                m_i = std::min(m_n, m_i + 2);
            } else {
                break;
            }
        }
    }

    bool literal(const char* word)
    {
        const qsizetype len = qsizetype(std::strlen(word));
        if (m_i + len <= m_n && std::memcmp(m_d + m_i, word, size_t(len)) == 0) {
            m_i += len;
            return true;
        }
        m_err = QStringLiteral("invalid literal");
        return false;
    }

    bool parseValue(Value& out, int depth)
    {
        if (depth > m_opt.maxDepth) {
            m_err = QStringLiteral("nesting too deep");
            return false;
        }
        if (m_i >= m_n) {
            m_err = QStringLiteral("unexpected end of data");
            return false;
        }
        const char c = m_d[m_i];
        if (c == '{')
            return parseObject(out, depth);
        if (c == '[')
            return parseArray(out, depth);
        if (c == '"') {
            QString s;
            if (!parseString(s))
                return false;
            out = Value(s);
            return true;
        }
        if (c == 't') {
            if (!literal("true"))
                return false;
            out = Value(true);
            return true;
        }
        if (c == 'f') {
            if (!literal("false"))
                return false;
            out = Value(false);
            return true;
        }
        if (c == 'n') {
            if (!literal("null"))
                return false;
            out = Value();
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9'))
            return parseNumber(out);
        m_err = QStringLiteral("unexpected character '%1'").arg(QLatin1Char(c));
        return false;
    }

    bool parseNumber(Value& out)
    {
        const qsizetype start = m_i;
        if (m_d[m_i] == '-')
            ++m_i;
        if (m_i >= m_n) {
            m_err = QStringLiteral("bad number");
            return false;
        }
        if (m_d[m_i] == '0') {
            ++m_i;
        } else if (m_d[m_i] >= '1' && m_d[m_i] <= '9') {
            while (m_i < m_n && m_d[m_i] >= '0' && m_d[m_i] <= '9')
                ++m_i;
        } else {
            m_err = QStringLiteral("bad number");
            return false;
        }
        if (m_i < m_n && m_d[m_i] == '.') {
            ++m_i;
            const qsizetype f = m_i;
            while (m_i < m_n && m_d[m_i] >= '0' && m_d[m_i] <= '9')
                ++m_i;
            if (m_i == f) {
                m_err = QStringLiteral("bad number");
                return false;
            }
        }
        if (m_i < m_n && (m_d[m_i] == 'e' || m_d[m_i] == 'E')) {
            ++m_i;
            if (m_i < m_n && (m_d[m_i] == '+' || m_d[m_i] == '-'))
                ++m_i;
            const qsizetype e = m_i;
            while (m_i < m_n && m_d[m_i] >= '0' && m_d[m_i] <= '9')
                ++m_i;
            if (m_i == e) {
                m_err = QStringLiteral("bad number");
                return false;
            }
        }
        out = Value::numberLiteral(QString::fromLatin1(m_d + start, int(m_i - start)));
        return true;
    }

    static int hexVal(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool parseHex4(char16_t& out)
    {
        if (m_i + 4 > m_n) {
            m_err = QStringLiteral("bad \\u escape");
            return false;
        }
        int v = 0;
        for (int k = 0; k < 4; ++k) {
            const int h = hexVal(m_d[m_i + k]);
            if (h < 0) {
                m_err = QStringLiteral("bad \\u escape");
                return false;
            }
            v = v * 16 + h;
        }
        m_i += 4;
        out = char16_t(v);
        return true;
    }

    bool parseString(QString& out)
    {
        ++m_i; // opening quote
        QByteArray raw;
        QString result;
        auto flushRaw = [&] {
            if (!raw.isEmpty()) {
                result += QString::fromUtf8(raw);
                raw.clear();
            }
        };
        while (m_i < m_n) {
            const char c = m_d[m_i];
            if (c == '"') {
                ++m_i;
                flushRaw();
                out = result;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                m_err = QStringLiteral("control character in string");
                return false;
            }
            if (c == '\\') {
                flushRaw();
                ++m_i;
                if (m_i >= m_n)
                    break;
                const char e = m_d[m_i++];
                switch (e) {
                case '"': result += u'"'; break;
                case '\\': result += u'\\'; break;
                case '/': result += u'/'; break;
                case 'b': result += u'\b'; break;
                case 'f': result += u'\f'; break;
                case 'n': result += u'\n'; break;
                case 'r': result += u'\r'; break;
                case 't': result += u'\t'; break;
                case 'u': {
                    char16_t u1;
                    if (!parseHex4(u1))
                        return false;
                    result += QChar(u1);
                    break;
                }
                default:
                    m_err = QStringLiteral("bad escape");
                    return false;
                }
                continue;
            }
            raw += c;
            ++m_i;
        }
        m_err = QStringLiteral("unterminated string");
        return false;
    }

    bool parseArray(Value& out, int depth)
    {
        ++m_i;
        out = Value::array();
        skipWs();
        if (m_i < m_n && m_d[m_i] == ']') {
            ++m_i;
            return true;
        }
        while (true) {
            skipWs();
            if (m_opt.allowTrailingCommas && m_i < m_n && m_d[m_i] == ']') {
                ++m_i;
                return true;
            }
            Value item;
            if (!parseValue(item, depth + 1))
                return false;
            out.append(std::move(item));
            skipWs();
            if (m_i < m_n && m_d[m_i] == ',') {
                ++m_i;
                continue;
            }
            if (m_i < m_n && m_d[m_i] == ']') {
                ++m_i;
                return true;
            }
            m_err = QStringLiteral("expected ',' or ']'");
            return false;
        }
    }

    bool parseObject(Value& out, int depth)
    {
        ++m_i;
        out = Value::object();
        skipWs();
        if (m_i < m_n && m_d[m_i] == '}') {
            ++m_i;
            return true;
        }
        while (true) {
            skipWs();
            if (m_opt.allowTrailingCommas && m_i < m_n && m_d[m_i] == '}') {
                ++m_i;
                return true;
            }
            if (m_i >= m_n || m_d[m_i] != '"') {
                m_err = QStringLiteral("expected a member name");
                return false;
            }
            QString key;
            if (!parseString(key))
                return false;
            skipWs();
            if (m_i >= m_n || m_d[m_i] != ':') {
                m_err = QStringLiteral("expected ':'");
                return false;
            }
            ++m_i;
            skipWs();
            Value v;
            if (!parseValue(v, depth + 1))
                return false;
            out.insert(key, std::move(v)); // duplicate keys: last wins, first position kept
            skipWs();
            if (m_i < m_n && m_d[m_i] == ',') {
                ++m_i;
                continue;
            }
            if (m_i < m_n && m_d[m_i] == '}') {
                ++m_i;
                return true;
            }
            m_err = QStringLiteral("expected ',' or '}'");
            return false;
        }
    }

    const char* m_d;
    qsizetype m_n;
    qsizetype m_i = 0;
    ParseOptions m_opt;
    QString m_err;
};

// ---------------------------------------------------------------------------
// Writer (Python json.dumps compatible)

void writeString(QByteArray& out, const QString& s, bool ensureAscii)
{
    out += '"';
    for (qsizetype i = 0; i < s.size(); ++i) {
        const char16_t c = s[i].unicode();
        switch (c) {
        case u'"': out += "\\\""; continue;
        case u'\\': out += "\\\\"; continue;
        case u'\n': out += "\\n"; continue;
        case u'\r': out += "\\r"; continue;
        case u'\t': out += "\\t"; continue;
        case u'\b': out += "\\b"; continue;
        case u'\f': out += "\\f"; continue;
        default: break;
        }
        if (c < 0x20 || (ensureAscii && c > 0x7F)) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
            out += buf;
        } else if (c < 0x80) {
            out += char(c);
        } else {
            // Encode this UTF-16 unit (or surrogate pair) as UTF-8.
            if (QChar::isHighSurrogate(c) && i + 1 < s.size() && s[i + 1].isLowSurrogate()) {
                out += QString(s.constData() + i, 2).toUtf8();
                ++i;
            } else {
                out += QString(QChar(c)).toUtf8();
            }
        }
    }
    out += '"';
}

void writeValue(QByteArray& out, const Value& v, const WriteOptions& o, int level)
{
    const bool pretty = o.indent >= 0;
    auto newline = [&](int lvl) {
        if (!pretty)
            return;
        out += '\n';
        out += QByteArray(o.indent * lvl, ' ');
    };
    switch (v.type()) {
    case Value::Type::Null: out += "null"; return;
    case Value::Type::Bool: out += v.toBool() ? "true" : "false"; return;
    case Value::Type::Number: {
        const QString t = v.numberText();
        if (o.pythonFloats && (t.contains(u'.') || t.contains(u'e') || t.contains(u'E')))
            out += pythonFloatRepr(t.toDouble()).toLatin1();
        else
            out += t.toLatin1();
        return;
    }
    case Value::Type::String: writeString(out, v.toString(), o.ensureAscii); return;
    case Value::Type::Array: {
        if (v.size() == 0) {
            out += "[]";
            return;
        }
        out += '[';
        for (int i = 0; i < v.size(); ++i) {
            if (i > 0)
                out += pretty ? "," : ", ";
            newline(level + 1);
            writeValue(out, v.at(i), o, level + 1);
        }
        newline(level);
        out += ']';
        return;
    }
    case Value::Type::Object: {
        if (v.keys().isEmpty()) {
            out += "{}";
            return;
        }
        out += '{';
        const QStringList& keys = v.keys();
        for (int i = 0; i < keys.size(); ++i) {
            if (i > 0)
                out += pretty ? "," : ", ";
            newline(level + 1);
            writeString(out, keys[i], o.ensureAscii);
            out += ": ";
            writeValue(out, v.at(i), o, level + 1); // by position: no search per member
        }
        newline(level);
        out += '}';
        return;
    }
    }
}

} // namespace

QString pythonFloatRepr(double d)
{
    if (std::isnan(d))
        return QStringLiteral("NaN");
    if (std::isinf(d))
        return d > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
    if (d == 0.0)
        return std::signbit(d) ? QStringLiteral("-0.0") : QStringLiteral("0.0");
    // Shortest round-trip digits, then Python's formatting rule: fixed for
    // exponents -4 <= e < 16, otherwise d.ddde±XX.
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::scientific);
    const std::string sci(buf, r.ptr);
    const auto epos = sci.find('e');
    std::string mant = sci.substr(0, epos);
    const int exp = std::stoi(sci.substr(epos + 1));
    const bool neg = !mant.empty() && mant[0] == '-';
    if (neg)
        mant.erase(0, 1);
    std::string digits;
    for (char c : mant)
        if (c != '.')
            digits += c;
    std::string out;
    if (exp >= -4 && exp < 16) {
        if (exp >= 0) {
            if (int(digits.size()) <= exp + 1) {
                out = digits + std::string(size_t(exp + 1 - int(digits.size())), '0') + ".0";
            } else {
                out = digits.substr(0, size_t(exp + 1)) + "." + digits.substr(size_t(exp + 1));
            }
        } else {
            out = "0." + std::string(size_t(-exp - 1), '0') + digits;
        }
    } else {
        out = digits.substr(0, 1);
        if (digits.size() > 1)
            out += "." + digits.substr(1);
        char e[16];
        std::snprintf(e, sizeof(e), "e%c%02d", exp < 0 ? '-' : '+', std::abs(exp));
        out += e;
    }
    return QString::fromLatin1((neg ? "-" : "") + out);
}

Result<Value> parse(const QByteArray& utf8, const ParseOptions& options)
{
    Parser p(utf8, options);
    return p.run();
}

QByteArray write(const Value& value, const WriteOptions& options)
{
    QByteArray out;
    writeValue(out, value, options, 0);
    if (options.trailingNewline)
        out += '\n';
    return out;
}

} // namespace fw::json
