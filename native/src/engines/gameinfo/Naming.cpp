#include "engines/gameinfo/Naming.h"

#include <QRegularExpression>
#include <QStringList>

namespace fw::gameinfo {

bool isValidContentId(const QString& contentId)
{
    static const QRegularExpression re(
        QStringLiteral("^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}$"));
    return re.match(contentId).hasMatch();
}

QString titleIdFromContentId(const QString& contentId)
{
    return isValidContentId(contentId) ? contentId.mid(7, 9) : QString();
}

bool isValidTitleId(const QString& titleId)
{
    static const QRegularExpression re(QStringLiteral("^[A-Z]{4}[0-9]{5}$"));
    return re.match(titleId).hasMatch();
}

QString findTitleIdIn(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("((?:PPSA|CUSA|PPLH)\\d{5})"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(text);
    return m.hasMatch() ? m.captured(1).toUpper() : QString();
}

QString normalizedTitleId(const QString& titleId)
{
    QString s = titleId.toUpper();
    s.remove(u'-');
    return s;
}

QString sanitizeTitle(const QString& input)
{
    if (input.isEmpty())
        return {};
    QString name = input;

    // Typographic normalisation first, so the drop pass keeps the meaning.
    static const std::pair<char16_t, const char*> kNormalise[] = {
        {u'‘', "'"}, {u'’', "'"}, {u'“', "\""}, {u'”', "\""},
        {u'´', "'"}, {u'`', "'"}, {u'–', "-"}, {u'—', "-"},
        {u'―', "-"}, {u'−', "-"}, {u'…', "..."}, {u'•', " "},
        {u'·', " "}, {u'‧', " "}, {u'®', ""},  {u'©', ""},
        {u'™', ""},  {u'℠', ""},
    };
    for (const auto& [from, to] : kNormalise)
        name.replace(QChar(from), QString::fromLatin1(to));

    // Drop C0/C1 control characters and DEL (tab survives, then collapses).
    QString kept;
    kept.reserve(name.size());
    for (QChar ch : name) {
        const char16_t u = ch.unicode();
        if (u == u'\t' || (u >= 0x20 && u < 0x7f) || u > 0x9f)
            kept.append(ch);
    }
    name = kept;

    // Windows-reserved characters and cmd.exe metacharacters.
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|&^%!`$]"));
    name.remove(bad);

    static const QRegularExpression ws(QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
    name.replace(ws, QStringLiteral(" "));
    name = name.trimmed();
    while (!name.isEmpty() && (name.front() == u' ' || name.front() == u'.'))
        name.remove(0, 1);
    while (!name.isEmpty() && (name.back() == u' ' || name.back() == u'.'))
        name.chop(1);

    // Cap at 80 code points (Python slices code points, not UTF-16 units).
    qsizetype units = 0;
    int points = 0;
    while (units < name.size() && points < 80) {
        units += (name.at(units).isHighSurrogate() && units + 1 < name.size()) ? 2 : 1;
        ++points;
    }
    name.truncate(units);
    return name.isEmpty() ? QStringLiteral("game") : name;
}

QString namePresetKey(NamePreset preset)
{
    switch (preset) {
    case NamePreset::TitleId: return QStringLiteral("ppsa");
    case NamePreset::TitleIdTitle: return QStringLiteral("ppsa_title");
    case NamePreset::TitleIdTitleVersion: return QStringLiteral("ppsa_title_v");
    }
    return QStringLiteral("ppsa_title_v");
}

std::optional<NamePreset> namePresetFromKey(const QString& key)
{
    if (key == u"ppsa")
        return NamePreset::TitleId;
    if (key == u"ppsa_title")
        return NamePreset::TitleIdTitle;
    if (key == u"ppsa_title_v")
        return NamePreset::TitleIdTitleVersion;
    return std::nullopt;
}

QString buildDumpName(const NameParts& parts, NamePreset preset, const NameOptions& options)
{
    QString titleId = parts.titleId;
    if (options.uppercaseTitleId && !titleId.isEmpty())
        titleId = titleId.toUpper();
    QStringList bits;
    if (!titleId.isEmpty())
        bits << titleId;
    if (preset != NamePreset::TitleId && !parts.title.isEmpty() && parts.title != titleId)
        bits << parts.title;
    if (bits.isEmpty() || titleId.isEmpty())
        return {};
    QString name = bits.join(u' ');
    if (preset == NamePreset::TitleIdTitleVersion && !parts.version.isEmpty())
        name += QStringLiteral(" (") + parts.version + u')';
    if (options.sanitize) {
        name = sanitizeTitle(name);
        while (!name.isEmpty() && (name.back() == u' ' || name.back() == u'.'))
            name.chop(1);
    }
    return name;
}

QString buildImageBaseName(const NameParts& parts)
{
    QStringList bits;
    if (!parts.titleId.isEmpty())
        bits << parts.titleId;
    if (!parts.title.isEmpty() && parts.title != parts.titleId)
        bits << parts.title;
    if (bits.isEmpty())
        return QStringLiteral("game");
    QString name = bits.join(u' ');
    if (!parts.version.isEmpty())
        name += QStringLiteral(" (") + parts.version + u')';
    return sanitizeTitle(name);
}

QString folderNameProblem(const QString& name)
{
    if (name.trimmed().isEmpty())
        return QStringLiteral("The name is empty.");
    static const QRegularExpression bad(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    if (name.contains(bad))
        return QStringLiteral("The name contains a character Windows does not allow (< > : \" / \\ | ? *).");
    if (name.endsWith(u'.') || name.endsWith(u' '))
        return QStringLiteral("The name ends with a dot or a space, which Windows removes.");
    if (name == u"." || name == u"..")
        return QStringLiteral("The name is reserved.");
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?$"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch())
        return QStringLiteral("The name is reserved by Windows.");
    if (name.size() > 240)
        return QStringLiteral("The name is too long.");
    return {};
}

} // namespace fw::gameinfo
