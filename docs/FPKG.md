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

## Limitations

* **Not yet tested with real Publishing Tools or on a console.** The
  project format, PlayGo layout and header checks follow published behaviour
  of existing tools. The automated tests use a stand-in for
  `prospero-pub-cmd.exe`. Please report results with the build logs.
* Source must be a dump **folder**. Build from an `.exfat`/`.ffpkg` by
  extracting it first (Extract tab).
* Paths must be ASCII; there is no automatic junction workaround yet.
* No DLC (`prospero_ac`) packages, no PS4→PS5 SELF repair (PS4-magic
  executables are reported), no `.ucp` digest repair, no DDS→PNG recovery.
