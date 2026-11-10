#include "engines/dlc/DlcIni.h"

#include "core/FileUtil.h"
#include "engines/dlc/DlcIdentity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <initializer_list>

namespace fw::dlc {

namespace {

// ---- messages (the Companion's texts) ------------------------------------------

const char16_t kMsgMissing[] = u"DLC INI does not exist.";
const char16_t kMsgTooLarge[] = u"DLC INI exceeds the 16 MiB safety limit.";
const char16_t kMsgBadBomText[] = u"DLC INI has invalid text for its declared BOM encoding.";
const char16_t kMsgNul[] = u"DLC INI contains NUL characters or an unsupported encoding.";
const char16_t kWarnLatin1[] =
    u"Non-UTF-8 text is preserved as single-byte text. Non-ASCII title edits must be representable in that encoding.";
const char16_t kWarnNoEntries[] = u"No content_id or contentId fields were found.";
const char16_t kWarnMalformed[] = u"Malformed Content ID; correct it before writing or building.";
const char16_t kWarnMultiple[] =
    u"Multiple Content IDs in one section: only identity/name fields in this entry's following block are updated; "
    u"shared fields before the first Content ID remain unchanged.";
const char16_t kWarnTitleIdDiffers[] = u"INI Title ID differs from the Content ID's embedded Title ID.";
const char16_t kMsgUnknownIndex[] = u"Unknown DLC entry index.";
const char16_t kMsgPlatform[] = u"PS4 and PS5 identifiers cannot be interchanged.";
const char16_t kMsgTitleBreaks[] = u"INI titles cannot contain line breaks or NUL characters.";
const char16_t kMsgValueBreaks[] = u"INI values cannot contain line breaks or NUL characters.";
const char16_t kMsgQuotedQuote[] = u"A quoted INI title cannot contain a double quote.";
const char16_t kMsgNeedsQuotes[] = u"This INI title needs quotes but contains an unsupported double quote.";
const char16_t kMsgConflict[] =
    u"Two target entries would change the same INI field differently. Write separate target files.";
const char16_t kMsgNotRepresentable[] = u"The edited title is not representable in the original INI encoding.";
const char16_t kMsgIdentityFailed[] = u"Updated INI failed identity validation; no output was saved.";
const char16_t kMsgTitleFailed[] = u"Updated INI failed title validation; no output was saved.";
const char16_t kMsgOverwriteSource[] = u"Original INI inputs cannot be overwritten. Choose an updated output name.";
const char16_t kMsgSourceChanged[] = u"The source INI changed after import. Reload it before applying edits.";
const char16_t kMsgGeneratedFailed[] = u"Generated INI failed validation.";

QString msg(const char16_t* text) { return QString::fromUtf16(text); }

// ---- encodings -----------------------------------------------------------------

enum class Codec { Utf8, Utf16Le, Utf16Be, Utf32Le, Utf32Be, Latin1 };

QString codecId(Codec c)
{
    switch (c) {
    case Codec::Utf8: return QStringLiteral("utf-8");
    case Codec::Utf16Le: return QStringLiteral("utf-16le");
    case Codec::Utf16Be: return QStringLiteral("utf-16be");
    case Codec::Utf32Le: return QStringLiteral("utf-32le");
    case Codec::Utf32Be: return QStringLiteral("utf-32be");
    case Codec::Latin1: return QStringLiteral("latin1");
    }
    return QStringLiteral("utf-8");
}

Codec codecFromId(const QString& id)
{
    for (Codec c : {Codec::Utf8, Codec::Utf16Le, Codec::Utf16Be, Codec::Utf32Le, Codec::Utf32Be, Codec::Latin1}) {
        if (codecId(c) == id)
            return c;
    }
    return Codec::Utf8;
}

QString codecDisplayName(Codec c)
{
    switch (c) {
    case Codec::Utf8: return QStringLiteral("utf-8");
    case Codec::Utf16Le: return QStringLiteral("utf-16LE");
    case Codec::Utf16Be: return QStringLiteral("utf-16BE");
    case Codec::Utf32Le: return QStringLiteral("utf-32LE");
    case Codec::Utf32Be: return QStringLiteral("utf-32BE");
    case Codec::Latin1: return QStringLiteral("iso-8859-1");
    }
    return QStringLiteral("utf-8");
}

bool isHighSurrogate(char16_t u) { return u >= 0xD800 && u <= 0xDBFF; }
bool isLowSurrogate(char16_t u) { return u >= 0xDC00 && u <= 0xDFFF; }

void appendCodePoint(QString& out, char32_t cp)
{
    if (cp >= 0x10000) {
        out.append(QChar(QChar::highSurrogate(cp)));
        out.append(QChar(QChar::lowSurrogate(cp)));
    } else {
        out.append(QChar(char16_t(cp)));
    }
}

// Strict UTF-8 (well-formed sequences only: no overlong forms, surrogates,
// code points above U+10FFFF or truncated sequences).
std::optional<QString> decodeUtf8(QByteArrayView in)
{
    const auto* p = reinterpret_cast<const uchar*>(in.data());
    const qsizetype n = in.size();
    QString out;
    out.reserve(n);
    qsizetype i = 0;
    while (i < n) {
        const uchar b0 = p[i];
        if (b0 < 0x80) {
            out.append(QChar(char16_t(b0)));
            ++i;
            continue;
        }
        int len = 0;
        char32_t cp = 0;
        uchar lo = 0x80;
        uchar hi = 0xBF;
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            len = 2;
            cp = b0 & 0x1Fu;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            len = 3;
            cp = b0 & 0x0Fu;
            if (b0 == 0xE0)
                lo = 0xA0;
            else if (b0 == 0xED)
                hi = 0x9F;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            len = 4;
            cp = b0 & 0x07u;
            if (b0 == 0xF0)
                lo = 0x90;
            else if (b0 == 0xF4)
                hi = 0x8F;
        } else {
            return std::nullopt;
        }
        if (n - i < len)
            return std::nullopt;
        for (int k = 1; k < len; ++k) {
            const uchar b = p[i + k];
            const uchar min = k == 1 ? lo : uchar(0x80);
            const uchar max = k == 1 ? hi : uchar(0xBF);
            if (b < min || b > max)
                return std::nullopt;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        appendCodePoint(out, cp);
        i += len;
    }
    return out;
}

// Strict UTF-16: even length, surrogates paired.
std::optional<QString> decodeUtf16(QByteArrayView in, bool bigEndian)
{
    if (in.size() % 2 != 0)
        return std::nullopt;
    const auto* p = reinterpret_cast<const uchar*>(in.data());
    const qsizetype n = in.size() / 2;
    QString out(n, Qt::Uninitialized);
    QChar* d = out.data();
    for (qsizetype i = 0; i < n; ++i) {
        const uchar a = p[2 * i];
        const uchar b = p[2 * i + 1];
        d[i] = QChar(char16_t(bigEndian ? (a << 8) | b : (b << 8) | a));
    }
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t u = d[i].unicode();
        if (isHighSurrogate(u)) {
            if (i + 1 < n && isLowSurrogate(d[i + 1].unicode())) {
                ++i;
                continue;
            }
            return std::nullopt;
        }
        if (isLowSurrogate(u))
            return std::nullopt;
    }
    return out;
}

// Strict UTF-32: length a multiple of 4, scalar values only.
std::optional<QString> decodeUtf32(QByteArrayView in, bool bigEndian)
{
    if (in.size() % 4 != 0)
        return std::nullopt;
    const auto* p = reinterpret_cast<const uchar*>(in.data());
    QString out;
    out.reserve(in.size() / 4);
    for (qsizetype i = 0; i < in.size(); i += 4) {
        const char32_t cp = bigEndian
            ? (char32_t(p[i]) << 24) | (char32_t(p[i + 1]) << 16) | (char32_t(p[i + 2]) << 8) | char32_t(p[i + 3])
            : (char32_t(p[i + 3]) << 24) | (char32_t(p[i + 2]) << 16) | (char32_t(p[i + 1]) << 8) | char32_t(p[i]);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return std::nullopt;
        appendCodePoint(out, cp);
    }
    return out;
}

std::optional<QString> decode(Codec c, QByteArrayView in)
{
    switch (c) {
    case Codec::Utf8: return decodeUtf8(in);
    case Codec::Utf16Le: return decodeUtf16(in, false);
    case Codec::Utf16Be: return decodeUtf16(in, true);
    case Codec::Utf32Le: return decodeUtf32(in, false);
    case Codec::Utf32Be: return decodeUtf32(in, true);
    case Codec::Latin1: return QString::fromLatin1(in);
    }
    return std::nullopt;
}

// Code points of `s`; an unpaired surrogate becomes U+FFFD (so the round-trip
// check reports it as not representable).
template <typename Fn>
void forEachCodePoint(const QString& s, Fn&& fn)
{
    const qsizetype n = s.size();
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t u = s.at(i).unicode();
        if (isHighSurrogate(u) && i + 1 < n && isLowSurrogate(s.at(i + 1).unicode())) {
            fn(QChar::surrogateToUcs4(u, s.at(i + 1).unicode()));
            ++i;
        } else if (isHighSurrogate(u) || isLowSurrogate(u)) {
            fn(char32_t(0xFFFD));
        } else {
            fn(char32_t(u));
        }
    }
}

