#pragma once

// A QTranslator fed from a JSON table instead of a Qt Linguist .qm file.
//
// The English source text is the key and the context is ignored, exactly like
// the legacy Python edition's `_()`: {"translations": {"Home": "Start", …}}.
// A string that is not in the table returns a null QString, so Qt falls back
// to the source text. An entry whose value is "" is a deliberate empty
// translation (used for the English plural suffixes "s" / "ies" / "y") and
// returns an empty, non-null string.

#include <QHash>
#include <QString>
#include <QTranslator>

namespace fw::i18n {

class JsonTranslator : public QTranslator {
    Q_OBJECT
public:
    explicit JsonTranslator(QObject* parent = nullptr);

    // Loads a language file (a path or a ":/i18n/<code>.json" resource).
    // Returns false and keeps the previous table on any parse error;
    // errorString() says what went wrong.
    bool loadJson(const QString& path);
    bool loadJsonData(const QByteArray& json);

    QString translate(const char* context, const char* sourceText, const char* disambiguation = nullptr,
                      int n = -1) const override;
    bool isEmpty() const override { return m_table.isEmpty(); }

    int count() const { return int(m_table.size()); }
    QString languageCode() const { return m_code; }
    QString languageName() const { return m_name; }
    bool isRightToLeft() const { return m_rtl; }
    QString errorString() const { return m_error; }
    const QHash<QString, QString>& table() const { return m_table; }

private:
    QHash<QString, QString> m_table;
    QString m_code, m_name, m_error;
    bool m_rtl = false;
};

} // namespace fw::i18n
