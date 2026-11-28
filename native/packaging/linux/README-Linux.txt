exFAT Image Builder for Linux
==============================

Two ways to run it:

  * The AppImage: make it executable once (chmod +x exFAT-Image-Builder-linux-*.AppImage)
    and double-click it, or start it from a terminal.
  * This folder (the .tar.gz): run ./exfat-image-builder. Use it when your system has no
    FUSE for AppImages, or when you want to keep everything in one folder.

Settings, the queue and history go to ~/.config/exfat-image-builder; logs, cache and the
temporary folder to ~/.local/share/exfat-image-builder. Portable mode: put an empty file
called portable.txt next to the AppImage (or into this folder) and everything stays in a
"data" folder beside it.

Most of the app works as on Windows. A few parts depend on other programs:

  * FPKG packages need Sony's Publishing Tools (a Windows program). They run through Wine:
    install Wine from your distribution, then point Settings > Tools at the tools.
  * DLC packages need fpkg-cli. PSVIETHOA FPKG Builder's Linux download includes it
    (fpkg-cli/fpkg-cli): point Settings > Tools > DLC engine at it.
  * Sleep or shut down after the queue uses systemd (systemctl).

The Linux build is new. If something does not work, open an issue and say which
distribution and desktop you use: https://github.com/kerrdec97/ps5-exfat-builder/issues

LICENCE.txt, THIRD_PARTY_NOTICES.md and the LICENSES folder are in usr/bin.