QByteArray encode(Codec c, const QString& s)
{
    QByteArray out;
    switch (c) {
    case Codec::Latin1:
        return s.toLatin1();
    case Codec::Utf8:
        out.reserve(s.size());
        forEachCodePoint(s, [&](char32_t cp) {
            if (cp < 0x80) {
                out.append(char(cp));
            } else if (cp < 0x800) {
                out.append(char(0xC0 | (cp >> 6)));
                out.append(char(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.append(char(0xE0 | (cp >> 12)));
                out.append(char(0x80 | ((cp >> 6) & 0x3F)));
                out.append(char(0x80 | (cp & 0x3F)));
            } else {
                out.append(char(0xF0 | (cp >> 18)));
                out.append(char(0x80 | ((cp >> 12) & 0x3F)));
                out.append(char(0x80 | ((cp >> 6) & 0x3F)));
                out.append(char(0x80 | (cp & 0x3F)));
            }
        });
        return out;
    case Codec::Utf16Le:
    case Codec::Utf16Be: {
        const bool be = c == Codec::Utf16Be;
        out.reserve(s.size() * 2);
        for (QChar ch : s) { // code units as they are; the strict decoder rejects unpaired surrogates
            const char16_t u = ch.unicode();
            const char hiByte = char(u >> 8);
            const char loByte = char(u & 0xFF);
            out.append(be ? hiByte : loByte);
            out.append(be ? loByte : hiByte);
        }
        return out;
    }
    case Codec::Utf32Le:
    case Codec::Utf32Be: {
        const bool be = c == Codec::Utf32Be;
        out.reserve(s.size() * 4);
        forEachCodePoint(s, [&](char32_t cp) {
            const char b[4] = {char(cp >> 24), char((cp >> 16) & 0xFF), char((cp >> 8) & 0xFF), char(cp & 0xFF)};
            if (be) {
                out.append(b, 4);
            } else {
                out.append(b[3]);
                out.append(b[2]);
                out.append(b[1]);
                out.append(b[0]);
            }
        });
        return out;
    }
    }
    return out;
}

struct Detected {
    Codec codec = Codec::Utf8;
    int bomLength = 0;
};

Detected detectBom(const QByteArray& b)
{
    if (b.startsWith(QByteArrayView("\xFF\xFE\x00\x00", 4)))
        return {Codec::Utf32Le, 4};
    if (b.startsWith(QByteArrayView("\x00\x00\xFE\xFF", 4)))
        return {Codec::Utf32Be, 4};
    if (b.startsWith(QByteArrayView("\xEF\xBB\xBF", 3)))
        return {Codec::Utf8, 3};
    if (b.startsWith(QByteArrayView("\xFF\xFE", 2)))
        return {Codec::Utf16Le, 2};
    if (b.startsWith(QByteArrayView("\xFE\xFF", 2)))
        return {Codec::Utf16Be, 2};
    return {Codec::Utf8, 0};
}

// ---- lines and fields ----------------------------------------------------------------

struct SplitLine {
    QString text;
    QString end;
};

// Splits on CRLF, lone CR and lone LF. A trailing terminator does not start an
// extra line; an empty text is one empty line.
std::vector<SplitLine> splitLines(const QString& s)
{
    std::vector<SplitLine> out;
    const qsizetype n = s.size();
    qsizetype start = 0;
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = s.at(i);
        if (c == u'\r') {
            const bool crlf = i + 1 < n && s.at(i + 1) == u'\n';
            out.push_back({s.mid(start, i - start), crlf ? QStringLiteral("\r\n") : QStringLiteral("\r")});
            if (crlf)
                ++i;
            start = i + 1;
        } else if (c == u'\n') {
            out.push_back({s.mid(start, i - start), QStringLiteral("\n")});
            start = i + 1;
        }
    }
    if (start < n || out.empty())
        out.push_back({s.mid(start), QString()});
    return out;
}

struct Field {
    bool hasKey = false;
    bool quoted = false;
    QString key;
    QString value;
    QString prefix; // text before the value (includes the opening quote when quoted)
    QString suffix; // text after the value (closing quote, spacing, inline comment)
};

qsizetype firstNonSpace(const QString& s, qsizetype from)
{
    qsizetype i = from;
    while (i < s.size() && s.at(i).isSpace())
        ++i;
    return i;
}

Field parseLine(const QString& text)
{
    Field f;
    const qsizetype n = text.size();
    const qsizetype lead = firstNonSpace(text, 0);
    if (lead < n) {
        const QChar c = text.at(lead);
        if (c == u'#' || c == u';' || c == u'[')
            return f;
    }
    const qsizetype eq = text.indexOf(u'=');
    if (eq < 0)
        return f;
    const QString key = text.left(eq).trimmed();
    if (key.isEmpty())
        return f;
    f.hasKey = true;
    f.key = key;
    const qsizetype p = firstNonSpace(text, eq + 1);
    if (p < n && text.at(p) == u'"') {
        const qsizetype close = text.indexOf(u'"', p + 1);
        if (close >= 0) {
            f.quoted = true;
            f.prefix = text.left(p + 1);
            f.value = text.mid(p + 1, close - p - 1);
            f.suffix = text.mid(close);
            return f;
        }
    }
    qsizetype end = n;
    for (qsizetype j = p; j < n; ++j) {
        const QChar c = text.at(j);
        if ((c == u';' || c == u'#') && j > 0 && text.at(j - 1).isSpace()) {
            end = j;
            break;
        }
    }
    qsizetype valueEnd = end;
    while (valueEnd > p && text.at(valueEnd - 1).isSpace())
        --valueEnd;
    f.prefix = text.left(p);
    f.value = text.mid(p, valueEnd - p);
    f.suffix = text.mid(valueEnd);
    return f;
}

bool isCommentText(const QString& trimmed) { return trimmed.startsWith(u'#') || trimmed.startsWith(u';'); }

bool sectionName(const QString& trimmed, QString* name)
{
    if (!trimmed.startsWith(u'['))
        return false;
    const qsizetype close = trimmed.indexOf(u']');
    if (close <= 0)
        return false;
    *name = trimmed.mid(1, close - 1);
    return true;
}

// Length of `^\s*[#;]\s*` in `text`, or -1 when the line is not a comment.
qsizetype commentMarkerLength(const QString& text)
{
    qsizetype i = firstNonSpace(text, 0);
    if (i >= text.size() || (text.at(i) != u'#' && text.at(i) != u';'))
        return -1;
    return firstNonSpace(text, i + 1);
}

// Keys compare ignoring ASCII case (every key the converter knows is ASCII).
bool keyEquals(QStringView a, QStringView b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        char16_t x = a.at(i).unicode();
        char16_t y = b.at(i).unicode();
        if (x >= u'A' && x <= u'Z')
            x = char16_t(x + 32);
        if (y >= u'A' && y <= u'Z')
            y = char16_t(y + 32);
        if (x != y)
            return false;
    }
    return true;
}

