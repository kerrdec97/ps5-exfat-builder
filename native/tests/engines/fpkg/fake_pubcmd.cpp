// Stand-in for prospero-pub-cmd.exe (and prospero-dds2png.exe) used by the
// FPKG tests. Our own implementation of the behaviour the tests rely on; it is
// not derived from Sony's tool. Plain C++17 so a copy runs from any folder.
//
// img_create [--oformat F] [--compression_level N] [--ref_pkg_path P] <gp5> <out>
//   Checks the project (no BOM, CRLF only, <psproject fmt="gp5">, volume type,
//   chunk_info for prospero_app and none for prospero_ac, entitlement key for
//   add-ons, keystone entry, every src_path exists), prints a ruler and 51 '='
//   without line breaks, writes <out> with a debug-package header (an update
//   writes a delta without header plus <out>.remastered.pkg) and
//   <out>.naps_metric.json.
// img_verify ... <pkg>          passes when <pkg> starts with 7F FIH.
// img_extract ... <pkg> <dest>  writes a small tree into the empty <dest>.
// Invoked as *dds2png*: <in.dds> <out.png> [--preserve-alpha] writes a 1x1 PNG.
//
// FAKE_PUBCMD_MODE: ok | stock (no plaintext marker) | error ([Error], exit 0)
//   | crash (exit 3) | ntstatus (0xC0000135) | badimage (0xC000007B)
//   | nooutput (exit 0, no file) | slow (bar over ~10 s) | split (bar in odd
//   chunks) | cr (CR line ends) | nobar (no bar) | strictpaths (non-ASCII
//   src_path or reserved dst_path fails) | nocompanion (update without
//   .remastered.pkg)
// FAKE_PUBCMD_VERIFY=fail, FAKE_PUBCMD_EXTRACT=fail|slow,
// FAKE_PUBCMD_EXTRACT_PARAM=<param.json text>, FAKE_DDS2PNG_MODE=fail|wrong,
// FAKE_PUBCMD_LOG=<file>: one JSON line per run (argv, cwd, TEMP, TMP, start
//   and end time, CPU count samples, priority class). FAKE_PUBCMD_DELAY_MS:
//   delay per bar step.
// FAKE_PUBCMD_CHILD_PIDFILE=<file>: img_create starts a child that sleeps and
//   writes its pid there (process-tree kill tests); the child also appends
//   one byte to <file>.ticks every 100 ms.
// FAKE_PUBCMD_TICKFILE=<file>: img_create appends one byte per bar step
//   (pause tests: a suspended tool stops counting).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

fs::path u8(const std::string& s)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

std::string pathToU8(const fs::path& p)
{
#if defined(__cpp_char8_t)
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
#else
    return p.u8string();
#endif
}

std::vector<std::string> utf8Args(int argc, char** argv)
{
    std::vector<std::string> out;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; i < n; ++i) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(len > 0 ? len - 1 : 0), '\0');
        if (len > 1)
            WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(s);
    }
    LocalFree(w);
#else
    for (int i = 0; i < argc; ++i)
        out.emplace_back(argv[i]);
#endif
    return out;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void out(const std::string& s)
{
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

std::string nl() { return env("FAKE_PUBCMD_MODE") == "cr" ? "\r" : "\n"; }

int cpuCount()
{
#ifdef _WIN32
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
        return -1;
    int n = 0;
    for (DWORD_PTR m = proc; m; m >>= 1)
        n += int(m & 1);
    return n;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return -1;
    return CPU_COUNT(&set);
#endif
}

// The Windows priority class (0x40 idle, 0x4000 below normal, 0x20 normal); 0 elsewhere.
unsigned long priorityClass()
{
#ifdef _WIN32
    return GetPriorityClass(GetCurrentProcess());
#else
    return 0;
#endif
}

std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += char(c);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            o += buf;
        } else {
            o += char(c);
        }
    }
    return o;
}

std::vector<int> g_cpuSamples;

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
const long long g_t0 = nowMs();

void record(const std::vector<std::string>& args)
{
    const std::string logPath = env("FAKE_PUBCMD_LOG");
    if (logPath.empty())
        return;
    std::ostringstream j;
    j << "{\"argv\":[";
    for (size_t i = 1; i < args.size(); ++i)
        j << (i > 1 ? "," : "") << '"' << jsonEscape(args[i]) << '"';
    std::error_code ec;
    j << "],\"cwd\":\"" << jsonEscape(pathToU8(fs::current_path(ec))) << "\",\"TEMP\":\"" << jsonEscape(env("TEMP"))
      << "\",\"TMP\":\"" << jsonEscape(env("TMP")) << "\",\"t0\":" << g_t0 << ",\"t1\":" << nowMs()
      << ",\"cpus\":" << cpuCount() << ",\"cpuSamples\":[";
    for (size_t i = 0; i < g_cpuSamples.size(); ++i)
        j << (i ? "," : "") << g_cpuSamples[i];
    j << "],\"priority\":" << priorityClass() << "}\n";
    std::ofstream f(u8(logPath), std::ios::app | std::ios::binary);
    f << j.str();
}

