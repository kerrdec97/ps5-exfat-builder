// Console stand-in for the single-file launcher tests: plays cmd.exe running
// the launcher without redirection. Started with CREATE_NO_WINDOW it owns a
// console without a window; it starts `<exe> <args...>` with no standard
// handles (as cmd starts a GUI program), waits, and then prints what landed in
// its console screen buffer to its own (redirected) standard output, followed
// by "exit=<code>".
//
//   fw_onefile_test_console <exe> [args...]

#include <windows.h>

#include <string>
#include <vector>

namespace {

void put(HANDLE h, const std::wstring& text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string u(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), u.data(), n, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(h, u.data(), DWORD(u.size()), &written, nullptr);
}

std::wstring quote(const std::wstring& a)
{
    return L"\"" + a + L"\"";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    HANDLE report = GetStdHandle(STD_OUTPUT_HANDLE);
    if (argc < 2) {
        put(report, L"usage: fw_onefile_test_console <exe> [args...]\n");
        return 64;
    }
    std::wstring cmd = quote(argv[1]);
    for (int i = 2; i < argc; ++i)
        cmd += L" " + quote(argv[i]);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(argv[1], cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        put(report, L"CreateProcess failed " + std::to_wstring(GetLastError()) + L"\n");
        return 65;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // The launcher returns when the program has exited; give the console a
    // moment for output the program wrote just before.
    Sleep(200);
    HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    std::wstring text;
    if (con != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(con, &info)) {
        const SHORT width = info.dwSize.X;
        std::vector<wchar_t> row(static_cast<size_t>(width), L' ');
        for (SHORT y = 0; y <= info.dwCursorPosition.Y; ++y) {
            DWORD read = 0;
            ReadConsoleOutputCharacterW(con, row.data(), DWORD(width), COORD{0, y}, &read);
            std::wstring line(row.data(), read);
            while (!line.empty() && line.back() == L' ')
                line.pop_back();
            text += line + L"\n";
        }
    } else {
        text = L"no console\n";
    }
    put(report, text + L"exit=" + std::to_wstring(code) + L"\n");
    return 0;
}
