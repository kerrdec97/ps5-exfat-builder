// Interface translations (docs/I18N.md): every shipped language file is valid
// JSON with the expected header, holds only keys that exist in the app's
// source strings (tr("…") literals under src/app plus the settings schema's
// labels, descriptions, sections, groups and choices), keeps every %n
// placeholder, and translates the first-screen strings (navigation, page
// titles, settings sections and labels). The translator returns the
// translation, falls back to English, and the main window comes up in German
// and in Arabic (right-to-left).
//
// FW_I18N_DUMP=<file>     writes the English source-string list (one per line)
// FW_I18N_COVERAGE=<file> writes the per-language coverage table (Markdown)
// FW_I18N_SHOTS=<dir>     saves screenshots of the German and Arabic windows

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/i18n/I18n.h"
#include "app/i18n/JsonTranslator.h"
#include "app/pages/Page.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Shell.h"
#include "app/ui/Theme.h"
#include "core/Languages.h"
#include "engines/general/Module.h"
#include "support/TestRegistry.h"
#include "support/TestSupport.h"

#include <QAbstractButton>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

// Decodes one or more adjacent C++ string literals ("a" "b") into text.
QString decodeLiterals(const QString& literals, bool* ok)
{
    QString out;
    bool inString = false;
    *ok = true;
    for (int i = 0; i < literals.size(); ++i) {
        const QChar c = literals.at(i);
        if (!inString) {
            if (c == u'"')
                inString = true;
            continue;
        }
        if (c == u'"') {
            inString = false;
            continue;
        }
        if (c != u'\\') {
            out += c;
            continue;
        }
        if (++i >= literals.size()) {
            *ok = false;
            return out;
        }
        const QChar e = literals.at(i);
        switch (e.unicode()) {
        case u'"': out += u'"'; break;
        case u'\\': out += u'\\'; break;
        case u'n': out += u'\n'; break;
        case u't': out += u'\t'; break;
        case u'r': out += u'\r'; break;
        case u'\'': out += u'\''; break;
        default: *ok = false; return out; // keep the sources to simple escapes
        }
    }
    return out;
}

// Every tr("…") literal under src/app (the app's own source strings).
QSet<QString> scanSourceStrings(QStringList* problems)
{
    QSet<QString> out;
    static const QRegularExpression re(QStringLiteral(R"(\btr\(\s*("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*))"));
    QDirIterator it(QString::fromUtf8(FW_APP_SOURCE_DIR), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files,
                    QDirIterator::Subdirectories);
    int files = 0;
    while (it.hasNext()) {
        const QString path = it.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        ++files;
        const QString src = QString::fromUtf8(f.readAll());
        auto m = re.globalMatch(src);
        while (m.hasNext()) {
            bool ok = true;
            const QString text = decodeLiterals(m.next().captured(1), &ok);
            if (!ok)
                *problems << QStringLiteral("%1: unsupported escape in %2").arg(path, text);
            else if (!text.isEmpty())
                out.insert(text);
        }
    }
    if (files < 40)
        *problems << QStringLiteral("only %1 source files found under %2").arg(files).arg(QString::fromUtf8(FW_APP_SOURCE_DIR));
    return out;
}

// The schema's user-visible strings (translated at display time).
QSet<QString> schemaStrings(const SettingsSchema& schema)
{
    QSet<QString> out;
    for (const SettingDef* d : schema.all()) {
        for (const QString& s : {d->section, d->group, d->label, d->description, d->unit, d->fileFilter})
            if (!s.isEmpty())
                out.insert(s);
        for (const SettingChoice& c : d->choices) {
            if (!c.label.isEmpty())
                out.insert(c.label);
            if (!c.description.isEmpty())
                out.insert(c.description);
        }
    }
    for (const QString& s : SettingsSchema::sectionOrder())
        out.insert(s);
    return out;
}

