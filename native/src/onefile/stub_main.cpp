// fw_onefile: the single-file launcher ("exFAT Image Builder.exe" in a
// release). It carries the deployed program (OnefileFormat.h), unpacks it once
// to a cache folder (Runtime.h) and then starts it with the same command line,
// working folder and standard handles, waits for it and returns its exit code.
//
// Environment:
//   FW_LAUNCHER_DIR, FW_LAUNCHER_EXE  set for the program: the launcher's folder and file
//   FW_ONEFILE_NO_UI=1     no progress window, no message boxes (errors go to stderr)
//   FW_ONEFILE_DEBUG=1     trace lines on stderr, "fw-onefile: ..."
//   FW_ONEFILE_NO_MUTEX=1  tests: first starts race for the folder name without the lock

#include "OnefileFormat.h"
#include "ProgressWindow.h"
#include "Runtime.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace fw::onefile;

namespace {

constexpr int kExitDamaged = 9101;
constexpr int kExitUnpackFailed = 9102;
constexpr int kExitLaunchFailed = 9103;
constexpr int kExitCancelled = 9104;

constexpr wchar_t kTitle[] = L"exFAT Image Builder";
constexpr wchar_t kReleases[] = L"https://github.com/kerrdec97/ps5-exfat-builder/releases";

bool g_noUi = false;
bool g_debug = false;
HANDLE g_stderr = nullptr;

bool envFlag(const wchar_t* name)
{
    wchar_t b[8] = {};
    const DWORD n = GetEnvironmentVariableW(name, b, 8);
    return n > 0 && n < 8 && b[0] != L'0';
}

void writeText(HANDLE h, const std::wstring& text)
{
    if (!h)
        return;
    DWORD mode = 0, n = 0;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, text.data(), DWORD(text.size()), &n, nullptr);
    } else {
        const std::string u = wideToUtf8(text);
        WriteFile(h, u.data(), DWORD(u.size()), &n, nullptr);
    }
}

void trace(const std::wstring& msg)
{
    if (g_debug)
        writeText(g_stderr, L"fw-onefile: " + msg + L"\n");
}

