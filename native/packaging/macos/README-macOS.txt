exFAT Image Builder for macOS
==============================

Drag "exFAT Image Builder" into Applications.

The app is not signed by Apple, so the first time macOS refuses to open it. Open it once,
then go to System Settings > Privacy & Security, scroll down and click "Open Anyway".
macOS asks once more; after that it opens normally.

When you choose a game folder on the Desktop, in Documents or Downloads, or on an
external or network drive, macOS asks whether the app may read it: allow it, or the app
cannot see the files. PS5 Connect asks to find devices on your local network.

Settings, the queue, history and logs go to ~/Library/Application Support/exFAT Image Builder.

Most of the app works as on Windows. A few parts depend on other programs:

  * FPKG packages need Sony's Publishing Tools (a Windows program). They run through Wine:
    PSVIETHOA FPKG Builder's macOS download includes Wine. On Apple Silicon Macs, Wine
    needs Rosetta 2.
  * DLC packages need fpkg-cli. PSVIETHOA FPKG Builder's macOS download includes it
    (the fpkg-cli folder next to its app): point Settings > Tools > DLC engine at it.
  * After the queue the Mac can sleep, shut down or restart (no hibernate on macOS).

The macOS build is new. If something does not work, open an issue and say which macOS
version and Mac you use: https://github.com/kerrdec97/ps5-exfat-builder/issues

LICENCE.txt, THIRD_PARTY_NOTICES.md and the LICENSES folder are inside the app
(right-click > Show Package Contents > Contents > Resources).
