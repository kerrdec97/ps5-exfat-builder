// The list of English source texts and the catalogues built into the app.
//
//  * src/app/resources/lang/_source.json is exactly what the code shows: the
//    FW_TR / FW_TR_NOOP literals, the settings schema, the job kinds, stage
//    names and the other names that are data (see support/I18nScan.h). When
//    the test fails after strings were marked or reworded, run it once with
//    FW_I18N_UPDATE=1 in the environment: it rewrites the file.
//  * every catalogue next to it is sound: its keys exist in the source list,
//    every entry passes the checks the app applies when it loads the file, and
//    the catalogues named in shipped() are complete.

#include "core/Dependencies.h"
#include "core/Settings.h"
#include "core/Tr.h"
#include "engines/general/Module.h"
#include "jobs/Job.h"
#include "support/I18nScan.h"
#include "support/TestRegistry.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <iterator>
#include <optional>

using namespace fw;
namespace scan = fwtest::i18nscan;

namespace {

// The languages offered as finished: each must translate at least 98 % of the
// source list. A catalogue that is not named here may be partial.
const QStringList& shipped()
{
    static const QStringList codes = {QStringLiteral("ar"), QStringLiteral("bg"), QStringLiteral("bn"), QStringLiteral("cs"), QStringLiteral("da"), QStringLiteral("de"), QStringLiteral("el"), QStringLiteral("es"), QStringLiteral("et"), QStringLiteral("fa"), QStringLiteral("fi"), QStringLiteral("fil"), QStringLiteral("fr"), QStringLiteral("he"), QStringLiteral("hi"), QStringLiteral("hr"), QStringLiteral("hu"), QStringLiteral("id"), QStringLiteral("it"), QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("lt"), QStringLiteral("lv"), QStringLiteral("ms"), QStringLiteral("nb"), QStringLiteral("nl"), QStringLiteral("pl"), QStringLiteral("pt-BR"), QStringLiteral("pt-PT"), QStringLiteral("ro"), QStringLiteral("ru"), QStringLiteral("sk"), QStringLiteral("sl"), QStringLiteral("sr-Latn"), QStringLiteral("sv"), QStringLiteral("th"), QStringLiteral("tr"), QStringLiteral("uk"), QStringLiteral("ur"), QStringLiteral("vi"), QStringLiteral("zh-CN"), QStringLiteral("zh-TW")};
    return codes;
}

QString srcRoot()
{
    return QDir(fwtest::dataDir() + QStringLiteral("/../../src")).canonicalPath();
}

QString langDir()
{
    return srcRoot() + QStringLiteral("/app/resources/lang");
}

using SourceList = QMap<QString, QStringList>; // text → where it is from

// A text of a data table that has nothing to translate: a number, or a number
// with its unit ("-1", "128 KB", "16 KiB"). Units stay as they are.
bool isNumberOnly(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("\\A[-+]?[0-9][0-9.,]*\\s?(?:[KMGT]i?B|B|%|ms|s)?\\z"));
    return re.match(text).hasMatch();
}

void add(SourceList& list, const QString& text, const QString& where)
{
    if (text.trimmed().isEmpty() || isNumberOnly(text))
        return;
    QStringList& places = list[text];
    if (!places.contains(where))
        places << where;
}

// "files", "threads": words. "MiB", "s", "%" are not translated.
bool isWord(const QString& unit)
{
    return unit.size() > 3;
}

