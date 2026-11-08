#include "OnefileFormat.h"

#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace fw::onefile {

bool isKnownMethod(uint8_t method)
{
    return method == kStore || method == kMszip || method == kXpress || method == kXpressHuff || method == kLzms;
}

const char* methodName(uint8_t method)
{
    switch (method) {
    case kStore: return "store";
    case kMszip: return "mszip";
    case kXpress: return "xpress";
    case kXpressHuff: return "xpress-huff";
    case kLzms: return "lzms";
    default: return "?";
    }
}

// --- CRC-32 (IEEE, reflected), slice-by-8 -------------------------------------
namespace {
struct CrcTables {
    uint32_t t[8][256];
    CrcTables()
    {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s)
                t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};
const CrcTables& crcTables()
{
    static const CrcTables tables;
    return tables;
}
} // namespace

uint32_t crc32(const void* data, size_t size, uint32_t crc)
{
    const auto& T = crcTables().t;
    const auto* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    while (size >= 8) {
        uint32_t a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = T[7][a & 0xFF] ^ T[6][(a >> 8) & 0xFF] ^ T[5][(a >> 16) & 0xFF] ^ T[4][a >> 24] ^
              T[3][b & 0xFF] ^ T[2][(b >> 8) & 0xFF] ^ T[1][(b >> 16) & 0xFF] ^ T[0][b >> 24];
        p += 8;
        size -= 8;
    }
    while (size--)
        crc = T[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// --- SHA-256 (CNG) ------------------------------------------------------------
Sha256::Sha256()
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return;
    }
    m_alg = alg;
    m_hash = hash;
}

Sha256::~Sha256()
{
    if (m_hash)
        BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash));
    if (m_alg)
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(m_alg), 0);
}

bool Sha256::update(const void* data, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        const ULONG n = ULONG(std::min<size_t>(size, 1u << 30));
        if (BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(m_hash), const_cast<PUCHAR>(p), n, 0) != 0)
            return false;
        p += n;
        size -= n;
    }
    return true;
}

bool Sha256::finish(uint8_t out[32])
{
    return BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash), out, 32, 0) == 0;
}

// --- strings and files --------------------------------------------------------
std::string toHex(const uint8_t* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 15]);
    }
    return s;
}

std::wstring utf8ToWide(std::string_view s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string wideToUtf8(std::wstring_view s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string u(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), u.data(), n, nullptr, nullptr);
    return u;
}