// One byte per tick: the file size is the count, and a reader never sees a
// half-written number.
void writeTick(const std::string& path)
{
    if (path.empty())
        return;
    std::ofstream f(u8(path), std::ios::app | std::ios::binary);
    f << 'x';
}

bool readFile(const std::string& path, std::string& data)
{
    std::ifstream f(u8(path), std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    data = ss.str();
    return true;
}

void putU16(std::string& d, size_t off, uint16_t v)
{
    d[off] = char(v & 0xFF);
    d[off + 1] = char(v >> 8);
}

void putU64(std::string& d, size_t off, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        d[off + size_t(i)] = char((v >> (8 * i)) & 0xFF);
}

void writePackage(const std::string& path, bool fih, bool marker)
{
    std::string d(0x2000, '\0');
    if (fih) {
        d.replace(0, 4, "\x7F" "FIH", 4);
        const size_t sb = 0x1000;
        putU64(d, 0x20, sb);
        putU16(d, sb + 0x1C, 13);
        if (marker)
            d.replace(sb + 0x370, 16, "PPRPLAIN-NOAUTH!");
    } else {
        d.replace(0, 5, "DELTA");
    }
    std::ofstream f(u8(path), std::ios::binary | std::ios::trunc);
    f << d;
}

std::string unescape(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            static const std::pair<const char*, char> ents[] = {{"&amp;", '&'}, {"&lt;", '<'},  {"&gt;", '>'},
                                                                {"&quot;", '"'}, {"&#13;", '\r'}, {"&#10;", '\n'},
                                                                {"&#09;", '\t'}};
            bool done = false;
            for (const auto& [e, c] : ents) {
                const size_t n = std::strlen(e);
                if (s.compare(i, n, e) == 0) {
                    o += c;
                    i += n;
                    done = true;
                    break;
                }
            }
            if (!done)
                o += s[i++];
        } else {
            o += s[i++];
        }
    }
    return o;
}

std::map<std::string, std::string> attributes(const std::string& tag)
{
    std::map<std::string, std::string> a;
    size_t i = 0;
    while ((i = tag.find('=', i)) != std::string::npos) {
        size_t k = i;
        while (k > 0 && tag[k - 1] != ' ')
            --k;
        const std::string name = tag.substr(k, i - k);
        if (i + 1 >= tag.size() || tag[i + 1] != '"') {
            ++i;
            continue;
        }
        const size_t end = tag.find('"', i + 2);
        if (end == std::string::npos)
            break;
        a[name] = unescape(tag.substr(i + 2, end - i - 2));
        i = end + 1;
    }
    return a;
}

std::vector<std::map<std::string, std::string>> elements(const std::string& doc, const std::string& name)
{
    std::vector<std::map<std::string, std::string>> out;
    const std::string open = "<" + name + " ";
    size_t i = 0;
    while ((i = doc.find(open, i)) != std::string::npos) {
        const size_t end = doc.find('>', i);
        if (end == std::string::npos)
            break;
        out.push_back(attributes(doc.substr(i + open.size(), end - i - open.size())));
        i = end;
    }
    return out;
}

int fail(const std::string& msg, int code = 1)
{
    out("[Error] " + msg + nl());
    return code;
}