int report(int code, const std::wstring& message)
{
    writeText(g_stderr, std::wstring(kTitle) + L": " + message + L"\n");
    if (!g_noUi)
        MessageBoxW(nullptr, message.c_str(), kTitle, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return code;
}

std::wstring join(const std::wstring& dir, const std::wstring& name)
{
    return !dir.empty() && dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

// --- standard handles -----------------------------------------------------------
// Redirected or piped handles (cmd "> file", "| more", PowerShell capturing
// output, QProcess, CI) pass straight through to the program.
//
// Started from a console without redirection, this GUI-subsystem launcher has
// no standard handles at all, and Windows never gives console handles to a GUI
// program it starts. With arguments (--self-test, --help, --version, ...) the
// launcher therefore borrows the parent's console and relays the program's
// output to it through a pipe. Without arguments (a normal start) it leaves
// the console alone, so a console window it was started from can close.
struct StdIo {
    HANDLE in = nullptr, out = nullptr, err = nullptr;
    HANDLE console = nullptr; // the parent's console output, when borrowed
};

HANDLE g_job = nullptr;

HANDLE usable(DWORD which)
{
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE)
        return nullptr;
    SetLastError(NO_ERROR);
    if (GetFileType(h) == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR)
        return nullptr;
    return h;
}

BOOL WINAPI onConsoleEvent(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
        return TRUE; // like the GUI program itself: Ctrl+C in the console does not stop it
    // The console is closing: Windows ends the launcher, the program keeps running.
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    return FALSE;
}

StdIo setupStdIo(bool hasArguments)
{
    StdIo io;
    io.in = usable(STD_INPUT_HANDLE);
    io.out = usable(STD_OUTPUT_HANDLE);
    io.err = usable(STD_ERROR_HANDLE);
    if (hasArguments && !io.in && !io.out && !io.err && AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        if (con != INVALID_HANDLE_VALUE) {
            io.console = io.err = con; // the launcher's own messages
            SetConsoleCtrlHandler(onConsoleEvent, TRUE);
        } else {
            FreeConsole();
        }
    }
    return io;
}

// Pipe from the program's stdout+stderr to the borrowed console.
struct ConsoleRelay {
    HANDLE read = nullptr, write = nullptr, console = nullptr, thread = nullptr;

    bool open(HANDLE con)
    {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        if (!CreatePipe(&read, &write, &sa, 0))
            return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        console = con;
        return true;
    }
    static DWORD WINAPI pump(void* arg)
    {
        auto* r = static_cast<ConsoleRelay*>(arg);
        char buf[16384];
        DWORD n = 0, written = 0;
        while (ReadFile(r->read, buf, sizeof(buf), &n, nullptr) && n > 0)
            WriteFile(r->console, buf, n, &written, nullptr);
        return 0;
    }
    void started() // the program holds its copy of the write end now
    {
        CloseHandle(write);
        write = nullptr;
        thread = CreateThread(nullptr, 0, pump, this, 0, nullptr);
    }
    void finish()
    {
        if (write)
            CloseHandle(write);
        // Something the program started may still hold the pipe: do not wait for it.
        if (thread && WaitForSingleObject(thread, 1000) == WAIT_TIMEOUT)
            CancelSynchronousIo(thread);
        if (thread) {
            WaitForSingleObject(thread, 1000);
            CloseHandle(thread);
        }
        if (read)
            CloseHandle(read);
        read = write = thread = nullptr;
    }
};

// --- command line ---------------------------------------------------------------
// Everything after the program name, unchanged. The program name is skipped
// the way the C runtime parses argv[0] (quotes toggle, no escapes).
std::wstring argumentsAfterProgram(const wchar_t* cmd)
{
    const wchar_t* p = cmd;
    bool quoted = false;
    while (*p) {
        if (*p == L'"') {
            quoted = !quoted;
        } else if (!quoted && (*p == L' ' || *p == L'\t')) {
            break;
        }
        ++p;
    }
    while (*p == L' ' || *p == L'\t')
        ++p;
    return p;
}

// --- launching ------------------------------------------------------------------
HANDLE makeJob()
{
    // The program dies with the launcher (closing it from Task Manager closes
    // both); anything the program starts itself is not tied to the launcher.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

DWORD launch(const std::wstring& exe, const std::wstring& args, const StdIo& io, HANDLE job, PROCESS_INFORMATION& pi)
{
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty())
        cmd += L" " + args;
    if (cmd.size() >= 32767)
        return ERROR_FILENAME_EXCED_RANGE;

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOW);
    STARTUPINFOW own{};
    own.cb = sizeof(own);
    GetStartupInfoW(&own);
    if (own.dwFlags & STARTF_USESHOWWINDOW) { // e.g. a shortcut set to "Maximised"
        si.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        si.StartupInfo.wShowWindow = own.wShowWindow;
    }

    // Only the three standard handles are inherited.
    std::vector<HANDLE> inherit;
    auto prepare = [&](HANDLE h) -> HANDLE {
        if (!h)
            return nullptr;
        if (std::find(inherit.begin(), inherit.end(), h) != inherit.end())
            return h;
        if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            return nullptr;
        inherit.push_back(h);
        return h;
    };
    const HANDLE in = prepare(io.in), out = prepare(io.out), err = prepare(io.err);
    if (g_debug) {
        wchar_t b[160];
        swprintf(b, 160, L"standard handles %p %p %p (%zu inherited)%ls", in, out, err, inherit.size(),
                 io.console ? L", relayed to the parent console" : L"");
        trace(b);
    }
    std::vector<uint8_t> attrBuf;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    if (!inherit.empty()) {
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = in;
        si.StartupInfo.hStdOutput = out;
        si.StartupInfo.hStdError = err;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attrBuf.resize(size);
        attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size))
            return GetLastError();
        if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                       inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const DWORD e = GetLastError();
            DeleteProcThreadAttributeList(attrs);
            return e;
        }
        si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        si.lpAttributeList = attrs;
    }
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | (attrs ? EXTENDED_STARTUPINFO_PRESENT : 0);
    // Working folder and environment: the launcher's own (the user's).
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, attrs ? TRUE : FALSE, flags, nullptr, nullptr,
                                   &si.StartupInfo, &pi);
    const DWORD e = ok ? 0 : GetLastError();
    if (attrs)
        DeleteProcThreadAttributeList(attrs);
    if (!ok)
        return e;
    if (job && !AssignProcessToJobObject(job, pi.hProcess))
        trace(L"could not tie the program to the launcher: " + errorText(GetLastError()));
    AllowSetForegroundWindow(pi.dwProcessId);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    pi.hThread = nullptr;
    return 0;
}

