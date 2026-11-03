@echo off
setlocal EnableExtensions
REM ===========================================================================
REM  exFAT Image Builder - release build (Farewell Edition)
REM
REM  Produces dist\exFAT Image Builder.exe plus the .sha256 companion file the
REM  in-app auto-updater verifies downloads against.
REM
REM  The build runs in an isolated virtual environment (.build-venv) that only
REM  contains the pinned packages from requirements-build.txt, so the exe
REM  never picks up unrelated packages that happen to be installed on the
REM  build machine (v4.0.2 accidentally bundled numpy, PyYAML and
REM  charset_normalizer this way).
REM
REM  Requirements: Windows 10/11, Python 3.11 64-bit available as "py -3.11".
REM  Options:     build.bat clean   - recreate .build-venv from scratch
REM ===========================================================================
cd /d "%~dp0"

set "PY=py -3.11"
%PY% -c "import sys; assert sys.version_info[:2]==(3,11)" 1>nul 2>nul
if errorlevel 1 (
    echo ERROR: Python 3.11 was not found ^(tried "py -3.11"^).
    echo Install 64-bit Python 3.11 from python.org and re-run.
    goto :fail
)

for %%F in (exfat_builder.py tkinter_theme.py requirements-build.txt) do (
    if not exist "%%F" (
        echo ERROR: %%F not found next to build.bat.
        goto :fail
    )
)
if not exist "ui\" (
    echo ERROR: ui\ folder not found next to exfat_builder.py.
    goto :fail
)
if not exist "vendor\lazy_mkpfs\main_file.py" (
    echo ERROR: vendor\lazy_mkpfs is missing - the PFS backend cannot be bundled.
    goto :fail
)

if /i "%~1"=="clean" if exist ".build-venv\" (
    echo Removing old build environment...
    rmdir /s /q ".build-venv"
)

if not exist ".build-venv\Scripts\python.exe" (
    echo Creating isolated build environment in .build-venv ...
    %PY% -m venv .build-venv || goto :fail
)
set "VPY=.build-venv\Scripts\python.exe"

echo Installing pinned dependencies...
"%VPY%" -m pip install --disable-pip-version-check -q --upgrade pip || goto :fail
"%VPY%" -m pip install --disable-pip-version-check -q -r requirements-build.txt || goto :fail
echo.

echo Running release checks (compile, bundled files, version, unit tests)...
"%VPY%" tools\release_check.py || goto :fail
echo.

"%VPY%" tools\make_version_info.py build\version_info.txt || goto :fail

set "ICON_ARG="
if exist "controller.ico" (
    set "ICON_ARG=--icon=controller.ico"
) else (
    echo WARNING: controller.ico not found - building without an icon.
)

echo Building exFAT Image Builder.exe ...
REM --noupx: UPX-compressed executables are a frequent source of antivirus
REM          false positives; the app never used UPX intentionally.
"%VPY%" -m PyInstaller --onefile --windowed --clean --noconfirm --noupx ^
    --name "exFAT Image Builder" ^
    %ICON_ARG% ^
    --version-file build\version_info.txt ^
    --paths . ^
    --paths vendor\lazy_mkpfs ^
    --add-data "vendor\lazy_mkpfs;vendor\lazy_mkpfs" ^
    --add-data "assets;assets" ^
    --add-data "CHANGELOG.md;." ^
    --add-data "THIRD_PARTY_NOTICES.md;." ^
    --add-data "LICENSES;LICENSES" ^
    --add-data "controller.ico;." ^
    --hidden-import PIL._tkinter_finder ^
    --hidden-import psutil ^
    --hidden-import mkpfs ^
    --hidden-import mkpfs.__main__ ^
    --hidden-import mkpfs.cli ^
    --hidden-import mkpfs.pfs ^
    --hidden-import mkpfs.utils ^
    --hidden-import mkpfs.consts ^
    --hidden-import mkpfs.logging ^
    --hidden-import mkpfs.pbar ^
    --hidden-import lazy_mkpfs ^
    --hidden-import isal ^
    --hidden-import zlib_ng ^
    --hidden-import cryptography ^
    --hidden-import cryptography.hazmat.primitives.ciphers ^
    --hidden-import cryptography.hazmat.primitives.ciphers.algorithms ^
    --hidden-import cryptography.hazmat.primitives.ciphers.modes ^
    --hidden-import cryptography.hazmat.backends ^
    --hidden-import cryptography.hazmat.backends.openssl ^
    --hidden-import cryptography.hazmat.backends.openssl.backend ^
    --collect-all mkpfs ^
    --collect-all cryptography ^
    --collect-all tkinterdnd2 ^
    --collect-submodules lazy_mkpfs ^
    --collect-submodules ui ^
    --collect-submodules ui.shared ^
    --exclude-module numpy ^
    --exclude-module yaml ^
    --exclude-module charset_normalizer ^
    --exclude-module pytest ^
    exfat_builder.py
if errorlevel 1 (
    echo.
    echo BUILD FAILED for the full version.
    goto :fail
)
echo.

if exist "exfat_builder_lite.py" (
    echo Building exFAT Image Builder Lite.exe ...
    "%VPY%" -m PyInstaller --onefile --windowed --clean --noconfirm --noupx ^
        --name "exFAT Image Builder Lite" ^
        %ICON_ARG% ^
        --hidden-import PIL._tkinter_finder ^
        --hidden-import psutil ^
        exfat_builder_lite.py
    if errorlevel 1 (
        echo.
        echo BUILD FAILED for the Lite version.
        goto :fail
    )
) else (
    echo Skipping Lite build - exfat_builder_lite.py not found.
)

echo.
echo Writing SHA256 companion file for the auto-updater...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$f='dist\exFAT Image Builder.exe'; $h=(Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash.ToLower(); ('{0}  exFAT Image Builder.exe' -f $h) | Out-File -Encoding ASCII -LiteralPath ($f + '.sha256'); Write-Host ('SHA256: ' + $h)"
if errorlevel 1 (
    echo WARNING: could not write the .sha256 file - generate it manually.
)

echo.
echo Done. Release files are in the dist\ folder:
dir /b dist
echo.
echo Upload BOTH "exFAT Image Builder.exe" and "exFAT Image Builder.exe.sha256"
echo to the GitHub release so the auto-updater can verify the download.
echo.
pause
exit /b 0

:fail
echo.
echo Build stopped.
pause
exit /b 1
