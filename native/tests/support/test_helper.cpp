// Portable helper process for tests.
//   fw_test_helper print <n>        print n numbered lines, exit 0
//   fw_test_helper exit <code>      exit with <code>
//   fw_test_helper sleep <ms>       sleep, then exit 0
//   fw_test_helper spawn-sleep <ms> start a child "sleep <ms>" and sleep too
//   fw_test_helper stderr           write to stderr, exit 3
//   fw_test_helper cr               progress-style output with \r separators

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    if (argc < 2)
        return 64;
    const std::string cmd = argv[1];
    const long arg = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 0;
    if (cmd == "print") {
        for (long i = 1; i <= arg; ++i)
            std::printf("line %ld\n", i);
        return 0;
    }
    if (cmd == "exit")
        return int(arg);
    if (cmd == "sleep") {
        std::this_thread::sleep_for(std::chrono::milliseconds(arg));
        return 0;
    }
    if (cmd == "spawn-sleep") {
        const std::string self = argv[0];
#ifdef _WIN32
        const std::string line = "start \"\" /B \"" + self + "\" sleep " + std::to_string(arg);
#else
        const std::string line = "\"" + self + "\" sleep " + std::to_string(arg) + " &";
#endif
        std::system(line.c_str());
        std::printf("spawned\n");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(arg));
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