bool keyIn(const QString& key, std::initializer_list<QStringView> names)
{
    for (QStringView n : names) {
        if (keyEquals(key, n))
            return true;
    }
    return false;
}

bool isContentKey(const QString& k) { return keyIn(k, {u"content_id", u"contentId"}); }
bool isTitleIdKey(const QString& k) { return keyIn(k, {u"title_id", u"titleId"}); }
bool isPrimaryIdKey(const QString& k) { return keyIn(k, {u"primary_id", u"primaryId"}); }
bool isNameKey(const QString& k) { return keyIn(k, {u"title", u"title_name", u"titleName", u"name"}); }

bool hasBreakOrNul(const QString& s) { return s.contains(u'\r') || s.contains(u'\n') || s.contains(QChar(u'\0')); }

// Unquoted values are wrapped in quotes when spacing or an inline-comment
// marker (`\s[#;]`) would otherwise be lost on the next read.
bool needsQuotes(const QString& v)
{
    if (v.isEmpty())
        return false;
    if (v.front().isSpace() || v.back().isSpace())
        return true;
    for (qsizetype j = 1; j < v.size(); ++j) {
        const QChar c = v.at(j);
        if ((c == u'#' || c == u';') && v.at(j - 1).isSpace())
            return true;
    }
    return false;
}

