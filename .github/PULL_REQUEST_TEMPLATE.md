## What this changes

<!-- One or two sentences: what and why. Link the issue: "Fixes #123". -->

## How it was tested

<!-- Which tests you added or ran (ctest -R ...), and what you tried in the app.
     Results from a real PS5 are welcome; say which firmware. Mock or emulated
     results are fine too, but please label them as such. -->

## Checklist

- [ ] Builds with MSVC 2022 and Qt 6.10.3 (`native/`), and `ctest` passes with `FW_OFFLINE=1`.
- [ ] A behavioural fix comes with a test that failed before the fix.
- [ ] User-visible text goes through `FW_TR` (British spelling) and `_source.json` is regenerated.
- [ ] No game files, packages, keys, Sony tools or other copyrighted data in the change or the tests (synthetic fixtures only).
- [ ] No code copied from GPL or other copyleft projects (the app is closed source); ideas and formats are described and credited instead.
- [ ] `CHANGELOG.md` / `native/RELEASE_NOTES.md` updated if users will notice the change.

Note: the app is closed source (`native/packaging/LICENCE.txt`). The CI build
of this pull request (Actions) is a test build only, never a release.
