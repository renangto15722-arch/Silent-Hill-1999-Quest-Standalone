@echo off
setlocal EnableExtensions
title Silent Hill VR - Quest installer
cd /d "%~dp0"

rem Silent Hill VR - Meta Quest installer.
rem Installs or updates the game, then copies the files you drag into this
rem window. Your saves, settings, disc and packs on the Quest are never deleted.

set "PKG=com.sh1vr.quest"
set "DATA=/sdcard/SilentHillVR"
set "PT_URL=https://dl.google.com/android/repository/platform-tools-latest-windows.zip"
set "LOADFOLDERS=TIM 1ST ITEM BG CHARA ANIM MISC SND VIN XA TEST"
set "TMPDIR=%TEMP%\SilentHillVR_install"
set "HASDISC=0"

echo ================================================
echo    Silent Hill VR - Meta Quest installer
echo ================================================

rem SHVR_DRYRUN=1 tests the file detection without a headset (copies are only printed)
if defined SHVR_DRYRUN goto :dragloop

set "APK="
for %%F in ("SilentHillVR-Quest*.apk") do set "APK=%%~nxF"
if not defined APK (
    echo [ERROR] SilentHillVR-Quest*.apk was not found next to this .bat.
    goto :fail
)

rem ---- 1. adb: PATH, then .\platform-tools, else Google's official download
set "ADB="
for %%A in (adb.exe) do if not "%%~$PATH:A"=="" set "ADB=%%~$PATH:A"
if not defined ADB if exist "platform-tools\adb.exe" set "ADB=%~dp0platform-tools\adb.exe"
if not defined ADB (
    echo.
    echo Getting adb ^(Google Android platform-tools^)...
    curl.exe -L --fail -o "platform-tools.zip" "%PT_URL%"
    if exist "platform-tools.zip" tar -xf "platform-tools.zip"
    if exist "platform-tools.zip" del "platform-tools.zip"
    if exist "platform-tools\adb.exe" set "ADB=%~dp0platform-tools\adb.exe"
)
if not defined ADB (
    echo [ERROR] Could not get adb. Download it from:
    echo   https://developer.android.com/tools/releases/platform-tools
    echo and unzip the "platform-tools" folder next to this .bat.
    goto :fail
)

rem ---- 2. the Quest
echo.
echo == Looking for the Quest
"%ADB%" start-server >nul 2>&1
:waitdevice
set "STATE="
for /f "skip=1 tokens=1,2" %%a in ('""%ADB%" devices"') do if not defined STATE set "STATE=%%b"
if "%STATE%"=="device" goto :haveDevice
if "%STATE%"=="unauthorized" (
    echo Put the headset on and accept "Allow USB debugging".
) else (
    echo No Quest found. Connect it with a USB cable ^(Developer Mode on^).
)
set "K="
set /p "K=Then press Enter (or type Q to quit): "
if /i "%K%"=="Q" goto :fail
goto :waitdevice
:haveDevice
echo Quest connected.

if /i "%~1"=="check" (
    echo [check] adb and the headset are OK; nothing was installed.
    goto :done
)

rem ---- 3. install / update the game, keeping everything on the Quest
echo.
echo == Installing the game ^(%APK%^)
echo Saves, settings, disc and packs are kept ^(they live in Quest\SilentHillVR^).
if not exist "%TMPDIR%" mkdir "%TMPDIR%"
"%ADB%" install -r "%APK%" > "%TMPDIR%\install.txt" 2>&1
if not errorlevel 1 goto :installed
type "%TMPDIR%\install.txt"
findstr /i /c:"UPDATE_INCOMPATIBLE" /c:"signatures do not match" /c:"INCONSISTENT_CERTIFICATES" "%TMPDIR%\install.txt" >nul
if errorlevel 1 (
    echo [ERROR] The install failed.
    goto :fail
)
echo.
echo The Silent Hill VR app already on the Quest has a different signature,
echo so the old app must be removed first. Your saves, settings, disc and
echo packs are NOT removed ^(they are outside the app^).
choice /c YN /m "Remove the old app and install this one"
if errorlevel 2 goto :fail
"%ADB%" uninstall %PKG% >nul
"%ADB%" install "%APK%"
if errorlevel 1 (
    echo [ERROR] The install failed.
    goto :fail
)
:installed
echo Game installed.