QString suffixAfterLastDash(const QString& cid) { return cid.mid(cid.lastIndexOf(u'-') + 1); }

QString asciiLower(const QString& s)
{
    QString out = s;
    for (QChar& c : out) {
        if (c >= u'A' && c <= u'Z')
            c = QChar(char16_t(c.unicode() + 32));
    }
    return out;
}

// Block metadata: one value per key (keys compared ignoring ASCII case), in
// first-appearance order; a repeated key keeps its place and takes the last value.
class MetadataBuilder {
public:
    explicit MetadataBuilder(std::vector<std::pair<QString, QString>>& md) : m_md(md) {}
    void set(const QString& key, const QString& value)
    {
        const QString folded = asciiLower(key);
        const auto it = m_index.constFind(folded);
        if (it != m_index.constEnd()) {
            m_md[*it].second = value;
            return;
        }
        m_index.insert(folded, m_md.size());
        m_md.emplace_back(key, value);
    }

private:
    std::vector<std::pair<QString, QString>>& m_md;
    QHash<QString, size_t> m_index;
};

bool sharesSection(const std::vector<DlcIniEntry>& entries, size_t k, size_t other)
{
    return other < entries.size() && entries[other].startLine == entries[k].startLine;
}

bool multipleInSection(const std::vector<DlcIniEntry>& entries, size_t k)
{
    return (k > 0 && sharesSection(entries, k, k - 1)) || sharesSection(entries, k, k + 1);
}