int imgCreate(const std::vector<std::string>& args)
{
    const std::string mode = env("FAKE_PUBCMD_MODE").empty() ? "ok" : env("FAKE_PUBCMD_MODE");
    std::string ref, level;
    std::vector<std::string> positional;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--oformat" || args[i] == "--tmp_path") {
            ++i;
        } else if (args[i] == "--compression_level" && i + 1 < args.size()) {
            level = args[++i];
        } else if (args[i] == "--ref_pkg_path" && i + 1 < args.size()) {
            ref = args[++i];
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.size() != 2)
        return fail("usage: img_create [options] <gp5> <out>");
    const std::string gp5 = positional[0], outPath = positional[1];
    if (!level.empty()) {
        const int n = std::atoi(level.c_str());
        if (n < -4 || n > 9)
            return fail("Compression level should be set to a number between -4 and 9");
    }
    if (!ref.empty() && !fs::exists(u8(ref)))
        return fail("File not found (" + ref + ")");
    std::string doc;
    if (!readFile(gp5, doc))
        return fail("File not found (" + gp5 + ")");
    if (doc.compare(0, 3, "\xEF\xBB\xBF") == 0)
        return fail("Format of the GP5 file is not valid (byte order mark)");
    for (size_t i = 0; i < doc.size(); ++i)
        if (doc[i] == '\n' && (i == 0 || doc[i - 1] != '\r'))
            return fail("Format of the GP5 file is not valid (line endings)");
    if (doc.find("<psproject fmt=\"gp5\">") == std::string::npos)
        return fail("Format of the GP5 file is not valid (psproject)");
    const size_t vt0 = doc.find("<volume_type>"), vt1 = doc.find("</volume_type>");
    const std::string vt = vt0 == std::string::npos ? "" : doc.substr(vt0 + 13, vt1 - vt0 - 13);
    const auto packages = elements(doc, "package");
    if (packages.size() != 1 || packages[0].count("passcode") == 0 || packages[0].at("passcode").size() != 32)
        return fail("Format of the GP5 file is not valid (package)");
    const bool hasChunkInfo = doc.find("<chunk_info ") != std::string::npos;
    if (vt == "prospero_app") {
        if (!hasChunkInfo)
            return fail("Format of the GP5 file is not valid (chunk_info)");
    } else if (vt == "prospero_ac") {
        if (hasChunkInfo || packages[0].count("entitlement_key") == 0)
            return fail("Format of the GP5 file is not valid (add-on volume)");
    } else {
        return fail("unsupported volume type " + vt);
    }
    bool keystone = false;
    for (const auto& f : elements(doc, "file")) {
        const std::string& src = f.count("src_path") ? f.at("src_path") : std::string();
        const std::string& dst = f.count("dst_path") ? f.at("dst_path") : std::string();
        if (dst == "sce_sys/keystone")
            keystone = true;
        if (mode == "strictpaths") {
            for (const unsigned char c : src)
                if (c >= 0x80)
                    return fail("invalid attribute value src_path (" + src + ")");
            if (dst.rfind("sce_sys/about/", 0) == 0 || dst == "sce_sys/param.sfo")
                return fail("Format of the GP5 file is not valid (reserved node found (dst_path " + dst + "))");
        }
        if (!fs::is_regular_file(u8(src)))
            return fail("File not found (" + src + ")");
    }
    if (vt == "prospero_app" && !keystone)
        return fail("The keystone file is missing (sce_sys/keystone)");

    if (!env("FAKE_PUBCMD_CHILD_PIDFILE").empty()) {
        const std::string self = args[0];
#ifdef _WIN32
        const std::string cmd = "start \"\" /B \"" + self + "\" child-sleep \"" + env("FAKE_PUBCMD_CHILD_PIDFILE") + "\"";
#else
        const std::string cmd = "\"" + self + "\" child-sleep \"" + env("FAKE_PUBCMD_CHILD_PIDFILE") + "\" &";
#endif
        (void)std::system(cmd.c_str());
    }

    out("[Info] Creating an image..." + nl());
    if (mode != "nobar") {
        out("|____|____|____|____|____|____|____|____|____|____|" + nl());
        out("0        20        40        60        80       100" + nl());
    }
    int delay = mode == "slow" ? 200 : 2;
    if (!env("FAKE_PUBCMD_DELAY_MS").empty())
        delay = std::atoi(env("FAKE_PUBCMD_DELAY_MS").c_str());
    g_cpuSamples.push_back(cpuCount());
    for (int i = 0; i < 51; ++i) {
        if (mode != "nobar") {
            if (mode == "split" && i % 7 == 3)
                out("==");
            else if (!(mode == "split" && i % 7 == 4))
                out("=");
        }
        sleepMs(delay);
        writeTick(env("FAKE_PUBCMD_TICKFILE"));
        if (i == 10 && mode != "nooutput") {
            std::ofstream f(u8(outPath), std::ios::binary | std::ios::trunc);
            f << "partial";
        }
        if (i % 10 == 9)
            g_cpuSamples.push_back(cpuCount());
    }
    out(nl());
    if (mode == "crash")
        return 3;
    if (mode == "ntstatus")
        return int(0xC0000135u);
    if (mode == "badimage")
        return int(0xC000007Bu);
    if (mode == "nooutput") {
        std::error_code ec;
        fs::remove(u8(outPath), ec);
        out("[Info] Process finished." + nl());
        return 0;
    }
    const bool marker = mode != "stock";
    if (!ref.empty()) {
        writePackage(outPath, false, false);
        if (mode != "nocompanion")
            writePackage(outPath + ".remastered.pkg", true, marker);
    } else {
        writePackage(outPath, true, marker);
    }
    {
        std::ofstream m(u8(outPath + ".naps_metric.json"), std::ios::binary);
        m << "{}";
    }
    if (mode == "error")
        out("[Error] Format of the png file is not valid (sce_sys/pic2.png)" + nl());
    out("[Warn] The version of the SDK toolchain is invalid or missing" + nl());
    out("[Info] Process finished." + nl());
    return 0;
}

