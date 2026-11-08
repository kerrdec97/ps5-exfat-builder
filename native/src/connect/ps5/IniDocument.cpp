#include "connect/ps5/IniDocument.h"

#include <algorithm>

namespace fw::ps5 {

namespace {

bool same(const QString& a, const QString& b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

bool isSectionLine(const QString& t, QString* name)
{
    if (!t.startsWith(u'['))
        return false;
    // The header ends at the first ']' that only a comment (or nothing)
    // follows: a name may hold brackets itself ("[image:Game [EU].exfat]").
    for (qsizetype close = t.indexOf(u']'); close >= 0; close = t.indexOf(u']', close + 1)) {
        const QString rest = t.mid(close + 1).trimmed();
        if (!rest.isEmpty() && !rest.startsWith(u'#') && !rest.startsWith(u';'))
            continue;
        *name = t.mid(1, close - 1).trimmed();
        return true;
    }
    return false;
}

bool needsQuotes(const QString& v)
{
    if (v.isEmpty())
        return false;
    if (v.front().isSpace() || v.back().isSpace())
        return true;
    for (QChar c : v)
        if (c == u'#' || c == u';' || c == u'"' || c == u'\\')
            return true;
    return false;
}

} // namespace

QString IniDocument::unquote(const QString& rawIn, bool v2)
{
    const QString raw = rawIn.trimmed();
    if (v2 && raw.startsWith(u'"')) {
        QString out;
        for (qsizetype i = 1; i < raw.size(); ++i) {
            const QChar c = raw.at(i);
            if (c == u'\\' && i + 1 < raw.size()) {
                out += raw.at(++i);
                continue;
            }
            if (c == u'"')
                return out;
            out += c;
        }
        return out; // unterminated: take what is there
    }
    // Both ShadowMount+ readers end an unquoted value at the first # or ;.
    qsizetype cut = raw.size();
    for (qsizetype i = 0; i < raw.size(); ++i)
        if (raw.at(i) == u'#' || raw.at(i) == u';') {
            cut = i;
            break;
        }
    return raw.left(cut).trimmed();
}

IniDocument::IniDocument(const QString& text, Quoting quoting) : m_quoting(quoting)
{
    if (text.isEmpty())
        return;
    QStringList rows = text.split(u'\n');
    m_trailingNewline = text.endsWith(u'\n');
    if (m_trailingNewline)
        rows.removeLast();
    if (!rows.isEmpty() && rows.front().endsWith(u'\r'))
        m_eol = QStringLiteral("\r\n");
    QString section;
    for (QString row : rows) {
        if (row.endsWith(u'\r'))
            row.chop(1);
        Line l;
        l.raw = row;
        const QString t = row.trimmed();
        QString name;
        if (isSectionLine(t, &name)) {
            l.kind = Line::Section;
            section = name;
            l.section = name;
        } else if (!t.isEmpty() && !t.startsWith(u'#') && !t.startsWith(u';')) {
            const qsizetype eq = t.indexOf(u'=');
            const QString key = eq > 0 ? t.left(eq).trimmed() : QString();
            if (!key.isEmpty()) {
                l.kind = Line::KeyValue;
                l.key = key;
                l.value = unquote(t.mid(eq + 1), m_quoting == Quoting::V2);
            }
            l.section = section;
        } else {
            l.section = section;
        }
        m_lines.push_back(l);
    }
}

QString IniDocument::formatValue(const QString& value) const
{
    if (m_quoting == Quoting::None || !needsQuotes(value))
        return value;
    QString out = QStringLiteral("\"");
    for (QChar c : value) {
        if (c == u'\\' || c == u'"')
            out += u'\\';
        out += c;
    }
    return out + u'"';
}

QString IniDocument::render(const Line& l) const
{
    if (!l.dirty)
        return l.raw;
    // Keep the spacing style of the line that is being changed.
    bool spaced = m_quoting == Quoting::V2;
    if (!l.raw.isEmpty()) {
        const qsizetype eq = l.raw.indexOf(u'=');
        spaced = eq > 0 && l.raw.at(eq - 1).isSpace();
    }
    QString indent;
    for (QChar c : l.raw) {
        if (!c.isSpace())
            break;
        indent += c;
    }
    return indent + l.key + (spaced ? QStringLiteral(" = ") : QStringLiteral("=")) + formatValue(l.value);
}

QString IniDocument::text() const
{
    QStringList out;
    out.reserve(qsizetype(m_lines.size()));
    for (const Line& l : m_lines)
        out << render(l);
    if (out.isEmpty())
        return {};
    return out.join(m_eol) + (m_trailingNewline ? m_eol : QString());
}

QStringList IniDocument::sections() const
{
    QStringList out;
    for (const Line& l : m_lines) {
        if (l.kind == Line::KeyValue && l.section.isEmpty() && !out.contains(QString()))
            out.prepend(QString());
        if (l.kind == Line::Section && !out.contains(l.section, Qt::CaseInsensitive))
            out << l.section;
    }
    return out;
}

bool IniDocument::hasSection(const QString& section) const
{
    if (section.isEmpty())
        return true;
    return sectionHeader(section) >= 0;
}

QStringList IniDocument::keys(const QString& section) const
{
    QStringList out;
    for (const Line& l : m_lines)
        if (l.kind == Line::KeyValue && same(l.section, section) && !out.contains(l.key, Qt::CaseInsensitive))
            out << l.key;
    return out;
}

bool IniDocument::contains(const QString& section, const QString& key) const
{
    return std::any_of(m_lines.begin(), m_lines.end(), [&](const Line& l) {
        return l.kind == Line::KeyValue && same(l.section, section) && same(l.key, key);
    });
}

QString IniDocument::value(const QString& section, const QString& key) const
{
    // The last occurrence wins, like ShadowMount+'s own readers.
    for (auto it = m_lines.rbegin(); it != m_lines.rend(); ++it)
        if (it->kind == Line::KeyValue && same(it->section, section) && same(it->key, key))
            return it->value;
    return {};
}

QStringList IniDocument::values(const QString& section, const QString& key) const
{
    QStringList out;
    for (const Line& l : m_lines)
        if (l.kind == Line::KeyValue && same(l.section, section) && same(l.key, key))
            out << l.value;
    return out;
}

int IniDocument::sectionHeader(const QString& section) const
{
    for (int i = 0; i < int(m_lines.size()); ++i)
        if (m_lines[size_t(i)].kind == Line::Section && same(m_lines[size_t(i)].section, section))
            return i;
    return -1;
}

int IniDocument::sectionEnd(const QString& section) const
{
    int begin = 0;
    if (!section.isEmpty()) {
        const int h = sectionHeader(section);
        if (h < 0)
            return -1;
        begin = h + 1;
    }
    int end = begin;
    while (end < int(m_lines.size()) && m_lines[size_t(end)].kind != Line::Section)
        ++end;
    // Before the blank lines that separate this section from the next one.
    int pos = end;
    while (pos > begin && m_lines[size_t(pos - 1)].kind == Line::Other && m_lines[size_t(pos - 1)].raw.trimmed().isEmpty())
        --pos;
    return pos;
}

void IniDocument::insertAt(int index, Line line)
{
    m_lines.insert(m_lines.begin() + std::clamp(index, 0, int(m_lines.size())), std::move(line));
}

void IniDocument::set(const QString& section, const QString& key, const QString& value)
{
    setList(section, key, {value});
}

void IniDocument::setList(const QString& section, const QString& key, const QStringList& values)
{
    // Unchanged: leave every line exactly as it is.
    if (values == this->values(section, key))
        return;
    int first = -1;
    QString likeRaw, keyAsWritten = key;
    for (int i = 0; i < int(m_lines.size());) {
        const Line& l = m_lines[size_t(i)];
        if (l.kind == Line::KeyValue && same(l.section, section) && same(l.key, key)) {
            if (first < 0) {
                first = i;
                likeRaw = l.raw;
                keyAsWritten = l.key;
            }
            m_lines.erase(m_lines.begin() + i);
        } else {
            ++i;
        }
    }
    if (values.isEmpty())
        return;
    int at = first;
    if (at < 0) {
        at = sectionEnd(section);
        if (at >= 0) {
            // Right after the section's last key, so new keys join the others
            // (not after a comment block that ends the section).
            const int begin = section.isEmpty() ? 0 : sectionHeader(section) + 1;
            for (int i = at - 1; i >= begin; --i)
                if (m_lines[size_t(i)].kind == Line::KeyValue) {
                    at = i + 1;
                    break;
                }
        }
        if (at < 0) {
            // A new section at the end, after one blank line.
            if (!m_lines.empty() && !m_lines.back().raw.trimmed().isEmpty()) {
                Line blank;
                blank.section = m_lines.back().section;
                m_lines.push_back(blank);
            }
            Line header;
            header.kind = Line::Section;
            header.section = section;
            header.raw = QStringLiteral("[%1]").arg(section);
            m_lines.push_back(header);
            at = int(m_lines.size());
        }
    }
    for (const QString& v : values) {
        Line l;
        l.kind = Line::KeyValue;
        l.section = section;
        l.key = keyAsWritten;
        l.value = v;
        l.raw = likeRaw;
        l.dirty = true;
        insertAt(at++, l);
    }
}

void IniDocument::remove(const QString& section, const QString& key)
{
    setList(section, key, {});
}

void IniDocument::removeSection(const QString& section)
{
    if (section.isEmpty()) {
        m_lines.erase(std::remove_if(m_lines.begin(), m_lines.end(),
                                     [](const Line& l) { return l.kind == Line::KeyValue && l.section.isEmpty(); }),
                      m_lines.end());
        return;
    }
    const int h = sectionHeader(section);
    if (h < 0)
        return;
    int end = h + 1;
    while (end < int(m_lines.size()) && m_lines[size_t(end)].kind != Line::Section)
        ++end;
    // Take the blank line that separated it from the section before, too.
    int begin = h;
    if (begin > 0 && m_lines[size_t(begin - 1)].kind == Line::Other && m_lines[size_t(begin - 1)].raw.trimmed().isEmpty())
        --begin;
    m_lines.erase(m_lines.begin() + begin, m_lines.begin() + end);
}

} // namespace fw::ps5