// First line of an entry's block: the section header for a section with one
// Content ID, the entry's own content line when the section has several.
int blockBegin(const std::vector<DlcIniEntry>& entries, size_t k)
{
    return multipleInSection(entries, k) ? entries[k].contentLine : entries[k].startLine;
}

std::vector<DlcIniEntry> readEntries(const std::vector<SplitLine>& lines)
{
    const int count = int(lines.size());
    std::vector<DlcIniEntry> entries;
    std::vector<int> headers; // section header lines in file order
    QString section;
    int sectionStart = 0;
    int lastComment = -1;
    for (int i = 0; i < count; ++i) {
        const QString& text = lines[size_t(i)].text;
        const QString trimmed = text.trimmed();
        QString name;
        if (sectionName(trimmed, &name)) {
            section = name;
            sectionStart = i;
            lastComment = -1;
            headers.push_back(i);
            continue;
        }
        if (isCommentText(trimmed)) {
            lastComment = i;
            continue;
        }
        const Field f = parseLine(text);
        if (!f.hasKey || !isContentKey(f.key))
            continue;
        DlcIniEntry e;
        e.index = int(entries.size());
        e.contentId = f.value;
        e.section = section;
        e.contentLine = i;
        e.startLine = sectionStart;
        e.titleId = titleIdOf(f.value);
        e.region = regionText(f.value);
        e.titleCommentLine = lastComment;
        const QString commentTitle =
            lastComment >= 0 ? lines[size_t(lastComment)].text.trimmed().mid(1).trimmed() : QString();
        e.title = commentTitle.isEmpty() ? suffixAfterLastDash(f.value) : commentTitle;
        if (!isContentId(f.value))
            e.warnings << msg(kWarnMalformed);
        entries.push_back(std::move(e));
        lastComment = -1;
    }

    auto sectionEnd = [&](int contentLine) {
        const auto next = std::upper_bound(headers.begin(), headers.end(), contentLine);
        return next == headers.end() ? count : *next;
    };

    for (size_t k = 0; k < entries.size(); ++k) {
        DlcIniEntry& e = entries[k];
        const bool multi = multipleInSection(entries, k);
        e.endLine = sharesSection(entries, k, k + 1) ? entries[k + 1].contentLine : sectionEnd(e.contentLine);
        if (multi)
            e.warnings << msg(kWarnMultiple);
        const QString derived = e.titleId;
        MetadataBuilder metadata(e.metadata);
        for (int l = blockBegin(entries, k); l < e.endLine; ++l) {
            const Field f = parseLine(lines[size_t(l)].text);
            if (!f.hasKey)
                continue;
            metadata.set(f.key, f.value);
            if (isTitleIdKey(f.key)) {
                if (!derived.isEmpty() && derived != f.value && !e.warnings.contains(msg(kWarnTitleIdDiffers)))
                    e.warnings << msg(kWarnTitleIdDiffers);
                e.titleId = f.value;
            } else if (isNameKey(f.key) && !f.value.isEmpty()) {
                e.title = f.value;
            }
        }
    }
    return entries;
}