std::wstring extendedPath(const std::wstring& p)
{
    if (p.rfind(L"\\\\?\\", 0) == 0 || p.rfind(L"\\\\.\\", 0) == 0)
        return p;
    if (p.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::wstring errorText(DWORD code)
{
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = n && buf ? std::wstring(buf, n) : std::wstring();
    if (buf)
        LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.'))
        s.pop_back();
    if (s.empty())
        s = L"error";
    return s + L" (" + std::to_wstring(code) + L")";
}

bool readAt(HANDLE file, uint64_t offset, void* buffer, size_t size)
{
    auto* p = static_cast<uint8_t*>(buffer);
    while (size > 0) {
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = DWORD(offset >> 32);
        const DWORD want = DWORD(std::min<size_t>(size, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(file, p, want, &got, &ov) || got == 0)
            return false;
        p += got;
        offset += got;
        size -= got;
    }
    return true;
}

// --- validation ---------------------------------------------------------------
bool isValidVersion(std::string_view v)
{
    if (v.empty() || v.size() > 31)
        return false;
    for (const char c : v) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
                        c == '_' || c == '+' || c == '-';
        if (!ok)
            return false;
    }
    return v != "." && v != "..";
}

int compareVersions(std::string_view a, std::string_view b)
{
    auto next = [](std::string_view& s) {
        const size_t i = s.find_first_of(".-+_");
        std::string_view part = s.substr(0, i);
        s = i == std::string_view::npos ? std::string_view() : s.substr(i + 1);
        return part;
    };
    auto numeric = [](std::string_view s) {
        return !s.empty() && s.size() <= 18 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    while (!a.empty() || !b.empty()) {
        const std::string_view pa = next(a), pb = next(b);
        if (numeric(pa) && numeric(pb)) {
            const unsigned long long na = std::stoull(std::string(pa)), nb = std::stoull(std::string(pb));
            if (na != nb)
                return na < nb ? -1 : 1;
        } else if (pa.empty() != pb.empty()) {
            // "5.0" == "5.0.0"; "5.0.0" > "5.0.0-rc" is not modelled: rare and harmless.
            const std::string_view rest = pa.empty() ? pb : pa;
            if (!(numeric(rest) && std::stoull(std::string(rest)) == 0))
                return pa.empty() ? -1 : 1;
        } else if (const int c = pa.compare(pb); c != 0) {
            return c < 0 ? -1 : 1;
        }
    }
    return 0;
}

bool isValidRelativePath(std::string_view p)
{
    if (p.empty() || p.size() > 4096 || p.front() == '/' || p.back() == '/')
        return false;
    size_t start = 0;
    while (start <= p.size()) {
        const size_t end = std::min(p.find('/', start), p.size());
        const std::string_view part = p.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.size() > 255)
            return false;
        if (part.back() == ' ' || part.back() == '.')
            return false; // Windows would silently drop it
        for (const char c : part) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                return false;
        }
        start = end + 1;
    }
    return true;
}

uint32_t trailerCrc(const Trailer& t)
{
    return crc32(&t, offsetof(Trailer, crc32));
}

bool readTrailer(HANDLE file, uint64_t fileSize, Trailer& out, std::string& why)
{
    // The payload ends where an Authenticode certificate table begins, if any.
    uint64_t end = fileSize;
    IMAGE_DOS_HEADER dos{};
    if (fileSize > sizeof(dos) && readAt(file, 0, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        dos.e_lfanew > 0 && uint64_t(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) <= fileSize) {
        IMAGE_NT_HEADERS64 nt{};
        if (readAt(file, uint64_t(dos.e_lfanew), &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE &&
            nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_SECURITY) {
            const auto& sec = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
            if (sec.VirtualAddress != 0 && sec.Size != 0 && uint64_t(sec.VirtualAddress) + sec.Size <= fileSize)
                end = sec.VirtualAddress; // a file offset for this directory
        }
    }
    for (uint64_t pad = 0; pad < 16; ++pad) {
        if (end < sizeof(Trailer) + pad)
            break;
        Trailer t{};
        if (!readAt(file, end - sizeof(Trailer) - pad, &t, sizeof(t)))
            break;
        if (std::memcmp(t.magic, kTrailerMagic, sizeof(kTrailerMagic)) != 0)
            continue;
        if (t.crc32 != trailerCrc(t)) {
            why = "the trailer checksum does not match";
            return false;
        }
        if (t.formatVersion != kFormatVersion || t.trailerSize != sizeof(Trailer)) {
            why = "unsupported format version " + std::to_string(t.formatVersion);
            return false;
        }
        const uint64_t trailerAt = end - sizeof(Trailer) - pad;
        if (t.payloadOffset > trailerAt || t.payloadSize != trailerAt - t.payloadOffset ||
            t.payloadSize < sizeof(PayloadHeader)) {
            why = "the payload size does not match the file size";
            return false;
        }
        if (t.version[sizeof(t.version) - 1] != 0 || !isValidVersion(t.version)) {
            why = "the version string is invalid";
            return false;
        }
        out = t;
        return true;
    }
    why = "no program is attached to this launcher";
    return false;
}

bool readIndex(HANDLE file, const Trailer& t, Index& out, std::string& why)
{
    PayloadHeader h{};
    if (!readAt(file, t.payloadOffset, &h, sizeof(h)) || std::memcmp(h.magic, kPayloadMagic, sizeof(kPayloadMagic)) != 0) {
        why = "the payload header is missing";
        return false;
    }
    if (h.fileCount == 0 || h.fileCount > 1000000 || h.chunkCount > 10000000 || h.entryFile >= h.fileCount ||
        h.indexBytes > (256u << 20) || uint64_t(h.indexBytes) > t.payloadSize - sizeof(h)) {
        why = "the payload header is invalid";
        return false;
    }
    std::vector<uint8_t> buf(h.indexBytes);
    if (!buf.empty() && !readAt(file, t.payloadOffset + sizeof(h), buf.data(), buf.size())) {
        why = "the payload index cannot be read";
        return false;
    }
    const uint64_t dataStart = sizeof(h) + uint64_t(h.indexBytes);
    size_t pos = 0;
    Index idx;
    idx.entryFile = h.entryFile;
    idx.totalRawBytes = h.totalRawBytes;
    idx.files.reserve(h.fileCount);
    for (uint32_t i = 0; i < h.fileCount; ++i) {
        FileEntry fe{};
        if (buf.size() - pos < sizeof(fe)) {
            why = "the payload index is truncated";
            return false;
        }
        std::memcpy(&fe, buf.data() + pos, sizeof(fe));
        pos += sizeof(fe);
        if (buf.size() - pos < fe.pathBytes) {
            why = "the payload index is truncated";
            return false;
        }
        File f;
        f.path.assign(reinterpret_cast<const char*>(buf.data() + pos), fe.pathBytes);
        pos += fe.pathBytes;
        f.rawSize = fe.rawSize;
        f.firstChunk = fe.firstChunk;
        f.chunkCount = fe.chunkCount;
        if (!isValidRelativePath(f.path) || utf8ToWide(f.path).empty()) {
            why = "the payload holds an invalid file name";
            return false;
        }
        idx.files.push_back(std::move(f));
    }
    if (buf.size() - pos != uint64_t(h.chunkCount) * sizeof(ChunkEntry)) {
        why = "the payload chunk table has the wrong size";
        return false;
    }
    idx.chunks.resize(h.chunkCount);
    if (h.chunkCount)
        std::memcpy(idx.chunks.data(), buf.data() + pos, size_t(h.chunkCount) * sizeof(ChunkEntry));
    for (const ChunkEntry& c : idx.chunks) {
        if (!isKnownMethod(c.method) || c.rawSize == 0 || c.rawSize > kMaxChunkRaw || c.storedSize == 0 ||
            (c.method == kStore && c.storedSize != c.rawSize) || c.offset < dataStart || c.offset > t.payloadSize ||
            c.storedSize > t.payloadSize - c.offset) {
            why = "the payload chunk table is invalid";
            return false;
        }
    }
    uint64_t total = 0;
    uint32_t nextChunk = 0;
    for (const File& f : idx.files) {
        if (f.firstChunk != nextChunk || f.chunkCount > h.chunkCount - nextChunk) {
            why = "the payload chunk table is invalid";
            return false;
        }
        uint64_t sum = 0;
        for (uint32_t c = 0; c < f.chunkCount; ++c)
            sum += idx.chunks[f.firstChunk + c].rawSize;
        if (sum != f.rawSize) {
            why = "the payload chunk table is invalid";
            return false;
        }
        nextChunk += f.chunkCount;
        total += f.rawSize;
    }
    if (nextChunk != h.chunkCount || total != h.totalRawBytes) {
        why = "the payload sizes do not add up";
        return false;
    }
    out = std::move(idx);
    return true;
}

} // namespace fw::onefile
