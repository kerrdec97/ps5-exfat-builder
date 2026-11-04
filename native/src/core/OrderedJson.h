#pragma once

// An order-preserving JSON document model.
//
// QJsonObject sorts keys, which changes files such as param.json and
// playgo-scenario.json that the PS5 toolchain and console read. This DOM keeps
// member order and the exact text of numbers, and writes output byte-compatible
// with Python's json.dumps(indent=N, ensure_ascii=False) (the legacy app's
// writer), so round-tripping a file changes only what the caller changed.

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default; // null
    Value(bool b) : m_type(Type::Bool), m_bool(b) {}
    Value(const QString& s) : m_type(Type::String), m_text(s) {}
    Value(const char* s) : m_type(Type::String), m_text(QString::fromUtf8(s)) {}
    Value(int n) : Value(qint64(n)) {}
    Value(qint64 n) : m_type(Type::Number), m_text(QString::number(n)) {}
    Value(double d);

    static Value numberLiteral(const QString& literal); // trusted JSON number text
    static Value array();
    static Value object();

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool toBool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
    QString toString(const QString& fallback = {}) const { return m_type == Type::String ? m_text : fallback; }
    qint64 toInt(qint64 fallback = 0) const;
    double toDouble(double fallback = 0.0) const;
    QString numberText() const { return m_type == Type::Number ? m_text : QString(); }

    // Arrays
    int size() const { return int(m_items.size()); }
    const Value& at(int i) const;
    Value& at(int i);
    void append(Value v);
    void removeAt(int i);

    // Objects (ordered)
    bool contains(const QString& key) const { return indexOf(key) >= 0; }
    const Value& value(const QString& key) const; // null Value if missing
    Value& operator[](const QString& key);        // appends a null member if missing
    void insert(const QString& key, Value v);     // replaces in place, or appends
    bool remove(const QString& key);
    const QStringList& keys() const { return m_keys; }
    int indexOf(const QString& key) const;

    // Navigation helper: "a.b.c" through objects. Null Value if any step is missing.
    const Value& path(const QString& dottedPath) const;

    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    QString m_text;              // string content, or number literal
    std::vector<Value> m_items;  // array items, or object values (parallel to m_keys)
    QStringList m_keys;          // object keys in order
};

struct ParseOptions {
    bool allowBom = true;            // skip a leading UTF-8 BOM
    bool allowTrailingCommas = false;
    bool allowComments = false;      // // and /* */ (tolerant reads of hand-edited files)
    int maxDepth = 512;
};

Result<Value> parse(const QByteArray& utf8, const ParseOptions& options = {});

struct WriteOptions {
    int indent = 2;          // < 0: compact single line
    bool ensureAscii = false; // escape non-ASCII as \uXXXX (Python ensure_ascii=True)
    bool trailingNewline = false;
};

QByteArray write(const Value& value, const WriteOptions& options = {});

} // namespace fw::json