bool samePath(const QString& a, const QString& b)
{
    const QString x = QDir::cleanPath(QFileInfo(a).absoluteFilePath());
    const QString y = QDir::cleanPath(QFileInfo(b).absoluteFilePath());
    return x.compare(y, Qt::CaseInsensitive) == 0;
}

} // namespace

// ---- DlcIniDocument ---------------------------------------------------------------------

Result<DlcIniDocument> DlcIniDocument::load(const QString& path)
{
    const QString abs = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QFileInfo fi(abs);
    if (abs.isEmpty() || !fi.isFile())
        return Error(ErrorCode::NotFound, msg(kMsgMissing), path);
    if (fi.size() > kMaxIniBytes)
        return Error(ErrorCode::InvalidArgument, msg(kMsgTooLarge), path);
    QFile f(toLongPath(abs));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("DLC INI could not be read."), f.errorString());
    const QByteArray bytes = f.read(kMaxIniBytes + 1);
    if (f.error() != QFileDevice::NoError)
        return Error(ErrorCode::IoError, QStringLiteral("DLC INI could not be read."), f.errorString());
    f.close();
    FW_TRY_ASSIGN(DlcIniDocument doc, parse(bytes, abs));
    doc.m_size = fi.size();
    doc.m_modified = fi.lastModified().toUTC();
    return doc;
}

Result<DlcIniDocument> DlcIniDocument::parse(const QByteArray& bytes, const QString& path)
{
    if (bytes.size() > kMaxIniBytes)
        return Error(ErrorCode::InvalidArgument, msg(kMsgTooLarge), path);

    DlcIniDocument doc;
    doc.m_path = path;
    doc.m_original = bytes;
    doc.m_size = bytes.size();

    const Detected det = detectBom(bytes);
    const QByteArrayView body = QByteArrayView(bytes).sliced(det.bomLength);
    Codec codec = det.codec;
    std::optional<QString> text = decode(codec, body);
    if (!text) {
        if (det.bomLength > 0)
            return Error(ErrorCode::CorruptData, msg(kMsgBadBomText), path);
        codec = Codec::Latin1;
        text = decode(codec, body);
        doc.m_warnings << msg(kWarnLatin1);
    }
    if (text->contains(QChar(u'\0')))
        return Error(ErrorCode::CorruptData, msg(kMsgNul), path);

    doc.m_codec = codecId(codec);
    doc.m_hasBom = det.bomLength > 0;
    doc.m_bom = bytes.left(det.bomLength);

    const std::vector<SplitLine> lines = splitLines(*text);
    doc.m_lines.reserve(lines.size());
    for (const SplitLine& l : lines)
        doc.m_lines.push_back({l.text, l.end});
    doc.m_entries = readEntries(lines);
    if (doc.m_entries.empty())
        doc.m_warnings << msg(kWarnNoEntries);
    return doc;
}

QString DlcIniDocument::encodingName() const
{
    return codecDisplayName(codecFromId(m_codec)) + (m_hasBom ? QStringLiteral(" (BOM)") : QStringLiteral(" (no BOM)"));
}

bool DlcIniDocument::sourceUnchanged() const
{
    if (m_path.isEmpty() || !m_modified.isValid())
        return false; // parsed from bytes: no file snapshot to compare with
    const QFileInfo fi(m_path);
    return fi.isFile() && fi.size() == m_size && fi.lastModified().toUTC() == m_modified;
}

