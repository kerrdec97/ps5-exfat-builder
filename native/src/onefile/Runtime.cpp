#include "Runtime.h"

#include <compressapi.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace fw::onefile {

namespace {

constexpr wchar_t kProduct[] = L"exFAT Image Builder";
constexpr wchar_t kVendor[] = L"kerrdec97";
constexpr wchar_t kLockName[] = L".onefile-lock";
constexpr ULONGLONG kTmpMinAgeMs = 10ull * 60 * 1000; // a younger .tmp- folder may still be unpacking

std::wstring env(const wchar_t* name)
{
    std::wstring v(32768, L'\0');
    const DWORD n = GetEnvironmentVariableW(name, v.data(), DWORD(v.size()));
    if (n == 0 || n >= v.size())
        return {};
    v.resize(n);
    return v;
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

std::wstring nativeRel(const std::string& rel)
{
    std::wstring w = utf8ToWide(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

std::wstring uniqueSuffix()
{
    static std::atomic<unsigned> counter{0};
    wchar_t b[64];
    swprintf(b, 64, L"%lu-%llx-%u", GetCurrentProcessId(), (unsigned long long)GetTickCount64(), counter.fetch_add(1));
    return b;
}

std::string markerText(const Payload& p)
{
    return "fw-onefile 1\nversion " + p.version + "\nsha256 " + p.sha + "\nfiles " + std::to_string(p.index.files.size()) +
           "\nbytes " + std::to_string(p.index.totalRawBytes) + "\n";
}

bool makeDirs(const std::wstring& dir)
{
    if (CreateDirectoryW(extendedPath(dir).c_str(), nullptr))
        return true;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS) {
        const DWORD a = GetFileAttributesW(extendedPath(dir).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }
    if (e != ERROR_PATH_NOT_FOUND)
        return false;
    const size_t cut = dir.find_last_of(L'\\');
    if (cut == std::wstring::npos || cut < 3)
        return false;
    if (!makeDirs(dir.substr(0, cut)))
        return false;
    if (CreateDirectoryW(extendedPath(dir).c_str(), nullptr))
        return true;
    e = GetLastError();
    return e == ERROR_ALREADY_EXISTS;
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

// Renames a folder, retrying while a scanner briefly holds a file in it.
DWORD renameFolder(const std::wstring& from, const std::wstring& to)
{
    DWORD e = 0;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (MoveFileExW(extendedPath(from).c_str(), extendedPath(to).c_str(), 0))
            return 0;
        e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION)
            return e;
        if (fileExists(to))
            return ERROR_ALREADY_EXISTS;
        Sleep(attempt < 10 ? 20 : 100);
    }
    return e;
}

struct Extractor {
    const Payload& p;
    const std::wstring& dir;
    Progress& progress;
    std::vector<uint32_t> order;
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex errMutex;
    std::wstring err;

    void setError(const std::wstring& e)
    {
        std::lock_guard lock(errMutex);
        if (err.empty())
            err = e;
        failed = true;
    }

    void work()
    {
        HANDLE self = CreateFileW(extendedPath(p.exePath).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (self == INVALID_HANDLE_VALUE) {
            setError(L"Cannot read " + p.exePath + L": " + errorText(GetLastError()));
            return;
        }
        DECOMPRESSOR_HANDLE codecs[8] = {};
        std::vector<uint8_t> in, out;
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= order.size() || failed || progress.cancel)
                break;
            const File& f = p.index.files[order[i]];
            const std::wstring path = dir + L"\\" + nativeRel(f.path);
            HANDLE h = CreateFileW(extendedPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                setError(L"Cannot create " + path + L": " + errorText(GetLastError()));
                break;
            }
            if (f.rawSize > 0) {
                FILE_ALLOCATION_INFO ai{};
                ai.AllocationSize.QuadPart = LONGLONG(f.rawSize);
                SetFileInformationByHandle(h, FileAllocationInfo, &ai, sizeof(ai));
            }
            bool ok = true;
            for (uint32_t c = 0; ok && c < f.chunkCount; ++c) {
                const ChunkEntry& ch = p.index.chunks[f.firstChunk + c];
                in.resize(ch.storedSize);
                if (!readAt(self, p.trailer.payloadOffset + ch.offset, in.data(), in.size())) {
                    setError(L"Cannot read " + p.exePath + L": " + errorText(GetLastError()));
                    ok = false;
                    break;
                }
                const uint8_t* data = in.data();
                if (ch.method != kStore) {
                    out.resize(ch.rawSize);
                    DECOMPRESSOR_HANDLE& d = codecs[ch.method];
                    SIZE_T got = 0;
                    if ((!d && !CreateDecompressor(ch.method, nullptr, &d)) ||
                        !Decompress(d, in.data(), in.size(), out.data(), out.size(), &got) || got != ch.rawSize) {
                        setError(L"The built-in copy of " + utf8ToWide(f.path) + L" is damaged.");
                        ok = false;
                        break;
                    }
                    data = out.data();
                }
                if (crc32(data, ch.rawSize) != ch.crc32) {
                    setError(L"The built-in copy of " + utf8ToWide(f.path) + L" is damaged.");
                    ok = false;
                    break;
                }
                if (!writeAll(h, data, ch.rawSize)) {
                    setError(L"Cannot write " + path + L": " + errorText(GetLastError()));
                    ok = false;
                    break;
                }
                progress.done += ch.rawSize;
                if (progress.cancel)
                    ok = false;
            }
            // On the disk before the marker says "complete": after a power cut
            // a folder with a marker must not hold files whose data never got there.
            if (ok && !FlushFileBuffers(h)) {
                setError(L"Cannot write " + path + L": " + errorText(GetLastError()));
                ok = false;
            }
            CloseHandle(h);
            if (!ok)
                break;
        }
        for (DECOMPRESSOR_HANDLE d : codecs)
            if (d)
                CloseDecompressor(d);
        CloseHandle(self);
    }
};

} // namespace

bool fileExists(const std::wstring& path)
{
    return GetFileAttributesW(extendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool openPayload(Payload& p, std::wstring& err)
{
    std::wstring self(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, self.data(), DWORD(self.size()));
    if (n == 0 || n >= self.size()) {
        err = L"Cannot find the launcher's own file.";
        return false;
    }
    self.resize(n);
    if (self.rfind(L"\\\\?\\", 0) == 0 && self.rfind(L"\\\\?\\UNC\\", 0) != 0)
        self = self.substr(4);
    p.exePath = self;
    p.exeDir = self.substr(0, self.find_last_of(L'\\'));
    if (p.exeDir.size() == 2) // "C:"
        p.exeDir += L'\\';
    p.file = CreateFileW(extendedPath(self).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (p.file == INVALID_HANDLE_VALUE) {
        err = L"Cannot read " + self + L": " + errorText(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(p.file, &size);
    p.fileSize = uint64_t(size.QuadPart);
    std::string why;
    if (!readTrailer(p.file, p.fileSize, p.trailer, why) || !readIndex(p.file, p.trailer, p.index, why)) {
        err = utf8ToWide(why);
        return false;
    }
    p.version = p.trailer.version;
    p.sha = toHex(p.trailer.payloadSha256, 32);
    p.folderName = utf8ToWide(p.version) + L"-" + utf8ToWide(p.sha.substr(0, 12));
    return true;
}

bool longPathsEnabled()
{
    wchar_t b[8] = {};
    const DWORD n = GetEnvironmentVariableW(L"FW_ONEFILE_SHORT_PATHS", b, 8);
    if (n > 0 && n < 8 && b[0] != L'0')
        return false;
    using Fn = BOOLEAN(NTAPI*)();
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto fn = ntdll ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlAreLongPathsEnabled"))) : nullptr;
    return fn && fn() != 0; // older than Windows 10 1607: never
}

size_t longestRuntimePath(const Payload& p)
{
    size_t longest = 0;
    for (const File& f : p.index.files)
        longest = std::max(longest, nativeRel(f.path).size());
    return 1 + p.folderName.size() + 1 + longest; // "\<folder>\<file>"
}

std::vector<std::wstring> runtimeRoots(const Payload& p, bool portable, TraceFn trace)
{
    std::vector<std::wstring> roots;
    const std::wstring tail = std::wstring(L"\\") + kVendor + L"\\" + kProduct + L"\\runtime";
    if (portable) {
        std::wstring base = p.exeDir;
        if (base.back() == L'\\')
            base.pop_back();
        roots.push_back(base + L"\\" + kProduct + L".data\\runtime");
    }
    std::wstring local = env(L"LOCALAPPDATA");
    if (local.empty()) {
        const std::wstring profile = env(L"USERPROFILE");
        if (!profile.empty())
            local = profile + L"\\AppData\\Local";
    }
    if (!local.empty())
        roots.push_back(fullPath(local) + tail);
    wchar_t tmp[MAX_PATH + 2] = {};
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (n > 0 && n <= MAX_PATH) {
        std::wstring t = fullPath(std::wstring(tmp, n));
        roots.push_back(t + tail);
    }
    // Drop duplicates (e.g. TEMP inside LOCALAPPDATA is fine, identical roots are not).
    std::vector<std::wstring> unique;
    for (const auto& r : roots) {
        const bool dup = std::any_of(unique.begin(), unique.end(), [&](const std::wstring& u) {
            return CompareStringOrdinal(u.c_str(), -1, r.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (!dup)
            unique.push_back(r);
    }
    // Unpacking works at any depth (\\?\ paths), starting the program and
    // loading its DLLs and plug-ins does not without long path support.
    if (longPathsEnabled())
        return unique;
    const size_t need = longestRuntimePath(p);
    std::vector<std::wstring> fit;
    for (const auto& r : unique) {
        if (r.size() + need < MAX_PATH)
            fit.push_back(r);
        else if (trace)
            trace(L"skipping " + r + L": too deep for paths of " + std::to_wstring(MAX_PATH) +
                  L" characters (long paths are off in Windows)");
    }
    return fit.empty() ? unique : fit;
}

HANDLE openIfComplete(const std::wstring& dir, const Payload& p)
{
    HANDLE h = CreateFileW(extendedPath(dir + L"\\" + kMarkerName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;
    const std::string expected = markerText(p);
    std::string got(expected.size() + 1, '\0');
    DWORD n = 0;
    const bool read = ReadFile(h, got.data(), DWORD(got.size()), &n, nullptr);
    got.resize(n);
    // Every file was written before the marker. One written later has been
    // changed since it was unpacked (a size check alone misses a DLL that was
    // altered in place), so the copy is not used.
    FILETIME markerWritten{};
    bool ok = read && got == expected && GetFileTime(h, nullptr, nullptr, &markerWritten);
    for (size_t i = 0; ok && i < p.index.files.size(); ++i) {
        const File& f = p.index.files[i];
        WIN32_FILE_ATTRIBUTE_DATA a{};
        ok = GetFileAttributesExW(extendedPath(dir + L"\\" + nativeRel(f.path)).c_str(), GetFileExInfoStandard, &a) &&
             !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
             ((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow) == f.rawSize &&
             CompareFileTime(&a.ftLastWriteTime, &markerWritten) <= 0;
    }
    if (!ok) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

bool prepareRoot(const std::wstring& root)
{
    if (!makeDirs(root))
        return false;
    HANDLE h = CreateFileW(extendedPath(root + L"\\.probe-" + uniqueSuffix()).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

Check verifyPayload(const Payload& p, Progress& progress)
{
    // The caller shows GetLastError() for a ReadError. The hash functions
    // report an NTSTATUS instead and leave whatever error was set before.
    const auto hashFailed = [] {
        SetLastError(ERROR_INTERNAL_ERROR);
        return Check::ReadError;
    };
    Sha256 sha;
    if (!sha.ok())
        return hashFailed();
    std::vector<uint8_t> buf(4u << 20);
    for (uint64_t done = 0; done < p.trailer.payloadSize;) {
        if (progress.cancel)
            return Check::Cancelled;
        const size_t n = size_t(std::min<uint64_t>(buf.size(), p.trailer.payloadSize - done));
        if (!readAt(p.file, p.trailer.payloadOffset + done, buf.data(), n))
            return Check::ReadError;
        if (!sha.update(buf.data(), n))
            return hashFailed();
        done += n;
        progress.done += n;
    }
    uint8_t digest[32];
    if (!sha.finish(digest))
        return hashFailed();
    return std::memcmp(digest, p.trailer.payloadSha256, 32) == 0 ? Check::Ok : Check::Mismatch;
}

Install install(const Payload& p, const std::wstring& root, Progress& progress, std::wstring& dir, HANDLE& marker,
                std::wstring& err, TraceFn trace)
{
    marker = INVALID_HANDLE_VALUE;
    const std::wstring tmp = root + L"\\.tmp-" + p.folderName + L"-" + uniqueSuffix();
    if (!CreateDirectoryW(extendedPath(tmp).c_str(), nullptr)) {
        err = L"Cannot create " + tmp + L": " + errorText(GetLastError());
        return Install::Failed;
    }
    // Held while unpacking: another launcher cannot rename (remove) the folder.
    HANDLE lock = CreateFileW(extendedPath(tmp + L"\\" + kLockName).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    auto abandon = [&](Install result) {
        if (lock != INVALID_HANDLE_VALUE)
            CloseHandle(lock);
        removeTree(tmp);
        return result;
    };
    if (lock == INVALID_HANDLE_VALUE) {
        err = L"Cannot write to " + tmp + L": " + errorText(GetLastError());
        return abandon(Install::Failed);
    }

    // Folders first (in order), then the files in parallel, largest first.
    std::set<std::wstring> dirs;
    for (const File& f : p.index.files) {
        const std::wstring rel = nativeRel(f.path);
        for (size_t cut = rel.find(L'\\'); cut != std::wstring::npos; cut = rel.find(L'\\', cut + 1))
            dirs.insert(rel.substr(0, cut));
    }
    for (const std::wstring& d : dirs) { // std::set order puts parents before children
        if (!CreateDirectoryW(extendedPath(tmp + L"\\" + d).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            err = L"Cannot create " + tmp + L"\\" + d + L": " + errorText(GetLastError());
            return abandon(Install::Failed);
        }
    }
    Extractor ex{p, tmp, progress, {}, {}, {}, {}, {}};
    for (uint32_t i = 0; i < p.index.files.size(); ++i)
        ex.order.push_back(i);
    std::stable_sort(ex.order.begin(), ex.order.end(),
                     [&](uint32_t a, uint32_t b) { return p.index.files[a].rawSize > p.index.files[b].rawSize; });
    const unsigned threads = std::max(1u, std::min({8u, std::thread::hardware_concurrency(), unsigned(ex.order.size())}));
    {
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t)
            pool.emplace_back([&ex] { ex.work(); });
        ex.work();
        for (auto& t : pool)
            t.join();
    }
    if (progress.cancel)
        return abandon(Install::Cancelled);
    if (ex.failed) {
        err = ex.err;
        return abandon(Install::Failed);
    }

    // Check what is on disk, then write the marker last.
    HANDLE m = INVALID_HANDLE_VALUE;
    {
        const std::string text = markerText(p);
        m = CreateFileW(extendedPath(tmp + L"\\" + kMarkerName).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        const bool ok = m != INVALID_HANDLE_VALUE && writeAll(m, text.data(), text.size()) && FlushFileBuffers(m);
        if (m != INVALID_HANDLE_VALUE)
            CloseHandle(m);
        if (!ok) {
            err = L"Cannot write to " + tmp + L": " + errorText(GetLastError());
            return abandon(Install::Failed);
        }
    }
    m = openIfComplete(tmp, p);
    if (m == INVALID_HANDLE_VALUE) {
        err = L"The unpacked files in " + tmp + L" do not match (were they removed by security software?).";
        return abandon(Install::Failed);
    }
    CloseHandle(m);
    CloseHandle(lock);
    lock = INVALID_HANDLE_VALUE;

    // Atomic move into place. Another launcher may have finished first.
    const std::wstring final = root + L"\\" + p.folderName;
    for (int round = 0; round < 3; ++round) {
        const DWORD e = renameFolder(tmp, final);
        if (e == 0) {
            dir = final;
            marker = openIfComplete(final, p);
            if (marker != INVALID_HANDLE_VALUE)
                return Install::Ok;
            break;
        }
        if (!fileExists(final))
            break;
        HANDLE other = openIfComplete(final, p);
        if (other != INVALID_HANDLE_VALUE) {
            trace(L"another launcher unpacked " + final + L" first");
            removeTree(tmp);
            dir = final;
            marker = other;
            return Install::Ok;
        }
        // An incomplete or damaged copy holds the name: move it aside.
        if (!discardFolder(root, final, trace))
            break;
    }
    // The final name stays blocked: run from the verified temporary folder;
    // a later start cleans it up.
    trace(L"running from " + tmp);
    marker = openIfComplete(tmp, p);
    if (marker == INVALID_HANDLE_VALUE) {
        err = L"The unpacked files in " + tmp + L" disappeared.";
        return Install::Failed;
    }
    dir = tmp;
    return Install::Ok;
}

bool discardFolder(const std::wstring& root, const std::wstring& dir, TraceFn trace)
{
    const std::wstring name = dir.substr(dir.find_last_of(L'\\') + 1);
    const std::wstring trash = root + L"\\.trash-" + name + L"-" + uniqueSuffix();
    if (!MoveFileExW(extendedPath(dir).c_str(), extendedPath(trash).c_str(), 0))
        return false; // in use
    if (!removeTree(trash))
        trace(L"could not fully remove " + trash);
    return true;
}

void removeOldRuntimes(const std::wstring& root, const std::wstring& keep, const Payload& p, TraceFn trace)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(extendedPath(root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories,
                                nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::vector<std::pair<std::wstring, FILETIME>> found;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            found.emplace_back(fd.cFileName, fd.ftCreationTime);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    const ULONGLONG now = (ULONGLONG(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
    for (const auto& [name, created] : found) {
        const std::wstring path = root + L"\\" + name;
        if (name == L"." || name == L".." || name == p.folderName ||
            CompareStringOrdinal(path.c_str(), -1, keep.c_str(), -1, TRUE) == CSTR_EQUAL)
            continue;
        if (name.rfind(L".trash-", 0) == 0) {
            removeTree(path);
            continue;
        }
        if (name.rfind(L".tmp-", 0) == 0) {
            const ULONGLONG c = (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            if (now > c && (now - c) / 10000 >= kTmpMinAgeMs && discardFolder(root, path, trace))
                trace(L"removed interrupted unpack " + name);
            continue;
        }
        // <version>-<12 hex>: ours. Remove this version's other builds and older versions.
        const size_t dash = name.find_last_of(L'-');
        if (dash == std::wstring::npos || name.size() - dash - 1 != 12)
            continue;
        const std::wstring hash = name.substr(dash + 1);
        if (!std::all_of(hash.begin(), hash.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }))
            continue;
        const std::string version = wideToUtf8(name.substr(0, dash));
        if (!isValidVersion(version) || compareVersions(version, p.version) > 0)
            continue;
        if (discardFolder(root, path, trace))
            trace(L"removed old runtime " + name);
        else
            trace(L"kept old runtime in use " + name);
    }
}

bool removeTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(extendedPath(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
                continue;
            const std::wstring child = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(extendedPath(child).c_str(), fd.dwFileAttributes & ~DWORD(FILE_ATTRIBUTE_READONLY));
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    ok = RemoveDirectoryW(extendedPath(child).c_str()) && ok; // the link, never its target
                else
                    ok = removeTree(child) && ok;
            } else {
                ok = DeleteFileW(extendedPath(child).c_str()) && ok;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(extendedPath(dir).c_str()) && ok;
}

} // namespace fw::onefile
