#pragma once

// The interface languages the app ships (docs/I18N.md). Shared by the
// general module (the general.language setting lists them) and the app's
// translator (app/i18n). English is the source language; the 16 others are
// the legacy Python edition's table codes.

#include <QString>

#include <vector>

namespace fw::i18n {

struct LanguageInfo {
    QString code;        // setting value and file name (:/i18n/<code>.json)
    QString englishName; // "German"
    QString nativeName;  // "Deutsch" — shown in the language picker
    bool rightToLeft;    // layout direction
};

// English first, then the 16 translated languages in the legacy order.
inline const std::vector<LanguageInfo>& supportedLanguages()
{
    static const std::vector<LanguageInfo> langs = {
        {QStringLiteral("en"), QStringLiteral("English"), QStringLiteral("English"), false},
        {QStringLiteral("zh"), QStringLiteral("Chinese"), QStringLiteral("中文"), false},
        {QStringLiteral("de"), QStringLiteral("German"), QStringLiteral("Deutsch"), false},
        {QStringLiteral("fr"), QStringLiteral("French"), QStringLiteral("Français"), false},
        {QStringLiteral("es"), QStringLiteral("Spanish"), QStringLiteral("Español"), false},
        {QStringLiteral("pt"), QStringLiteral("Portuguese"), QStringLiteral("Português"), false},
        {QStringLiteral("ja"), QStringLiteral("Japanese"), QStringLiteral("日本語"), false},
        {QStringLiteral("ko"), QStringLiteral("Korean"), QStringLiteral("한국어"), false},
        {QStringLiteral("ru"), QStringLiteral("Russian"), QStringLiteral("Русский"), false},
        {QStringLiteral("ar"), QStringLiteral("Arabic"), QStringLiteral("العربية"), true},
        {QStringLiteral("it"), QStringLiteral("Italian"), QStringLiteral("Italiano"), false},
        {QStringLiteral("nl"), QStringLiteral("Dutch"), QStringLiteral("Nederlands"), false},
        {QStringLiteral("pl"), QStringLiteral("Polish"), QStringLiteral("Polski"), false},
        {QStringLiteral("tr"), QStringLiteral("Turkish"), QStringLiteral("Türkçe"), false},
        {QStringLiteral("th"), QStringLiteral("Thai"), QStringLiteral("ไทย"), false},
        {QStringLiteral("vi"), QStringLiteral("Vietnamese"), QStringLiteral("Tiếng Việt"), false},
        {QStringLiteral("id"), QStringLiteral("Indonesian"), QStringLiteral("Bahasa Indonesia"), false},
    };
    return langs;
}

// The setting value that means "follow the operating system's language".
inline const QString kSystemLanguage = QStringLiteral("system");

inline const LanguageInfo* findLanguage(const QString& code)
{
    for (const LanguageInfo& l : supportedLanguages())
        if (l.code == code)
            return &l;
    return nullptr;
}

} // namespace fw::i18n
