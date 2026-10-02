@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ============================================================
rem  gp-os-installer.bat
rem  Аналог gp-os-installer.sh для Windows
rem  Установка raw-образа GitrexOS на BIOS-диск или USB-устройство
rem ============================================================

set "USAGE=Usage: %~nx0 [--dry-run] IMAGE [DEVICE]"
set "DRY_RUN=0"
set "IMAGE="
set "DEVICE="

rem ---------- разбор аргументов ----------
if "%~1"=="--dry-run" (
    set "DRY_RUN=1"
    shift
)

set "IMAGE=%~1"
set "DEVICE=%~2"

if "%IMAGE%"=="" (
    echo %USAGE%
    echo.
    echo Install GitrexOS raw image to a BIOS disk or USB device.
    echo Example: %~nx0 build\gp-os.img \\.\PhysicalDrive1
    exit /b 2
)

if not exist "%IMAGE%" (
    echo Image not found: %IMAGE% 1>&2
    exit /b 1
)

if "%DRY_RUN%"=="1" (
    echo DRY RUN: would install %IMAGE%
    if not "%DEVICE%"=="" echo DRY RUN: target %DEVICE%
    exit /b 0
)

rem ---------- если устройство не задано — показать список ----------
if "%DEVICE%"=="" (
    echo Available physical drives:
    echo.
    powershell -NoProfile -Command ^
        "Get-Disk | Select-Object Number,FriendlyName,Size,PartitionStyle | Format-Table -AutoSize"
    echo.
    set /p "DEVICE=Target device (for example \\.\PhysicalDrive1): "
)

rem ---------- проверка, что цель безопасна ----------
echo %DEVICE% | findstr /R /C:"^\\\\\.\\PhysicalDrive[0-9][0-9]*$" >nul
if errorlevel 1 (
    echo Refusing unsafe target: %DEVICE% 1>&2
    exit /b 1
)

rem ---------- подтверждение ----------
echo WARNING: all data on %DEVICE% will be overwritten.
set /p "CONFIRM=Type INSTALL GITREXOS to continue: "
if not "%CONFIRM%"=="INSTALL GITREXOS" (
    echo Cancelled.
    exit /b 1
)

rem ---------- запись образа ----------
echo Writing %IMAGE% to %DEVICE% ...
powershell -NoProfile -Command ^
    "$ErrorActionPreference='Stop';" ^
    "$img='%IMAGE%'; $dev='%DEVICE%';" ^
    "$fs=[System.IO.File]::OpenRead($img);" ^
    "$ds=[System.IO.File]::OpenWrite($dev);" ^
    "try {" ^
    "  $buf=New-Object byte[] (4MB);" ^
    "  $total=$fs.Length; $done=0;" ^
    "  while (($read=$fs.Read($buf,0,$buf.Length)) -gt 0) {" ^
    "    $ds.Write($buf,0,$read); $done+=$read;" ^
    "    Write-Progress -Activity 'Writing image' -Status ('{0:N0} / {1:N0} bytes' -f $done,$total) -PercentComplete (($done/$total)*100);" ^
    "  }" ^
    "  $ds.Flush(); $ds.Close(); $fs.Close();" ^
    "  Write-Host 'Flush complete.';" ^
    "} catch { $fs.Close(); $ds.Close(); throw }"

if errorlevel 1 (
    echo Installation failed. 1>&2
    exit /b 1
)

rem ---------- сброс кэшей ----------
powershell -NoProfile -Command ^
    "Write-VolumeCache -DriveLetter ($env:SystemDrive.Substring(0,1)) -ErrorAction SilentlyContinue" >nul 2>&1

echo GitrexOS installed. Reboot and select the target disk in BIOS.
exit /b 0