bool isLoaderFailure(DWORD code)
{
    return code == 0xC0000135 /* DLL not found */ || code == 0xC000007B /* bad image */ ||
           code == 0xC0000139 /* entry point */ || code == 0xC0000138 /* ordinal */ || code == 0xC0000142 /* DLL init */;
}

// --- unpacking (worker thread) with the progress window (main thread) ------------
struct Unpack {
    const Payload* p = nullptr;
    const std::vector<std::wstring>* roots = nullptr;
    bool useMutex = true;
    Progress progress;
    HANDLE cancelEvent = nullptr;
    int code = 0;
    std::wstring message;
    std::wstring dir, root;
    HANDLE marker = INVALID_HANDLE_VALUE;
};

std::wstring mutexName(const Payload& p, const std::wstring& root)
{
    std::wstring key = join(root, p.folderName);
    CharLowerBuffW(key.data(), DWORD(key.size()));
    uint64_t h = 1469598103934665603ull;
    for (const wchar_t c : key) {
        h ^= uint64_t(c);
        h *= 1099511628211ull;
    }
    wchar_t b[96];
    swprintf(b, 96, L"Local\\kerrdec97.exFATImageBuilder.onefile.%016llx", (unsigned long long)h);
    return b;
}

void unpackWork(Unpack& u)
{
    const Payload& p = *u.p;
    HANDLE mutex = nullptr;
    bool owned = false;
    if (u.useMutex && !u.roots->empty()) {
        mutex = CreateMutexW(nullptr, FALSE, mutexName(p, u.roots->front()).c_str());
        if (mutex) {
            u.progress.phase = Progress::Waiting;
            HANDLE waits[2] = {mutex, u.cancelEvent};
            const DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0 + 1) {
                CloseHandle(mutex);
                u.code = kExitCancelled;
                return;
            }
            owned = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED_0; // abandoned: its .tmp folder is cleaned up later
        }
    }
    struct Release {
        HANDLE m;
        bool owned;
        ~Release()
        {
            if (owned)
                ReleaseMutex(m);
            if (m)
                CloseHandle(m);
        }
    } release{mutex, owned};

    // Another launcher may have unpacked it while this one waited.
    for (const std::wstring& root : *u.roots) {
        const std::wstring dir = join(root, p.folderName);
        HANDLE m = openIfComplete(dir, p);
        if (m != INVALID_HANDLE_VALUE) {
            trace(L"unpacked by another launcher: " + dir);
            u.dir = dir;
            u.root = root;
            u.marker = m;
            return;
        }
    }

    u.progress.total = p.trailer.payloadSize + p.index.totalRawBytes;
    u.progress.done = 0;
    u.progress.phase = Progress::Checking;
    const ULONGLONG t0 = GetTickCount64();
    switch (verifyPayload(p, u.progress)) {
    case Check::Ok:
        break;
    case Check::Cancelled:
        u.code = kExitCancelled;
        return;
    case Check::Mismatch:
        u.code = kExitDamaged;
        u.message = L"This copy of exFAT Image Builder is damaged: its built-in files do not match their SHA-256 "
                    L"checksum.\n\nDownload it again from " +
                    std::wstring(kReleases);
        return;
    case Check::ReadError:
        u.code = kExitDamaged;
        u.message = L"exFAT Image Builder cannot read its own file\n" + p.exePath + L"\n\n" + errorText(GetLastError());
        return;
    }
    trace(L"payload SHA-256 verified in " + std::to_wstring(GetTickCount64() - t0) + L" ms");

    u.progress.phase = Progress::Unpacking;
    std::wstring lastError;
    for (const std::wstring& root : *u.roots) {
        if (!prepareRoot(root)) {
            trace(L"cannot write to " + root);
            if (lastError.empty())
                lastError = L"Cannot write to " + root + L": " + errorText(GetLastError());
            continue;
        }
        const ULONGLONG t1 = GetTickCount64();
        std::wstring dir, err;
        HANDLE marker = INVALID_HANDLE_VALUE;
        const Install r = install(p, root, u.progress, dir, marker, err, trace);
        if (r == Install::Ok) {
            trace(L"unpacked " + std::to_wstring(p.index.files.size()) + L" files to " + dir + L" in " +
                  std::to_wstring(GetTickCount64() - t1) + L" ms");
            u.dir = dir;
            u.root = root;
            u.marker = marker;
            return;
        }
        if (r == Install::Cancelled) {
            u.code = kExitCancelled;
            return;
        }
        trace(L"unpacking to " + root + L" failed: " + err);
        lastError = err;
        u.progress.done = p.trailer.payloadSize;
    }
    u.code = kExitUnpackFailed;
    u.message = L"exFAT Image Builder could not unpack its files.\n\n" + lastError +
                L"\n\nIt needs about " + std::to_wstring((p.index.totalRawBytes >> 20) + 1) +
                L" MB of free space, once per version. Check the free space and that security software is not "
                L"blocking the folder.";
}

