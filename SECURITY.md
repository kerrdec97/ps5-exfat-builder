# Security policy

## Supported versions

Security fixes go into the newest release of exFAT Image Builder 5. Older
releases (5.0.x before the newest, and the Python versions 1 to 4) do not get
fixes: please update first and check whether the problem is still there.

## Reporting a vulnerability

Please **do not open a public issue** for a security problem.

Report it privately through GitHub instead:

1. Open the repository's **Security** tab.
2. Choose **Report a vulnerability**
   (direct link: <https://github.com/kerrdec97/ps5-exfat-builder/security/advisories/new>).
3. Describe the problem, the version and edition you used and the steps to
   reproduce it. A small, synthetic test file that shows the problem helps;
   never send game files, packages, keys or Sony tools.

Only the maintainer sees the report. You will get an answer as soon as
possible; this is a one-person project, so please allow some time. Once a
fix is released, the advisory can be published with credit to you if you
wish.

## What counts

For example: a crafted image, archive, package, game folder or network
response from the console that makes the app write outside the folders it
was given, run code, or crash in a way that could be exploited; the update
check or a download accepting a file that is not the pinned or signed one;
or a problem report leaking more than it says it does.

Not security problems: antivirus warnings about the unsigned exe (see
[docs/ANTIVIRUS.md](docs/ANTIVIRUS.md)), and anything that needs a console
exploit or jailbreak to work, which is outside this project.

## Checking a download

Every release lists the SHA-256 of its files in `SHA256SUMS.txt`. Download
the app only from the
[Releases page](https://github.com/kerrdec97/ps5-exfat-builder/releases) and
compare the hash before you run it (see the README).