QSet<QString> placeholders(const QString& s)
{
    QSet<QString> out;
    static const QRegularExpression re(QStringLiteral("%[1-9]"));
    auto m = re.globalMatch(s);
    while (m.hasNext())
        out.insert(m.next().captured(0));
    return out;
}

// What every user sees first: these must be translated in every language.
QStringList firstScreenStrings(const SettingsSchema& schema)
{
    QStringList must = {
        QStringLiteral("Home"), QStringLiteral("Build image"), QStringLiteral("Extract & verify"), QStringLiteral("Edit image"),
        QStringLiteral("Backport"), QStringLiteral("Queue"), QStringLiteral("PS5 connect"), QStringLiteral("Library"),
        QStringLiteral("Logs"), QStringLiteral("Settings"), QStringLiteral("About"), QStringLiteral("Workflows"),
        QStringLiteral("Tools"), QStringLiteral("Build now"), QStringLiteral("Build package"), QStringLiteral("Extract"),
        QStringLiteral("Cancel"), QStringLiteral("Close"), QStringLiteral("Save"), QStringLiteral("Retry"),
        QStringLiteral("Queued"), QStringLiteral("Running"), QStringLiteral("Done"), QStringLiteral("Failed"),
        QStringLiteral("Paused"), QStringLiteral("Cancelled"), QStringLiteral("Ready"), QStringLiteral("Not connected"),
        QStringLiteral("Show advanced"), QStringLiteral("Search settings"), QStringLiteral("Reset section"),
        QStringLiteral("Games"), QStringLiteral("Languages"), QStringLiteral("DLC emulator"),
        QStringLiteral("FTP browser"), QStringLiteral("Payloads"), QStringLiteral("Kernel log"), QStringLiteral("Diagnostics"),
        QStringLiteral("No jobs yet"), QStringLiteral("No games found"), QStringLiteral("No jobs in the queue"),
        QStringLiteral("Use default"), QStringLiteral("Overridden for this job"), QStringLiteral("Takes effect after a restart."),
    };
    for (const QString& s : SettingsSchema::sectionOrder())
        must << s;
    for (const SettingDef* d : schema.all()) {
        must << d->label;
        if (!d->group.isEmpty())
            must << d->group;
    }
    must.removeDuplicates();
    return must;
}

} // namespace