DWORD WINAPI unpackThread(void* arg)
{
    unpackWork(*static_cast<Unpack*>(arg));
    return 0;
}

void waitWithProgress(HINSTANCE instance, HANDLE thread, Unpack& u, ProgressWindow& win)
{
    const ULONGLONG start = GetTickCount64();
    bool triedWindow = false;
    for (;;) {
        DWORD timeout = INFINITE;
        if (win.isOpen()) {
            timeout = 50;
        } else if (!g_noUi && !triedWindow) {
            const ULONGLONG elapsed = GetTickCount64() - start;
            timeout = elapsed >= 1000 ? 0 : DWORD(1000 - elapsed);
        }
        const DWORD r = MsgWaitForMultipleObjectsEx(1, &thread, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0)
            return;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_noUi && !triedWindow && GetTickCount64() - start >= 1000) {
            triedWindow = true;
            win.create(instance, kTitle);
        }
        if (!win.isOpen())
            continue;
        if (win.closeRequested() && !u.progress.cancel) {
            u.progress.cancel = true;
            SetEvent(u.cancelEvent);
        }
        const std::wstring version = utf8ToWide(u.p->version);
        if (u.progress.cancel) {
            win.setText(L"Cancelling…");
            win.setProgress(-1);
        } else if (u.progress.phase == Progress::Waiting) {
            win.setText(L"Another copy of exFAT Image Builder is unpacking its files…");
            win.setProgress(-1);
        } else {
            win.setText(u.progress.phase == Progress::Checking
                            ? L"Checking exFAT Image Builder " + version + L"…"
                            : L"Unpacking exFAT Image Builder " + version + L" (first start of this version only)…");
            const uint64_t total = std::max<uint64_t>(1, u.progress.total.load());
            win.setProgress(int(std::min<uint64_t>(1000, u.progress.done.load() * 1000 / total)));
        }
    }
}

// Unpacks (or waits for another launcher to) and fills dir/root/marker.
int ensureUnpacked(HINSTANCE instance, const Payload& p, const std::vector<std::wstring>& roots, ProgressWindow& win,
                   std::wstring& dir, std::wstring& root, HANDLE& marker, std::wstring& message)
{
    Unpack u;
    u.p = &p;
    u.roots = &roots;
    u.useMutex = !envFlag(L"FW_ONEFILE_NO_MUTEX");
    u.cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, unpackThread, &u, 0, nullptr);
    if (!thread) {
        unpackWork(u);
    } else {
        waitWithProgress(instance, thread, u, win);
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    if (u.cancelEvent)
        CloseHandle(u.cancelEvent);
    dir = u.dir;
    root = u.root;
    marker = u.marker;
    message = u.message;
    return u.code;
}

