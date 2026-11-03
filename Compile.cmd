@echo off
title CustomOS Automated Compiler
echo ===================================================
echo       CustomOS Automated Compilation System
echo ===================================================
echo.

set "SOURCE_DIR=%~dp0files"
set "OUTPUT_DIR=%~dp0UefiDisk\EFI\BOOT"
set "BINARY_NAME=bootx64.efi"

echo [1/6] Initializing build environment and paths...
echo       - Source Directory: %SOURCE_DIR%
echo       - Target Directory: %OUTPUT_DIR%
echo.

if not exist "%SOURCE_DIR%" (
    echo [X] Error: 'files' directory not found!
    goto :FAIL
)

cd /d "%SOURCE_DIR%"

echo [2/6] Verifying core source code dependencies...
if not exist "bootloader.c" (
    echo [X] Error: Missing bootloader.c!
    goto :FAIL
)
if not exist "kernel.c" (
    echo [X] Error: Missing kernel.c!
    goto :FAIL
)
echo       - Detected bootloader.c and kernel.c successfully.
echo.

echo [3/6] Compiling unified UEFI application via GCC...
echo       - Target Architecture: x86_64
echo       - Subsystem: UEFI Application (Subsystem 10)
echo       - Entry Point: efi_main
gcc -shared -nostdlib -mno-red-zone -fno-builtin -Wl,-e,efi_main -Wl,--subsystem,10 -mabi=ms -D__X86_64__ -o "%BINARY_NAME%" bootloader.c kernel.c
if %ERRORLEVEL% NEQ 0 goto :FAIL
echo       - Compilation and linking completed with zero errors.
echo.

echo [4/6] Checking target deployment directories...
if not exist "%OUTPUT_DIR%" (
    echo       - Output directory missing. Creating path structure...
    mkdir "%OUTPUT_DIR%"
) else (
    echo       - Output directory structure verified.
)
echo.

echo [5/6] Deploying executable binary to UefiDisk mapping...
copy /Y "%BINARY_NAME%" "%OUTPUT_DIR%\%BINARY_NAME%" >nul
if %ERRORLEVEL% NEQ 0 goto :FAIL
echo       - Successfully placed %BINARY_NAME% into EFI\BOOT mapping.
echo.

echo [6/6] Cleaning up local build workspace artifacts...
del "%BINARY_NAME%" >nul
echo       - Local staging area sanitized.
echo.

echo ===================================================
echo  SUCCESS: System fully compiled and deployed!
echo ===================================================
pause
exit /b 0

:FAIL
echo.
echo [X] Error occurred during compile! Error in code. Check and fix!
pause
exit /b 1
