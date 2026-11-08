#pragma once

// Single-file launcher format, shared by the launcher (fw_onefile) and the
// packer (fw_onefile_pack). Windows only, no Qt.
//
// A packed launcher is the launcher executable with the program appended:
//
//   [launcher .exe][zero padding to 16 bytes][payload][Trailer]
//
// signtool may later append an Authenticode certificate table; readTrailer()
// then finds the trailer just before it.
//
//   payload = PayloadHeader
//             fileCount  x (FileEntry + UTF-8 path, '/' separated)
//             chunkCount x ChunkEntry
//             chunk data
//
// Offsets inside the payload are relative to its first byte; integers are
// little-endian. Each file is cut into chunks of at most kMaxChunkRaw bytes.
// A chunk is stored as is or compressed with the Windows Compression API
// (cabinet.dll, buffer mode) and carries the CRC-32 of its raw bytes. The
// trailer holds the SHA-256 of the whole payload.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fw::onefile {

inline constexpr char kTrailerMagic[8] = {'F', 'W', '1', 'F', 'I', 'L', 'E', 'T'};
inline constexpr char kPayloadMagic[8] = {'F', 'W', '1', 'F', 'I', 'L', 'E', 'P'};
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint32_t kDefaultChunkRaw = 8u << 20;
inline constexpr uint32_t kMaxChunkRaw = 64u << 20;

// Chunk methods: 0 = stored; otherwise a COMPRESS_ALGORITHM_* value.
enum Method : uint8_t {
    kStore = 0,
    kMszip = 2,
    kXpress = 3,
    kXpressHuff = 4,
    kLzms = 5,
};
bool isKnownMethod(uint8_t method);
const char* methodName(uint8_t method);

#pragma pack(push, 1)
struct Trailer {
    char magic[8];
    uint32_t formatVersion;
    uint32_t trailerSize;
    uint64_t payloadOffset; // from the start of the file
    uint64_t payloadSize;
    uint8_t payloadSha256[32];
    char version[32]; // NUL padded, [0-9A-Za-z._+-]
    uint8_t reserved[28];
    uint32_t crc32; // of the bytes above
};
static_assert(sizeof(Trailer) == 128);

struct PayloadHeader {
    char magic[8];
    uint32_t fileCount;
    uint32_t chunkCount;
    uint32_t indexBytes; // file and chunk entries after this header
    uint32_t entryFile;  // index of the program to start
    uint64_t totalRawBytes;
    uint8_t reserved[16];
};
static_assert(sizeof(PayloadHeader) == 48);

struct FileEntry { // followed by pathBytes of UTF-8
    uint64_t rawSize;
    uint32_t firstChunk;
    uint32_t chunkCount;
    uint16_t pathBytes;
    uint16_t flags;
};
static_assert(sizeof(FileEntry) == 20);

struct ChunkEntry {
    uint64_t offset; // of the stored bytes, relative to the payload start
    uint32_t storedSize;
    uint32_t rawSize;
    uint32_t crc32; // of the raw bytes
    uint8_t method;
    uint8_t reserved[3];
};
static_assert(sizeof(ChunkEntry) == 24);
#pragma pack(pop)

struct File {
    std::string path; // UTF-8, '/' separated, validated
    uint64_t rawSize = 0;
    uint32_t firstChunk = 0;
    uint32_t chunkCount = 0;
};

struct Index {
    std::vector<File> files;
    std::vector<ChunkEntry> chunks;
    uint32_t entryFile = 0;
    uint64_t totalRawBytes = 0;
};

// --- helpers ------------------------------------------------------------------
uint32_t crc32(const void* data, size_t size, uint32_t crc = 0);

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    bool ok() const { return m_hash != nullptr; }
    bool update(const void* data, size_t size);
    bool finish(uint8_t out[32]);

private:
    void* m_alg = nullptr;
    void* m_hash = nullptr;
};

std::string toHex(const uint8_t* data, size_t size);
std::wstring utf8ToWide(std::string_view s);
std::string wideToUtf8(std::wstring_view s);

// "\\?\" form of an absolute path for file APIs (no MAX_PATH limit).
std::wstring extendedPath(const std::wstring& absolute);
// Message text of a Win32 error code (no trailing newline).
std::wstring errorText(DWORD code);

bool readAt(HANDLE file, uint64_t offset, void* buffer, size_t size);

// Version strings name folders: 1-31 of [0-9A-Za-z._+-].
bool isValidVersion(std::string_view v);
// Dotted comparison, numeric where both parts are numbers. <0, 0, >0.
int compareVersions(std::string_view a, std::string_view b);
// Relative payload path: no drive, no "..", no characters Windows rejects.
bool isValidRelativePath(std::string_view p);

uint32_t trailerCrc(const Trailer& t);

// Finds and checks the trailer of `file`. False (with a reason) when the file
// carries no payload or the trailer is damaged.
bool readTrailer(HANDLE file, uint64_t fileSize, Trailer& out, std::string& why);

// Reads and checks the payload header and index (not the chunk data).
bool readIndex(HANDLE file, const Trailer& t, Index& out, std::string& why);

} // namespace fw::onefile
