// fw::i18n (core/Tr.h): English is the fallback, a catalogue is built-in and/or
// the user's, a translation that could break a text is never used, and the
// Language setting lists what there is.

#include "core/Settings.h"
#include "core/Tr.h"
#include "engines/general/Module.h"
#include "support/TestRegistry.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <thread>
#include <vector>

using namespace fw;

namespace {

using Strings = QList<QPair<QString, QString>>;

bool writeCatalogue(const QString& path, const QString& code, const QString& name, const QString& englishName, const Strings& strings)
{
    QJsonObject map;
    for (const auto& [english, translation] : strings)
        map.insert(english, translation);
    QJsonObject root{{QStringLiteral("strings"), map}};
    if (!code.isEmpty())
        root.insert(QStringLiteral("code"), code);
    if (!name.isEmpty())
        root.insert(QStringLiteral("name"), name);
    if (!englishName.isEmpty())
        root.insert(QStringLiteral("englishName"), englishName);
    QDir().mkpath(QFileInfo(path).absolutePath());
    return fwtest::writeTextFile(path, QJsonDocument(root).toJson());
}

bool writeSourceList(const QString& dir, const QStringList& texts)
{
    QJsonArray list;
    for (const QString& t : texts)
        list.append(QJsonObject{{QStringLiteral("text"), t}, {QStringLiteral("where"), QJsonArray{QStringLiteral("test")}}});
    QDir().mkpath(dir);
    return fwtest::writeTextFile(dir + QStringLiteral("/_source.json"), QJsonDocument(list).toJson());
}

const i18n::Language* languageOf(const QList<i18n::Language>& all, const QString& code)
{
    for (const i18n::Language& l : all)
        if (l.code == code)
            return &l;
    return nullptr;
}

bool anyNoteHas(const QString& part)
{
    for (const QString& n : i18n::notes())
        if (n.contains(part))
            return true;
    return false;
}

} // namespace

