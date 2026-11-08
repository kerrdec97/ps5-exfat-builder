// Stand-in for the packed program in the single-file launcher tests: a GUI
// (WIN32) program like the real app. It reports what it was started with on
// standard output (UTF-8, one "key=value" per line) and writes one line to
// standard error.
//
//   --exit=<n>    exit with <n> (default 0)
//   --sleep=<ms>  sleep before exiting

#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <string>

namespace {

void put(HANDLE h, const std::wstring& line)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return;
    const std::wstring text = line + L"\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string u(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), u.data(), n, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(h, u.data(), DWORD(u.size()), &written, nullptr);
}

std::wstring env(const wchar_t* name)
{
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    return n > 0 && n < 4096 ? std::wstring(buf, n) : std::wstring(L"<unset>");
}

} // namespace

int wmain()
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    wchar_t exe[4096] = {};
    GetModuleFileNameW(nullptr, exe, 4096);
    wchar_t cwd[4096] = {};
    GetCurrentDirectoryW(4096, cwd);
    put(out, std::wstring(L"exe=") + exe);
    put(out, std::wstring(L"cwd=") + cwd);
    put(out, L"launcher_dir=" + env(L"FW_LAUNCHER_DIR"));
    put(out, L"launcher_exe=" + env(L"FW_LAUNCHER_EXE"));
    put(out, std::wstring(L"raw=") + GetCommandLineW());
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    put(out, L"argc=" + std::to_wstring(argc - 1));
    int code = 0;
    DWORD sleepMs = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        put(out, L"arg=" + a);
        if (a.rfind(L"--exit=", 0) == 0)
            code = _wtoi(a.c_str() + 7);
        else if (a.rfind(L"--sleep=", 0) == 0)
            sleepMs = DWORD(_wtoi(a.c_str() + 8));
    }
    LocalFree(argv);
    put(out, L"end");
    put(GetStdHandle(STD_ERROR_HANDLE), L"child-stderr");
    if (sleepMs)
        Sleep(sleepMs);
    return code;
}
