#pragma once

// The launcher's unpacked runtime folders:
//
//   <root>\<version>-<first 12 hex of the payload SHA-256>\   complete copy
//   <root>\.tmp-<...>\                                        being unpacked
//   <root>\.trash-<...>\                                      being removed
//
// <root> is <exe dir>\exFAT Image Builder.data\runtime in portable mode,
// else %LOCALAPPDATA%\kerrdec97\exFAT Image Builder\runtime, else the same
// under %TEMP%. A folder is complete when its marker file (written last, before
// the folder gets its final name) matches the payload and every file has its
// size. While the program runs the launcher keeps the marker open, which stops
// another launcher from renaming (removing) the folder.

#include "OnefileFormat.h"

#include <atomic>
#include <string>
#include <vector>

namespace fw::onefile {

inline constexpr wchar_t kMarkerName[] = L".onefile-complete";

struct Payload {
    std::wstring exePath; // the launcher itself
    std::wstring exeDir;
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t fileSize = 0;
    Trailer trailer{};
    Index index;
    std::string version;
    std::string sha;         // 64 hex digits
    std::wstring folderName; // <version>-<sha12>
};

struct Progress {
    enum Phase { Waiting, Checking, Unpacking };
    std::atomic<int> phase{Checking};
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{1};
    std::atomic<bool> cancel{false};
};

using TraceFn = void (*)(const std::wstring&);

// Opens the launcher's own file and reads the trailer and index. `damaged` is
// false when no payload is attached at all.
bool openPayload(Payload& p, std::wstring& err);

// Runtime roots, most preferred first.
std::vector<std::wstring> runtimeRoots(const Payload& p, bool portable);

// The marker of `dir` (open, deny-write/delete) when `dir` holds a complete
// copy of this payload; INVALID_HANDLE_VALUE otherwise.
HANDLE openIfComplete(const std::wstring& dir, const Payload& p);

// True when `root` exists or can be created and accepts new files.
bool prepareRoot(const std::wstring& root);

// SHA-256 of the payload against the trailer.
enum class Check { Ok, Mismatch, ReadError, Cancelled };
Check verifyPayload(const Payload& p, Progress& progress);

// Unpacks into <root>\.tmp-..., checks every file, writes the marker and moves
// the folder to <root>\<folderName>. `dir` receives the folder to run from
// (the temporary one if the final name stays blocked) and `marker` its open
// marker.
enum class Install { Ok, Cancelled, Failed };
Install install(const Payload& p, const std::wstring& root, Progress& progress, std::wstring& dir, HANDLE& marker,
                std::wstring& err, TraceFn trace);

// Renames `dir` aside and deletes it; false when it is in use.
bool discardFolder(const std::wstring& root, const std::wstring& dir, TraceFn trace);

// Removes runtime folders of this or older versions, interrupted unpacks and
// leftovers under `root`, except `keep` and folders in use.
void removeOldRuntimes(const std::wstring& root, const std::wstring& keep, const Payload& p, TraceFn trace);

bool removeTree(const std::wstring& dir);
bool fileExists(const std::wstring& path);

} // namespace fw::onefile
