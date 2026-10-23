@echo off
REM Copies EZ2AC_FNEX to the XP cabinet's disk, leaving out what cannot or must not run there.
REM   deploy_to_cabinet.bat "G:\000 - MAIN" "F:\WINDOWS"
REM   %1 = destination folder, %2 = the cabinet's Windows folder (for GdiPlus.dll; optional)
REM Safe to run again: robocopy only copies what changed.
REM
REM Left out:
REM   ddraw.dll, d3dimm.dll, dgVoodoo*    dgVoodoo needs Vista+; on XP the game uses the real DirectDraw
REM   ffmpeg-modern.exe                   Vista+ build; the mod would pick it for BGA conversion and fail.
REM                                       ffmpeg.exe (2008, XP) stays as the fallback.
REM   Video\bms*                          per-song BGA hardlinks; a copy would be ~23 MB per song, and
REM                                       the mod recreates them on the first launch
REM   BMSData_backup_*, logs, *.bak*      backups and this PC's logs
REM Added:
REM   gdiplus.dll                         XP keeps GDI+ only in WinSxS, where a DLL without a manifest
REM                                       cannot find it; the copy next to EZ2B.exe is what loads.
setlocal
set "SRC=%~dp0EZ2AC_FNEX"
set "DST=%~1"
if "%DST%"=="" (echo usage: %~nx0 "G:\000 - MAIN" "F:\WINDOWS" & exit /b 1)
set "RC=/R:2 /W:2 /NP /NFL /NDL /MT:8"
set "FAIL=0"

robocopy "%SRC%" "%DST%" %RC% /XF ddraw.dll d3dimm.dll dgVoodoo.conf dgVoodooCpl.exe ffmpeg-modern.exe DDrawCompat-EZ2B.log *.bak* EZ2CustomMod_injected.marker.txt prepare_for_migration.bat
if errorlevel 8 set "FAIL=1"
for %%D in (BMSData Sound system bg CustomCache Translation LaneCover) do (
    robocopy "%SRC%\%%D" "%DST%\%%D" /E /DCOPY:T %RC%
    if errorlevel 8 set "FAIL=1"
)
robocopy "%SRC%\Video" "%DST%\Video" /E /DCOPY:T %RC% /XD bms*
if errorlevel 8 set "FAIL=1"

if not "%~2"=="" (
    for /d %%G in ("%~2\WinSxS\x86_Microsoft.Windows.GdiPlus_6595b64144ccf1df_1.0.2600.*") do copy /y "%%G\GdiPlus.dll" "%DST%\gdiplus.dll" >nul
    if not exist "%DST%\gdiplus.dll" (echo gdiplus.dll NOT found under %~2\WinSxS & set "FAIL=1")
)
if "%FAIL%"=="1" (echo DEPLOY FAILED - see robocopy output above & exit /b 1)
echo Deploy OK: %DST%
endlocal
