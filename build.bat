@echo off
echo Installing dependencies...
pip install pyinstaller pillow

echo.
echo Building exFAT Image Builder.exe...
py -m PyInstaller --onefile --windowed --name "exFAT Image Builder" --uac-admin --icon="controller.ico" --hidden-import PIL --hidden-import PIL.Image --hidden-import PIL.ImageTk exfat_builder.py

echo.
echo Done! Your exe is in the dist\ folder.
pause
