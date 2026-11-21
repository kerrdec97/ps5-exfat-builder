# Antivirus warnings

Some antivirus programs, Microsoft Defender among them, now and then flag or
remove `exFAT Image Builder.exe`. This page explains why that can happen, how
to check that your copy is the real one, and what to do instead of turning
your protection off.

## Why it can happen

* **The exe is not code-signed.** A signature tells Windows who made a
  program; unsigned programs from small projects have no reputation yet, so
  SmartScreen and some engines treat them with more suspicion.
* **The single-file exe unpacks itself.** On first start it unpacks the app
  and the bundled PS5 Backport engine into your user folder
  (`%LOCALAPPDATA%\kerrdec97\exFAT Image Builder\runtime`). Malware does
  that too, so heuristic and machine-learning detections (names such as
  `Wacatac` or anything ending in `!ml`) sometimes fire on it.
  These names are guesses from the program's behaviour, not matches with
  known malware.
* **A new release is new to every scanner.** Detections often disappear
  within days, once the vendors have looked at the file.

None of this means a flagged file is safe. Check it as below.

## 1. Check that your copy is the real one

Download the app **only** from the
[Releases page](https://github.com/kerrdec97/ps5-exfat-builder/releases).
Each release has `SHA256SUMS.txt` listing the SHA-256 hash of every file.
In PowerShell, in the folder of the download:

```powershell
Get-FileHash ".\exFAT.Image.Builder.exe" -Algorithm SHA256
```

The hash must be exactly the one in `SHA256SUMS.txt` (capital and small
letters make no difference). If it is different, **do not run the file**:
delete it and download it again from the Releases page. If it is still
different, please report it privately (see [SECURITY.md](../SECURITY.md)).

You can also upload the file to [VirusTotal](https://www.virustotal.com) to
see what many engines say about it. A few heuristic hits on an unsigned,
self-unpacking program are common; many engines naming the same malware
family are not, and are a reason not to run it.

## 2. Use the folder edition

Each release also offers the **folder edition**
(`exFAT-Image-Builder-<version>-win64-folder.zip`). It is the same program,
already unpacked: nothing unpacks itself on first start, which is what most
heuristic detections react to. Check the zip's hash against
`SHA256SUMS.txt` too, extract it anywhere and run
`exFAT Image Builder.exe` inside.

## 3. Report the false positive

If you have checked the hash and your antivirus still flags the file, please
tell the antivirus vendor, so the detection is fixed for everyone:

* **Microsoft Defender:** submit the file at
  <https://www.microsoft.com/en-us/wdsi/filesubmission>. Choose *Home
  customer* (or *Enterprise customer* on a work PC), select the product that
  flagged it, and pick *Incorrectly detected as malware/malicious*. Give the
  detection name and say it is the release from
  `https://github.com/kerrdec97/ps5-exfat-builder/releases`. Microsoft
  usually answers within a few days; once it has, update Defender's
  definitions (Windows Security > Virus & threat protection > Protection
  updates > Check for updates) and download the file again.
* **Other vendors** have their own false-positive forms; search for
  "&lt;vendor name&gt; false positive submission". VirusTotal shows which
  engines flagged the file.

You are also welcome to open an issue with the detection name, the
antivirus and the version of the app (no need to attach the file), so
others with the same warning can find it.

## What not to do

* **Do not turn off your antivirus, real-time protection or SmartScreen,
  and do not add exclusions** for the app or its folders. That would also
  let real malware through, and none of it is needed: the folder edition and
  a false-positive report solve the problem without lowering your
  protection.
* Do not download the app from other sites, re-uploads or "unlocked"
  versions. Only the files on the Releases page match `SHA256SUMS.txt`.
