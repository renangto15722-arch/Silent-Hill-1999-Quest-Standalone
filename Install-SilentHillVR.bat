@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Silent Hill VR - Quest installer
cd /d "%~dp0"

set "APK="
for %%F in ("SilentHillVR-Quest*.apk") do set "APK=%%~nxF"
set "PKG=com.sh1vr.quest"
set "DATA=/sdcard/SilentHillVR"
set "PT_URL=https://dl.google.com/android/repository/platform-tools-latest-windows.zip"

echo ================================================
echo   Silent Hill VR - Meta Quest installer
echo ================================================
echo.
echo  Optional, before you start: put your own disc files
echo  "Silent Hill (USA).bin" and "Silent Hill (USA).cue"
echo  next to this .bat (or in a folder named "disc") and they
echo  will be copied to the Quest too. An HD texture pack .zip
echo  can go in a folder named "texturemods", and the HD maps
echo  files in a folder named "load".
echo.

if not defined APK (
    echo [ERROR] SilentHillVR-Quest*.apk was not found next to this .bat.
    goto :fail
)

rem ---- 1. adb: PATH, then .\platform-tools, else download Google's official one
set "ADB="
for %%A in (adb.exe) do if not "%%~$PATH:A"=="" set "ADB=%%~$PATH:A"
if not defined ADB if exist "platform-tools\adb.exe" set "ADB=%~dp0platform-tools\adb.exe"
if not defined ADB (
    echo adb was not found. Downloading Google's Android platform-tools...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ErrorActionPreference='Stop'; [Net.ServicePointManager]::SecurityProtocol='Tls12';" ^
      "Invoke-WebRequest -UseBasicParsing '%PT_URL%' -OutFile 'platform-tools.zip';" ^
      "Expand-Archive -Force 'platform-tools.zip' '.'; Remove-Item 'platform-tools.zip'"
    if exist "platform-tools\adb.exe" set "ADB=%~dp0platform-tools\adb.exe"
)
if not defined ADB (
    echo [ERROR] Could not get adb. Download it from:
    echo   https://developer.android.com/tools/releases/platform-tools
    echo and unzip the "platform-tools" folder next to this .bat.
    goto :fail
)
echo Using adb: !ADB!
echo.

rem ---- 2. the Quest
"!ADB!" start-server >nul 2>&1
:waitdevice
set "STATE="
for /f "skip=1 tokens=1,2" %%a in ('"!ADB!" devices') do if not defined STATE set "STATE=%%b"
if "!STATE!"=="device" goto :haveDevice
if "!STATE!"=="unauthorized" (
    echo The Quest is connected but this PC is not allowed yet.
    echo Put the headset on and accept "Allow USB debugging", then press a key.
) else (
    echo No Quest found. Connect it with a USB cable, make sure Developer Mode
    echo is on, then press a key.
)
pause >nul
goto :waitdevice
:haveDevice
echo Quest connected.
echo.

if /i "%~1"=="check" (
    echo [check] adb and the headset are OK; nothing was installed.
    goto :done
)

rem ---- 3. install
echo Installing %APK% ...
"!ADB!" install -r "%APK%"
if errorlevel 1 (
    echo.
    echo [ERROR] The install failed. If it says the signatures do not match,
    echo uninstall the old Silent Hill VR app first ^(your saves are kept^).
    goto :fail
)

rem ---- 4. storage permission, so the game can use /sdcard/SilentHillVR
"!ADB!" shell appops set %PKG% MANAGE_EXTERNAL_STORAGE allow
"!ADB!" shell mkdir -p "%DATA%/gamedata/texturemods" "%DATA%/gamedata/save"

rem ---- 5. optional: the disc and texture packs
set "DISCDIR="
if exist "Silent Hill (USA).bin" set "DISCDIR=."
if exist "disc\Silent Hill (USA).bin" set "DISCDIR=disc"
if defined DISCDIR (
    if not exist "!DISCDIR!\Silent Hill (USA).cue" (
        echo [WARNING] Found the .bin but not "Silent Hill (USA).cue"; copying the .bin only.
    )
    echo Copying your disc to the Quest ^(this takes a minute^)...
    "!ADB!" push "!DISCDIR!\Silent Hill (USA).bin" "%DATA%/gamedata/"
    if exist "!DISCDIR!\Silent Hill (USA).cue" "!ADB!" push "!DISCDIR!\Silent Hill (USA).cue" "%DATA%/gamedata/"
) else (
    "!ADB!" shell ls "%DATA%/gamedata/Silent\ Hill\ \(USA\).bin" >nul 2>&1
    if errorlevel 1 (
        echo.
        echo NOTE: no disc found. Copy "Silent Hill (USA).bin" and ".cue" to
        echo   Quest\SilentHillVR\gamedata\   ^(or run this .bat again with them next to it^)
    ) else (
        echo Disc already on the Quest.
    )
)
if exist "load\" (
    echo Copying the HD maps ^(folder "load"^)...
    "!ADB!" push "load" "%DATA%/gamedata/"
)
if exist "texturemods\*.zip" (
    for %%Z in ("texturemods\*.zip") do (
        echo Copying texture pack %%~nxZ ^(large files take a while^)...
        "!ADB!" push "%%Z" "%DATA%/gamedata/texturemods/"
    )
)

echo.
echo ================================================
echo   Done. On the Quest: Library ^> Unknown Sources
echo   ^> Silent Hill VR
echo ================================================
goto :done

:fail
echo.
echo Installation did not finish.
:done
echo.
pause
endlocal
