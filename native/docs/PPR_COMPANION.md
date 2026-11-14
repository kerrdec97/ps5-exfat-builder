# ppr-pkg Companion 1.5.1 in exFAT Image Builder

The owner's ppr-pkg Companion ("DualBackports") was a launcher that added
features to ppr-pkg (LibProsperoPkg.Gui). Its behaviour was read from its
embedded .NET add-on and ported into this app's own pages and engines.

## Features

| Companion | In this app | Engine |
|---|---|---|
| Backport tab: base package + Backport 4.xx / 5.xx+ folders + optional game folder + new version → `<TITLEID>-backport-4xx.pkg` / `-5xxplus.pkg` | Build image › FPKG › **Backport** (folder or the Backport tab's ZIP) | `engines/fpkg/BackportUpdate.*` (`fpkg.backport`), Sony's prospero-pub-cmd |
| Create tab "dual backports": one SDK base PKG + separate updates | Build image › FPKG › **App** with backport files | same job, `buildBase` |
| Base inspection (kind, digest at 0x30..0x4F, param.json), delta checks (LIH, base digest, CE-107891-6), remastered identity/version check, two-pass img_verify (integrity required, format diagnostic), `-2/-3` naming, `.naps_metric.json` kept | same, with the Companion's messages | `PackageInspect.*`, `verifyPublisherPackage` |
| SDK Fix toolchain verification (fix15/fix13/fix12 SHA-256 pairs, ppr-pkg DLL refused, 7 `ext\` files) | required for backport updates | `checkSdkFixToolchain` (Toolchain.cpp) |
| Publisher TEMP short-name alias / 120-character limit | every publisher run (`img_create`, `img_verify`, `img_extract`): a private folder under the job's temp folder | `publisherTempPath`, `publisherEnv` (FpkgBuilder.cpp) |
| Process priority copied to Sony's publisher | Performance mode, on every publisher run, with the CPU-core limit (also the one changed while the job runs) | `applyProcessPriority` (core), `applyJobLimits` (FpkgBuilder.cpp) |
| Pause / Resume of a running build | the queue's Pause: the publisher and its child processes are suspended until Resume | `runPubTool` (PubCmd.cpp) |
| DLC Converter tab (INIs/PKGs, edit, icon0, Change region…, Preview, Convert, Save updated INIs, results report) | **DLC** page | `engines/dlc/*` (`dlc.convert`), fpkg-cli |
| DLC region lookup (PlayStation Store, SerialStation, ProsperoPatches, 7-day cache) | Change region… and the region line | `DlcLookup.*` (cache in the app's cache folder) |
| Backport files from the Backport tab | right-click a backported game › Package as an installable update | navigates to Build › Backport |

## Settings

| Companion setting | In this app |
|---|---|
| Performance mode (Low impact / Balanced / Maximum) | Settings › Performance › **Performance mode** (CPU, disk and memory priority of Sony's publisher and fpkg-cli; cores when "CPU threads per job" is 0). Default here: Maximum (the app's previous behaviour); the Companion's default was Balanced. |
| Compression workers (Automatic / All / All but one / Half / 8 / 4 / 2) | Settings › Performance › CPU threads per job (0 = automatic from the mode) |
| All 31 PlayGo languages | Settings › PlayGo › **All 31 PlayGo languages** (off by default; the Companion ticked it by default) |
| attributePub (SDK Fix rule): Report only / Apply | Settings › FPKG › Choose the package size class (attributePub, SDK Fix rule): on = apply, off = leave the dump's values |
| [DlcConverter] OutputFolder | Settings › DLC › Output folder (also the DLC page's Save to) |
| DRM (Free / Additional content), Entitlement key, PKG source passcode | DLC page › Options; the key and the passcode can also be kept in Settings › DLC keys (`dlc.entitlementKey`, `dlc.sourcePasscode`, stored in settings.json; a problem report hides them). fpkg-cli always builds Additional content (entitlement) DLC; Free is not offered. |
| ppr-pkg Create-tab engine fields used by the DLC tab (Kraken level, Threads, PFS format, image mode, Kraken backend, SDK, deterministic) | Settings › DLC › Engine (also per job under Advanced options) |
| [Backport] OutputFolder | the Build page's Save to folder (default: the base package's folder) |
| [SdkFix] Enabled | always on: this app's FPKG engine is the GP5 + Sony publisher pipeline |
| PubToolsDll (libScePubTools for ppr-pkg's Kraken) | not needed: fpkg-cli uses its own copy or the built-in encoder |
| Dark mode, engine folder/manifest check, CloseAfterBuild, PprPkgWindow, StageRoot/StageCopyFallback, VerifyOriginal | not applicable: they concern hosting ppr-pkg's window or its hard-link staging; this app never stages or modifies the game folder |
| Build queue | the app's own Queue page |

## Known differences

* Like the Companion, an update keeps the base package's PlayGo chunk count
  and languages (read from the base's playgo-chunk.dat by the app itself, not
  through LibProsperoPkg), whatever the PlayGo settings say. If that table
  cannot be read, the update falls back to the game folder's own PlayGo layout
  and the log says so. A base the job builds itself (dual backport) gets the
  generated layout its updates get, never the game folder's own PlayGo table
  (Settings › PlayGo › use the game's own table), so base and updates put the
  game's files in the same chunks.
* Cancel while the publisher checks a finished package (the base it built, or
  an update's delta and its full image) keeps that package under its name in
  the output folder and finishes the job as with verification off: nothing
  more is built, the outcome lists the packages verified so far and the kept
  one, its verdict is *Skipped* ("Verification was cancelled, so the package
  is not verified.", `details.verificationCancelled`), a warning names the
  kept package, and the queue records Succeeded. Only the full image, a
  by-product of the checks, goes. A problem img_verify reported before the
  cancel still fails the check.
* A base package whose digest field (0x30..0x4F) is empty or a repeated
  pattern is refused: every file "references" such a digest, so the
  CE-107891-6 check would prove nothing.
* Without a game folder the base package is extracted into a work folder next
  to the outputs (or under the temporary folder when one is set in Settings ›
  Storage), not into the app's own data folder.
* fpkg-cli cannot build entitlement-only (no data) DLC or DLC with Free DRM;
  such packages are listed and can be exported as updated INIs, but not
  rebuilt.
* The Companion verified the base before each update; here it is verified
  once per job while it is held open read-only for the whole job.
