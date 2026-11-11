#include "core/Tr.h"

#include "core/Log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace fw::i18n {

namespace {

constexpr qint64 kMaxCatalogueBytes = 8 * 1024 * 1024;
constexpr qint64 kMaxSourceListBytes = 32 * 1024 * 1024;
constexpr int kMaxNotes = 200;

struct Catalogue {
    QString code;
    bool pseudo = false;
    QHash<QString, QString> strings;
};

// What translate() reads. A catalogue is complete before it is published
// here and is never changed or freed afterwards, so readers need no lock.
std::atomic<const Catalogue*> g_active{nullptr};

struct State {
    std::mutex mutex;
    QString resourceDir;
    QString userDir;
    bool listed = false;
    QList<Language> languages;
    bool sourceRead = false;
    QStringList source;
    QSet<QString> sourceSet;
    QStringList notes;
    QSet<QString> noteSet;
    std::vector<std::unique_ptr<const Catalogue>> published; // kept: a reader may still hold an older one
};

// Never destroyed: a worker thread may translate while the process exits.
State& state()
{
    static State* const s = new State;
    return *s;
}

// One catalogue file, checked.
struct FileData {
    QString code;
    QString name;
    QString englishName;
    QHash<QString, QString> strings;
};

// A language of both folders merged.
struct Merged {
    Language language;
    QHash<QString, QString> strings;
};

QString shortened(const QString& text)
{
    return text.size() > 60 ? text.left(59) + QChar(0x2026) : text;
}

// Language names are shown as plain text in a list: nothing that a widget
// could take for markup or a mnemonic, and no more than a line.
QString cleanName(const QString& raw)
{
    const QString name = raw.simplified();
    if (name.isEmpty() || name.size() > 60 || name.contains(u'<') || name.contains(u'&'))
        return {};
    return name;
}

QStringList placeholdersOf(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("%(?:L?[0-9]{1,2}|n)"));
    QStringList found;
    for (auto it = re.globalMatch(text); it.hasNext();) {
        const QString token = it.next().captured(0);
        if (!found.contains(token))
            found << token;
    }
    found.sort();
    return found;
}

QStringList tagsOf(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("<[^<>]*>"));
    QStringList found;
    for (auto it = re.globalMatch(text); it.hasNext();)
        found << it.next().captured(0);
    found.sort();
    return found;
}

bool hasUnsafeCharacter(const QString& text)
{
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (u < 0x20 && u != u'\n' && u != u'\t')
            return true;
        // Direction overrides and isolates can make a text read as another one.
        if ((u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069))
            return true;
    }
    return false;
}

// The *.json files of a folder by lower-case code. Names that begin with "_"
// (the source list) are not catalogues.
QMap<QString, QString> catalogueFiles(const QString& dir, QStringList& notes)
{
    QMap<QString, QString> files;
    if (dir.isEmpty())
        return files;
    const QFileInfoList entries = QDir(dir).entryInfoList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : entries) {
        const QString base = fi.completeBaseName();
        if (base.startsWith(u'_'))
            continue;
        if (!isValidCode(base)) {
            notes << QStringLiteral("Language file %1 is ignored: its name is not a language code (such as de.json or pt-BR.json).")
                         .arg(fi.fileName());
            continue;
        }
        if (base == u"en" || base == pseudoCode()) {
            notes << QStringLiteral("Language file %1 is ignored: \"%2\" is reserved.").arg(fi.fileName(), base);
            continue;
        }
        files.insert(base.toLower(), fi.filePath());
    }
    return files;
}

std::optional<FileData> readCatalogueFile(const QString& path, QStringList& notes)
{
    const QFileInfo fi(path);
    const QString shown = fi.fileName();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        notes << QStringLiteral("Language file %1 is ignored: it could not be opened (%2).").arg(shown, f.errorString());
        return std::nullopt;
    }
    // The size is checked on what was read, too: a file can grow after size().
    const QByteArray bytes = f.size() > kMaxCatalogueBytes ? QByteArray() : f.read(kMaxCatalogueBytes + 1);
    if (f.size() > kMaxCatalogueBytes || bytes.size() > kMaxCatalogueBytes) {
        notes << QStringLiteral("Language file %1 is ignored: it is larger than 8 MiB.").arg(shown);
        return std::nullopt;
    }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        notes << QStringLiteral("Language file %1 is ignored: it is not valid JSON (%2).")
                     .arg(shown, parse.error != QJsonParseError::NoError ? parse.errorString() : QStringLiteral("not an object"));
        return std::nullopt;
    }
    const QJsonObject root = doc.object();
    const QString base = fi.completeBaseName();
    FileData data;
    data.code = root.value(QStringLiteral("code")).toString();
    if (data.code.isEmpty())
        data.code = base;
    if (!isValidCode(data.code) || data.code.compare(base, Qt::CaseInsensitive) != 0) {
        notes << QStringLiteral("Language file %1 is ignored: its code \"%2\" is not the file's name.").arg(shown, shortened(data.code));
        return std::nullopt;
    }
    data.name = cleanName(root.value(QStringLiteral("name")).toString());
    data.englishName = cleanName(root.value(QStringLiteral("englishName")).toString());

    const QJsonObject strings = root.value(QStringLiteral("strings")).toObject();
    data.strings.reserve(strings.size());
    int dropped = 0;
    QString firstDropped;
    for (auto it = strings.constBegin(); it != strings.constEnd(); ++it) {
        const QString translation = it.value().toString();
        const QString why = it.value().isString() ? entryProblem(it.key(), translation) : QStringLiteral("it is not text");
        if (why.isEmpty()) {
            data.strings.insert(it.key(), translation);
            continue;
        }
        if (dropped++ == 0)
            firstDropped = QStringLiteral("\"%1\": %2").arg(shortened(it.key()), why);
    }
    if (dropped > 0)
        notes << QStringLiteral("Language file %1: %2 of %3 entries are not used (the English text is shown). The first: %4.")
                     .arg(shown)
                     .arg(dropped)
                     .arg(strings.size())
                     .arg(firstDropped);
    return data;
}

