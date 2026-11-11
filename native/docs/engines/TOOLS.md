# tools — Language Stripper, dump check, problem reports

Target `fw_engine_tools` (`native/src/engines/tools/`). Depends on `gameinfo`
(game roots, `param.json`, `param.sfo`) and, for the dump check, on the
`backport` module's readers (fake-signed SELF, ELF header, SDK table, the
pinned fakelib trio). Job kinds and their parameters are listed in
`Module.h`. This document covers the dump check; the Language Stripper and
the report bundle are described in their headers.

## Dump check (`tools.dump.check`, `DumpCheck.h`)

"Is this dump fit to build from?": a read-only pass over a game folder that
reports missing or damaged files and what a tool left behind. It is the
*check* half of a "dump fixer"; repairing (restoring originals, backporting
again) stays with the Backport tab.

`tools.dump.check {folder, title?}` → `JobOutcome::details`:

```
{ folder,
  findings: [ { severity: "error" | "warning" | "info", code, path?, message,
                count?, paths? } ],        // errors first, then warnings, then notes
  counts:   { files, folders, bytes,
              executables: { total, "fake-signed", "encrypted", "compressed", "plain-elf",
                             "truncated", "malformed", "empty", "unknown", "unreadable" },
              errors, warnings, infos, listed, truncated, byCode: { <code>: n } },
  facts:    { platform, gameFolder, titleId, contentId, contentVersion, title,
              paramSdk, requiredFirmware, ebootClass, ebootSdk, executableSdks: { "4.50": n },
              fakelib: { present, files, libraries, names, engineLibraries },
              backportState, backportSummary } }
```

* `path` is relative to the checked folder (`/` separators); it is absent for
  a finding about the dump as a whole.
* A finding about many files (`empty-files`, `junk-files`, …) is one entry
  with `count` and the first 20 `paths`.
* The job **succeeds whenever the walk completed**, whatever it found:
  `verification` is `Passed` with no errors and no warnings, otherwise
  `Inconclusive`. `summary` is "No problems found in 18,442 files" or
  "2 problems and 3 warnings in 18,442 files". The first 20 problems and
  warnings are also in `warnings` and in the log.
* It fails only for a folder that is not there, and ends `Cancelled` when it
  is cancelled (checked at every directory entry; a paused job waits there).

### How it reads

* **One walk** of the folder (`QDirIterator` per folder, no recursion by Qt).
  Symbolic links and junctions are reported (`link`) and never entered. A
  folder whose listing comes back empty is asked for with
  `ensureFolderListable()`, so a folder the user may not open is a finding,
  not an empty folder.
* When the folder has no `eboot.bin`, `gameinfo::findGameRoots()` looks for
  the game below it (depth 5, 4,000 folders): one game → checked as that game
  (`game-nested`); several → only the file checks run (`several-games`).
* **Executables** (`eboot.bin`, `*.elf`, `*.self`, `*.prx`, `*.sprx`, anywhere):
  only the header is read. The first 64 KiB; when the SELF entry table or the
  ELF program header table reaches further, one more read of at most 1 MiB
  (a SELF whose entry table alone is larger is `exec-malformed`).
  `backport::selfio::inspect()` parses it and tells encrypted and compressed
  SELFs; the bounds rules of that reader (entry table, ELF header copy, each
  data entry inside the file, an entry no larger than its program header) are
  then applied against the size of the whole file, which the reader did not
  see. A plain ELF goes through `ElfImage::parse(Lenient)` and its `PT_LOAD`
  ranges are checked against the file size.
* **SDK of an executable**: the 24 bytes of its process / module param
  (`SceParam.h`: size, magic, PS5 SDK at +0x14), read where the SELF stores
  that part of the ELF (the data entry whose program header covers the param,
  as `selfio::unwrap()` places it) or straight from a plain ELF. Nothing is
  unwrapped and no file is read whole. A test compares the result with
  `unwrap()` + `elfSdkMajor()`.
* The three pinned fakelib files of the Backport engine (`CanonicalTrio.h`)
  are recognised by size and SHA-256; a file is hashed only when its name and
  size match (the largest is 1.5 MB).

### Finding codes