int run(HINSTANCE instance)
{
    g_noUi = envFlag(L"FW_ONEFILE_NO_UI");
    g_debug = envFlag(L"FW_ONEFILE_DEBUG");
    const std::wstring args = argumentsAfterProgram(GetCommandLineW());
    const StdIo io = setupStdIo(!args.empty());
    g_stderr = io.err;

    Payload p;
    std::wstring why;
    if (!openPayload(p, why)) {
        if (why == L"no program is attached to this launcher")
            return report(kExitDamaged, L"This launcher carries no program.");
        return report(kExitDamaged, L"This copy of exFAT Image Builder is damaged (" + why + L").\n\nDownload it again from " +
                                        std::wstring(kReleases));
    }
    const bool portable = fileExists(join(p.exeDir, L"portable.txt"));
    const std::vector<std::wstring> roots = runtimeRoots(p, portable);
    trace(L"version " + utf8ToWide(p.version) + L", payload " + utf8ToWide(p.sha.substr(0, 12)) +
          (portable ? L", portable" : L""));

    SetEnvironmentVariableW(L"FW_LAUNCHER_DIR", p.exeDir.c_str());
    SetEnvironmentVariableW(L"FW_LAUNCHER_EXE", p.exePath.c_str());
    HANDLE job = makeJob();
    g_job = job;
    const File& entry = p.index.files[p.index.entryFile];
    std::wstring entryRel = utf8ToWide(entry.path);
    std::replace(entryRel.begin(), entryRel.end(), L'/', L'\\');

    ProgressWindow win;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::wstring dir, root;
        HANDLE marker = INVALID_HANDLE_VALUE;
        if (attempt == 0) {
            for (const std::wstring& r : roots) {
                const std::wstring d = join(r, p.folderName);
                marker = openIfComplete(d, p);
                if (marker != INVALID_HANDLE_VALUE) {
                    trace(L"cached " + d);
                    dir = d;
                    root = r;
                    break;
                }
            }
        }
        const bool fresh = marker == INVALID_HANDLE_VALUE;
        if (fresh) {
            std::wstring message;
            const int code = ensureUnpacked(instance, p, roots, win, dir, root, marker, message);
            if (code != 0) {
                win.destroy();
                if (code == kExitCancelled)
                    return code;
                return report(code, message);
            }
        }

        const std::wstring exe = join(dir, entryRel);
        StdIo childIo = io;
        ConsoleRelay relay;
        if (io.console && relay.open(io.console))
            childIo.out = childIo.err = relay.write;
        PROCESS_INFORMATION pi{};
        const DWORD e = launch(exe, args, childIo, job, pi);
        if (e != 0) {
            relay.finish();
            CloseHandle(marker);
            if (!fresh && attempt == 0) {
                trace(L"cached copy does not start (" + errorText(e) + L"), unpacking again");
                discardFolder(root, dir, trace);
                continue;
            }
            win.destroy();
            return report(kExitLaunchFailed, L"exFAT Image Builder could not start\n" + exe + L"\n\n" + errorText(e));
        }
        if (relay.write)
            relay.started();
        win.destroy();
        removeOldRuntimes(root, dir, p, trace);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(marker);
        relay.finish();
        if (!fresh && attempt == 0 && isLoaderFailure(code)) {
            // A damaged cached DLL (power loss, disk error): unpack again once.
            wchar_t hex[16];
            swprintf(hex, 16, L"0x%08lX", code);
            trace(L"cached copy failed to load (" + std::wstring(hex) + L"), unpacking again");
            discardFolder(root, dir, trace);
            continue;
        }
        g_job = nullptr;
        if (job)
            CloseHandle(job);
        CloseHandle(p.file);
        if (io.console) {
            CloseHandle(io.console);
            FreeConsole();
        }
        return int(code);
    }
    return kExitLaunchFailed;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // People start this file from Downloads or the Desktop. Every DLL loaded by
    // name from here on (the delay-loaded cabinet.dll, bcrypt.dll and
    // comctl32.dll, and whatever the system loads for the window) comes from
    // System32, never from the folder the launcher happens to lie in.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // At log-off and shutdown Windows ends the launcher after the program
    // (which has the default level 0x280): the program is in a job that dies
    // with the launcher and must get to handle the end of the session itself.
    SetProcessShutdownParameters(0x100, 0);
    return run(instance);
}