rem storage permission, so the game can use /sdcard/SilentHillVR
"%ADB%" shell appops set %PKG% MANAGE_EXTERNAL_STORAGE allow >nul 2>&1
"%ADB%" shell mkdir -p %DATA%/gamedata/texturemods %DATA%/gamedata/load %DATA%/gamedata/save >nul 2>&1

rem ---- 4. what the Quest already has
echo.
echo == On the Quest now
"%ADB%" shell ls %DATA%/gamedata/ 2>nul | findstr /i /e ".bin" >nul && set "HASDISC=1"
if "%HASDISC%"=="1" (echo   Disc:          yes) else (echo   Disc:          MISSING - drag your .bin + .cue below)
set "LIST="
for /f "delims=" %%L in ('""%ADB%" shell ls %DATA%/gamedata/texturemods/ 2>nul"') do call :addlist "%%L"
if not defined LIST set "LIST=none"
echo   HD textures:   %LIST%
set "LIST="
for /f "delims=" %%L in ('""%ADB%" shell ls %DATA%/gamedata/load/ 2>nul"') do (
    for %%N in (%LOADFOLDERS%) do if /i "%%L"=="%%N" call :addlist "%%L"
)
if not defined LIST set "LIST=none"
echo   HD maps/menus: %LIST%

rem ---- 5. drag & drop
:dragloop
echo.
echo == Add your files
echo Drag a file or folder into this window and press Enter.
echo   - your disc:           Silent Hill (USA).bin + .cue
echo   - HD textures:         the pack .zip (it stays zipped)
echo   - HD maps / HD menus:  their .zip or folder (TIM, 1ST, ITEM ...)
echo Press Enter on an empty line when you are done.
:ask
echo.
set "IN="
set /p "IN=Drag here: "
if not defined IN goto :finish
call :each %IN%
goto :ask

:finish
echo.
echo ================================================
if not "%HASDISC%"=="1" echo  The disc is still missing: run this again and drag your .bin + .cue.
echo  Done. On the Quest: Library ^> Unknown Sources ^> Silent Hill VR
echo ================================================
goto :done

rem ================= subroutines =================

:addlist
if defined LIST (set "LIST=%LIST%, %~1") else (set "LIST=%~1")
goto :eof

:each
if "%~1"=="" goto :eof
call :handle "%~1"
shift
goto :each

:push
rem %1 = source, %2 = destination folder on the Quest
if defined SHVR_DRYRUN (
    echo     [copy] %1 -^> %~2
    goto :eof
)
"%ADB%" push %1 "%~2"
if errorlevel 1 echo   [ERROR] Copy failed: %1
goto :eof

:handle
rem (no path inside parentheses blocks: a name like "Silent Hill (USA).bin" would close them)
echo.
if exist "%~1" goto :handleExists
echo Not found: %~1
goto :eof
:handleExists
echo ^> %~nx1
set "ATTR=%~a1"
if /i "%ATTR:~0,1%"=="d" goto :handleDir
if /i "%~x1"==".bin" goto :handleBin
if /i "%~x1"==".cue" goto :handleCue
if /i "%~x1"==".zip" goto :handleZip
if /i "%~x1"==".rar" goto :handleRar
if /i "%~x1"==".7z"  goto :handleRar
if /i "%~x1"==".apk" (
    echo   That is the game app; it was already installed above.
    goto :eof
)
echo   Unknown file. Drag the disc ^(.bin/.cue^), an HD texture pack ^(.zip^) or HD maps ^(.zip/folder^).
goto :eof

:handleRar
echo   Unzip it first ^(7-Zip / WinRAR^), then drag the folder.
goto :eof

:handleBin
echo   Found: game disc
call :pushdisc "%~1"
if exist "%~dpn1.cue" (
    call :pushdisc "%~dpn1.cue"
) else (
    echo   ^(no .cue with the same name next to it - drag it too if you have one^)
)
goto :eof

:handleCue
echo   Found: game disc ^(.cue^)
call :pushdisc "%~1"
if exist "%~dpn1.bin" call :pushdisc "%~dpn1.bin"
goto :eof

