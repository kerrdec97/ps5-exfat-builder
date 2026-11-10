#pragma once

// Order-preserving view of JSON text.
//
// QJsonObject sorts keys and stores numbers as doubles, which is fine for
// reading values but loses two things the legacy app relied on: the order of
// keys in the file (param.json's "first language that has a title" fallback)
// and byte-exact round trips (Y2JB edits must change only the values they
// target). This scanner finds members and their value spans in the original
// UTF-8 text so callers can read in file order and replace single values.

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace fw::gameinfo::json {

struct Member {
    QString key;
    qsizetype valueBegin = 0; // byte offset of the value's first character
    qsizetype valueEnd = 0;   // one past the value's last character
    char valueType = 0;       // '{', '[', '"', 'n' (number), 't'/'f' (bool), 'z' (null)
};

// Members of the top-level object (strict JSON, UTF-8, optional BOM).
Result<std::vector<Member>> topLevelMembers(const QByteArray& text);
// Members of the object reached by following `path` (object keys) from the top.
Result<std::vector<Member>> membersAt(const QByteArray& text, const QStringList& path);

// Returns `text` with each member's value replaced by the given raw JSON text.
// Replacements must not overlap.
QByteArray replaceValues(const QByteArray& text, std::vector<std::pair<Member, QByteArray>> replacements);

// `"..."` with JSON escapes, as UTF-8.
QByteArray quote(const QString& s);
// The decoded value of a string member (empty for other types).
QString stringValue(const QByteArray& text, const Member& m);

// Lenient clean-up for hand-edited or tool-written JSON: strips a UTF-8/UTF-16
// BOM (UTF-16 is converted), // and /* */ comments, trailing commas, NUL
// padding and anything after the top-level value. Invalid UTF-8 is replaced
// with U+FFFD like the legacy reader (errors='replace'). `changed` is set when
// the output differs from the input in more than a UTF-8 BOM and surrounding
// white space.
QByteArray repair(const QByteArray& text, bool* changed = nullptr);

} // namespace fw::gameinfo::json
