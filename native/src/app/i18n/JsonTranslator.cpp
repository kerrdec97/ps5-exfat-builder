#include "app/i18n/JsonTranslator.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

// The language files live in a .qrc compiled into the static fw_app_lib; a
// static library's resources must be registered explicitly, from the global
// namespace.
static void initI18nResources()
{
    Q_INIT_RESOURCE(i18n);
}

namespace fw::i18n {

JsonTranslator::JsonTranslator(QObject* parent) : QTranslator(parent) {}

bool JsonTranslator::loadJson(const QString& path)
{
    initI18nResources();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    return loadJsonData(f.readAll());
}

bool JsonTranslator::loadJsonData(const QByteArray& json)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        m_error = QStringLiteral("not a JSON object: %1").arg(err.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    const QJsonValue strings = root.value(QStringLiteral("translations"));
    if (!strings.isObject()) {
        m_error = QStringLiteral("missing \"translations\" object");
        return false;
    }
    QHash<QString, QString> table;
    const QJsonObject o = strings.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!it.value().isString()) {
            m_error = QStringLiteral("translation of \"%1\" is not a string").arg(it.key());
            return false;
        }
        table.insert(it.key(), it.value().toString());
    }
    m_table = std::move(table);
    m_code = root.value(QStringLiteral("language")).toString();
    m_name = root.value(QStringLiteral("name")).toString();
    m_rtl = root.value(QStringLiteral("direction")).toString() == QLatin1String("rtl");
    m_error.clear();
    return true;
}

QString JsonTranslator::translate(const char* /*context*/, const char* sourceText, const char* /*disambiguation*/,
                                  int /*n*/) const
{
    if (!sourceText || !*sourceText)
        return {};
    const auto it = m_table.constFind(QString::fromUtf8(sourceText));
    if (it == m_table.constEnd())
        return {}; // null: Qt falls back to the source text
    if (it.value().isEmpty())
        return QString(QLatin1String("")); // empty but not null: a deliberate empty translation
    return it.value();
}

} // namespace fw::i18n