Result<QByteArray> DlcIniDocument::updatedBytes(const std::map<int, DlcTarget>& changes) const
{
    std::map<int, QString> edits;   // line -> new text (without terminator)
    std::map<int, QString> inserts; // line -> text inserted before it (with terminator)

    auto setLine = [&](std::map<int, QString>& into, int line, const QString& text) -> Status {
        const auto it = into.find(line);
        if (it != into.end() && it->second != text)
            return Error(ErrorCode::InvalidArgument, msg(kMsgConflict));
        into[line] = text;
        return success();
    };

    auto editField = [&](int line, const QString& value) -> Status {
        if (hasBreakOrNul(value))
            return Error(ErrorCode::InvalidArgument, msg(kMsgValueBreaks));
        const Field f = parseLine(m_lines[size_t(line)].text);
        if (!f.hasKey)
            return Error(ErrorCode::Internal, msg(kMsgIdentityFailed), QStringLiteral("line %1 has no key").arg(line + 1));
        QString v = value;
        if (f.quoted) {
            if (value.contains(u'"'))
                return Error(ErrorCode::InvalidArgument, msg(kMsgQuotedQuote));
        } else if (needsQuotes(value)) {
            if (value.contains(u'"'))
                return Error(ErrorCode::InvalidArgument, msg(kMsgNeedsQuotes));
            v = u'"' + value + u'"';
        }
        // An empty value directly followed by an inline comment ("key = ; note"):
        // a comment only starts after white space, so keep one in front of it.
        const bool commentFollows = f.value.isEmpty() && !f.suffix.isEmpty() && (f.suffix.front() == u';' || f.suffix.front() == u'#');
        return setLine(edits, line, f.prefix + v + (commentFollows ? QStringLiteral(" ") : QString()) + f.suffix);
    };

    const int lineCount = int(m_lines.size());
    for (const auto& [index, target] : changes) {
        if (index < 0 || index >= int(m_entries.size()))
            return Error(ErrorCode::InvalidArgument, msg(kMsgUnknownIndex), QStringLiteral("changes"));
        FW_TRY(validateIdentity(target.titleId, target.contentId));
        const DlcIniEntry& e = m_entries[size_t(index)];
        if (isContentId(e.contentId) && e.contentId.mid(7, 4) != target.titleId.left(4))
            return Error(ErrorCode::InvalidArgument, msg(kMsgPlatform));
        if (e.contentLine < 0 || e.contentLine >= lineCount)
            return Error(ErrorCode::Internal, msg(kMsgIdentityFailed), QStringLiteral("content line out of range"));

        FW_TRY(editField(e.contentLine, target.contentId));
        bool titleEdited = false;
        const int begin = std::max(0, blockBegin(m_entries, size_t(index)));
        const int end = std::min(lineCount, e.endLine);
        for (int l = begin; l < end; ++l) {
            const Field f = parseLine(m_lines[size_t(l)].text);
            if (!f.hasKey)
                continue;
            if (isTitleIdKey(f.key)) {
                FW_TRY(editField(l, target.titleId));
            } else if (isPrimaryIdKey(f.key) && f.value == e.contentId) {
                FW_TRY(editField(l, target.contentId));
            } else if (isNameKey(f.key) && target.title && *target.title != e.title) {
                // Only an edited title is written: a block may hold several
                // name-like keys with different values.
                FW_TRY(editField(l, *target.title));
                titleEdited = true;
            }
        }

        if (!titleEdited && target.title && *target.title != e.title) {
            const QString& title = *target.title;
            if (hasBreakOrNul(title))
                return Error(ErrorCode::InvalidArgument, msg(kMsgTitleBreaks));
            const int tcl = e.titleCommentLine;
            const qsizetype marker = tcl >= 0 && tcl < lineCount ? commentMarkerLength(m_lines[size_t(tcl)].text) : -1;
            if (marker >= 0) {
                FW_TRY(setLine(edits, tcl, m_lines[size_t(tcl)].text.left(marker) + title));
            } else {
                const QString& lineEnd = m_lines[size_t(e.contentLine)].end;
                FW_TRY(setLine(inserts, e.contentLine,
                               QStringLiteral("# ") + title + (lineEnd.isEmpty() ? QStringLiteral("\r\n") : lineEnd)));
            }
        }
    }

    QString text;
    for (int i = 0; i < lineCount; ++i) {
        if (const auto ins = inserts.find(i); ins != inserts.end())
            text += ins->second;
        const auto ed = edits.find(i);
        text += ed != edits.end() ? ed->second : m_lines[size_t(i)].text;
        text += m_lines[size_t(i)].end;
    }

    const Codec codec = codecFromId(m_codec);
    const QByteArray body = encode(codec, text);
    const std::optional<QString> back = decode(codec, body);
    if (!back || *back != text)
        return Error(ErrorCode::InvalidArgument, msg(kMsgNotRepresentable));
    const QByteArray out = m_bom + body;

    auto reparsed = parse(out, m_path);
    if (!reparsed.ok())
        return Error(ErrorCode::VerificationFailed, msg(kMsgIdentityFailed), reparsed.error().message);
    const std::vector<DlcIniEntry>& now = reparsed->entries();
    if (now.size() != m_entries.size())
        return Error(ErrorCode::VerificationFailed, msg(kMsgIdentityFailed),
                     QStringLiteral("%1 entries after the edit, %2 before").arg(now.size()).arg(m_entries.size()));
    for (const auto& [index, target] : changes) {
        const DlcIniEntry& n = now[size_t(index)];
        if (n.contentId != target.contentId || n.titleId != target.titleId)
            return Error(ErrorCode::VerificationFailed, msg(kMsgIdentityFailed),
                         QStringLiteral("entry %1 reads back as %2 / %3").arg(index + 1).arg(n.titleId, n.contentId));
    }
    for (const auto& [index, target] : changes) {
        if (target.title && *target.title != m_entries[size_t(index)].title && now[size_t(index)].title != *target.title)
            return Error(ErrorCode::VerificationFailed, msg(kMsgTitleFailed),
                         QStringLiteral("entry %1 title reads back as \"%2\"").arg(index + 1).arg(now[size_t(index)].title));
    }
    return out;
}