class TstI18n : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    int m_case = 0;

    // A fresh pair of folders for one test: <built-in>, <user>.
    QPair<QString, QString> folders()
    {
        const QString base = m_root.filePath(QStringLiteral("case%1").arg(++m_case));
        QDir().mkpath(base + QStringLiteral("/res"));
        QDir().mkpath(base + QStringLiteral("/user"));
        return {base + QStringLiteral("/res"), base + QStringLiteral("/user")};
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void cleanup()
    {
        i18n::load(QStringLiteral("en"));
        i18n::configure({}, {});
    }

    void englishWithoutACatalogue()
    {
        QCOMPARE(i18n::currentCode(), QStringLiteral("en"));
        QCOMPARE(i18n::translate(QStringLiteral("Add to queue")), QStringLiteral("Add to queue"));
        QCOMPARE(FW_TR("Add to queue"), QStringLiteral("Add to queue"));
        QCOMPARE(FW_TR("Two " "parts"), QStringLiteral("Two parts"));
        QCOMPARE(FW_TR_NOOP("In a table"), QStringLiteral("In a table"));
        QVERIFY(i18n::available().isEmpty());
        QVERIFY(i18n::sourceTexts().isEmpty());
    }

    void builtInCatalogue()
    {
        const auto [res, user] = folders();
        QVERIFY(writeCatalogue(res + QStringLiteral("/de.json"), QStringLiteral("de"), QStringLiteral("Deutsch"), QStringLiteral("German"),
                               {{QStringLiteral("Add to queue"), QStringLiteral("Zur Warteschlange hinzufügen")},
                                {QStringLiteral("%1 of %2 files"), QStringLiteral("%1 von %2 Dateien")}}));
        i18n::configure(res, user);
        QString problem;
        QVERIFY(i18n::load(QStringLiteral("de"), &problem));
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(i18n::currentCode(), QStringLiteral("de"));
        QCOMPARE(FW_TR("Add to queue"), QStringLiteral("Zur Warteschlange hinzufügen"));
        QCOMPARE(FW_TR("%1 of %2 files").arg(3).arg(7), QStringLiteral("3 von 7 Dateien"));
        QCOMPARE(FW_TR("Not in the catalogue"), QStringLiteral("Not in the catalogue"));
        // A marked table entry is translated where it is shown.
        const QString stored = FW_TR_NOOP("Add to queue");
        QCOMPARE(stored, QStringLiteral("Add to queue"));
        QCOMPARE(i18n::translate(stored), QStringLiteral("Zur Warteschlange hinzufügen"));

        QVERIFY(i18n::load(QStringLiteral("en")));
        QCOMPARE(i18n::currentCode(), QStringLiteral("en"));
        QCOMPARE(FW_TR("Add to queue"), QStringLiteral("Add to queue"));
        // An empty code is English too.
        QVERIFY(i18n::load(QStringLiteral("de")));
        QVERIFY(i18n::load(QString()));
        QCOMPARE(i18n::currentCode(), QStringLiteral("en"));
    }

    void userFileCorrectsAndAdds()
    {
        const auto [res, user] = folders();
        QVERIFY(writeCatalogue(res + QStringLiteral("/de.json"), QStringLiteral("de"), QStringLiteral("Deutsch"), QStringLiteral("German"),
                               {{QStringLiteral("Queue"), QStringLiteral("Schlange")},
                                {QStringLiteral("Cancel"), QStringLiteral("Abbrechen")},
                                {QStringLiteral("%1 files"), QStringLiteral("%1 Dateien")}}));
        // The user's file: one correction, one new entry, one entry that may
        // not be used (the built-in one stays), and no names of its own.
        QVERIFY(writeCatalogue(user + QStringLiteral("/de.json"), QStringLiteral("de"), {}, {},
                               {{QStringLiteral("Queue"), QStringLiteral("Warteschlange")},
                                {QStringLiteral("Browse"), QStringLiteral("Durchsuchen")},
                                {QStringLiteral("%1 files"), QStringLiteral("Dateien")}}));
        // A language the app does not ship.
        QVERIFY(writeCatalogue(user + QStringLiteral("/eo.json"), QStringLiteral("eo"), QStringLiteral("Esperanto"), QStringLiteral("Esperanto"),
                               {{QStringLiteral("Queue"), QStringLiteral("Vico")}}));
        i18n::configure(res, user);

        const QList<i18n::Language> all = i18n::available();
        QCOMPARE(all.size(), 2);
        const i18n::Language* de = languageOf(all, QStringLiteral("de"));
        const i18n::Language* eo = languageOf(all, QStringLiteral("eo"));
        QVERIFY(de && eo);
        QVERIFY(de->builtIn);
        QCOMPARE(de->nativeName, QStringLiteral("Deutsch"));
        QCOMPARE(de->englishName, QStringLiteral("German"));
        QVERIFY(!eo->builtIn);
        QCOMPARE(eo->nativeName, QStringLiteral("Esperanto"));

        QString problem;
        QVERIFY(i18n::load(QStringLiteral("de"), &problem));
        QVERIFY2(problem.contains(QStringLiteral("1 of 3 entries")), qPrintable(problem));
        QCOMPARE(FW_TR("Queue"), QStringLiteral("Warteschlange"));
        QCOMPARE(FW_TR("Cancel"), QStringLiteral("Abbrechen"));
        QCOMPARE(FW_TR("Browse"), QStringLiteral("Durchsuchen"));
        QCOMPARE(FW_TR("%1 files"), QStringLiteral("%1 Dateien"));

        QVERIFY(i18n::load(QStringLiteral("eo")));
        QCOMPARE(FW_TR("Queue"), QStringLiteral("Vico"));
        QCOMPARE(FW_TR("Cancel"), QStringLiteral("Cancel"));
    }

    void entryCheck_data()
    {
        QTest::addColumn<QString>("english");
        QTest::addColumn<QString>("translation");
        QTest::addColumn<bool>("usable");
        const auto row = [](const char* name, const QString& en, const QString& tr, bool ok) { QTest::newRow(name) << en << tr << ok; };
        row("plain", QStringLiteral("Open"), QStringLiteral("Öffnen"), true);
        row("empty", QStringLiteral("Open"), QString(), false);
        row("only spaces", QStringLiteral("Open"), QStringLiteral("  "), false);
        row("placeholders reordered", QStringLiteral("%1 of %2 files"), QStringLiteral("%2 Dateien, davon %1"), true);
        row("placeholder repeated", QStringLiteral("%1 files"), QStringLiteral("%1 Dateien (%1)"), true);
        row("placeholder missing", QStringLiteral("%1 of %2 files"), QStringLiteral("%1 von Dateien"), false);
        row("placeholder added", QStringLiteral("%1 files"), QStringLiteral("%1 von %2 Dateien"), false);
        row("placeholder invented", QStringLiteral("Files"), QStringLiteral("%1 Dateien"), false);
        row("localised number kept", QStringLiteral("%L1 MiB"), QStringLiteral("%L1 MiB"), true);
        row("localised number lost", QStringLiteral("%L1 MiB"), QStringLiteral("%1 MiB"), false);
        row("two digits are one placeholder", QStringLiteral("%1 files"), QStringLiteral("%15 Dateien"), false);
        row("count kept", QStringLiteral("%n files"), QStringLiteral("%n Dateien"), true);
        row("count lost", QStringLiteral("%n files"), QStringLiteral("Dateien"), false);
        row("per cent sign", QStringLiteral("100 % done"), QStringLiteral("100 % fertig"), true);
        row("markup invented", QStringLiteral("Open"), QStringLiteral("<b>Öffnen</b>"), false);
        row("stray angle bracket", QStringLiteral("Open"), QStringLiteral("Öffnen <"), false);
        row("markup kept", QStringLiteral("<b>%1</b> ready"), QStringLiteral("<b>%1</b> bereit"), true);
        row("markup moved", QStringLiteral("See <a href=\"x\">more</a>"), QStringLiteral("<a href=\"x\">Mehr</a> dazu"), true);
        row("tag changed", QStringLiteral("<b>%1</b> ready"), QStringLiteral("<i>%1</i> bereit"), false);
        row("link changed", QStringLiteral("See <a href=\"x\">more</a>"), QStringLiteral("<a href=\"http://evil\">Mehr</a>"), false);
        row("tag dropped", QStringLiteral("<b>Done</b>"), QStringLiteral("Fertig"), false);
        row("tag not closed", QStringLiteral("<b>Done</b>"), QStringLiteral("<b>Fertig</b><"), false);
        row("less than stays", QStringLiteral("a < b"), QStringLiteral("a < b"), true);
        row("ampersands kept", QStringLiteral("Save && close"), QStringLiteral("Speichern && schließen"), true);
        row("ampersand invented", QStringLiteral("Save and close"), QStringLiteral("Speichern & schließen"), false);
        row("ampersand lost", QStringLiteral("Save && close"), QStringLiteral("Speichern & schließen"), false);
        row("line break", QStringLiteral("One\nTwo"), QStringLiteral("Eins\nZwei"), true);
        row("control character", QStringLiteral("Open"), QStringLiteral("Öff") + QChar(0x07) + QStringLiteral("nen"), false);
        row("direction override", QStringLiteral("Open"), QChar(0x202E) + QStringLiteral("nepO"), false);
        row("out of proportion", QStringLiteral("Open"), QString(400, u'x'), false);
        row("long but in proportion", QStringLiteral("Open"), QString(200, u'x'), true);
    }

    void entryCheck()
    {
        QFETCH(QString, english);
        QFETCH(QString, translation);
        QFETCH(bool, usable);
        const QString why = i18n::entryProblem(english, translation);
        QVERIFY2(why.isEmpty() == usable, qPrintable(why));

        // The same through a file: an entry that is not usable shows English.
        const auto [res, user] = folders();
        QVERIFY(writeCatalogue(res + QStringLiteral("/de.json"), QStringLiteral("de"), QStringLiteral("Deutsch"), QStringLiteral("German"),
                               {{english, translation}, {QStringLiteral("Kept"), QStringLiteral("Behalten")}}));
        i18n::configure(res, user);
        QString problem;
        QVERIFY(i18n::load(QStringLiteral("de"), &problem));
        QCOMPARE(i18n::translate(english), usable ? translation : english);
        QCOMPARE(i18n::translate(QStringLiteral("Kept")), QStringLiteral("Behalten"));
        QCOMPARE(problem.isEmpty(), usable);
    }

    void entryThatIsNotText()
    {
        const auto [res, user] = folders();
        QVERIFY(fwtest::writeTextFile(res + QStringLiteral("/de.json"),
                                      R"({"code":"de","name":"Deutsch","englishName":"German","strings":{"Open":7,"Close":["x"],"Save":null,"Queue":"Warteschlange"}})"));
        i18n::configure(res, user);
        QString problem;
        QVERIFY(i18n::load(QStringLiteral("de"), &problem));
        QVERIFY2(problem.contains(QStringLiteral("3 of 4 entries")), qPrintable(problem));
        QCOMPARE(FW_TR("Open"), QStringLiteral("Open"));
        QCOMPARE(FW_TR("Queue"), QStringLiteral("Warteschlange"));
    }

    void filesThatAreIgnored()
    {
        const auto [res, user] = folders();
        const Strings one{{QStringLiteral("Open"), QStringLiteral("X")}};
        // Larger than 8 MiB (valid JSON all the same). No raw literals for the
        // halves of a JSON text: moc counts the braces inside them.
        {
            QFile f(user + QStringLiteral("/xx.json"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{\"code\":\"xx\",\"name\":\"Big\",\"strings\":{\"Open\":\"X\"},\"padding\":\"");
            f.write(QByteArray(8 * 1024 * 1024 + 16, 'a'));
            f.write("\"}");
        }
        QVERIFY(fwtest::writeTextFile(user + QStringLiteral("/yy.json"), "{\"code\":\"yy\",\"strings\":{\"Open\":\"X\"")); // cut off
        QVERIFY(fwtest::writeTextFile(user + QStringLiteral("/zz.json"), "[\"Open\",\"X\"]"));                             // not an object
        QVERIFY(writeCatalogue(user + QStringLiteral("/German.json"), QStringLiteral("de"), QStringLiteral("Deutsch"), {}, one));
        QVERIFY(writeCatalogue(user + QStringLiteral("/aa.json"), QStringLiteral("bb"), QStringLiteral("Other"), {}, one));
        QVERIFY(writeCatalogue(user + QStringLiteral("/ab.json"), QStringLiteral("../ab"), QStringLiteral("Path"), {}, one));
        QVERIFY(writeCatalogue(user + QStringLiteral("/en.json"), QStringLiteral("en"), QStringLiteral("English"), {}, one));
        QVERIFY(writeCatalogue(user + QStringLiteral("/qps.json"), QStringLiteral("qps"), QStringLiteral("Pseudo"), {}, one));
        QVERIFY(writeCatalogue(user + QStringLiteral("/_notes.json"), QStringLiteral("no"), QStringLiteral("Notes"), {}, one));
        QVERIFY(fwtest::writeTextFile(user + QStringLiteral("/README.txt"), "not a catalogue"));
        // One that is fine, so the folder is read at all.
        QVERIFY(writeCatalogue(user + QStringLiteral("/eo.json"), QStringLiteral("eo"), QStringLiteral("Esperanto"), {}, one));
        i18n::configure(res, user);

        const QList<i18n::Language> all = i18n::available();
        QCOMPARE(all.size(), 1);
        QCOMPARE(all.first().code, QStringLiteral("eo"));
        QVERIFY(anyNoteHas(QStringLiteral("xx.json is ignored: it is larger than 8 MiB")));
        QVERIFY(anyNoteHas(QStringLiteral("yy.json is ignored: it is not valid JSON")));
        QVERIFY(anyNoteHas(QStringLiteral("zz.json is ignored: it is not valid JSON")));
        QVERIFY(anyNoteHas(QStringLiteral("German.json is ignored: its name is not a language code")));
        QVERIFY(anyNoteHas(QStringLiteral("aa.json is ignored: its code \"bb\"")));
        QVERIFY(anyNoteHas(QStringLiteral("ab.json is ignored: its code")));
        QVERIFY(anyNoteHas(QStringLiteral("en.json is ignored: \"en\" is reserved")));
        QVERIFY(anyNoteHas(QStringLiteral("qps.json is ignored: \"qps\" is reserved")));
        QVERIFY(!anyNoteHas(QStringLiteral("_notes.json")));
        QVERIFY(!anyNoteHas(QStringLiteral("README")));

        for (const QString& code : {QStringLiteral("xx"), QStringLiteral("yy"), QStringLiteral("zz"), QStringLiteral("aa"),
                                    QStringLiteral("bb"), QStringLiteral("ab"), QStringLiteral("nn")}) {
            QString problem;
            QVERIFY2(!i18n::load(code, &problem), qPrintable(code));
            QVERIFY2(!problem.isEmpty(), qPrintable(code));
            QCOMPARE(i18n::currentCode(), QStringLiteral("en"));
            QCOMPARE(FW_TR("Open"), QStringLiteral("Open"));
        }
        // Not codes at all: nothing is looked up on disk for them.
        for (const QString& code : {QStringLiteral("../eo"), QStringLiteral("EO"), QStringLiteral("e"), QStringLiteral("eo.json"),
                                    QStringLiteral("eo/"), QStringLiteral("german")}) {
            QVERIFY2(!i18n::isValidCode(code), qPrintable(code));
            QVERIFY2(!i18n::load(code), qPrintable(code));
            QCOMPARE(i18n::currentCode(), QStringLiteral("en"));
        }
        for (const QString& code : {QStringLiteral("de"), QStringLiteral("fil"), QStringLiteral("pt-BR"), QStringLiteral("zh-Hans")})
            QVERIFY2(i18n::isValidCode(code), qPrintable(code));
    }

    // A language's names are shown in the picker as they are: one that holds
    // a direction override (it could read as another language's name) or a
    // control character is not used, as a translation with one is not.
    void namesThatCouldMisleadAreNotShown()
    {
        const auto [res, user] = folders();
        const Strings one{{QStringLiteral("Open"), QStringLiteral("X")}};
        QVERIFY(writeCatalogue(user + QStringLiteral("/xa.json"), QStringLiteral("xa"), QChar(0x202E) + QStringLiteral("hsilgnE"),
                               QStringLiteral("Test A"), one));
        QVERIFY(writeCatalogue(user + QStringLiteral("/xb.json"), QStringLiteral("xb"), QStringLiteral("Test B"),
                               QStringLiteral("Eng") + QChar(0x2066) + QStringLiteral("lish"), one));
        QVERIFY(writeCatalogue(user + QStringLiteral("/xc.json"), QStringLiteral("xc"), QStringLiteral("Te") + QChar(0x07) + QStringLiteral("st"),
                               QStringLiteral("Test C"), one));
        // A right-to-left name with its marks is a name like any other.
        const QString arabic = QStringLiteral("العربية") + QChar(0x200F);
        QVERIFY(writeCatalogue(user + QStringLiteral("/xd.json"), QStringLiteral("xd"), arabic, QStringLiteral("Test D"), one));
        i18n::configure(res, user);
        const QList<i18n::Language> all = i18n::available();
        QCOMPARE(all.size(), 4);
        QCOMPARE(languageOf(all, QStringLiteral("xa"))->nativeName, QStringLiteral("Test A"));
        QCOMPARE(languageOf(all, QStringLiteral("xb"))->englishName, QStringLiteral("Test B"));
        QCOMPARE(languageOf(all, QStringLiteral("xc"))->nativeName, QStringLiteral("Test C"));
        QCOMPARE(languageOf(all, QStringLiteral("xd"))->nativeName, arabic);
    }

    void failedLoadLeavesEnglish()
    {
        const auto [res, user] = folders();
        QVERIFY(writeCatalogue(res + QStringLiteral("/de.json"), QStringLiteral("de"), QStringLiteral("Deutsch"), QStringLiteral("German"),
                               {{QStringLiteral("Open"), QStringLiteral("Öffnen")}}));
        i18n::configure(res, user);
        QVERIFY(i18n::load(QStringLiteral("de")));
        QCOMPARE(FW_TR("Open"), QStringLiteral("Öffnen"));
        QString problem;
        QVERIFY(!i18n::load(QStringLiteral("fr"), &problem));
        QVERIFY2(problem.contains(QStringLiteral("no usable language file for \"fr\"")), qPrintable(problem));
        QCOMPARE(i18n::currentCode(), QStringLiteral("en"));
        QCOMPARE(FW_TR("Open"), QStringLiteral("Open"));
    }

    void codeIsFoundWhateverItsCase()
    {
        const auto [res, user] = folders();
        QVERIFY(writeCatalogue(res + QStringLiteral("/pt-BR.json"), QStringLiteral("pt-BR"), QStringLiteral("Português (Brasil)"),
                               QStringLiteral("Portuguese (Brazil)"), {{QStringLiteral("Open"), QStringLiteral("Abrir")}}));
        i18n::configure(res, user);
        QVERIFY(i18n::load(QStringLiteral("pt-br")));
        QCOMPARE(i18n::currentCode(), QStringLiteral("pt-BR")); // as the catalogue spells it
        QCOMPARE(FW_TR("Open"), QStringLiteral("Abrir"));
    }

    void pseudoLanguage()
    {
        const auto [res, user] = folders();
        i18n::configure(res, user);
        QVERIFY(i18n::load(i18n::pseudoCode()));
        QCOMPARE(i18n::currentCode(), QStringLiteral("qps"));
        const QString open = QChar(0x27E6), close = QChar(0x27E7);
        QCOMPARE(FW_TR("Open"), open + QStringLiteral("Open") + close);
        QCOMPARE(FW_TR("%1 of %2 files").arg(1).arg(2), open + QStringLiteral("1 of 2 files") + close);
        QCOMPARE(i18n::translate(QString()), QString());
        QVERIFY(i18n::available().isEmpty()); // never in the picker
    }

    // Arabic, Persian, Hebrew and Urdu are written right to left (with a
    // region too); a catalogue may say so for a language the app does not know.
    void rightToLeftLanguages()
    {
        for (const char* code : {"ar", "fa", "he", "ur", "ar-EG", "fa-IR", "qps-rtl"})
            QVERIFY2(i18n::isRightToLeftCode(QString::fromLatin1(code)), code);
        for (const char* code : {"en", "de", "pt-BR", "zh-CN", "qps", "ara", "her", ""})
            QVERIFY2(!i18n::isRightToLeftCode(QString::fromLatin1(code)), code);
        QVERIFY(!i18n::isRightToLeft()); // English

        const auto [res, user] = folders();
        QVERIFY(writeCatalogue(res + QStringLiteral("/ar.json"), QStringLiteral("ar"), QStringLiteral("Arabic"), QStringLiteral("Arabic"),
                               {{QStringLiteral("Open"), QStringLiteral("فتح")}}));
        QVERIFY(writeCatalogue(res + QStringLiteral("/ar-EG.json"), QStringLiteral("ar-EG"), QStringLiteral("Arabic (Egypt)"),
                               QStringLiteral("Arabic (Egypt)"), {{QStringLiteral("Open"), QStringLiteral("فتح")}}));
        QVERIFY(writeCatalogue(res + QStringLiteral("/de.json"), QStringLiteral("de"), QStringLiteral("Deutsch"), QStringLiteral("German"),
                               {{QStringLiteral("Open"), QStringLiteral("Öffnen")}}));
        // A language the app does not know: its catalogue says the direction ("rtl" only; anything else is LTR).
        const auto withDirection = [](const QString& path, const QString& code, const QString& direction) {
            const QJsonObject root{{QStringLiteral("code"), code}, {QStringLiteral("name"), code}, {QStringLiteral("direction"), direction},
                                   {QStringLiteral("strings"), QJsonObject{{QStringLiteral("Open"), QStringLiteral("x")}}}};
            return fwtest::writeTextFile(path, QJsonDocument(root).toJson());
        };
        QVERIFY(withDirection(res + QStringLiteral("/sd.json"), QStringLiteral("sd"), QStringLiteral("rtl")));
        QVERIFY(withDirection(res + QStringLiteral("/sw.json"), QStringLiteral("sw"), QStringLiteral("RTL")));
        QVERIFY(withDirection(user + QStringLiteral("/de.json"), QStringLiteral("de"), QStringLiteral("ltr")));
        i18n::configure(res, user);

        const QStringList rtl{QStringLiteral("ar"), QStringLiteral("ar-EG"), QStringLiteral("sd")};
        const QStringList ltr{QStringLiteral("de"), QStringLiteral("sw")};
        for (const QString& code : rtl) {
            QVERIFY2(i18n::load(code), qPrintable(code));
            QVERIFY2(i18n::isRightToLeft(), qPrintable(code));
        }
        for (const QString& code : ltr) {
            QVERIFY2(i18n::load(code), qPrintable(code));
            QVERIFY2(!i18n::isRightToLeft(), qPrintable(code));
        }
        for (const i18n::Language& l : i18n::available())
            QCOMPARE(l.rightToLeft, rtl.contains(l.code));
        QVERIFY(i18n::load(QStringLiteral("en")));
        QVERIFY(!i18n::isRightToLeft());
    }

    // "qps-rtl": the pseudo-language's brackets swapped, a right-to-left mark first, and the direction.
    void rightToLeftPseudoLanguage()
    {
        const auto [res, user] = folders();
        QVERIFY(writeCatalogue(user + QStringLiteral("/qps-rtl.json"), QStringLiteral("qps-rtl"), QStringLiteral("Pseudo"), {},
                               {{QStringLiteral("Open"), QStringLiteral("x")}}));
        i18n::configure(res, user);
        QVERIFY(i18n::load(i18n::pseudoRtlCode()));
        QCOMPARE(i18n::currentCode(), QStringLiteral("qps-rtl"));
        QVERIFY(i18n::isRightToLeft());
        QCOMPARE(FW_TR("Open"), QString(QChar(0x200F)) + QChar(0x27E7) + QStringLiteral("Open") + QChar(0x27E6));
        QCOMPARE(i18n::translate(QString()), QString());
        QVERIFY(i18n::available().isEmpty()); // never in the picker; a file of that name is not a catalogue
        QVERIFY(anyNoteHas(QStringLiteral("qps-rtl.json is ignored")));
        QVERIFY(i18n::load(i18n::pseudoCode()));
        QVERIFY(!i18n::isRightToLeft());
    }

    void availableWithCoverage()
    {
        const auto [res, user] = folders();
        QVERIFY(writeSourceList(res, {QStringLiteral("One"), QStringLiteral("Two"), QStringLiteral("Three"), QStringLiteral("%1 four")}));
        // Two usable entries of four source texts; one entry the app no longer
        // has and one that may not be used count for nothing.
        QVERIFY(writeCatalogue(res + QStringLiteral("/de.json"), QStringLiteral("de"), QStringLiteral("Deutsch"), QStringLiteral("German"),
                               {{QStringLiteral("One"), QStringLiteral("Eins")},
                                {QStringLiteral("Two"), QStringLiteral("Zwei")},
                                {QStringLiteral("Gone"), QStringLiteral("Weg")},
                                {QStringLiteral("%1 four"), QStringLiteral("vier")}}));
        QVERIFY(writeCatalogue(res + QStringLiteral("/zh-CN.json"), QStringLiteral("zh-CN"), QStringLiteral("简体中文"), QStringLiteral("Chinese (Simplified)"),
                               {{QStringLiteral("One"), QStringLiteral("一")},
                                {QStringLiteral("Two"), QStringLiteral("二")},
                                {QStringLiteral("Three"), QStringLiteral("三")},
                                {QStringLiteral("%1 four"), QStringLiteral("%1 四")}}));
        // The user completes German and brings a language without names.
        QVERIFY(writeCatalogue(user + QStringLiteral("/de.json"), QStringLiteral("de"), {}, {}, {{QStringLiteral("Three"), QStringLiteral("Drei")}}));
        QVERIFY(writeCatalogue(user + QStringLiteral("/eo.json"), {}, {}, {}, {{QStringLiteral("One"), QStringLiteral("Unu")}}));
        // A name a list could take for markup is not shown.
        QVERIFY(writeCatalogue(user + QStringLiteral("/ia.json"), QStringLiteral("ia"), QStringLiteral("<b>Interlingua</b>"), QStringLiteral("Inter&lingua"),
                               {{QStringLiteral("One"), QStringLiteral("Un")}}));
        i18n::configure(res, user);
        QCOMPARE(i18n::sourceTexts().size(), 4);

        const QList<i18n::Language> all = i18n::available();
        QStringList codes;
        for (const auto& l : all)
            codes << l.code;
        // Sorted by English name: Chinese (Simplified), eo, German, ia.
        QCOMPARE(codes, (QStringList{QStringLiteral("zh-CN"), QStringLiteral("eo"), QStringLiteral("de"), QStringLiteral("ia")}));
        const i18n::Language* de = languageOf(all, QStringLiteral("de"));
        const i18n::Language* zh = languageOf(all, QStringLiteral("zh-CN"));
        const i18n::Language* eo = languageOf(all, QStringLiteral("eo"));
        const i18n::Language* ia = languageOf(all, QStringLiteral("ia"));
        QCOMPARE(de->coverage, 75);
        QVERIFY(de->builtIn);
        QCOMPARE(zh->coverage, 100);
        QCOMPARE(zh->nativeName, QStringLiteral("简体中文"));
        QCOMPARE(eo->coverage, 25);
        QVERIFY(!eo->builtIn);
        QCOMPARE(eo->nativeName, QStringLiteral("eo"));
        QCOMPARE(eo->englishName, QStringLiteral("eo"));
        QCOMPARE(ia->nativeName, QStringLiteral("ia"));
        QCOMPARE(ia->englishName, QStringLiteral("ia"));
    }

    void readsFromManyThreads()
    {
        const auto [res, user] = folders();
        Strings a, b;
        for (int i = 0; i < 500; ++i) {
            a.append({QStringLiteral("text %1").arg(i), QStringLiteral("A %1").arg(i)});
            b.append({QStringLiteral("text %1").arg(i), QStringLiteral("B %1").arg(i)});
        }
        QVERIFY(writeCatalogue(res + QStringLiteral("/aa.json"), QStringLiteral("aa"), QStringLiteral("A"), QStringLiteral("A"), a));
        QVERIFY(writeCatalogue(res + QStringLiteral("/bb.json"), QStringLiteral("bb"), QStringLiteral("B"), QStringLiteral("B"), b));
        i18n::configure(res, user);
        QVERIFY(i18n::load(QStringLiteral("aa")));

        // Readers run while the catalogue is replaced: every answer is one of
        // the two complete catalogues' (or English for a text neither has).
        std::atomic<bool> stop{false};
        std::atomic<int> wrong{0};
        std::atomic<qint64> reads{0};
        std::vector<std::thread> readers;
        for (int t = 0; t < 8; ++t) {
            readers.emplace_back([&, t] {
                int i = t * 31;
                while (!stop.load(std::memory_order_relaxed)) {
                    i = (i + 7) % 500;
                    const QString got = i18n::translate(QStringLiteral("text %1").arg(i));
                    if (got != QStringLiteral("A %1").arg(i) && got != QStringLiteral("B %1").arg(i))
                        ++wrong;
                    if (i18n::translate(QStringLiteral("nobody translated this")) != QStringLiteral("nobody translated this"))
                        ++wrong;
                    const QString code = i18n::currentCode();
                    if (code != u"aa" && code != u"bb")
                        ++wrong;
                    ++reads;
                }
            });
        }
        for (int round = 0; round < 40; ++round)
            if (!i18n::load(round % 2 ? QStringLiteral("aa") : QStringLiteral("bb")))
                ++wrong;
        while (reads.load() < 20000)
            std::this_thread::yield();
        stop = true;
        for (std::thread& t : readers)
            t.join();
        QCOMPARE(wrong.load(), 0);
    }

    // ---- the Language setting (engines/general) ---------------------------------

    void settingListsEnglishAlone()
    {
        // Nothing configured: what every other test program sees.
        fwtest::Registry reg;
        const SettingDef* def = reg.schema->find(general::keys::kLanguage);
        QVERIFY(def);
        QCOMPARE(def->key, QStringLiteral("general.language"));
        QCOMPARE(def->section, QStringLiteral("General"));
        QCOMPARE(def->label, QStringLiteral("Language"));
        QCOMPARE(def->type, SettingType::Enum);
        QCOMPARE(def->defaultValue.toString(), QStringLiteral("en"));
        QVERIFY(def->restartRequired);
        QVERIFY(!def->perJob);
        QCOMPARE(def->choices.size(), 1);
        QCOMPARE(def->choices.first().value, QStringLiteral("en"));
        QCOMPARE(def->choices.first().label, QStringLiteral("English"));
        // The first row of the first section.
        QCOMPARE(reg.schema->inSection(QStringLiteral("General")).front()->key, general::keys::kLanguage);

        QVERIFY(reg.schema->normalize(general::keys::kLanguage, QStringLiteral("en")).ok());
        QVERIFY(!reg.schema->normalize(general::keys::kLanguage, QStringLiteral("de")).ok());
    }

    void settingListsTheCatalogues()
    {
        const auto [res, user] = folders();
        const Strings one{{QStringLiteral("Open"), QStringLiteral("X")}};
        QVERIFY(writeCatalogue(res + QStringLiteral("/de.json"), QStringLiteral("de"), QStringLiteral("Deutsch"), QStringLiteral("German"), one));
        QVERIFY(writeCatalogue(res + QStringLiteral("/zh-CN.json"), QStringLiteral("zh-CN"), QStringLiteral("简体中文"), QStringLiteral("Chinese (Simplified)"), one));
        QVERIFY(writeCatalogue(res + QStringLiteral("/pt-BR.json"), QStringLiteral("pt-BR"), QStringLiteral("Português (Brasil)"), QStringLiteral("Portuguese (Brazil)"), one));
        QVERIFY(writeCatalogue(user + QStringLiteral("/eo.json"), QStringLiteral("eo"), QStringLiteral("Esperanto"), QStringLiteral("Esperanto"), one));
        i18n::configure(res, user);

        fwtest::Registry reg;
        const SettingDef* def = reg.schema->find(general::keys::kLanguage);
        QVERIFY(def);
        QStringList values, labels;
        for (const SettingChoice& c : def->choices) {
            values << c.value;
            labels << c.label;
        }
        QCOMPARE(values, (QStringList{QStringLiteral("en"), QStringLiteral("zh-CN"), QStringLiteral("eo"), QStringLiteral("de"), QStringLiteral("pt-BR")}));
        QCOMPARE(labels, (QStringList{QStringLiteral("English"), QStringLiteral("简体中文 (Chinese (Simplified))"), QStringLiteral("Esperanto"),
                                      QStringLiteral("Deutsch (German)"), QStringLiteral("Português (Brasil) (Portuguese (Brazil))")}));

        QVERIFY(QDir().mkpath(m_root.filePath(QStringLiteral("lang-settings"))));
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("lang-settings/settings.json")));
        QVERIFY(store.load().ok());
        QCOMPARE(store.value(general::keys::kLanguage).toString(), QStringLiteral("en"));
        QVERIFY(store.setValue(general::keys::kLanguage, QStringLiteral("de")).ok());
        QVERIFY(!store.setValue(general::keys::kLanguage, QStringLiteral("fr")).ok());
        QVERIFY(store.save().ok());

        // The catalogue is gone at the next start: the stored code is not a
        // choice any more and the setting is English again.
        i18n::configure({}, {});
        fwtest::Registry later;
        SettingsStore reloaded(later.schema, store.filePath());
        QVERIFY(reloaded.load().ok());
        QCOMPARE(reloaded.value(general::keys::kLanguage).toString(), QStringLiteral("en"));
    }

    void legacyLanguageIsMapped()
    {
        const auto [res, user] = folders();
        const Strings one{{QStringLiteral("Open"), QStringLiteral("X")}};
        for (const QString& code : {QStringLiteral("de"), QStringLiteral("zh-CN"), QStringLiteral("pt-BR")})
            QVERIFY(writeCatalogue(res + QStringLiteral("/%1.json").arg(code), code, code, code, one));
        i18n::configure(res, user);
        fwtest::Registry reg;
        const auto& legacy = reg.schema->legacyKeys();
        const auto it = legacy.find(QStringLiteral("language"));
        QVERIFY(it != legacy.end());
        QCOMPARE(it->second.key, general::keys::kLanguage);
        const auto mapped = [&](const QString& old) { return it->second.convert(old).value_or(QVariant()).toString(); };
        QCOMPARE(mapped(QStringLiteral("de")), QStringLiteral("de"));
        QCOMPARE(mapped(QStringLiteral("zh")), QStringLiteral("zh-CN"));
        QCOMPARE(mapped(QStringLiteral("pt")), QStringLiteral("pt-BR"));
        QCOMPARE(mapped(QStringLiteral("en")), QStringLiteral("en"));
        QCOMPARE(mapped(QStringLiteral("ar")), QStringLiteral("en")); // no catalogue
        QCOMPARE(mapped(QStringLiteral("fr")), QStringLiteral("en")); // none here either
        QCOMPARE(mapped(QStringLiteral(" DE ")), QStringLiteral("de"));

        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("legacy-lang/settings.json")));
        QVERIFY(store.load().ok());
        const QString old = m_root.filePath(QStringLiteral("old-settings.json"));
        QVERIFY(fwtest::writeTextFile(old, R"({"language": "zh"})"));
        QCOMPARE(store.importLegacy(old).value(), 1);
        QCOMPARE(store.value(general::keys::kLanguage).toString(), QStringLiteral("zh-CN"));
        QVERIFY(fwtest::writeTextFile(old, R"({"language": "ar"})"));
        QVERIFY(store.importLegacy(old).ok());
        QCOMPARE(store.value(general::keys::kLanguage).toString(), QStringLiteral("en"));

        // Without catalogues every old language is English.
        i18n::configure({}, {});
        fwtest::Registry bare;
        QCOMPARE(bare.schema->legacyKeys().at(QStringLiteral("language")).convert(QStringLiteral("de")).value_or(QVariant()).toString(),
                 QStringLiteral("en"));
    }
};

QTEST_GUILESS_MAIN(TstI18n)
#include "tst_i18n.moc"
