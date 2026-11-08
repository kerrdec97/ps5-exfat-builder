#pragma once

// A line-preserving INI editor for the console tools' config files
// (ShadowMount+ config.ini / profiles.ini, flat and sectioned).
//
// The text is kept as lines; reads look keys up, writes change only the lines
// they touch. Comments, blank lines, the order of everything, unknown keys and
// unknown sections ([smb:NAME] connections, ...) survive a load-edit-save
// round trip byte for byte. This mirrors ShadowMount+'s own rule for its web
// editor: "Web edits preserve unrelated settings and comments".
//
//  * Section "" is everything before the first [section] header (the whole
//    file for flat configs).
//  * Sections and keys compare case-insensitively, after trimming; when a
//    key repeats, value() gives the last one (ShadowMount+'s readers do).
//  * Both modes end an unquoted value at the first # or ; (inline comment),
//    like ShadowMount+'s flat and v2 parsers. Quoted (v2) mode also reads
//    "..." values with \\ and \" escapes, and writes values containing
//    # ; " \ or leading/trailing spaces that way. Flat mode never quotes.

#include <QString>
#include <QStringList>

#include <vector>

namespace fw::ps5 {

class IniDocument {
public:
    enum class Quoting { None, V2 };

    IniDocument() = default;
    explicit IniDocument(const QString& text, Quoting quoting = Quoting::None);

    // The edited text. An untouched document gives back exactly the input
    // (line endings are normalised to the input's first line ending).
    QString text() const;
    Quoting quoting() const { return m_quoting; }

    QStringList sections() const;                 // in file order, "" first when it has keys
    bool hasSection(const QString& section) const;
    QStringList keys(const QString& section) const; // distinct, in file order
    bool contains(const QString& section, const QString& key) const;
    QString value(const QString& section, const QString& key) const;       // first occurrence
    QStringList values(const QString& section, const QString& key) const;  // every occurrence

    // Sets the value of the first occurrence (later duplicates are removed);
    // a missing key is appended to the end of its section (the section is
    // created at the end of the file when missing).
    void set(const QString& section, const QString& key, const QString& value);
    // Replaces every occurrence with one line per value, at the place of the
    // first occurrence (or appended like set()). An empty list removes the key.
    void setList(const QString& section, const QString& key, const QStringList& values);
    void remove(const QString& section, const QString& key);
    void removeSection(const QString& section);

    // Value text as written in this document's quoting.
    QString formatValue(const QString& value) const;
    static QString unquote(const QString& raw, bool v2);

private:
    struct Line {
        enum Kind { Other, Section, KeyValue } kind = Other;
        QString raw;      // the original text (without line ending)
        QString section;  // section this line belongs to (Section: its own name)
        QString key;      // KeyValue
        QString value;    // KeyValue, unquoted
        bool dirty = false;
    };
    QString render(const Line& l) const;
    int sectionEnd(const QString& section) const; // index after the section's last non-blank line (-1: missing)
    int sectionHeader(const QString& section) const;
    void insertAt(int index, Line line);

    std::vector<Line> m_lines;
    Quoting m_quoting = Quoting::None;
    QString m_eol = QStringLiteral("\n");
    bool m_trailingNewline = true;
};

} // namespace fw::ps5
