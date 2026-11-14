// fw_onefile_pack: builds the single-file launcher from the launcher stub and a
// deployed program folder (see OnefileFormat.h), and inspects the result.
//
//   fw_onefile_pack pack --stub <fw_onefile_stub.exe> --input <folder>
//                        --entry <program path inside the folder> --output <file.exe>
//                        [--version <v>] [--method lzms|xpress-huff|xpress|mszip|store]
//                        [--chunk-mib <n>] [--jobs <n>]
//   fw_onefile_pack list <file.exe>
//   fw_onefile_pack verify <file.exe>
//
// Every file under --input is packed (sub-folders included). Chunks that do not
// shrink by at least 3 % are stored as they are, so already-compressed files
// cost no unpacking time.

#include "OnefileFormat.h"

#include <compressapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef FW_ONEFILE_VERSION
#define FW_ONEFILE_VERSION "0.0.0"
#endif

using namespace fw::onefile;

namespace {

void writeOut(HANDLE h, const std::wstring& text)
{
    DWORD mode = 0, n = 0;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, text.data(), DWORD(text.size()), &n, nullptr);
    } else {
        const std::string u = wideToUtf8(text);
        WriteFile(h, u.data(), DWORD(u.size()), &n, nullptr);
    }
}
void out(const std::wstring& text) { writeOut(GetStdHandle(STD_OUTPUT_HANDLE), text + L"\n"); }
int fail(const std::wstring& text)
{
    writeOut(GetStdHandle(STD_ERROR_HANDLE), L"fw_onefile_pack: " + text + L"\n");
    return 1;
}

std::wstring fullPath(const std::wstring& p)
{
    std::wstring buf(32768, L'\0');
    const DWORD n = GetFullPathNameW(p.c_str(), DWORD(buf.size()), buf.data(), nullptr);
    if (n == 0 || n >= buf.size())
        return p;
    buf.resize(n);
    while (buf.size() > 3 && (buf.back() == L'\\' || buf.back() == L'/'))
        buf.pop_back();
    return buf;
}

std::wstring mib(uint64_t bytes)
{
    wchar_t b[64];
    swprintf(b, 64, L"%.1f MiB", double(bytes) / (1024.0 * 1024.0));
    return b;
}

struct SourceFile {
    std::wstring abs;
    std::string rel; // UTF-8, '/'
    uint64_t size = 0;
    uint32_t firstChunk = 0;
    uint32_t chunkCount = 0;
};

bool walk(const std::wstring& root, const std::wstring& rel, std::vector<SourceFile>& outFiles, std::wstring& err)
{
    WIN32_FIND_DATAW fd{};
    const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    HANDLE h = FindFirstFileExW(extendedPath(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"cannot list " + dir + L": " + errorText(GetLastError());
        return false;
    }
    bool ok = true;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const std::wstring childRel = rel.empty() ? name : rel + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                err = L"folder link not supported: " + childRel;
                ok = false;
                break;
            }
            if (!walk(root, childRel, outFiles, err)) {
                ok = false;
                break;
            }
            continue;
        }
        SourceFile f;
        f.abs = root + L"\\" + childRel;
        std::wstring r = childRel;
        std::replace(r.begin(), r.end(), L'\\', L'/');
        f.rel = wideToUtf8(r);
        f.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        outFiles.push_back(std::move(f));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

struct Chunk {
    size_t file = 0;
    uint64_t fileOffset = 0;
    uint32_t raw = 0;
    std::vector<uint8_t> data; // stored bytes
    ChunkEntry entry{};
};