int imgVerify(const std::vector<std::string>& args)
{
    if (env("FAKE_PUBCMD_VERIFY") == "fail")
        return fail("Integrity check failed");
    std::string data;
    if (args.size() < 3 || !readFile(args.back(), data))
        return fail("File not found");
    if (data.compare(0, 4, "\x7F" "FIH", 4) != 0)
        return fail("Cannot parse the package (not a full image)");
    out("[Info] Verification passed" + nl());
    return 0;
}

int imgExtract(const std::vector<std::string>& args)
{
    const std::string mode = env("FAKE_PUBCMD_EXTRACT");
    if (mode == "fail")
        return fail("Cannot open package");
    if (args.size() < 4)
        return fail("usage: img_extract [options] <pkg> <dest>");
    const fs::path dest = u8(args.back());
    std::error_code ec;
    if (!fs::is_directory(dest) || !fs::is_empty(dest, ec))
        return fail("The output directory must be empty");
    auto write = [&](const std::string& rel, const std::string& data) {
        const fs::path p = dest / u8(rel);
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f << data;
    };
    write("eboot.bin", std::string("\x7F" "ELF", 4) + std::string(100, '\0'));
    const std::string param = env("FAKE_PUBCMD_EXTRACT_PARAM");
    write("sce_sys/param.json", param.empty() ? "{}" : param);
    write("sce_sys/keystone", std::string(96, 'K'));
    write("Data/base_only.pak", std::string(300, 'B'));
    write("Data/shared.pak", std::string(200, 'O'));
    write("playgo-chunks/001.bin", std::string(1024, '\0'));
    if (mode == "slow")
        for (int i = 0; i < 100; ++i)
            sleepMs(100);
    out("[Info] Extraction finished" + nl());
    return 0;
}

// Minimal 1x1 PNG of the given colour type.
uint32_t crc32(const std::string& data)
{
    uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char b : data) {
        c ^= b;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

std::string be32(uint32_t v)
{
    return std::string{char(v >> 24), char((v >> 16) & 0xFF), char((v >> 8) & 0xFF), char(v & 0xFF)};
}

std::string chunk(const std::string& type, const std::string& data)
{
    return be32(uint32_t(data.size())) + type + data + be32(crc32(type + data));
}

int dds2png(const std::vector<std::string>& args)
{
    if (args.size() < 3)
        return 2;
    const std::string mode = env("FAKE_DDS2PNG_MODE");
    if (mode == "fail") {
        out("conversion failed" + nl());
        return 1;
    }
    std::string in;
    if (!readFile(args[1], in))
        return 3;
    const bool alpha = args.size() > 3 && args[3] == "--preserve-alpha";
    int type = alpha ? 6 : 2;
    if (mode == "wrong")
        type = alpha ? 2 : 6;
    std::string ihdr = be32(1) + be32(1) + std::string{char(8), char(type), char(0), char(0), char(0)};
    const std::string png = std::string("\x89PNG\r\n\x1A\n", 8) + chunk("IHDR", ihdr) + chunk("IDAT", std::string(12, '\0'))
        + chunk("IEND", "");
    std::ofstream f(u8(args[2]), std::ios::binary | std::ios::trunc);
    f << png;
    return f ? 0 : 4;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() >= 3 && args[1] == "child-sleep") {
#ifdef _WIN32
        const unsigned long pid = GetCurrentProcessId();
#else
        const unsigned long pid = (unsigned long)getpid();
#endif
        std::ofstream f(u8(args[2]), std::ios::trunc);
        f << pid;
        f.close();
        for (int tick = 0; tick < 600; ++tick) {
            sleepMs(100);
            writeTick(args[2] + ".ticks");
        }
        return 0;
    }
    const std::string self = args.empty() ? std::string() : pathToU8(u8(args[0]).filename());
    int rc = 0;
    if (self.find("dds2png") != std::string::npos) {
        rc = dds2png(args);
    } else if (args.size() < 2) {
        rc = fail("no command");
    } else if (args[1] == "img_create") {
        rc = imgCreate(args);
    } else if (args[1] == "img_verify") {
        rc = imgVerify(args);
    } else if (args[1] == "img_extract") {
        rc = imgExtract(args);
    } else {
        rc = fail("unknown command " + args[1]);
    }
    record(args);
    return rc;
}