Status DlcIniDocument::saveUpdated(const QString& outPath, const std::map<int, DlcTarget>& changes) const
{
    if (!m_path.isEmpty()) {
        if (samePath(outPath, m_path))
            return Error(ErrorCode::InvalidArgument, msg(kMsgOverwriteSource), outPath);
        QFile src(toLongPath(m_path));
        QByteArray now;
        bool readOk = src.open(QIODevice::ReadOnly);
        if (readOk) {
            now = src.read(kMaxIniBytes + 1);
            readOk = src.error() == QFileDevice::NoError;
        }
        if (!readOk || now != m_original)
            return Error(ErrorCode::VerificationFailed, msg(kMsgSourceChanged), m_path);
    }
    FW_TRY_ASSIGN(const QByteArray bytes, updatedBytes(changes));
    return writeNewFile(outPath, bytes);
}

Result<QByteArray> DlcIniDocument::newIniForPackage(const QString& title, const QString& contentId)
{
    QString t = title;
    t.replace(u'\r', u' ').replace(u'\n', u' ');
    const QByteArray bytes =
        encode(Codec::Utf8, QStringLiteral("#") + t + QStringLiteral("\r\ncontent_id=") + contentId + QStringLiteral("\r\n"));
    auto doc = parse(bytes);
    if (!doc.ok())
        return Error(ErrorCode::VerificationFailed, msg(kMsgGeneratedFailed), doc.error().message);
    if (doc->entries().size() != 1 || doc->entries().front().contentId != contentId)
        return Error(ErrorCode::VerificationFailed, msg(kMsgGeneratedFailed));
    return bytes;
}

// ---- files ---------------------------------------------------------------------------------

Status writeNewFile(const QString& path, const QByteArray& data)
{
    const QString name = QFileInfo(path).fileName();
    if (path.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No output file name was given."));
    if (QFileInfo::exists(path))
        return Error(ErrorCode::AlreadyExists,
                     QStringLiteral("%1 already exists. Existing outputs are never overwritten.").arg(name), path);
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (QFileInfo::exists(path))
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 already exists. Existing outputs are never overwritten.").arg(name), path);
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(name), f.errorString());
    }
    Status st = f.write(data) == data.size()
        ? flushToDisk(f, name)
        : Status(Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(name), f.errorString()));
    f.close();
    if (st.ok() && f.error() != QFileDevice::NoError)
        st = Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(name), f.errorString());
    if (!st.ok()) {
        QFile::remove(toLongPath(path));
        return st;
    }
    return success();
}

} // namespace fw::dlc