:pushdisc
rem skip a file already copied in this run (dragging .bin and .cue together)
echo ;%DONEDISC%; | findstr /i /c:";%~f1;" >nul && goto :eof
set "DONEDISC=%DONEDISC%;%~f1"
echo   Copying %~nx1 ^(large files take a minute^)...
call :push "%~f1" "%DATA%/gamedata/"
set "HASDISC=1"
goto :eof

:handleDir
rem a texture pack folder (DuckStation texupload-*.png files)
dir /s /b "%~1\texupload-*" >nul 2>&1
if not errorlevel 1 (
    echo   Found: HD texture pack ^(folder^)
    call :push "%~f1" "%DATA%/gamedata/texturemods/"
    goto :eof
)
rem HD maps / menus: every TIM, 1ST, ITEM ... folder at or below this one
set "FOUND=0"
call :describe "%~f1"
for %%N in (%LOADFOLDERS%) do if /i "%~nx1"=="%%N" (
    set "FOUND=1"
    call :push "%~f1" "%DATA%/gamedata/load/"
)
if "%FOUND%"=="1" goto :loadDone
for /d /r "%~1" %%D in (*) do (
    for %%N in (%LOADFOLDERS%) do if /i "%%~nxD"=="%%N" (
        set "FOUND=1"
        echo   Copying %%~nxD ...
        call :push "%%~fD" "%DATA%/gamedata/load/"
    )
)
:loadDone
if "%FOUND%"=="1" (
    echo   Installed in gamedata/load.
    goto :eof
)
rem a folder with the disc in it
set "FOUND=0"
for %%B in ("%~1\*.bin") do (
    set "FOUND=1"
    call :handle "%%~fB"
)
if "%FOUND%"=="0" echo   Nothing I know in this folder ^(no disc, HD textures or HD maps^).
goto :eof

:describe
rem prints how many HD map images and other HD images a folder has
set "NMAP=0"
set "NALL=0"
for /f %%C in ('dir /s /b /a-d "%~1\MP_*.png" "%~1\MP_*.dds" "%~1\MR_*.png" "%~1\MR_*.dds" 2^>nul ^| find /c /v ""') do set "NMAP=%%C"
for /f %%C in ('dir /s /b /a-d "%~1\*.png" "%~1\*.dds" 2^>nul ^| find /c /v ""') do set "NALL=%%C"
set /a NOTHER=NALL-NMAP
if %NALL% GTR 0 (
    echo   Found: HD maps ^(%NMAP% files^) + HD menus/images ^(%NOTHER% files^)
)
goto :eof

:handleZip
if not exist "%TMPDIR%" mkdir "%TMPDIR%"
tar -tf "%~1" > "%TMPDIR%\zip.txt" 2>nul
if errorlevel 1 (
    echo   This .zip could not be opened.
    goto :eof
)
findstr /i /c:"texupload-" "%TMPDIR%\zip.txt" >nul
if not errorlevel 1 (
    echo   Found: HD texture pack
    echo   Copying ^(it is big, this takes a few minutes^)...
    call :push "%~f1" "%DATA%/gamedata/texturemods/"
    echo   Installed in gamedata/texturemods.
    goto :eof
)
findstr /i /r /c:"TIM[/\\]" /c:"1ST[/\\]" /c:"ITEM[/\\]" /c:"BG[/\\]" /c:"CHARA[/\\]" /c:"ANIM[/\\]" /c:"MISC[/\\]" /c:"SND[/\\]" /c:"VIN[/\\]" /c:"XA[/\\]" "%TMPDIR%\zip.txt" >nul
if not errorlevel 1 (
    if exist "%TMPDIR%\pack" rmdir /s /q "%TMPDIR%\pack"
    mkdir "%TMPDIR%\pack"
    tar -xf "%~1" -C "%TMPDIR%\pack"
    call :handleDir "%TMPDIR%\pack"
    rmdir /s /q "%TMPDIR%\pack"
    goto :eof
)
findstr /i /e /c:".bin" "%TMPDIR%\zip.txt" >nul
if not errorlevel 1 (
    echo   This .zip has a disc inside: unzip it first, then drag the .bin.
    goto :eof
)
echo   Nothing I know in this .zip ^(no HD textures or HD maps^).
goto :eof

:fail
echo.
echo Installation did not finish.
:done
echo.
pause
endlocal
