#include "app/i18n/I18n.h"

#include "app/i18n/JsonTranslator.h"

#include <QGuiApplication>
#include <QLocale>
#include <QPointer>

namespace fw::i18n {

namespace {
QPointer<JsonTranslator> g_translator;
QString g_current = QStringLiteral("en");
} // namespace

QString trs(const QString& text)
{
    if (text.isEmpty())
        return text;
    return QCoreApplication::translate("fw", text.toUtf8().constData());
}

SettingDef localized(const SettingDef& def)
{
    SettingDef d = def;
    d.label = trs(def.label);
    d.description = trs(def.description);
    d.unit = trs(def.unit);
    d.fileFilter = trs(def.fileFilter);
    for (SettingChoice& c : d.choices) {
        c.label = trs(c.label);
        c.description = trs(c.description);
    }
    return d;
}

QString systemLanguage()
{
    const QStringList ui = QLocale::system().uiLanguages();
    for (const QString& tag : ui) {
        const QString lang = tag.section(QLatin1Char('-'), 0, 0).section(QLatin1Char('_'), 0, 0).toLower();
        if (findLanguage(lang))
            return lang;
    }
    return QStringLiteral("en");
}

QString resolve(const QString& settingValue)
{
    const QString v = settingValue.trimmed().toLower();
    if (v.isEmpty() || v == QLatin1String("en"))
        return QStringLiteral("en");
    if (v == kSystemLanguage)
        return systemLanguage();
    if (findLanguage(v))
        return v;
    return QStringLiteral("en");
}

QString install(QCoreApplication* app, const QString& code)
{
    const QString wanted = resolve(code);
    if (g_translator) {
        if (app)
            app->removeTranslator(g_translator);
        delete g_translator.data();
        g_translator = nullptr;
    }
    const LanguageInfo* info = findLanguage(wanted);
    bool rtl = false;
    g_current = QStringLiteral("en");
    if (app && info && wanted != QLatin1String("en")) {
        auto* t = new JsonTranslator(app);
        if (t->loadJson(QStringLiteral(":/i18n/%1.json").arg(wanted)) && app->installTranslator(t)) {
            g_translator = t;
            g_current = wanted;
            rtl = info->rightToLeft;
        } else {
            delete t;
        }
    }
    if (auto* gui = qobject_cast<QGuiApplication*>(app))
        gui->setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
    return g_current;
}

QString currentLanguage()
{
    return g_current;
}

QString midSentence(const QString& value)
{
    return g_current == QLatin1String("en") ? value.toLower() : value;
}

} // namespace fw::i18n