| Code | Severity | Raised when |
|---|---|---|
| `eboot-missing` | error | no `eboot.bin` in the game folder (and no game below it) |
| `eboot-empty` | error | the game's `eboot.bin` has 0 bytes |
| `param-missing` | error | no `sce_sys/param.json` (and no `param.sfo`) |
| `param-unreadable` | error | `param.json` (or a PS4 dump's `param.sfo`) cannot be parsed |
| `param-incomplete` | error | `param.json` lacks `titleId`, `contentId` or `contentVersion` |
| `ps4-dump` | info | `param.sfo` and no `param.json`: "PS4 dump: only basic checks apply" (no backport state) |
| `icon-missing` | warning | no `sce_sys/icon0.png` |
| `game-nested` | info | the game is in a folder below the checked one (named in `path`) |
| `several-games` | info | several games below the checked folder |
| `exec-encrypted` | error | an encrypted retail SELF: it cannot run from a dump |
| `exec-compressed` | error | a SELF with compressed segments |
| `exec-truncated` | error | the header, the entry table, the ELF header copy or a segment ends after the end of the file |
| `exec-malformed` | error | a SELF or ELF that breaks the format's rules; also a game `eboot.bin` that is neither |
| `exec-empty` | error | an executable of 0 bytes (the game's own `eboot.bin` is `eboot-empty`) |
| `exec-unreadable` | error | the file could not be opened |
| `exec-unknown` | warning | named like an executable but neither a SELF nor an ELF, or an ELF of a kind the console does not load (32-bit, big-endian) |
| `exec-unsigned` | warning | a plain ELF: fine for a build, will not load on a console as it is |
| `exec-fake-signed` | info | how many executables are fake-signed (one entry, `count`) |
| `decrypted-copies` | info | plain ELFs inside a `decrypted` folder (a dump extra; one entry, not a warning each) |
| `fakelib-present` | info | what the game's `fakelib` folder holds (also when it is empty) |
| `backport-state` | info | "Looks original / backported / re-tagged …", see below |
| `fakelib-eboot-not-signed` | warning | `fakelib` holds libraries but `eboot.bin` is not fake-signed |
| `sdk-differs` | info | the SDK in `eboot.bin` is not the `sdkVersion` of `param.json` |
| `backup-files` | info | `*.bak` files (left by a tool that changed files in place) |
| `empty-files` | info | files of 0 bytes that are not executables |
| `case-collision` | error | names in one folder that differ only in letter case (simple case folding) |
| `bad-name` | error | a name exFAT or Windows cannot store: trailing dot or space, a reserved device name, `< > : " / \ | ? *` or a control character, more than 255 UTF-16 units |
| `junk-files` | info | `Thumbs.db`, `desktop.ini`, `.DS_Store`, `*.part`, `*.tmp` |
| `link` | warning | a symbolic link or junction inside the dump (not followed) |
| `folder-unreadable` | error | a folder that cannot be listed |
| `large-files` | info | files over 4 GiB − 1 byte: they need exFAT or PFS, a FAT32 drive cannot hold them |

### Backport state (PS5 games)

Facts first, then one sentence (`facts.backportSummary`, also the
`backport-state` finding). `facts.backportState` is one of:

| State | Meaning |
|---|---|
| `backported` | `fakelib` holds libraries other than the two emulators, or holds a pinned library of the Backport engine, or holds libraries while the executables' SDK differs from `param.json` |
| `emulator-libraries` | `fakelib` holds only `libSceAmpr.sprx` / `libScePlayGo.sprx` and the SDKs agree (a backport's switchable pair, but also what a packed folder carries) |
| `retagged` | no `fakelib` libraries, but `eboot.bin` targets another SDK than `param.json` names (an SDK fix, or a backport whose fakelib was removed) |
| `original` | none of the above and `eboot.bin` is a readable executable |
| `unknown` | `eboot.bin` is missing or could not be read as an executable |

The SDK of the executables comes from `eboot.bin`'s process param (as the
Backport tab reads it); `param.json` gives `sdkVersion` and
`requiredSystemSoftwareVersion` as written. `facts.executableSdks` counts the
executables per SDK (at most 16 different values are kept).

### Bounds

* Listed findings: at most 500, at most 100 of one code, errors before
  warnings before notes; `counts` and `counts.byCode` are exact and
  `counts.truncated` says that something was counted but not listed.
* Per file: one or two header reads (≤ 1 MiB in the worst case, 64 KiB
  normally), 24 bytes for the SDK. `param.json` and `param.sfo` are read by the
  gameinfo readers, which refuse oversized files.
* Memory grows with the largest single folder (its names, for the
  case-collision check) and the number of folders waiting to be walked, not
  with the dump.

### What it deliberately does not do

* **It changes nothing.** No file is written, renamed or deleted, no time
  stamp is set; a test hashes the dump before and after.
* It does not repair, restore or re-sign anything, and it does not delete
  junk files.
* It does not read file contents: a data file that is the right size but
  holds the wrong bytes is not noticed (there is no list of expected hashes
  for a dump). Verifying an *image* against its source is Image tools › Verify.
* It does not decrypt, and it says nothing about whether a game will run on a
  given firmware; the backport state is a description, not a verdict.
* It does not look inside archives, images or PlayGo chunk files.

Tests: `tests/engines/tools/tst_dumpcheck.cpp` (a clean dump; one crafted
dump per finding code; the dump unchanged after the check; cancel; 10,000
empty files and a flood of one finding kind stay capped with exact counts).
UI: Library › Games › "Check dump…" (`LibraryPage`, `DumpCheckSheet`),
`tests/app/tst_issues_library.cpp`.