// Built-in catalogue, then the user's file over it. Empty when neither folder
// has a usable file for the code.
std::optional<Merged> readLanguage(const QString& builtInPath, const QString& userPath, QStringList& notes)
{
    std::optional<FileData> builtIn, user;
    if (!builtInPath.isEmpty())
        builtIn = readCatalogueFile(builtInPath, notes);
    if (!userPath.isEmpty())
        user = readCatalogueFile(userPath, notes);
    if (!builtIn && !user)
        return std::nullopt;

    Merged m;
    m.language.builtIn = builtIn.has_value();
    if (builtIn) {
        m.language.code = builtIn->code;
        m.language.nativeName = builtIn->name;
        m.language.englishName = builtIn->englishName;
        m.strings = std::move(builtIn->strings);
    }
    if (user) {
        if (m.language.code.isEmpty())
            m.language.code = user->code;
        if (!user->name.isEmpty())
            m.language.nativeName = user->name;
        if (!user->englishName.isEmpty())
            m.language.englishName = user->englishName;
        if (m.strings.isEmpty()) {
            m.strings = std::move(user->strings);
        } else {
            for (auto it = user->strings.constBegin(); it != user->strings.constEnd(); ++it)
                m.strings.insert(it.key(), it.value());
        }
    }
    if (m.language.nativeName.isEmpty())
        m.language.nativeName = m.language.englishName.isEmpty() ? m.language.code : m.language.englishName;
    if (m.language.englishName.isEmpty())
        m.language.englishName = m.language.nativeName;
    return m;
}

// The state's mutex is held.
void readSourceLocked(State& s)
{
    if (s.sourceRead)
        return;
    s.sourceRead = true;
    s.source.clear();
    s.sourceSet.clear();
    if (s.resourceDir.isEmpty())
        return;
    QFile f(QDir(s.resourceDir).filePath(QStringLiteral("_source.json")));
    if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxSourceListBytes)
        return;
    const QJsonArray entries = QJsonDocument::fromJson(f.readAll()).array();
    s.source.reserve(entries.size());
    for (const QJsonValue& e : entries) {
        const QString text = e.toObject().value(QStringLiteral("text")).toString();
        if (text.isEmpty() || s.sourceSet.contains(text))
            continue;
        s.sourceSet.insert(text);
        s.source << text;
    }
}

// Remembers the notes that are new and writes them to the log. Not under the
// state's lock while logging: a log sink is other people's code.
void report(const QStringList& notes)
{
    if (notes.isEmpty())
        return;
    QStringList fresh;
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        for (const QString& n : notes) {
            if (s.noteSet.contains(n) || s.notes.size() >= kMaxNotes)
                continue;
            s.noteSet.insert(n);
            s.notes << n;
            fresh << n;
        }
    }
    for (const QString& n : fresh)
        Logger::instance().log(LogLevel::Warning, QStringLiteral("i18n"), n);
}

void publish(std::unique_ptr<const Catalogue> next)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    const Catalogue* raw = next.get();
    if (next)
        s.published.push_back(std::move(next));
    g_active.store(raw, std::memory_order_release);
}

} // namespace

void configure(const QString& resourceDir, const QString& userDir)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.resourceDir = resourceDir;
    s.userDir = userDir;
    s.listed = false;
    s.languages.clear();
    s.sourceRead = false;
    s.source.clear();
    s.sourceSet.clear();
}

QString resourceDir()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.resourceDir;
}

QString userDir()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.userDir;
}

QString pseudoCode()
{
    return QStringLiteral("qps");
}