class TstI18n : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    fwtest::Registry m_reg;
    QSet<QString> m_source; // tr() literals + schema strings

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        theme::apply(qApp);
        QStringList problems;
        m_source = scanSourceStrings(&problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(u'\n')));
        QVERIFY(m_source.size() > 1000);
        QVERIFY(m_source.contains(QStringLiteral("Build image")));
        m_source.unite(schemaStrings(*m_reg.schema));
        QVERIFY(m_source.contains(QStringLiteral("Interface language")));
        const QString dump = qEnvironmentVariable("FW_I18N_DUMP");
        if (!dump.isEmpty()) {
            QStringList all(m_source.begin(), m_source.end());
            all.sort();
            QFile f(dump);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QJsonDocument(QJsonArray::fromStringList(all)).toJson());
        }
    }

    void cleanupTestCase() { i18n::install(qApp, QStringLiteral("en")); }

    void languageListMatchesLegacy()
    {
        const auto& langs = i18n::supportedLanguages();
        QCOMPARE(int(langs.size()), 17);
        QCOMPARE(langs.front().code, QStringLiteral("en"));
        const QStringList legacy = QStringLiteral("zh de fr es pt ja ko ru ar it nl pl tr th vi id").split(u' ');
        for (const QString& code : legacy)
            QVERIFY2(i18n::findLanguage(code), qPrintable(code));
        QVERIFY(i18n::findLanguage(QStringLiteral("ar"))->rightToLeft);
        QVERIFY(!i18n::findLanguage(QStringLiteral("de"))->rightToLeft);
        // The setting lists "system", then every language by its own name.
        const SettingDef* d = m_reg.schema->find(general::keys::kLanguage);
        QVERIFY(d);
        QCOMPARE(d->section, QStringLiteral("General"));
        QCOMPARE(d->defaultValue.toString(), QStringLiteral("en"));
        QVERIFY(d->restartRequired);
        QCOMPARE(d->choices.size(), 18);
        QCOMPARE(d->choices.first().value, i18n::kSystemLanguage);
        QCOMPARE(d->choices.at(3).label, QStringLiteral("Deutsch"));
        QCOMPARE(d->choices.at(7).label, QStringLiteral("日本語"));
    }

    void languageFiles_data()
    {
        QTest::addColumn<QString>("code");
        for (const auto& l : i18n::supportedLanguages())
            if (l.code != QLatin1String("en"))
                QTest::newRow(qPrintable(l.code)) << l.code;
    }

    void languageFiles()
    {
        QFETCH(QString, code);
        const auto* info = i18n::findLanguage(code);
        QVERIFY(info);
        QFile f(QStringLiteral(":/i18n/%1.json").arg(code));
        QVERIFY2(f.open(QIODevice::ReadOnly), "language file missing from the resources");
        const QByteArray json = f.readAll();
        QJsonParseError err{};
        const QJsonObject root = QJsonDocument::fromJson(json, &err).object();
        QCOMPARE(err.error, QJsonParseError::NoError);
        QCOMPARE(root.value(QStringLiteral("language")).toString(), code);
        QCOMPARE(root.value(QStringLiteral("name")).toString(), info->nativeName);
        QCOMPARE(root.value(QStringLiteral("direction")).toString(), info->rightToLeft ? QStringLiteral("rtl") : QStringLiteral("ltr"));
        QVERIFY2(root.value(QStringLiteral("status")).toString().contains(QLatin1String("machine")), "the header must say the file is machine-translated");

        i18n::JsonTranslator t;
        QVERIFY2(t.loadJsonData(json), qPrintable(t.errorString()));
        QVERIFY(t.count() > 0);
        QStringList unknown, badPlaceholders, empty;
        const auto& table = t.table();
        for (auto it = table.cbegin(); it != table.cend(); ++it) {
            if (!m_source.contains(it.key()))
                unknown << it.key();
            if (placeholders(it.key()) != placeholders(it.value()) && !it.value().isEmpty())
                badPlaceholders << it.key();
            if (it.value().isEmpty() && it.key() != QLatin1String("s") && it.key() != QLatin1String("ies") && it.key() != QLatin1String("y"))
                empty << it.key();
        }
        QVERIFY2(unknown.isEmpty(), qPrintable(QStringLiteral("keys not in the sources: ") + unknown.join(QStringLiteral(" | "))));
        QVERIFY2(badPlaceholders.isEmpty(), qPrintable(QStringLiteral("placeholders differ: ") + badPlaceholders.join(QStringLiteral(" | "))));
        QVERIFY2(empty.isEmpty(), qPrintable(QStringLiteral("empty translations: ") + empty.join(QStringLiteral(" | "))));
        // Plural suffixes are deliberately empty in every language.
        for (const char* suffix : {"s", "ies", "y"})
            QVERIFY2(table.contains(QString::fromLatin1(suffix)) && table.value(QString::fromLatin1(suffix)).isEmpty(), suffix);

        QStringList missing;
        for (const QString& s : firstScreenStrings(*m_reg.schema))
            if (!table.contains(s))
                missing << s;
        QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("first-screen strings not translated: ") + missing.join(QStringLiteral(" | "))));
    }

    void coverageReport()
    {
        QStringList lines;
        lines << QStringLiteral("| Language | Code | Translated | Of | Coverage |") << QStringLiteral("|---|---|---|---|---|");
        const int total = m_source.size();
        for (const auto& l : i18n::supportedLanguages()) {
            if (l.code == QLatin1String("en"))
                continue;
            i18n::JsonTranslator t;
            QVERIFY(t.loadJson(QStringLiteral(":/i18n/%1.json").arg(l.code)));
            int n = 0;
            for (auto it = t.table().cbegin(); it != t.table().cend(); ++it)
                if (m_source.contains(it.key()))
                    ++n;
            lines << QStringLiteral("| %1 (%2) | %3 | %4 | %5 | %6 % |")
                         .arg(l.englishName, l.nativeName, l.code)
                         .arg(n)
                         .arg(total)
                         .arg(100.0 * n / total, 0, 'f', 1);
            qInfo("%s: %d / %d (%.1f %%)", qPrintable(l.code), n, total, 100.0 * n / total);
        }
        const QString out = qEnvironmentVariable("FW_I18N_COVERAGE");
        if (!out.isEmpty()) {
            QFile f(out);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write((lines.join(u'\n') + u'\n').toUtf8());
        }
    }

    void translatorLookupAndFallback()
    {
        QCOMPARE(i18n::install(qApp, QStringLiteral("de")), QStringLiteral("de"));
        QCOMPARE(i18n::currentLanguage(), QStringLiteral("de"));
        QCOMPARE(qApp->layoutDirection(), Qt::LeftToRight);
        i18n::JsonTranslator ref;
        QVERIFY(ref.loadJson(QStringLiteral(":/i18n/de.json")));
        QCOMPARE(fw::tr("Settings"), ref.table().value(QStringLiteral("Settings")));
        QVERIFY(fw::tr("Settings") != QStringLiteral("Settings"));
        QCOMPARE(QObject::tr("Settings"), fw::tr("Settings"));         // context is ignored
        QCOMPARE(i18n::trs(QStringLiteral("Settings")), fw::tr("Settings"));
        QCOMPARE(fw::tr("This string is not in any table"), QStringLiteral("This string is not in any table"));
        QCOMPARE(i18n::trs(QString()), QString());
        // Deliberately empty translation: empty but not null, so Qt does not fall back.
        QVERIFY(fw::tr("s").isEmpty());
        QCOMPARE(QStringLiteral("%1 x%2").arg(3).arg(fw::tr("s")), QStringLiteral("3 x"));
        // Schema strings are translated at display time only.
        const SettingDef* d = m_reg.schema->find(general::keys::kLanguage);
        QVERIFY(d);
        QCOMPARE(d->label, QStringLiteral("Interface language"));
        const SettingDef loc = i18n::localized(*d);
        QCOMPARE(loc.key, d->key);
        QCOMPARE(loc.section, d->section);
        QCOMPARE(loc.label, ref.table().value(QStringLiteral("Interface language")));
        QCOMPARE(loc.choices.at(3).label, QStringLiteral("Deutsch"));
        QCOMPARE(loc.choices.at(3).value, QStringLiteral("de"));

        QCOMPARE(i18n::resolve(QStringLiteral("DE")), QStringLiteral("de"));
        QCOMPARE(i18n::resolve(QStringLiteral("klingon")), QStringLiteral("en"));
        QCOMPARE(i18n::resolve(QString()), QStringLiteral("en"));
        QVERIFY(i18n::findLanguage(i18n::resolve(i18n::kSystemLanguage)));
        QVERIFY(i18n::findLanguage(i18n::systemLanguage()));

        QCOMPARE(i18n::install(qApp, QStringLiteral("en")), QStringLiteral("en"));
        QCOMPARE(fw::tr("Settings"), QStringLiteral("Settings"));
        QCOMPARE(fw::tr("s"), QStringLiteral("s"));
        QCOMPARE(i18n::install(qApp, QStringLiteral("nope")), QStringLiteral("en"));
    }

    void legacyLanguageImports()
    {
        SettingsStore store(m_reg.schema, m_root.filePath(QStringLiteral("legacy/settings.json")));
        QVERIFY(store.load().ok());
        const QString legacyFile = m_root.filePath(QStringLiteral("legacy/old.json"));
        QDir().mkpath(m_root.filePath(QStringLiteral("legacy")));
        QVERIFY(fwtest::writeTextFile(legacyFile, "{\"language\": \"ja\"}"));
        QVERIFY(store.importLegacy(legacyFile).ok());
        QCOMPARE(store.value(general::keys::kLanguage).toString(), QStringLiteral("ja"));
        QVERIFY(fwtest::writeTextFile(legacyFile, "{\"language\": \"en_us\"}"));
        QVERIFY(store.importLegacy(legacyFile).ok());
        QCOMPARE(store.value(general::keys::kLanguage).toString(), QStringLiteral("en"));
        QVERIFY(fwtest::writeTextFile(legacyFile, "{\"language\": \"xx\"}"));
        QVERIFY(store.importLegacy(legacyFile).ok());
        QCOMPARE(store.value(general::keys::kLanguage).toString(), QStringLiteral("en")); // unknown code skipped
    }

    void mainWindowInGerman() { checkWindow(QStringLiteral("de"), Qt::LeftToRight); }
    void mainWindowInArabic() { checkWindow(QStringLiteral("ar"), Qt::RightToLeft); }

