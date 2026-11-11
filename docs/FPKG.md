# FPKG packages (PS5 debug packages)

The **Build** page can turn a PS5 game dump into an installable debug package
(`.pkg`). Choose **FPKG** as the format. The app prepares the project and
drives your own copy of Sony's Publishing Tools, which do the packing.

## What you need

| What | Why |
|---|---|
| `prospero-pub-cmd.exe` from Publishing Tools you are entitled to use | It packs (`img_create`), checks (`img_verify`) and unpacks (`img_extract`). The app does not include or download any Sony software. Choose the `.exe`, its folder, or a toolkit folder with `toolchain\` or `bin\` in **Settings › Tools**. The `SCE_PROSPERO_PUBTOOLS` environment variable is used when the setting is empty. |
| Publishing Tools that make plaintext debug packages | A package without the plaintext debug marker cannot be installed on a debug console. The app checks every package for it and reports a package without it as failed. |
| Microsoft Visual C++ 2015-2022 Redistributable (x64) | The Publishing Tools need it. The app tells you when it is missing. |
| A dump with `sce_sys\param.json` and its own 96-byte `sce_sys\keystone` | The Content ID, version and title come from `param.json`; the tools need the dump's keystone. |
| For backport packages: a verified SDK Fix toolchain (`sdk-fpkg279-fix15` recommended; `fix13` and `fix12` accepted, but they write SDK version 0 into packages) | Updates against a base package need it. The app checks the files and says what is wrong. |
| Free space on the output drive | About 1.1 times the game's size (twice that for an update), plus the game's size again when the source is an image. |

## Building a package

1. On the **Build** page choose the source and the format **FPKG**.
2. Under **Package** choose the **Package type** and fill in its fields (below).
3. Choose the folder under **Save to**, or leave it empty.
4. Start the build. It runs as a job in the queue; the log shows each step.

### App

The game itself. The source can be:

* a game folder (the folder that holds `sce_sys`);
* an image: `.exfat`, `.ffpkg`, `.ffpfs` or `.ffpfsc`. Its files are copied to
  a temporary work folder next to the output first (or to the temporary
  folder from **Settings › Storage**, when one is set), and removed afterwards;
* a `.gp5` project of your own. It is passed to the tools as it is; its
  version and title cannot be changed on the page.

**Content ID** is read from `param.json`. **Version** and **Title** can be
changed for the package; the dump is never modified.

You can add backport files (4.xx and/or 5.xx+) to an App build. The job then
builds the base package and the backport updates in one go.

### Add-on

Additional content (DLC) with data, volume type `prospero_ac`. The source is
the add-on's own folder (for example `addcont0`), not the game. It must hold
the add-on's own `sce_sys\param.json`. Enter the 32-character hexadecimal
**Entitlement key**, or use **From dlc_emu.ini…** to pick an add-on the game's
`dlc_emu.ini` lists. Add-ons without data (`prospero_al`) are not supported.

Add-on packages are experimental: they have not been tested on a console.

### Update

An update for a game that is already installed. The source is the folder with
the new or changed files (its `param.json` carries the new version, or you
enter the version on the page). **Base package** is the `.pkg` the console
already has. It must have been built by the Publishing Tools, with the same
Content ID and a lower version.

The build writes `UPDATE_<name>.pkg` (the update to install) and
`<name>.remastered.pkg` (the full game with the update, used for checking).
Install the base package first, then the update.

Files the update folder does not have are taken from the original game
folder when you give one; otherwise the base package is unpacked first, which
needs about twice its size in free space.

### Backport

Backport files as their own small update that installs over the base package,
so the base stays the original game. Choose the **Base package**, the
**Backport 4.xx** and/or **Backport 5.xx+** files (a folder or a ZIP, with the
files laid out as in the game's top folder: `eboot.bin`, `sce_module\`,
`fakelib\` …) and, if you have it, the game folder the base was built from.

The result is `<TITLEID>-backport-4xx.pkg` and/or
`<TITLEID>-backport-5xxplus.pkg`. The two are alternatives: install the base
package first, then the one update that fits your firmware. An update only
installs over exactly the base package it was built against.

## Where the output goes

* The folder under **Save to**. When it is empty: the default output folder
  from **Settings › Storage**, else `<source>-pkg` next to the source. Backport
  packages go next to the base package by default.
* A full package is named `<Content ID>-A<version>-V<version>.pkg`.
* `<name>-build-logs\` next to the package holds `01-create-gp5.log` (what was
  packaged, what was left out and why) and `02-img-create.log` (the tool's
  run). The logs are always kept.
* When a package of that name already exists, **Settings › FPKG › When the
  package already exists** decides: **Keep both** renames the old one to
  `name (1).pkg`; **Overwrite** replaces it only after the new package passed
  its checks (if it does not, the old one is put back and the new one is kept
  as `name (1).pkg`); **Stop with an error** refuses the build before packing.
  Backport packages never replace a file: a second build is named `…-2.pkg`.

## Checks

* Before packing: the tools, `param.json`, the keystone, file names the tools
  do not accept (non-ASCII characters, `%`, `;`, a name ending in a dot),
  links, names that differ only in letter case, and free space.
* After packing: the package header and the plaintext debug marker, always.
  Then `img_verify`, unless you turned it off under **Settings ›
  Verification**. When `img_verify` reports a problem the package is kept and
  the job is marked with a warning. Backport packages must pass the integrity
  check, or nothing is published.
* Left out of every package, and listed in the log: files the tools generate
  themselves (`param.sfo`, PlayGo tables, `sce_sys\about`, `.dds` …), image
  and package files in the game's top folder, dump helper libraries, and
  operating-system clutter (`Thumbs.db`, `.DS_Store` …).

**Pause** suspends the Publishing Tools until you resume. **Cancel** stops
them at once and removes the unfinished package; the logs stay.

## Limits

* File and folder names inside the game must be plain ASCII (a rule of the
  Publishing Tools). A non-ASCII path to the game or output folder is fine:
  the app gives the tools a temporary ASCII link.
* A FAT32 drive cannot hold a package of 4 GB or more. Use NTFS or exFAT.
* The base package of an update or backport must be a debug package built by
  the Publishing Tools. Retail (encrypted) packages cannot be used.
* Add-ons need their own `param.json` and entitlement key; the app does not
  make them up.
* The app cannot test a package on a console. Keep the build logs when you
  report a problem.
