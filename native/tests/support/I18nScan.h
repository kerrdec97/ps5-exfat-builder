#pragma once

// Finds the English texts of the interface in the C++ sources (tst_i18n_source
// keeps src/app/resources/lang/_source.json equal to what this finds):
//
//  * the literals of FW_TR("...") and FW_TR_NOOP("...") anywhere under src/;
//  * outside src/app, texts that are data shown by the interface although the
//    module that owns them does not translate: the names of job stages
//    (QStringLiteral literals in the body of a phases()/...Phases() function,
//    in the arguments of setPhases(), and as the name given to begin() and
//    beginStep()) and the name and purpose of a tool
//    (dependencies.add(id, name, purpose, probe)).
//
// Header only, so a test includes it without a change to the build lists.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <optional>

namespace fwtest::i18nscan {

struct Found {
    QString text;
    QString where;
};

struct Result {
    QList<Found> found;
    QStringList problems; // "file:line: what is wrong"
};

// A source file in two forms of the same length: `code` without comments, and
// `bare` without comments and with the inside of every literal blanked, so a
// pattern found in `bare` is code and never part of a text.
struct Source {
    QString code;
    QString bare;
};

inline int lineOf(const QString& code, qsizetype pos)
{
    return int(code.left(pos).count(u'\n')) + 1;
}

// At a quote: the index after the literal's closing quote (or the end).
inline qsizetype skipQuoted(const QString& code, qsizetype pos)
{
    const QChar quote = code.at(pos);
    for (++pos; pos < code.size(); ++pos) {
        if (code.at(pos) == u'\\')
            ++pos;
        else if (code.at(pos) == quote || code.at(pos) == u'\n')
            return pos + 1;
    }
    return code.size();
}

// At the R of R"delim(: the index after the raw literal, or -1 when this is
// no raw literal. `content` receives the text between the parentheses.
inline qsizetype skipRaw(const QString& code, qsizetype pos, QString* content = nullptr)
{
    if (pos + 1 >= code.size() || code.at(pos) != u'R' || code.at(pos + 1) != u'"')
        return -1;
    const qsizetype open = code.indexOf(u'(', pos + 2);
    if (open < 0 || open - (pos + 2) > 16)
        return -1;
    const QString closer = u')' + code.mid(pos + 2, open - (pos + 2)) + u'"';
    const qsizetype close = code.indexOf(closer, open + 1);
    if (close < 0)
        return -1;
    if (content)
        *content = code.mid(open + 1, close - open - 1);
    return close + closer.size();
}

inline Source prepare(const QString& text)
{
    Source s{text, text};
    const auto blank = [](QString& in, qsizetype from, qsizetype to) {
        for (qsizetype k = from; k < to; ++k)
            if (in.at(k) != u'\n' && in.at(k) != u'\r')
                in[k] = u' ';
    };
    qsizetype i = 0;
    while (i < text.size()) {
        const QChar c = text.at(i);
        qsizetype end = -1;
        if (c == u'/' && i + 1 < text.size() && text.at(i + 1) == u'/') {
            end = text.indexOf(u'\n', i);
            if (end < 0)
                end = text.size();
            blank(s.code, i, end);
            blank(s.bare, i, end);
        } else if (c == u'/' && i + 1 < text.size() && text.at(i + 1) == u'*') {
            end = text.indexOf(QLatin1String("*/"), i + 2);
            end = end < 0 ? text.size() : end + 2;
            blank(s.code, i, end);
            blank(s.bare, i, end);
        } else if (c == u'R' && skipRaw(text, i) > 0) { // also uR"(...)": its prefix was passed as plain text
            end = skipRaw(text, i);
            blank(s.bare, i + 2, end - 1);
        } else if (c == u'"' || (c == u'\'' && !(i > 0 && text.at(i - 1).isDigit()))) { // not the ' of 1'000
            end = skipQuoted(text, i);
            blank(s.bare, i + 1, end - 1);
        } else {
            end = i + 1;
        }
        i = end;
    }
    return s;
}

// The text of one "..." literal (pos at the opening quote), escapes decoded;
// `pos` moves behind the closing quote.
inline std::optional<QString> decodeQuoted(const QString& code, qsizetype& pos)
{
    QString text;
    qsizetype i = pos + 1;
    // \x41, \u2026, \U0001F600: the code point, as QStringLiteral takes it.
    const auto appendHex = [&](int maxDigits) {
        char32_t value = 0;
        for (int n = 0; i < code.size() && n < maxDigits; ++n, ++i) {
            const char16_t u = code.at(i).unicode();
            if (u >= u'0' && u <= u'9')
                value = value * 16 + char32_t(u - u'0');
            else if (u >= u'a' && u <= u'f')
                value = value * 16 + char32_t(u - u'a' + 10);
            else if (u >= u'A' && u <= u'F')
                value = value * 16 + char32_t(u - u'A' + 10);
            else
                break;
        }
        text += QString::fromUcs4(&value, 1);
    };
    while (i < code.size()) {
        const QChar c = code.at(i);
        if (c == u'"') {
            pos = i + 1;
            return text;
        }
        if (c == u'\n')
            return std::nullopt;
        if (c != u'\\') {
            text += c;
            ++i;
            continue;
        }
        if (++i >= code.size())
            return std::nullopt;
        const char16_t e = code.at(i).unicode();
        ++i;
        switch (e) {
        case u'n': text += u'\n'; break;
        case u't': text += u'\t'; break;
        case u'r': text += u'\r'; break;
        case u'a': text += QChar(0x07); break;
        case u'b': text += QChar(0x08); break;
        case u'f': text += QChar(0x0C); break;
        case u'v': text += QChar(0x0B); break;
        case u'x': appendHex(8); break;
        case u'u': appendHex(4); break;
        case u'U': appendHex(8); break;
        default:
            if (e >= u'0' && e <= u'7') {
                uint value = uint(e - u'0');
                for (int n = 0; n < 2 && i < code.size() && code.at(i) >= u'0' && code.at(i) <= u'7'; ++n, ++i)
                    value = value * 8 + uint(code.at(i).unicode() - u'0');
                text += QChar(char16_t(value));
            } else {
                text += QChar(e); // \\ \" \' \?
            }
        }
    }
    return std::nullopt;
}

inline qsizetype skipSpace(const QString& code, qsizetype pos)
{
    while (pos < code.size() && code.at(pos).isSpace())
        ++pos;
    return pos;
}

// One or more adjacent string literals from `pos` on (white space between
// them), joined as the compiler joins them. `pos` moves behind the last one.
// Nothing when there is no literal at `pos`. `code` has no comments.
inline std::optional<QString> readLiterals(const QString& code, qsizetype& pos)
{
    QString joined;
    bool any = false;
    qsizetype i = skipSpace(code, pos);
    while (i < code.size()) {
        if (code.at(i) == u'"') {
            const auto part = decodeQuoted(code, i);
            if (!part)
                return std::nullopt;
            joined += *part;
        } else if (code.at(i) == u'R') {
            QString raw;
            const qsizetype end = skipRaw(code, i, &raw);
            if (end < 0)
                break;
            joined += raw;
            i = end;
        } else {
            break;
        }
        any = true;
        pos = i;
        i = skipSpace(code, i);
    }
    if (!any)
        return std::nullopt;
    return joined;
}

// At an opening bracket in `bare`: the index of the one that closes it, or -1.
inline qsizetype matchClose(const QString& bare, qsizetype open)
{
    const QChar opener = bare.at(open);
    const QChar closer = opener == u'(' ? u')' : opener == u'{' ? u'}' : u']';
    int depth = 0;
    for (qsizetype i = open; i < bare.size(); ++i) {
        if (bare.at(i) == opener)
            ++depth;
        else if (bare.at(i) == closer && --depth == 0)
            return i;
    }
    return -1;
}

// The ranges [from, to) of a call's arguments, split at the commas of the
// outermost level. `open` is the call's opening parenthesis in `bare`.
inline QList<QPair<qsizetype, qsizetype>> argumentRanges(const QString& bare, qsizetype open)
{
    QList<QPair<qsizetype, qsizetype>> ranges;
    const qsizetype close = matchClose(bare, open);
    if (close < 0)
        return ranges;
    int depth = 0;
    qsizetype start = open + 1;
    for (qsizetype i = open + 1; i < close; ++i) {
        const QChar c = bare.at(i);
        if (c == u'(' || c == u'{' || c == u'[')
            ++depth;
        else if (c == u')' || c == u'}' || c == u']')
            --depth;
        else if (c == u',' && depth == 0) {
            ranges.append({start, i});
            start = i + 1;
        }
    }
    ranges.append({start, close});
    return ranges;
}

// The text of every QStringLiteral("...") between `from` and `to`.
inline QStringList stringLiteralsIn(const Source& s, qsizetype from, qsizetype to)
{
    static const QRegularExpression re(QStringLiteral("\\bQStringLiteral\\s*\\("));
    QStringList out;
    for (auto it = re.globalMatchView(QStringView(s.bare).mid(from, to - from)); it.hasNext();) {
        qsizetype pos = from + it.next().capturedEnd();
        if (const auto text = readLiterals(s.code, pos))
            out << *text;
    }
    return out;
}

// FW_TR("...") and FW_TR_NOOP("...").
inline void scanMarked(const Source& s, const QString& where, Result& out)
{
    static const QRegularExpression re(QStringLiteral("\\bFW_TR(?:_NOOP)?\\s*\\("));
    for (auto it = re.globalMatch(s.bare); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const qsizetype lineStart = s.bare.lastIndexOf(u'\n', m.capturedStart()) + 1;
        if (QStringView(s.bare).mid(lineStart, m.capturedStart() - lineStart).trimmed().startsWith(u'#'))
            continue; // the macros' own definitions
        qsizetype pos = m.capturedEnd();
        const auto text = readLiterals(s.code, pos);
        const qsizetype after = skipSpace(s.code, pos);
        const QString at = QStringLiteral("%1:%2").arg(where).arg(lineOf(s.code, m.capturedStart()));
        if (!text || after >= s.code.size() || s.code.at(after) != u')') {
            out.problems << at + QStringLiteral(": the argument must be string literals and nothing else (the text is the catalogue key)");
            continue;
        }
        if (text->trimmed().isEmpty()) {
            out.problems << at + QStringLiteral(": an empty text cannot be translated");
            continue;
        }
        out.found.append({*text, where});
    }
}

// Stage names of jobs.
inline void scanStages(const Source& s, const QString& where, Result& out)
{
    const QString tag = where + QStringLiteral(" (stage)");
    const auto take = [&](qsizetype open) {
        const qsizetype close = matchClose(s.bare, open);
        if (close < 0)
            return;
        for (const QString& text : stringLiteralsIn(s, open, close))
            out.found.append({text, tag});
    };
    // QStringList phases() const override { ... } and helpers such as buildPhases().
    static const QRegularExpression def(
        QStringLiteral("\\b(\\w*[pP]hases)\\s*\\([^()]*\\)\\s*(?:const\\b)?\\s*(?:override\\b)?\\s*\\{"));
    for (auto it = def.globalMatch(s.bare); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (!m.captured(1).startsWith(QLatin1String("set")))
            take(m.capturedEnd() - 1);
    }
    static const QRegularExpression set(QStringLiteral("\\bsetPhases\\s*\\("));
    for (auto it = set.globalMatch(s.bare); it.hasNext();)
        take(it.next().capturedEnd() - 1);
    static const QRegularExpression begin(QStringLiteral("(?:->|\\.)\\s*begin(?:Step)?\\s*\\(\\s*QStringLiteral\\s*\\("));
    for (auto it = begin.globalMatch(s.bare); it.hasNext();) {
        qsizetype pos = it.next().capturedEnd();
        if (const auto text = readLiterals(s.code, pos))
            out.found.append({*text, tag});
    }
}

// dependencies.add(id, name, purpose, probe): the name and the purpose.
inline void scanTools(const Source& s, const QString& where, Result& out)
{
    static const QRegularExpression add(QStringLiteral("\\bdependencies\\s*\\.\\s*add\\s*\\("));
    for (auto it = add.globalMatch(s.bare); it.hasNext();) {
        const auto args = argumentRanges(s.bare, it.next().capturedEnd() - 1);
        for (int i = 1; i <= 2 && i < args.size(); ++i)
            for (const QString& text : stringLiteralsIn(s, args.at(i).first, args.at(i).second))
                out.found.append({text, where + QStringLiteral(" (tool)")});
    }
}

// One file's text. `where` is its path below native/src; the data patterns
// (stages, tools) are not looked for in the interface's own code.
inline void scanText(const QString& text, const QString& where, Result& out)
{
    const Source s = prepare(text);
    scanMarked(s, where, out);
    if (!where.startsWith(QLatin1String("app/"))) {
        scanStages(s, where, out);
        scanTools(s, where, out);
    }
}

// Everything under `srcRoot` (native/src).
inline Result scanTree(const QString& srcRoot)
{
    Result out;
    QStringList files;
    for (QDirIterator it(srcRoot, {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
        files << it.next();
    files.sort();
    const QDir root(srcRoot);
    for (const QString& path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            out.problems << path + QStringLiteral(": could not be read");
            continue;
        }
        scanText(QString::fromUtf8(f.readAll()), root.relativeFilePath(path), out);
    }
    return out;
}

} // namespace fwtest::i18nscan
