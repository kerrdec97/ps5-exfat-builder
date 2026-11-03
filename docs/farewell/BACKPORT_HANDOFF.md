# Backport GUI → exFAT Image Builder: read-only inspection brief

For a Claude session running **on the owner's PC** (the cloud session that
develops `farewell-edition-dev` cannot reach `D:\`). This step only
inspects; it changes nothing.

## Ground rules

* Source: `D:\fixing input\unity_input_gui`, branch
  `snapshot/20261021-f1-pro-v45`, with **seven uncommitted changes that must
  stay exactly as they are**.
* Read only. Do **not** run `git stash`, `checkout`, `reset`, `clean`,
  `pull`, `commit`, `add`, formatters or builds in that folder, and don't
  write files inside it. Write the report somewhere else (e.g.
  `D:\backport-inspection-report.md`).
* Don't copy code or binaries into the exFAT Image Builder yet. Anything
  bundled (e.g. a standard `right.sprx`) needs its origin and licence
  recorded first.

## 1. Record the state (proves nothing changed)

```
git -C "D:\fixing input\unity_input_gui" rev-parse --abbrev-ref HEAD
git -C "D:\fixing input\unity_input_gui" rev-parse HEAD
git -C "D:\fixing input\unity_input_gui" status --porcelain=v1
git -C "D:\fixing input\unity_input_gui" diff --stat
```

Also record the SHA-256 of each of the seven changed files
(`Get-FileHash -Algorithm SHA256`). Run the same commands again at the end;
the output must be identical.

## 2. Inventory

* Top-level layout, language, entry point, how the engine is packaged.
* Licence files and third-party notices; the licence of every bundled
  binary or data file (fakelib providers, `right.sprx`, PSSR files,
  firmware import tables).

## 3. Engine features to locate (file and function names)

The PS5 Backport GUI docs describe these; for each, give the files and
functions, inputs/outputs, external tools or data used, and any tests:

1. Import-table audit of executables against firmware 4.03–13.60.
2. The "confirmed fix families" and how a fix is chosen.
3. Audio check across firmware bands (issues #10, #11, #13, #28).
4. Build verification: ZIP CRC/hash re-read, signed-executable structure,
   fakelib set check.
5. "Already backported" detection.
6. Destination validation (backport/originals folders).
7. Handling of `sce_sys/about/right.sprx` "Invalid param magic 0x79A6EB9E"
   (issues #2, #7, #8): where the replacement file comes from.
8. Handling of a dump's own `fakelib` (issues #17, #27).
9. Invalid `param.json` SDK values.
10. PS5 Pro PSSR 2 test.
11. SDK 5–13 target support.

## 4. What the seven uncommitted changes do

One paragraph per file: what changed and why, as far as the diff shows.
Don't modify them.

## 5. Return

Paste the report (or its path) back into the cloud session. It will port
the engine checks into `farewell-edition-dev` with tests, keeping the
existing safeguards (see `docs/farewell/STATUS.md`).
