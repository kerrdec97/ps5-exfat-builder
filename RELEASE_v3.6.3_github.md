🚀 What's New in v3.6.3

A major update focused on faster workflows, smarter progress tracking, improved extraction performance, and a cleaner user experience.

## ⭐ Highlights

### ⚡ Faster Existing-Image Conversions
Converting existing `.exfat` and `.ffpkg` images to `.ffpfsc` is now dramatically faster on NTFS drives.
- Uses instant hard-link staging where supported.
- Eliminates unnecessary full-image copies.
- Reduces temporary disk space requirements.
- Falls back automatically when hard-links are unavailable.

### 📊 Smarter Progress Tracking
Progress is now based on bytes copied rather than file counts.
- More accurate percentages.
- Better ETA calculations.
- Correct behaviour for large PS5 game files.
- No more progress bars reaching high percentages too early.

### 🎯 Format-Aware Build Phases
Build stages now match the selected output format.
- exFAT / ffpkg builds show 5 phases.
- ffpfsc builds show 6 phases.
- Removed inactive phases from formats that don't require them.

### 🚀 Optional Multithreaded Extraction
New Windows-only Robocopy extraction mode.
- Uses Robocopy `/MT` for faster extraction.
- Configurable thread count.
- Ideal for NVMe and SSD users.
- Classic extraction remains available.

## 🔧 Technical Improvements

**Build Finalization Visibility**
The UI now correctly reports finalization stages after copying completes.
- Finalizing image
- Verifying copied data
- Finishing disk writes
- Dismounting image
- Extraction complete

**Telemetry Improvements**
- Startup speed spikes filtered.
- Improved ETA stability.
- Better speed smoothing.
- File counters can no longer exceed totals.

**Universal mkpfs Tool Support**
The experimental mkpfs integration is now a generic external tool hook.
- Improved launcher detection.
- Better logging.
- Automatic settings migration.
- Compatible with multiple mkpfs-style tools.

**New Release Experience**
- Single release announcement per version.
- Startup donation popup removed.
- Integrated support section.
- Data-driven release notes for future updates.

## 💬 Feedback Wanted

This release includes significant changes to build staging, extraction, progress tracking, telemetry, and the overall user experience.

While every effort has been made to test these features, it's impossible to test every combination of hardware, storage device, filesystem, game dump, and workflow alone.

If you encounter bugs, unexpected behaviour, performance issues, compatibility problems, or simply have ideas for improvement, please share your feedback. Many of the features and fixes in exFAT Image Builder exist today because of suggestions and reports from the community.

Even if everything works perfectly, I'd love to hear your results—especially build times, extraction performance, and how the new features perform on your setup.

Thank you to everyone who tests, reports issues, suggests new ideas, and supports the project. Your feedback is one of the most important tools for improving exFAT Image Builder. ❤️

---

## 🙏 Feedback & Support

🐞 GitHub Issues
https://github.com/kerrdec97/ps5-exfat-builder/issues

💬 Discord
scottish_deckerr

📢 Telegram (Direct Contact)
@Deckerr97

👥 Telegram Community
https://t.me/ps5exfatbuilder
