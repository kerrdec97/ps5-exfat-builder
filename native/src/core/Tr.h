#pragma once

// Interface languages.
//
// English is the source and the key: FW_TR("Add to queue") looks the English
// text up in the loaded catalogue and returns it unchanged when there is no
// entry or no catalogue (the default). A catalogue is a JSON file,
//
//   { "code": "de", "name": "Deutsch", "englishName": "German",
//     "strings": { "English text": "Deutscher Text", ... } }
//
// built into the app (`<resource folder>/<code>.json`) and/or kept by the user
// (`<user folder>/<code>.json`, which adds a language or corrects single
// entries of a built-in one). main() loads one catalogue before the first
// window; the language changes with a restart. docs/I18N.md has the rules for
// marking strings and for the catalogue files.
//
// A translation is data and can never break the app: an entry is dropped (the
// English text is shown) when entryProblem() finds it unsafe.

#include <QList>
#include <QString>
#include <QStringList>

namespace fw::i18n {

struct Language {
    QString code;        // "de", "pt-BR"
    QString nativeName;  // "Deutsch"
    QString englishName; // "German"
    bool builtIn = false; // shipped with the app (a user file may still correct it)
    int coverage = 0;    // per cent of the app's source strings that have a usable entry
};

// Where catalogues are read from: the folder of the built-in ones (a resource
// path such as ":/lang") and the user's folder (`<data>/lang`). Either may be
// empty. Call before load() and available(); the app does it once in main(),
// tests point it at temporary folders. Does not change the loaded catalogue.
void configure(const QString& resourceDir, const QString& userDir);
QString resourceDir();
QString userDir();

// The translation of `english`, or `english` itself when the loaded catalogue
// has no entry for it. Any thread.
QString translate(const QString& english);

// Loads the catalogue of `code` (built-in, then the user's file merged over
// it). "en" or an empty code unloads; "qps" is the pseudo-language that shows
// every translated text as ⟦text⟧ (for checking what is marked). Returns false
// and stays English when the code is not valid or has no usable file.
// `problem` receives what was ignored or dropped, one note per line (it can be
// set although the call succeeded). Meant to be called once, at start; texts
// that were already translated keep the language they were made in.
bool load(const QString& code, QString* problem = nullptr);

// "en" when nothing is loaded.
QString currentCode();

// Every language that has a usable catalogue, built-in or the user's, sorted
// by English name. English itself and "qps" are not in the list. Read from
// disk once and remembered until configure() is called again.
QList<Language> available();

// The fixed code of the pseudo-language.
QString pseudoCode();
// True for a well-formed language code: [a-z]{2,3}(-[A-Za-z]{2,4})?
bool isValidCode(const QString& code);

// Why `translation` may not stand in for `english` (empty: it may). Refused:
// an empty text, placeholders (%1 to %99, %L1, %n) that are not the same set,
// markup or "&" the English text does not have in the same way, control
// characters, a text out of all proportion to the English one.
QString entryProblem(const QString& english, const QString& translation);

// The English texts the app can show (`<resource folder>/_source.json`), the
// base of Language::coverage. Empty when the list is not there.
QStringList sourceTexts();

// What load() and available() ignored or dropped so far in this process, each
// note once. Every note is also written to the log when it is made; main()
// repeats the ones made before the log file was open.
QStringList notes();

} // namespace fw::i18n

// The text as the user reads it. The argument is one string literal (adjacent
// literals are joined), kept byte for byte: it is the catalogue key.
#define FW_TR(s) ::fw::i18n::translate(QStringLiteral(s))
// Marks a literal that is stored first and shown later (static tables, data
// handed to another module): fw::i18n::translate() it where it is shown.
#define FW_TR_NOOP(s) QStringLiteral(s)
