@echo off
echo Installing dependencies...
py -3.11 -m pip install pyinstaller pillow tkinterdnd2 mkpfs
echo.

if not exist "controller.ico" (
    echo WARNING: controller.ico not found in current folder.
    echo The build will continue without an icon.
    echo Place controller.ico next to this .bat to embed it.
    echo.
    set ICON_ARG=
) else (
    set ICON_ARG=--icon="controller.ico"
)

if not exist "exfat_builder.py" (
    echo ERROR: exfat_builder.py not found in current folder.
    pause
    exit /b 1
)

if not exist "ui\" (
    echo ERROR: ui\ folder not found in current folder.
    echo Make sure the ui\ subfolder is next to exfat_builder.py.
    pause
    exit /b 1
)

echo Building exFAT Image Builder.exe (Full version)...
py -3.11 -m PyInstaller --onefile --windowed --clean --noconfirm ^
    --name "exFAT Image Builder" ^
    %ICON_ARG% ^
    --hidden-import PIL._tkinter_finder ^
    --hidden-import mkpfs ^
    --hidden-import mkpfs.cli ^
    --hidden-import mkpfs.pfs ^
    --hidden-import mkpfs.utils ^
    --hidden-import mkpfs.consts ^
    --hidden-import mkpfs.logging ^
    --hidden-import mkpfs.pbar ^
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
    --collect-submodules ui ^
    --collect-submodules ui.shared ^
    --paths . ^
    exfat_builder.py
if errorlevel 1 (
    echo.
    echo BUILD FAILED for Full version.
    pause
    exit /b 1
)
echo.

if exist "exfat_builder_lite.py" (
    echo Building exFAT Image Builder Lite.exe...
    py -3.11 -m PyInstaller --onefile --windowed --clean --noconfirm ^
        --name "exFAT Image Builder Lite" ^
        %ICON_ARG% ^
        --hidden-import PIL._tkinter_finder ^
        exfat_builder_lite.py
    if errorlevel 1 (
        echo.
        echo BUILD FAILED for Lite version.
        pause
        exit /b 1
    )
) else (
    echo Skipping Lite build - exfat_builder_lite.py not found in current folder.
)

echo.
echo Done! Exes are in the dist\ folder.
echo.
echo Reminder: generate the SHA256 for the auto-updater:
echo   cd dist
echo   powershell -Command "$h=(Get-FileHash -Algorithm SHA256 'exFAT Image Builder.exe').Hash.ToLower(); \"$h  exFAT Image Builder.exe\" ^| Out-File -Encoding ASCII 'exFAT Image Builder.exe.sha256'"
echo.
pause
