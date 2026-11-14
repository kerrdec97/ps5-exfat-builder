// Portable helper process for tests.
//   fw_test_helper print <n>                  print n numbered lines, exit 0
//   fw_test_helper exit <code>                exit with <code>
//   fw_test_helper sleep <ms> [pidfile]       sleep, then exit 0
//   fw_test_helper spawn-sleep <ms> [pidfile] start a child "sleep <ms> [pidfile]" and sleep too
//   fw_test_helper stderr                     write to stderr, exit 3
//   fw_test_helper cr                         progress-style output with \r separators
//   fw_test_helper tick <n> [file]            n steps 20 ms apart: prints "tick <i>", or with a
//                                             file appends one byte to it per step; exit 0
//   fw_test_helper spawn-tick <n> <file>      start a child "tick <n> <file>" and print n ticks too
//
// With a pidfile the sleeping process writes its own process id there first,
// so a test can check that a grandchild is really gone after a cancel. The
// ticks show whether a process (and what it started) ran during a pause.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

long ownPid()
{
#ifdef _WIN32
    return long(_getpid());
#else
    return long(getpid());
#endif
}

// Written under another name and renamed, so a reader never sees half a number.
void writePidFile(const std::string& path)
{
    const std::string part = path + ".part";
    if (std::FILE* f = std::fopen(part.c_str(), "wb")) {
        std::fprintf(f, "%ld", ownPid());
        std::fclose(f);
        std::rename(part.c_str(), path.c_str());
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return 64;
    const std::string cmd = argv[1];
    const long arg = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 0;
    const std::string pidFile = argc > 3 ? argv[3] : "";
    if (cmd == "print") {
        for (long i = 1; i <= arg; ++i)
            std::printf("line %ld\n", i);
        return 0;
    }
    if (cmd == "exit")
        return int(arg);
    if (cmd == "sleep") {
        if (!pidFile.empty())
            writePidFile(pidFile);
        std::this_thread::sleep_for(std::chrono::milliseconds(arg));
        return 0;
    }
    if (cmd == "spawn-sleep") {
        const std::string self = argv[0];
        const std::string tail = pidFile.empty() ? std::string() : " \"" + pidFile + "\"";
#ifdef _WIN32
        const std::string line = "start \"\" /B \"" + self + "\" sleep " + std::to_string(arg) + tail;
#else
        const std::string line = "\"" + self + "\" sleep " + std::to_string(arg) + tail + " &";
#endif
        const int rc = std::system(line.c_str());
        (void)rc;
        std::printf("spawned\n");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(arg));
        return 0;
    }
    if (cmd == "tick" || cmd == "spawn-tick") {
        const std::string file = argc > 3 ? argv[3] : "";
        if (cmd == "spawn-tick") {
            if (file.empty())
                return 64;
            const std::string self = argv[0];
#ifdef _WIN32
            const std::string line = "start \"\" /B \"" + self + "\" tick " + std::to_string(arg) + " \"" + file + "\"";
#else
            const std::string line = "\"" + self + "\" tick " + std::to_string(arg) + " \"" + file + "\" &";
#endif
            const int rc = std::system(line.c_str());
            (void)rc;
        }
        for (long i = 1; i <= arg; ++i) {
            if (cmd == "tick" && !file.empty()) {
                if (std::FILE* f = std::fopen(file.c_str(), "ab")) {
                    std::fputc('.', f);
                    std::fclose(f);
                }
            } else {
                std::printf("tick %ld\n", i);
                std::fflush(stdout);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return 0;
    }
    if (cmd == "stderr") {
        std::fprintf(stderr, "something failed\n");
        return 3;
    }
    if (cmd == "cr") {
        for (int i = 0; i <= 100; i += 25)
            std::printf("progress %d%%\r", i);
        std::printf("done\n");
        return 0;
    }
    return 65;
}