SourceList collect(QStringList* problems)
{
    SourceList list;
    const scan::Result scanned = scan::scanTree(srcRoot());
    *problems = scanned.problems;
    for (const scan::Found& f : scanned.found)
        add(list, f.text, f.where);

    // The settings as the app registers them. Without catalogues, so the
    // Language setting lists English alone; language names are never source
    // texts anyway.
    i18n::configure({}, {});
    const fwtest::Registry reg;
    for (const SettingDef* d : reg.schema->all()) {
        const QString where = QStringLiteral("setting ") + d->key;
        add(list, d->section, QStringLiteral("settings section"));
        add(list, d->group, QStringLiteral("settings group in ") + d->section);
        add(list, d->label, where);
        add(list, d->description, where);
        add(list, d->fileFilter, where);
        if (isWord(d->unit))
            add(list, d->unit, where);
        if (d->key == general::keys::kLanguage)
            continue;
        for (const SettingChoice& c : d->choices) {
            add(list, c.label, where);
            add(list, c.description, where);
        }
    }
    for (const QString& kind : reg.jobs.kinds())
        add(list, reg.jobs.displayName(kind), QStringLiteral("job ") + kind);
    for (const JobState s : {JobState::Queued, JobState::Running, JobState::Paused, JobState::Cancelling, JobState::Succeeded,
                             JobState::Failed, JobState::Cancelled, JobState::Interrupted})
        add(list, jobStateName(s), QStringLiteral("job state"));
    for (const Verification v : {Verification::NotRun, Verification::Passed, Verification::Failed, Verification::Skipped, Verification::Inconclusive})
        add(list, verificationName(v), QStringLiteral("verification result"));
    for (const DependencyStatus s : {DependencyStatus::Ready, DependencyStatus::Missing, DependencyStatus::Outdated, DependencyStatus::Error,
                                     DependencyStatus::NotApplicable})
        add(list, dependencyStatusName(s), QStringLiteral("tool status"));

    for (QStringList& places : list)
        places.sort();
    return list;
}

QByteArray quoted(const QString& text)
{
    const QByteArray array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact); // ["..."]
    return array.mid(1, array.size() - 2);
}

// One entry per line, sorted by text: a change shows as the lines it touches.
QByteArray toJson(const SourceList& list)
{
    QByteArray out = "[\n";
    for (auto it = list.constBegin(); it != list.constEnd(); ++it) {
        out += "{\"text\": " + quoted(it.key()) + ", \"where\": [";
        for (qsizetype i = 0; i < it.value().size(); ++i)
            out += (i ? ", " : "") + quoted(it.value().at(i));
        out += std::next(it) == list.constEnd() ? "]}\n" : "]},\n";
    }
    out += "]\n";
    return out;
}

std::optional<SourceList> fromJson(const QByteArray& bytes)
{
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isArray())
        return std::nullopt;
    SourceList list;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        QStringList places;
        for (const QJsonValue& w : o.value(QStringLiteral("where")).toArray())
            places << w.toString();
        list.insert(o.value(QStringLiteral("text")).toString(), places);
    }
    return list;
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QStringList textsOf(const scan::Result& r)
{
    QStringList out;
    for (const scan::Found& f : r.found)
        out << f.text;
    return out;
}

// Sample C++ for the scanner's own tests. Kept in files, not in raw literals
// here: moc reads this file and would count the samples' braces and quotes.
QString sample(const QString& name)
{
    return QString::fromUtf8(readFile(fwtest::dataDir() + QStringLiteral("/i18n/") + name));
}

} // namespace

