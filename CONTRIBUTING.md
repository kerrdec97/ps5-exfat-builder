# Contributing

exFAT Image Builder 5 is proprietary software maintained by its owner
(`native/packaging/LICENCE.txt`). Bug reports, translations and proposals are
welcome as GitHub issues. Code changes are accepted only when the owner asks
for them, and they become part of the closed-source app.

Developers and coding agents: read [AGENTS.md](AGENTS.md) and
[native/docs/ARCHITECTURE.md](native/docs/ARCHITECTURE.md) first.

## Reporting a problem

* The app's version (About page) and Windows version.
* What you did, what you expected, what happened.
* The log: Logs page › Open log folder, or *Report a problem…* on the About
  page, which collects the logs and the settings for you to attach.
* For packages: the build log, not the game files. Never attach game data,
  keys or Sony tools.

## Translations

The interface languages are JSON catalogues in
`native/src/app/resources/lang/`. How to correct or add one, and the glossary
of words that stay English: [native/docs/I18N.md](native/docs/I18N.md).

## Changing code

1. Work on a feature branch (or your own worktree), never on `main`.
2. Build and test on Windows: [native/docs/TESTING.md](native/docs/TESTING.md).
3. A behavioural fix comes with a test that fails before the fix and passes
   after it. Use synthetic fixtures; never commercial game data.
4. Follow the code around you: C++20, `fw::Result<T>` / `fw::Status` and
   `FW_TRY` for errors, no exceptions across modules, the job rules in
   `ARCHITECTURE.md`, the design system in `native/src/app/ui/` (no colours
   outside `Theme`).
5. User-visible text goes through `FW_TR(...)` in British English, with
   "…" as one character. Regenerate `_source.json` (`FW_I18N_UPDATE=1` and
   `tst_i18n_source`) and say which texts are new.
6. Update the engine document in `native/docs/engines/` or the page document
   (`native/docs/UI.md`) when behaviour changes, and add a `CHANGELOG.md`
   entry under the next version.
7. Commit in small steps with descriptive messages (what changed and why).

## What never goes into the repository

* Sony software (Publishing Tools, DLLs), fpkg-cli, YouTube packages,
  private build output (`dist-private/`, `tools/fpkg-toolkit`,
  `tools/fpkg-cli`, `tools/youtube`).
* Game dumps, packages, images, keys or other commercial data.
* GPL or other copyleft code copied into the app. The app already contains
  ports of GPL-3.0 code, recorded in
  [native/docs/DEPENDENCIES.md](native/docs/DEPENDENCIES.md); adding more is
  the owner's decision.
* Unofficial binaries. Dependencies come from published releases, pinned by
  SHA-256.
* Logs, settings or test evidence from your own PC (the acceptance
  scripts' evidence folder is git-ignored).

## Releases

Only the owner merges to `main`, tags versions and publishes releases:
[native/docs/RELEASE_CHECKLIST.md](native/docs/RELEASE_CHECKLIST.md).
