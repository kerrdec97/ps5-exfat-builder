[README (1).md](https://github.com/user-attachments/files/27017323/README.1.md)
# PS5 exFAT Image Builder

A Windows GUI tool for building exFAT disk images from PS5 game folders
**Created by DecKerr97**

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
![License](https://img.shields.io/badge/license-MIT-green)

---

## What does it do?

Converts a PS5 game folder (containing `eboot.bin`) into an `.exfat` disk image that can be loaded by etaHEN on a jailbroken PS5. It handles the entire process — sizing, mounting, formatting and copying — automatically.

---

## Requirements

Your friends only need **one thing** installed:

- **[OSFMount by PassMark](https://www.osforensics.com/tools/mount-disk-images.html)** — free, used to mount the disk image during building

That's it. No Python, no command line, nothing else. Just OSFMount + the `.exe`.

**System:** Windows 10 or 11 (64-bit)

---

## Download & Install

1. Go to the [**Releases**](../../releases) page
2. Download `exFAT Image Builder.exe`
3. Install [OSFMount](https://www.osforensics.com/tools/mount-disk-images.html)
4. Double-click the `.exe` — click **Yes** on the admin prompt
5. Done

---

## How to use it

### Building a game image

1. **Select game folder** — the folder containing `eboot.bin` (e.g. `F:\PPSA03596-app`)
   - The app automatically detects the game title, version and cover art from `param.sfo`, `pfs-version.dat`, `param.json` or `nptitle.dat`
   - The output filename is filled in automatically e.g. `God of War Ragnarok (01.000.000).exfat`
2. **Select output directory** — where to save the `.exfat` file (remembered between sessions)
3. Click **+ Add to Queue** — repeat for as many games as you want
4. Click **Build All**

### Sending to PS5 via FTP

1. Make sure the PS5 FTP server is running (enabled in etaHEN/GoldHEN settings)
2. Enter your PS5's IP address and port (`2122` is the GoldHEN default, `2121` for etaHEN)
3. Set the PS5 path (default: `/data/etaHEN/games/`)
4. Click **Test Connection** to verify
5. After a build completes, click **↑ PS5** on the queue item — or enable **Auto-upload after build**

---

## Features

### Game Detection
- Reads `param.sfo`, `pfs-version.dat`, `param.json`, `nptitle.dat` for game title and version
- `pfs-version.dat` gives the real installed patch version (e.g. `01.007.000`) — not just the base app version
- Falls back to extracting the PPSA/CUSA ID from the folder name
- Displays game cover art from `icon0.png`
- Shows estimated image size before building

### Build Queue
- Add multiple games, build them all in one click
- Duplicate game folder detection
- Disk space check before building
- Right-click any queue item: open source folder, open output folder, rebuild, upload to PS5, remove
- Build single item from right-click menu
- Build history saved to `~/.exfat_builder_history.json`

### Progress & ETA
- Real-time progress bar with stage indicators: Mount → Format → Copy files → Dismount
- During copy phase: reads free space directly from the mounted drive every second
- Shows: elapsed time, GB written, GB remaining, MB/s speed, ETA
- FTP uploads show the same stats: GB sent, GB remaining, speed, ETA with 5-second rolling average

### FTP / PS5
- Upload finished images directly to your PS5 over FTP
- Cancel upload at any time
- PS5 file browser — navigate, browse and delete files on your PS5
- Ping PS5 — quick check if the PS5 is reachable before uploading
- Auto-upload option — prompts to upload after each build

### Settings
- **Temp folder** — point to an external drive if your system drive is low on space. The app builds the image here before saving it to your output directory. You need enough free space for the full game size (e.g. a 50 GB game needs 50 GB free in the temp folder)
- **Clear temp files** — removes leftover `exfat_builder_*` folders
- All settings saved automatically between launches

### Other
- Drag and drop game folders onto the app
- Collapsible output log (auto-expands when a build starts)
- Black background, white text — easy to read
- `[` `]` bracket-safe — handles folder names like `[DLPSGAME.COM]-PPSA12345-app`
- Auto-elevates to Administrator on launch (required for OSFMount)

---

## Output filename format

```
Game Title (01.000.000).exfat
```

Examples:
```
God of War Ragnarok (02.001.000).exfat
ASTRO BOT (01.007.000).exfat
Phantom Breaker Battle Grounds Ultimate (01.000.000).exfat
PPSA08804 (01.000.000).exfat   ← when no title found
```

---

## Game metadata — how does it know the game name?

It reads files **inside the game folder itself** — it does not connect to the internet or look anything up online.

Priority order for version:
1. `sce_sys/pfs-version.dat` — real installed patch version
2. `sce_sys/param.sfo` VERSION key
3. `sce_sys/param.sfo` APP_VER key
4. `sce_sys/param.json` version field

Priority order for title:
1. `sce_sys/param.sfo` TITLE key
2. `sce_sys/param.json` titleName field
3. `sce_sys/nptitle.dat`
4. PPSA/CUSA ID extracted from the folder name

The title ID (PPSA/CUSA) always comes from Sony's own metadata files inside the dump — not from a web database. If a game is correctly extracted it will always match what PlayStation shows.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `Source directory not found` | Folder name has `[` `]` — rename it to remove brackets |
| Build fails exit code 1 | `eboot.bin` not found in the selected folder |
| OSFMount error | Make sure OSFMount is installed and the app is running as Administrator |
| Game not auto-named | Check that `sce_sys/param.sfo` or `sce_sys/param.json` exists in the game folder |
| Version shows `01.000.000` for everything | `pfs-version.dat` may be missing — this is normal for some extraction tools |
| FTP won't connect | Make sure the PS5 FTP server is running in etaHEN/GoldHEN settings |
| Build All button not visible | Resize the window taller |
| Cover art not showing | Recompile with `build.bat` which installs Pillow |

---

## Building from source

```
git clone https://github.com/YOUR_USERNAME/ps5-exfat-builder
cd ps5-exfat-builder
```

Run `build.bat` — it installs PyInstaller + Pillow and compiles the exe:
```
build.bat
```

Or manually:
```
pip install pyinstaller pillow
py -m PyInstaller --onefile --windowed --name "exFAT Image Builder" --uac-admin --icon="controller.ico" --hidden-import PIL --hidden-import PIL.Image --hidden-import PIL.ImageTk exfat_builder.py
```

The `.exe` will be in `dist\`.

---

## Project files

```
ps5-exfat-builder/
├── exfat_builder.py       # Full source — single file app
├── controller.ico         # App icon
├── build.bat              # Compile to .exe
├── README.md
├── LICENSE
└── .gitignore
```

`make_image.bat` and `New-OsfExfatImage.ps1` are embedded inside `exfat_builder.py` as base64 and extracted to a temp folder at runtime.

---

## Credits

- **DecKerr97** — tool author
- **[NookieAI](https://github.com/NookieAI)** — inspiration for the project
- **stonedmodder** — inspiration from Porkfolio
- [OSFMount by PassMark](https://www.osforensics.com/tools/mount-disk-images.html) — disk image mounting
- [ShadowMountPlus](https://github.com/LightningMods/ShadowMountPlus) — the `make_image.bat` / `New-OsfExfatImage.ps1` scripts this tool wraps

---

## License

MIT — see [LICENSE](LICENSE) for details.

> **Disclaimer:** This tool is for use with game backups you legally own. The authors are not responsible for misuse.
