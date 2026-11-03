# Building PS5 packages (FPKG)

The Build tab can turn a PS5 game dump folder into an installable debug
package (`.pkg`). Choose **FPKG** under *Output format*.

## What you need to supply

The app does **not** include any Sony software and never downloads it.
You provide:

| What | Why |
|---|---|
| `prospero-pub-cmd.exe` from a Publishing Tools build you are entitled to use | It does the actual packing (`img_create`) and the optional deep check (`img_verify`). Select the `.exe` itself, its folder, or a toolkit folder containing `toolchain\` or `bin\`. |
| A Publishing Tools build that makes **plaintext debug** packages | A stock Publishing Tools install makes packages a debug console will not install. The app checks every package for the plaintext debug marker. A package without it is reported as a **failure**, not a success, and is not kept. |
| Microsoft Visual C++ 2015-2022 Redistributable (x64) | The Publishing Tools need it. The app checks for it and tells you if it is missing. |
| A dump with `sce_sys/param.json` and its 96-byte `sce_sys/keystone` | The Content ID and version come from `param.json`. The debug toolchain uses the dump's keystone. |

The tool location is saved in your settings (`fpkg_pubtools_path`). You can
also set the `SCE_PROSPERO_PUBTOOLS` environment variable.

## What the app does

1. **Pre-flight.** Checks the tools, reads `param.json` (Content ID, version),
   checks the keystone, and scans the folder. Problems such as non-ASCII
   paths, `%` or `;` in names, names ending in a dot, links, and missing free
   space are all reported before anything slow starts.
2. **Project.** Writes a GP5 project beside the output:
   * files in the order the Publishing Tools lay them out (each folder sorted
     case-insensitively);
   * files the SDK generates itself (`param.sfo`, PlayGo tables,
     `sce_sys/about`, `.dds` and so on), dump tool output (`.exfat`, `.ffpkg`,
     …), the dump emulation libraries `fakelib/libSceAmpr.sprx` and
     `libScePlayGo.sprx`, and OS clutter (`._*`, `.DS_Store`, `Thumbs.db`, …)
     are left out. Every exclusion is listed in `01-create-gp5.log`;
   * empty folders (for example DLC mount folders such as `addcont0`) are
     declared so they exist in the package;
   * a normalised copy of `param.json`: DRM type `standard`, and for full
     game builds the package size class chosen from the data size. If that
     forces a lower `addcontMountLevel`, the log warns that separately
     installed DLC may not mount;
   * a PlayGo layout where every chunk is installed before the game starts,
     every scenario lists every chunk, and chunks without game data get a
     1 MiB placeholder. Default: 100 chunks (1-255 allowed).
3. **Pack.** Runs `img_create` into `<name>.partial.pkg` and shows its
   progress bar (or the bytes written when no bar is printed). Any `[Error]`
   line fails the build even if the tool exits 0. Known SDK errors are
   explained in plain words.
4. **Check.** Reads the package header: FIH magic, unsigned flag, superblock
   mode, plaintext debug marker. Only then is the file renamed to
   `<ContentID>-A<ver>-V<ver>.pkg`. An existing file of that name is never
   overwritten; a numbered copy is saved instead.
5. **Verify** (optional, on by default). Runs `img_verify`. A failure here
   keeps the package and marks the job with a warning.
6. **Clean up.** Removes the project files unless *Keep project files* is
   ticked, and always keeps `<name>-build-logs\` (`01-create-gp5.log`,
   `02-img-create.log`).

**Cancel** stops the Publishing Tools at once (the whole process tree) and
deletes the partial package. The final name only ever appears on success.

## Compression

*Tool default* passes no level, so the Publishing Tools use their own
default. *Fast (2)*, *Standard (4)*, *Smallest (7)* and *Maximum (9)* pass
`--compression_level`.

## Updates

Fill in *Base package (updates only)* to build an update against an earlier
package (`--ref_pkg_path`). The base must have been built by the Publishing
Tools, with the same Content ID and a lower `contentVersion`; the dump must
carry the new version. The build writes `UPDATE_<name>.pkg` (the delta) and
`<name>.remastered.pkg` (the full game); the header check runs on the full
package. The size class is not changed for updates.

## Verifying or extracting an existing package

*Verify a package…* runs the header checks and your tool's `img_verify`.
*Extract a package…* runs `img_extract` into an **empty** folder (a folder
with files in it is refused). Both write their output to the Output Log.

## Add-on (DLC) packages

**What the Publishing Tools interface offers.** The strings of
`prospero-pub-cmd.exe` (see `docs/farewell/FPKG_RESEARCH.md` §1.4 and §1.11)
list three volume types: `prospero_app` (game), `prospero_ac` (add-on with
content) and `prospero_al` (entitlement-only add-on). They also list an
`entitlement_key` that the GP5 `<package>` element carries for add-ons. The
reference FPKG builder never built add-ons through these tools; it used a
different library. So this path has **not been run against real
Publishing Tools or installed on a console**.

**What this app does.** *Build an add-on (DLC)…* in the FPKG panel builds a
`prospero_ac` package:
* input: the add-on's content folder, which must contain the add-on's own
  `sce_sys/param.json` (as a dumped add-on does), and its 32-hex-character
  entitlement key;
* both can be filled from the selected game's `dlc_emu.ini`: `[PSAC]` entries
  whose `mount_point` folder exists locally with a `param.json` are offered,
  and the others are listed with the reason they can't be built;
* the project uses volume type `prospero_ac`, the entitlement key, the
  add-on's Content ID and no PlayGo chunk table;
* packing, progress, `[Error]` handling, the header/plaintext-marker check,
  never-overwrite naming and optional `img_verify` work as for games.

**Precise limitations.**
* `prospero_al` (entitlement-only) is **refused**. It has no file system,
  and the PS5 debug installer refuses such packages, so a "successful" build
  would be useless.
* An add-on **without** `sce_sys/param.json` is refused. The fields the
  Publishing Tools need in an add-on's `param.json` are not documented, so
  the app does not make one up. An unlock-only add-on is possible with a
  folder that holds just a real `param.json`.
* The entitlement key is never guessed or extracted: you enter it, or it
  comes from `dlc_emu.ini`. (Reading it from `license.info` needs a library
  this app doesn't use.)
* Add-ons are built one at a time from the dialog, not through the queue.

## Coverage summary

| Area | Status | Tested how |
|---|---|---|
| Compression (`--compression_level`, presets) | Done | argv tests; stand-in tool |
| PFS | Done by the Publishing Tools (`img_create`); the app marks placeholder payloads `pfs_compression="disable"` and checks the outer superblock (mode 13, plaintext marker) | header tests |
| GP5 project (order, exclusions, empty dirs, `param.json` normalisation) | Done | unit tests; stand-in tool parses it |
| PlayGo (every chunk initial and in every scenario, placeholders, languages) | Done | unit tests |
| Create (`img_create`, progress, cancel, errors, naming) | Done | end-to-end against the stand-in on Linux and on Windows (compiled stand-in) |
| Verify (header checks + `img_verify`) | Done | stand-in tool |
| Extract (`img_extract`, empty folder only) | Done | stand-in tool |
| Update builds (`--ref_pkg_path`, `.remastered.pkg`) | Done | stand-in tool |
| Add-on `prospero_ac` | Done (experimental) | GP5 tests; end-to-end against the stand-in; `dlc_emu.ini` mapping |
| Add-on `prospero_al` | Refused (installer rejects it) | test |
| Non-ASCII folder paths | Done on Windows (junctions) | Windows CI |
| Real Publishing Tools / console install | **Not tested** | needs the owner's tools and a PS5 |

## Limitations

* **Not yet tested with real Publishing Tools or on a console.** The
  project format, PlayGo layout and header checks follow published behaviour
  of existing tools. The automated tests use a stand-in for
  `prospero-pub-cmd.exe`. Please report results with the build logs.
* Source must be a dump **folder**. Build from an `.exfat`/`.ffpkg` by
  extracting it first (Extract tab).
* File and folder names **inside** the game must be ASCII (the
  Publishing Tools' rule). A non-ASCII path **to** the game or output
  folder is handled on Windows with a temporary NTFS junction under an ASCII
  temp folder, removed afterwards.
* No PS4→PS5 SELF repair (PS4-magic executables are reported), no `.ucp`
  digest repair, no DDS→PNG recovery, no source-layout PlayGo (the dump's
  own chunk table is not reused).
* Building straight from an `.exfat` / `.ffpkg` image is not offered; extract
  it first (Extract tab).
