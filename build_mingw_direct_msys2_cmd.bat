@echo off
set MSYS2=C:\msys64
if not exist "%MSYS2%\mingw32\bin\g++.exe" (
  echo %MSYS2%\mingw32\bin\g++.exe not found
  echo Install it in MSYS2: pacman -S --needed mingw-w64-i686-gcc
  pause
  exit /b 1
)
set PATH=%MSYS2%\mingw32\bin;%PATH%
py -3 build_mingw_direct.py --clean
pause