bool isValidCode(const QString& code)
{
    static const QRegularExpression re(QStringLiteral("\\A[a-z]{2,3}(?:-[A-Za-z]{2,4})?\\z"));
    return re.match(code).hasMatch();
}

QString entryProblem(const QString& english, const QString& translation)
{
    if (translation.trimmed().isEmpty())
        return QStringLiteral("the translation is empty");
    if (placeholdersOf(english) != placeholdersOf(translation))
        return QStringLiteral("its placeholders (%1, %2, ...) are not those of the English text");
    if (!english.contains(u'<')) {
        if (translation.contains(u'<'))
            return QStringLiteral("it has \"<\", which the English text has not");
    } else if (english.count(u'<') != translation.count(u'<') || tagsOf(english) != tagsOf(translation)) {
        return QStringLiteral("its tags (<b>, <a ...>, ...) are not those of the English text");
    }
    if (english.count(u'&') != translation.count(u'&'))
        return QStringLiteral("it does not have as many \"&\" as the English text");
    if (hasUnsafeCharacter(translation))
        return QStringLiteral("it has control characters");
    if (translation.size() > english.size() * 10 + 200)
        return QStringLiteral("it is far longer than the English text");
    return {};
}

QString translate(const QString& english)
{
    const Catalogue* const c = g_active.load(std::memory_order_acquire);
    if (!c)
        return english;
    if (c->pseudo) {
        if (english.isEmpty())
            return english;
        QString wrapped;
        wrapped.reserve(english.size() + 2);
        wrapped += QChar(0x27E6);
        wrapped += english;
        wrapped += QChar(0x27E7);
        return wrapped;
    }
    const auto it = c->strings.constFind(english);
    return it == c->strings.cend() ? english : *it;
}

bool load(const QString& requested, QString* problem)
{
    const QString code = requested.trimmed();
    QStringList notes;
    std::unique_ptr<Catalogue> next;
    bool ok = true;
    if (code.isEmpty() || code.compare(QLatin1String("en"), Qt::CaseInsensitive) == 0) {
        // English: no catalogue.
    } else if (code == pseudoCode()) {
        next = std::make_unique<Catalogue>();
        next->code = code;
        next->pseudo = true;
    } else if (!isValidCode(code)) {
        notes << QStringLiteral("\"%1\" is not a language code; the interface stays English.").arg(shortened(code));
        ok = false;
    } else {
        QString resources, user;
        {
            State& s = state();
            std::lock_guard lock(s.mutex);
            resources = s.resourceDir;
            user = s.userDir;
        }
        QStringList listing; // what the folders hold is reported by available(), not here
        const QString key = code.toLower();
        const std::optional<Merged> merged =
            readLanguage(catalogueFiles(resources, listing).value(key), catalogueFiles(user, listing).value(key), notes);
        if (merged) {
            next = std::make_unique<Catalogue>();
            next->code = merged->language.code;
            next->strings = merged->strings;
        } else {
            notes << QStringLiteral("There is no usable language file for \"%1\"; the interface stays English.").arg(code);
            ok = false;
        }
    }
    publish(std::move(next));
    report(notes);
    if (problem)
        *problem = notes.join(u'\n');
    return ok;
}

QString currentCode()
{
    const Catalogue* const c = g_active.load(std::memory_order_acquire);
    return c ? c->code : QStringLiteral("en");
}

QList<Language> available()
{
    QStringList notes;
    QList<Language> out;
    {
        State& s = state();
        std::lock_guard lock(s.mutex);
        if (!s.listed) {
            s.listed = true;
            s.languages.clear();
            readSourceLocked(s);
            const QMap<QString, QString> builtIn = catalogueFiles(s.resourceDir, notes);
            const QMap<QString, QString> user = catalogueFiles(s.userDir, notes);
            QStringList codes = builtIn.keys();
            for (const QString& k : user.keys())
                if (!codes.contains(k))
                    codes << k;
            for (const QString& k : codes) {
                std::optional<Merged> m = readLanguage(builtIn.value(k), user.value(k), notes);
                if (!m)
                    continue;
                if (!s.source.isEmpty()) {
                    qint64 usable = 0;
                    for (auto it = m->strings.constBegin(); it != m->strings.constEnd(); ++it)
                        if (s.sourceSet.contains(it.key()))
                            ++usable;
                    m->language.coverage = int(usable * 100 / s.source.size());
                }
                s.languages << m->language;
            }
            std::sort(s.languages.begin(), s.languages.end(), [](const Language& a, const Language& b) {
                const int byName = a.englishName.compare(b.englishName, Qt::CaseInsensitive);
                return byName != 0 ? byName < 0 : a.code < b.code;
            });
        }
        out = s.languages;
    }
    report(notes);
    return out;
}

QStringList sourceTexts()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    readSourceLocked(s);
    return s.source;
}

QStringList notes()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.notes;
}

} // namespace fw::i18n