class TstI18nSource : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QVERIFY2(QFileInfo(srcRoot() + QStringLiteral("/core/Tr.h")).isFile(), qPrintable(srcRoot()));
    }

    void cleanup()
    {
        i18n::load(QStringLiteral("en"));
        i18n::configure({}, {});
    }

    // ---- the scanner ------------------------------------------------------------

    void scannerReadsMarkedTexts()
    {
        const QString code = sample(QStringLiteral("scan_marked.txt"));
        QVERIFY(!code.isEmpty());
        scan::Result r;
        scan::scanText(code, QStringLiteral("app/pages/Example.cpp"), r);
        QVERIFY2(r.problems.isEmpty(), qPrintable(r.problems.join(u'\n')));
        const QStringList expected{
            QStringLiteral("Plain"),
            QStringLiteral("Two parts, a \"quote\", a back\\slash,\na line and a\ttab"),
            QStringLiteral("Spaced"),
            QStringLiteral("Größe: %1 …"),
            QStringLiteral("Escapes: … é A"),
            QStringLiteral("Raw \"text\" \\n"),
            QStringLiteral("Table entry"),
            QStringLiteral("With a // slash and /* stars */"),
            QStringLiteral("After a quote character"),
            QStringLiteral("Split by a comment"),
            QStringLiteral("After a digit separator"),
        };
        QCOMPARE(textsOf(r), expected);
        for (const scan::Found& f : r.found)
            QCOMPARE(f.where, QStringLiteral("app/pages/Example.cpp"));
    }

    void scannerRefusesWhatIsNotALiteral()
    {
        const QString code = QStringLiteral("void f()\n"
                                            "{\n"
                                            "    auto a = FW_TR(title);\n"
                                            "    auto b = FW_TR(\"Text\" + other);\n"
                                            "    auto c = FW_TR(\"\");\n"
                                            "    auto d = FW_TR(QStringLiteral(\"Wrapped twice\"));\n"
                                            "    auto e = FW_TR(u\"Wide\");\n"
                                            "    auto ok = FW_TR(\"Fine\");\n"
                                            "}\n");
        scan::Result r;
        scan::scanText(code, QStringLiteral("app/x.cpp"), r);
        QCOMPARE(textsOf(r), QStringList{QStringLiteral("Fine")});
        QCOMPARE(r.problems.size(), 5);
        for (int line = 3; line <= 7; ++line)
            QVERIFY2(r.problems.at(line - 3).startsWith(QStringLiteral("app/x.cpp:%1:").arg(line)), qPrintable(r.problems.at(line - 3)));
    }

    void scannerFindsStagesAndTools()
    {
        const QString code = sample(QStringLiteral("scan_data.txt"));
        QVERIFY(!code.isEmpty());
        scan::Result r;
        scan::scanText(code, QStringLiteral("engines/x/Jobs.cpp"), r);
        QVERIFY2(r.problems.isEmpty(), qPrintable(r.problems.join(u'\n')));
        QStringList stages, tools;
        for (const scan::Found& f : r.found) {
            if (f.where == QStringLiteral("engines/x/Jobs.cpp (stage)"))
                stages << f.text;
            else if (f.where == QStringLiteral("engines/x/Jobs.cpp (tool)"))
                tools << f.text;
            else
                QFAIL(qPrintable(f.where));
        }
        stages.sort();
        QCOMPARE(stages, (QStringList{QStringLiteral("Checking the header"), QStringLiteral("Connecting"), QStringLiteral("Packing"),
                                      QStringLiteral("Planning"), QStringLiteral("Saving"), QStringLiteral("Scanning"),
                                      QStringLiteral("Speed test"), QStringLiteral("Unpacking ZIP"), QStringLiteral("Verifying"),
                                      QStringLiteral("Writing image")}));
        QCOMPARE(tools, (QStringList{QStringLiteral("Publishing Tools (pub-cmd)"), QStringLiteral("Builds packages. You supply your own copy.")}));

        // The interface's own code has no such data: there a text is marked.
        scan::Result ui;
        scan::scanText(code, QStringLiteral("app/ui/Widgets.cpp"), ui);
        QVERIFY(ui.found.isEmpty());
    }

    // ---- the source list ----------------------------------------------------------

    void sourceListIsCurrent()
    {
        QStringList problems;
        const SourceList now = collect(&problems);
        QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("\n") + problems.join(u'\n')));
        QVERIFY2(now.size() > 300, qPrintable(QString::number(now.size()))); // the settings alone are more
        QVERIFY(now.contains(QStringLiteral("Language")));
        QVERIFY(now.contains(QStringLiteral("Build exFAT image"))); // a job kind
        QVERIFY(now.contains(QStringLiteral("Writing image")));     // a stage
        QVERIFY(!now.contains(QStringLiteral("English")) || !now.value(QStringLiteral("English")).contains(QStringLiteral("setting general.language")));

        const QString path = langDir() + QStringLiteral("/_source.json");
        const QByteArray fresh = toJson(now);
        QCOMPARE(fromJson(fresh).value_or(SourceList()), now); // what is written reads back the same
        if (qEnvironmentVariableIntValue("FW_I18N_UPDATE") == 1 && fromJson(readFile(path)) != now) {
            QVERIFY2(fwtest::writeTextFile(path, fresh), qPrintable(path));
            qInfo("FW_I18N_UPDATE: wrote %lld source texts to %s", qlonglong(now.size()), qPrintable(QDir::toNativeSeparators(path)));
        }

        const std::optional<SourceList> onDisk = fromJson(readFile(path));
        QVERIFY2(onDisk.has_value(), qPrintable(path + QStringLiteral(" is missing or not a JSON list")));
        if (*onDisk == now)
            return;
        QStringList report;
        int added = 0, removed = 0, moved = 0;
        for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
            if (!onDisk->contains(it.key())) {
                if (++added <= 25)
                    report << QStringLiteral("  new:    \"%1\"  (%2)").arg(it.key().left(90), it.value().join(QStringLiteral(", ")));
            } else if (onDisk->value(it.key()) != it.value()) {
                ++moved;
            }
        }
        for (auto it = onDisk->constBegin(); it != onDisk->constEnd(); ++it)
            if (!now.contains(it.key()) && ++removed <= 25)
                report << QStringLiteral("  gone:   \"%1\"").arg(it.key().left(90));
        const QString summary =
            QStringLiteral("\n_source.json is not what the code shows: %1 new, %2 gone, %3 used elsewhere.\n%4\n"
                           "Run this test once with FW_I18N_UPDATE=1 in the environment to rewrite the file, and commit it.")
                .arg(added)
                .arg(removed)
                .arg(moved)
                .arg(report.join(u'\n'));
        QFAIL(qPrintable(summary));
    }

    // ---- the catalogues -----------------------------------------------------------

    void cataloguesAreSound()
    {
        const std::optional<SourceList> source = fromJson(readFile(langDir() + QStringLiteral("/_source.json")));
        QVERIFY(source.has_value() && !source->isEmpty());

        QStringList wrong, seen;
        const QFileInfoList files = QDir(langDir()).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QFileInfo& fi : files) {
            const QString file = fi.fileName();
            if (file.startsWith(u'_'))
                continue;
            const auto fail = [&](const QString& what) { wrong << file + QStringLiteral(": ") + what; };
            QJsonParseError parse;
            const QJsonDocument doc = QJsonDocument::fromJson(readFile(fi.filePath()), &parse);
            if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
                fail(QStringLiteral("not a JSON object (%1 at byte %2)").arg(parse.errorString()).arg(parse.offset));
                continue;
            }
            const QJsonObject root = doc.object();
            for (const QString& key : root.keys())
                if (!QStringList{QStringLiteral("code"), QStringLiteral("name"), QStringLiteral("englishName"), QStringLiteral("strings")}.contains(key))
                    fail(QStringLiteral("unknown field \"%1\"").arg(key));
            const QString code = root.value(QStringLiteral("code")).toString();
            seen << code;
            if (code != fi.completeBaseName() || !i18n::isValidCode(code) || code == u"en" || code == i18n::pseudoCode())
                fail(QStringLiteral("\"code\" must be the file's name and a language code, found \"%1\"").arg(code));
            for (const QString& field : {QStringLiteral("name"), QStringLiteral("englishName")}) {
                const QString value = root.value(field).toString();
                if (value.isEmpty() || value != value.simplified() || value.contains(u'<') || value.contains(u'&') || value.size() > 60)
                    fail(QStringLiteral("\"%1\" is missing or not a plain name").arg(field));
            }
            if (!root.value(QStringLiteral("strings")).isObject()) {
                fail(QStringLiteral("\"strings\" is missing"));
                continue;
            }
            const QJsonObject strings = root.value(QStringLiteral("strings")).toObject();
            int usable = 0;
            for (auto it = strings.constBegin(); it != strings.constEnd(); ++it) {
                const QString english = it.key();
                const QString shown = english.left(70);
                if (!source->contains(english)) {
                    fail(QStringLiteral("\"%1\" is not a text of the app (any more): remove the entry").arg(shown));
                    continue;
                }
                if (!it.value().isString()) {
                    fail(QStringLiteral("\"%1\": the translation is not text").arg(shown));
                    continue;
                }
                const QString translation = it.value().toString();
                if (const QString why = i18n::entryProblem(english, translation); !why.isEmpty()) {
                    fail(QStringLiteral("\"%1\": %2").arg(shown, why));
                    continue;
                }
                if ((translation.front().isSpace() && !english.front().isSpace()) || (translation.back().isSpace() && !english.back().isSpace())) {
                    fail(QStringLiteral("\"%1\": the translation begins or ends with a space the English text has not").arg(shown));
                    continue;
                }
                ++usable;
            }
            if (usable == 0)
                fail(QStringLiteral("no usable entry"));
            if (shipped().contains(code) && usable * 100 < 98 * source->size()) {
                QStringList missing;
                for (auto it = source->constBegin(); it != source->constEnd() && missing.size() < 40; ++it)
                    if (!strings.contains(it.key()))
                        missing << QStringLiteral("    \"%1\"").arg(it.key().left(90));
                fail(QStringLiteral("translates %1 of %2 texts; a shipped language needs 98 %. Missing, among others:\n%3")
                         .arg(usable)
                         .arg(source->size())
                         .arg(missing.join(u'\n')));
            }
        }
        for (const QString& code : shipped())
            if (!seen.contains(code))
                wrong << QStringLiteral("shipped language \"%1\" has no catalogue").arg(code);
        QVERIFY2(wrong.isEmpty(), qPrintable(QStringLiteral("\n") + wrong.join(u'\n')));
    }

    // A catalogue that is not in the resource list would never reach the app.
    void resourceListNamesEveryFile()
    {
        const QString qrc = QString::fromUtf8(readFile(srcRoot() + QStringLiteral("/app/resources/lang.qrc")));
        QVERIFY(qrc.contains(QStringLiteral("<qresource prefix=\"/lang\">")));
        const QStringList files = QDir(langDir()).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        QVERIFY(files.contains(QStringLiteral("_source.json")));
        for (const QString& file : files)
            QVERIFY2(qrc.contains(QStringLiteral("<file alias=\"%1\">lang/%1</file>").arg(file)), qPrintable(file));
        QCOMPARE(qrc.count(QStringLiteral("<file ")), files.size());
    }

    // The files as the app reads them (from the source folder here, from its
    // resources there).
    void cataloguesLoad()
    {
        i18n::configure(langDir(), {});
        QVERIFY(!i18n::sourceTexts().isEmpty());
        const QList<i18n::Language> all = i18n::available();
        const QStringList files = QDir(langDir()).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        QCOMPARE(all.size(), files.size() - 1); // all but _source.json
        for (const i18n::Language& l : all) {
            QVERIFY2(l.builtIn, qPrintable(l.code));
            QVERIFY2(l.coverage >= (shipped().contains(l.code) ? 98 : 1) && l.coverage <= 100, qPrintable(QStringLiteral("%1: %2 %").arg(l.code).arg(l.coverage)));
            QVERIFY2(l.nativeName != l.code && l.englishName != l.code, qPrintable(l.code));
            QString problem;
            QVERIFY2(i18n::load(l.code, &problem), qPrintable(l.code));
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
            QCOMPARE(i18n::currentCode(), l.code);
        }
        // German is there from the start; one of its texts as a sample.
        QVERIFY(i18n::load(QStringLiteral("de")));
        QCOMPARE(i18n::translate(QStringLiteral("Language")), QStringLiteral("Sprache"));
    }
};

QTEST_GUILESS_MAIN(TstI18nSource)
#include "tst_i18n_source.moc"
