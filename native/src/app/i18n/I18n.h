#pragma once

// Interface translations (docs/I18N.md).
//
//  * fw::tr(text)        — translates one user-visible string. Inside a
//                          Q_OBJECT class the unqualified `tr(...)` resolves
//                          to QObject::tr instead; both end in the same table,
//                          because the JsonTranslator ignores the context and
//                          uses the English text as the key.
//  * i18n::trs(QString)  — the same for text that only exists at run time
//                          (the engines' SettingDef labels and descriptions).
//  * i18n::install(...)  — loads :/i18n/<code>.json into the application and
//                          switches the layout direction for right-to-left
//                          languages. English (or an unknown code) removes the
//                          translator.
//
// Strings produced by engines (job summaries, errors, log lines) are never
// translated: they are what users paste into problem reports.

#include "core/Languages.h"
#include "core/Settings.h"

#include <QCoreApplication>
#include <QString>

namespace fw {

inline QString tr(const char* text)
{
    return QCoreApplication::translate("fw", text);
}

namespace i18n {

// Translates text that is a QString at run time (setting labels, choice
// labels, section and group names). Empty in → empty out.
QString trs(const QString& text);

// A copy of `def` with label, description, unit, file filter and the choice
// labels/descriptions translated. key, section, group and choice values are
// untouched: they are identifiers.
SettingDef localized(const SettingDef& def);

// The language the operating system asks for, when the app ships it; "en"
// otherwise. Matches on the language part of QLocale::system().uiLanguages()
// ("de-AT" → "de", "zh-Hant-TW" → "zh", "pt-BR" → "pt").
QString systemLanguage();

// Turns a setting value ("system", "de", "", garbage) into a shipped code.
QString resolve(const QString& settingValue);

// Installs the translator for `code` into the application (replacing the one
// installed before) and sets the layout direction. Returns the code that is
// now active ("en" when the file could not be loaded).
QString install(QCoreApplication* app, const QString& code);

// The code install() activated last, "en" before any call.
QString currentLanguage();

} // namespace i18n
} // namespace fw
