# Third-party notices — exFAT Image Builder, Farewell Edition (native)

The native application is licensed under the GNU General Public License,
version 3 or later (LICENSES/GPL-3.0.txt). It contains or links the
following third-party components. Full licence texts are in LICENSES/.

| Component | Use | Licence |
|---|---|---|
| Qt 6 (Core, Gui, Widgets, Svg, Network, Concurrent) | Application framework, dynamically linked | LGPL-3.0 (https://www.qt.io/licensing). Qt source: https://download.qt.io/official_releases/qt/ |
| zlib | Compression | zlib licence |
| Lazy_MkPFS / PSBrew MkPFS | The PFS/FFPFSC engine is a C++ port of this code | GPL-3.0 |
| UFS2Tool (SvenGDK) | The UFS2/FFPKG engine follows its image layout | BSD-2-Clause (LICENSES/UFS2Tool-BSD-2-Clause.txt) |
| make_fself (john-tornblom, modified by Nazky) | The Backport engine's fake-SELF writer/reader (`SelfFormat.h`, `SelfWriter.*`, `SelfReader.*`) is a port | GPL-3.0 |
| LibProsperoPKG (SvenGDK) | Facts used by the FPKG engine: PS5 flat-path hash, UCP layout | GPL-3.0-or-later |
| Lucide icons | Interface icons | ISC (LICENSES/Lucide-ISC.txt) |
| Microsoft Visual C++ runtime (`vcruntime140*.dll`, `msvcp140*.dll`, ...) | Shipped next to the program inside the single-file exe | Microsoft Visual Studio redistributable files ("Distributable Code") |

No Sony software is included. FPKG packages are built with the user's own
Publishing Tools, located through Settings > Tools.