private:
    void checkWindow(const QString& code, Qt::LayoutDirection direction)
    {
        QCOMPARE(i18n::install(qApp, code), code);
        QCOMPARE(qApp->layoutDirection(), direction);
        i18n::JsonTranslator ref;
        QVERIFY(ref.loadJson(QStringLiteral(":/i18n/%1.json").arg(code)));
        AppContext ctx;
        QVERIFY(ctx.initialize().ok());
        MainWindow window(ctx);
        window.resize(1440, 920);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.layoutDirection(), direction);
        // Navigation labels come from the table.
        for (const char* key : {"home", "build", "queue", "settings"}) {
            auto* item = window.sidebar()->item(QString::fromLatin1(key));
            QVERIFY2(item, key);
            const QString english = QString::fromLatin1(key == QLatin1String("home") ? "Home" : key == QLatin1String("build") ? "Build image" : key == QLatin1String("queue") ? "Queue" : "Settings");
            QCOMPARE(item->text(), ref.table().value(english));
            QVERIFY(item->text() != english);
        }
        // Page title and the settings section list.
        window.navigate(QStringLiteral("settings"));
        pages::Page* settings = window.page(QStringLiteral("settings"));
        QVERIFY(settings);
        QStringList labels;
        for (QLabel* l : settings->findChildren<QLabel*>())
            labels << l->text();
        QVERIFY2(labels.contains(ref.table().value(QStringLiteral("General"))), "translated section title not shown");
        QStringList buttons;
        for (QAbstractButton* b : settings->findChildren<QAbstractButton*>())
            buttons << b->text();
        QVERIFY2(buttons.contains(ref.table().value(QStringLiteral("Storage"))), "translated section button not shown");
        // The setting label is translated at display time; the schema stays English.
        auto rows = settings->findChildren<ui::SettingRow*>();
        bool found = false;
        for (ui::SettingRow* r : rows)
            if (r->def().key == general::keys::kLanguage) {
                found = true;
                QCOMPARE(r->labelWidget()->text(), ref.table().value(QStringLiteral("Interface language")));
            }
        QVERIFY(found);
        QCOMPARE(ctx.schema().find(general::keys::kLanguage)->label, QStringLiteral("Interface language"));
        const QString shots = qEnvironmentVariable("FW_I18N_SHOTS");
        if (!shots.isEmpty())
            QVERIFY(window.screenshotAllPages(QDir(shots).filePath(code)) > 0);
        i18n::install(qApp, QStringLiteral("en"));
    }
};

QTEST_MAIN(TstI18n)
#include "tst_i18n.moc"