class Codec {
public:
    ~Codec()
    {
        for (auto& [m, h] : m_comp)
            CloseCompressor(h);
        for (auto& [m, h] : m_decomp)
            CloseDecompressor(h);
    }
    // Compressed bytes of `in` or false.
    bool compress(uint8_t method, const uint8_t* in, size_t size, std::vector<uint8_t>& outBuf)
    {
        COMPRESSOR_HANDLE& h = m_comp[method];
        if (!h && !CreateCompressor(method, nullptr, &h))
            return false;
        SIZE_T need = 0;
        if (!Compress(h, in, size, nullptr, 0, &need) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return false;
        outBuf.resize(need);
        SIZE_T got = 0;
        if (!Compress(h, in, size, outBuf.data(), outBuf.size(), &got))
            return false;
        outBuf.resize(got);
        return true;
    }
    bool decompress(uint8_t method, const uint8_t* in, size_t size, uint8_t* outBuf, size_t rawSize)
    {
        DECOMPRESSOR_HANDLE& h = m_decomp[method];
        if (!h && !CreateDecompressor(method, nullptr, &h))
            return false;
        SIZE_T got = 0;
        return Decompress(h, in, size, outBuf, rawSize, &got) && got == rawSize;
    }

private:
    std::map<uint8_t, COMPRESSOR_HANDLE> m_comp;
    std::map<uint8_t, DECOMPRESSOR_HANDLE> m_decomp;
};

bool readFileRange(const std::wstring& path, uint64_t offset, uint8_t* buf, size_t size)
{
    HANDLE h = CreateFileW(extendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = readAt(h, offset, buf, size);
    CloseHandle(h);
    return ok;
}

bool writeAll(HANDLE h, const void* data, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        DWORD n = 0;
        if (!WriteFile(h, p, DWORD(std::min<size_t>(size, 1u << 30)), &n, nullptr) || n == 0)
            return false;
        p += n;
        size -= n;
    }
    return true;
}

bool parseMethod(const std::wstring& s, uint8_t& m)
{
    if (s == L"lzms") m = kLzms;
    else if (s == L"xpress-huff") m = kXpressHuff;
    else if (s == L"xpress") m = kXpress;
    else if (s == L"mszip") m = kMszip;
    else if (s == L"store") m = kStore;
    else return false;
    return true;
}

int cmdPack(const std::map<std::wstring, std::wstring>& opt)
{
    auto get = [&](const wchar_t* k) {
        auto it = opt.find(k);
        return it == opt.end() ? std::wstring() : it->second;
    };
    const std::wstring stub = get(L"stub"), input = get(L"input"), output = get(L"output"), entryArg = get(L"entry");
    if (stub.empty() || input.empty() || output.empty() || entryArg.empty())
        return fail(L"pack needs --stub, --input, --entry and --output");
    const std::string version = opt.count(L"version") ? wideToUtf8(get(L"version")) : std::string(FW_ONEFILE_VERSION);
    if (!isValidVersion(version))
        return fail(L"invalid --version (1-31 of 0-9 A-Z a-z . _ + -)");
    uint8_t method = kLzms;
    if (opt.count(L"method") && !parseMethod(get(L"method"), method))
        return fail(L"unknown --method");
    uint32_t chunkRaw = kDefaultChunkRaw;
    if (opt.count(L"chunk-mib")) {
        const long v = wcstol(get(L"chunk-mib").c_str(), nullptr, 10);
        if (v < 1 || uint64_t(v) << 20 > kMaxChunkRaw)
            return fail(L"--chunk-mib must be 1..64");
        chunkRaw = uint32_t(v) << 20;
    }
    unsigned jobs = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    if (opt.count(L"jobs"))
        jobs = unsigned(std::max(1L, wcstol(get(L"jobs").c_str(), nullptr, 10)));

    const auto t0 = std::chrono::steady_clock::now();
    const std::wstring root = fullPath(input), outPath = fullPath(output), stubPath = fullPath(stub);
    std::vector<SourceFile> files;
    std::wstring err;
    if (!walk(root, L"", files, err))
        return fail(err);
    // Never pack the output (or its partial file) into itself.
    const std::wstring partial = outPath + L".partial";
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const SourceFile& f) {
                                   return CompareStringOrdinal(f.abs.c_str(), -1, outPath.c_str(), -1, TRUE) == CSTR_EQUAL ||
                                          CompareStringOrdinal(f.abs.c_str(), -1, partial.c_str(), -1, TRUE) == CSTR_EQUAL;
                               }),
                files.end());
    std::sort(files.begin(), files.end(), [](const SourceFile& a, const SourceFile& b) {
        const std::wstring wa = utf8ToWide(a.rel), wb = utf8ToWide(b.rel);
        return CompareStringOrdinal(wa.c_str(), -1, wb.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    if (files.empty())
        return fail(L"no files under " + root);
    if (files.size() > 1000000)
        return fail(L"too many files");

    std::wstring entryRel = entryArg;
    std::replace(entryRel.begin(), entryRel.end(), L'\\', L'/');
    uint32_t entryIndex = UINT32_MAX;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!isValidRelativePath(files[i].rel))
            return fail(L"unsupported file name: " + utf8ToWide(files[i].rel));
        const std::wstring w = utf8ToWide(files[i].rel);
        if (CompareStringOrdinal(w.c_str(), -1, entryRel.c_str(), -1, TRUE) == CSTR_EQUAL)
            entryIndex = uint32_t(i);
    }
    if (entryIndex == UINT32_MAX)
        return fail(L"--entry " + entryArg + L" is not in " + root);

    // Cut into chunks.
    std::vector<Chunk> chunks;
    uint64_t totalRaw = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        files[i].firstChunk = uint32_t(chunks.size());
        for (uint64_t off = 0; off < files[i].size; off += chunkRaw) {
            Chunk c;
            c.file = i;
            c.fileOffset = off;
            c.raw = uint32_t(std::min<uint64_t>(chunkRaw, files[i].size - off));
            chunks.push_back(std::move(c));
        }
        files[i].chunkCount = uint32_t(chunks.size()) - files[i].firstChunk;
        totalRaw += files[i].size;
    }

    // Compress in parallel.
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex errMutex;
    std::wstring workerErr;
    auto worker = [&] {
        Codec codec;
        std::vector<uint8_t> raw, packed, probe;
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= chunks.size() || failed)
                return;
            Chunk& c = chunks[i];
            raw.resize(c.raw);
            if (!readFileRange(files[c.file].abs, c.fileOffset, raw.data(), raw.size())) {
                std::lock_guard lock(errMutex);
                workerErr = L"cannot read " + files[c.file].abs + L": " + errorText(GetLastError());
                failed = true;
                return;
            }
            c.entry.rawSize = c.raw;
            c.entry.crc32 = crc32(raw.data(), raw.size());
            bool useStored = method == kStore;
            if (!useStored && method == kLzms) {
                // LZMS is slow to compress; a fast XPRESS pass spots data that does not compress.
                useStored = !codec.compress(kXpress, raw.data(), raw.size(), probe) || probe.size() >= raw.size() - raw.size() / 50;
            }
            if (!useStored) {
                if (codec.compress(method, raw.data(), raw.size(), packed) && packed.size() < raw.size() - raw.size() / 32) {
                    c.entry.method = method;
                    c.data = packed;
                } else {
                    useStored = true;
                }
            }
            if (useStored) {
                c.entry.method = kStore;
                c.data = raw;
            }
            c.entry.storedSize = uint32_t(c.data.size());
        }
    };
    {
        std::vector<std::thread> pool;
        for (unsigned j = 0; j < std::min<size_t>(jobs, std::max<size_t>(1, chunks.size())); ++j)
            pool.emplace_back(worker);
        for (auto& t : pool)
            t.join();
    }
    if (failed)
        return fail(workerErr);

    // Lay out the payload.
    std::vector<uint8_t> index;
    for (const SourceFile& f : files) {
        FileEntry fe{};
        fe.rawSize = f.size;
        fe.firstChunk = f.firstChunk;
        fe.chunkCount = f.chunkCount;
        fe.pathBytes = uint16_t(f.rel.size());
        const auto* p = reinterpret_cast<const uint8_t*>(&fe);
        index.insert(index.end(), p, p + sizeof(fe));
        index.insert(index.end(), f.rel.begin(), f.rel.end());
    }
    const size_t chunkTableAt = index.size();
    index.resize(index.size() + chunks.size() * sizeof(ChunkEntry));
    uint64_t dataOffset = sizeof(PayloadHeader) + index.size();
    for (size_t i = 0; i < chunks.size(); ++i) {
        chunks[i].entry.offset = dataOffset;
        dataOffset += chunks[i].entry.storedSize;
        std::memcpy(index.data() + chunkTableAt + i * sizeof(ChunkEntry), &chunks[i].entry, sizeof(ChunkEntry));
    }
    PayloadHeader header{};
    std::memcpy(header.magic, kPayloadMagic, sizeof(header.magic));
    header.fileCount = uint32_t(files.size());
    header.chunkCount = uint32_t(chunks.size());
    header.indexBytes = uint32_t(index.size());
    header.entryFile = entryIndex;
    header.totalRawBytes = totalRaw;

    // Stub first; it must not carry a payload already.
    HANDLE sh = CreateFileW(extendedPath(stubPath).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (sh == INVALID_HANDLE_VALUE)
        return fail(L"cannot open the stub " + stubPath + L": " + errorText(GetLastError()));
    LARGE_INTEGER stubSize{};
    GetFileSizeEx(sh, &stubSize);
    std::vector<uint8_t> stubBytes(size_t(stubSize.QuadPart));
    const bool stubRead = readAt(sh, 0, stubBytes.data(), stubBytes.size());
    Trailer existing{};
    std::string why;
    const bool alreadyPacked = readTrailer(sh, uint64_t(stubSize.QuadPart), existing, why);
    CloseHandle(sh);
    if (!stubRead || stubBytes.size() < 1024 || stubBytes[0] != 'M' || stubBytes[1] != 'Z')
        return fail(L"the stub is not an executable: " + stubPath);
    if (alreadyPacked)
        return fail(L"the stub already carries a payload: " + stubPath);

    HANDLE oh = CreateFileW(extendedPath(partial).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (oh == INVALID_HANDLE_VALUE)
        return fail(L"cannot create " + partial + L": " + errorText(GetLastError()));
    Sha256 sha;
    bool ok = sha.ok() && writeAll(oh, stubBytes.data(), stubBytes.size());
    const uint64_t payloadOffset = (stubBytes.size() + 15) & ~uint64_t(15);
    const uint8_t zeros[16] = {};
    ok = ok && writeAll(oh, zeros, size_t(payloadOffset - stubBytes.size()));
    auto emit = [&](const void* p, size_t n) { return writeAll(oh, p, n) && sha.update(p, n); };
    ok = ok && emit(&header, sizeof(header)) && emit(index.data(), index.size());
    for (const Chunk& c : chunks)
        ok = ok && emit(c.data.data(), c.data.size());
    Trailer t{};
    std::memcpy(t.magic, kTrailerMagic, sizeof(t.magic));
    t.formatVersion = kFormatVersion;
    t.trailerSize = sizeof(Trailer);
    t.payloadOffset = payloadOffset;
    t.payloadSize = dataOffset;
    ok = ok && sha.finish(t.payloadSha256);
    std::memcpy(t.version, version.data(), version.size());
    t.crc32 = trailerCrc(t);
    ok = ok && writeAll(oh, &t, sizeof(t));
    ok = FlushFileBuffers(oh) && ok;
    const DWORD writeError = GetLastError(); // before CloseHandle and DeleteFileW replace it
    CloseHandle(oh);
    if (!ok) {
        DeleteFileW(extendedPath(partial).c_str());
        return fail(L"writing " + partial + L" failed: " + errorText(writeError));
    }
    if (!MoveFileExW(extendedPath(partial).c_str(), extendedPath(outPath).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = GetLastError();
        DeleteFileW(extendedPath(partial).c_str());
        return fail(L"cannot replace " + outPath + L": " + errorText(e));
    }

    size_t stored = 0;
    for (const Chunk& c : chunks)
        stored += c.entry.method == kStore ? 1 : 0;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    wchar_t line[512];
    swprintf(line, 512, L"packed %zu files, %ls -> payload %ls (%zu of %zu chunks stored), %ls total, %.1f s",
             files.size(), mib(totalRaw).c_str(), mib(dataOffset).c_str(), stored, chunks.size(),
             mib(payloadOffset + dataOffset + sizeof(Trailer)).c_str(), secs);
    out(line);
    out(L"version " + utf8ToWide(version) + L", sha256 " + utf8ToWide(toHex(t.payloadSha256, 32)) + L", entry " +
        utf8ToWide(files[entryIndex].rel));
    out(L"wrote " + outPath);
    return 0;
}

struct Opened {
    HANDLE h = INVALID_HANDLE_VALUE;
    Trailer t{};
    Index idx;
    ~Opened()
    {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};

bool openPacked(const std::wstring& path, Opened& o, std::wstring& err)
{
    o.h = CreateFileW(extendedPath(fullPath(path)).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (o.h == INVALID_HANDLE_VALUE) {
        err = L"cannot open " + path + L": " + errorText(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(o.h, &size);
    std::string why;
    if (!readTrailer(o.h, uint64_t(size.QuadPart), o.t, why) || !readIndex(o.h, o.t, o.idx, why)) {
        err = path + L": " + utf8ToWide(why);
        return false;
    }
    return true;
}

int cmdList(const std::wstring& path)
{
    Opened o;
    std::wstring err;
    if (!openPacked(path, o, err))
        return fail(err);
    out(L"version " + utf8ToWide(o.t.version) + L"\nsha256 " + utf8ToWide(toHex(o.t.payloadSha256, 32)) + L"\nentry " +
        utf8ToWide(o.idx.files[o.idx.entryFile].path) + L"\npayload " + mib(o.t.payloadSize) + L" at offset " +
        std::to_wstring(o.t.payloadOffset) + L"\nfiles " + std::to_wstring(o.idx.files.size()) + L", " +
        mib(o.idx.totalRawBytes));
    for (const File& f : o.idx.files) {
        uint64_t stored = 0;
        uint8_t m = kStore;
        for (uint32_t c = 0; c < f.chunkCount; ++c) {
            stored += o.idx.chunks[f.firstChunk + c].storedSize;
            m = std::max(m, o.idx.chunks[f.firstChunk + c].method);
        }
        wchar_t line[128];
        swprintf(line, 128, L"%12llu %12llu %-11hs ", (unsigned long long)f.rawSize, (unsigned long long)stored, methodName(m));
        out(line + utf8ToWide(f.path));
    }
    return 0;
}

int cmdVerify(const std::wstring& path)
{
    Opened o;
    std::wstring err;
    if (!openPacked(path, o, err))
        return fail(err);
    Sha256 sha;
    std::vector<uint8_t> buf(4u << 20);
    for (uint64_t done = 0; done < o.t.payloadSize;) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), o.t.payloadSize - done));
        if (!readAt(o.h, o.t.payloadOffset + done, buf.data(), n) || !sha.update(buf.data(), n))
            return fail(L"read failed");
        done += n;
    }
    uint8_t digest[32];
    if (!sha.finish(digest) || std::memcmp(digest, o.t.payloadSha256, 32) != 0)
        return fail(L"SHA-256 mismatch: the payload is damaged");
    Codec codec;
    std::vector<uint8_t> in, raw;
    for (size_t i = 0; i < o.idx.chunks.size(); ++i) {
        const ChunkEntry& c = o.idx.chunks[i];
        in.resize(c.storedSize);
        raw.resize(c.rawSize);
        if (!readAt(o.h, o.t.payloadOffset + c.offset, in.data(), in.size()))
            return fail(L"read failed");
        if (c.method == kStore)
            raw = in;
        else if (!codec.decompress(c.method, in.data(), in.size(), raw.data(), raw.size()))
            return fail(L"chunk " + std::to_wstring(i) + L" does not decompress");
        if (crc32(raw.data(), raw.size()) != c.crc32)
            return fail(L"chunk " + std::to_wstring(i) + L" has the wrong CRC-32");
    }
    out(L"OK: " + std::to_wstring(o.idx.files.size()) + L" files, " + mib(o.idx.totalRawBytes) + L", sha256 " +
        utf8ToWide(toHex(digest, 32)));
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return fail(L"usage: fw_onefile_pack pack|list|verify ... (see pack_main.cpp)");
    const std::wstring cmd = argv[1];
    if (cmd == L"list" || cmd == L"verify") {
        if (argc != 3)
            return fail(L"usage: fw_onefile_pack " + cmd + L" <packed exe>");
        return cmd == L"list" ? cmdList(argv[2]) : cmdVerify(argv[2]);
    }
    if (cmd != L"pack")
        return fail(L"unknown command " + cmd);
    std::map<std::wstring, std::wstring> opt;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a.rfind(L"--", 0) != 0 || i + 1 >= argc)
            return fail(L"bad argument " + a);
        opt[a.substr(2)] = argv[++i];
    }
    return cmdPack(opt);
}
