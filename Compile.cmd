@echo off
title CustomOS Automated Compiler
echo ===================================================
echo       CustomOS Automated Compilation System
echo ===================================================
echo.

set "SOURCE_DIR=%~dp0files"
set "EFI_OUTPUT_DIR=%~dp0UefiDisk\EFI\BOOT"
set "DISK_ROOT=%~dp0UefiDisk"

echo [1/6] Initializing build environment and paths...
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

echo [3/6] Compiling standalone UEFI bootloader (bootx64.efi)...
gcc -shared -nostdlib -mno-red-zone -fno-builtin -Wl,-e,efi_main -Wl,--subsystem,10 -mabi=ms -D__X86_64__ -o "bootx64.efi" bootloader.c
if %ERRORLEVEL% NEQ 0 goto :FAIL
echo       - Bootloader compilation completed.
echo.

echo [4/6] Compiling kernel entry stub and C source into flat binary (kernel.bin)...
as kernel_entry.s -o kernel_entry.o
if %ERRORLEVEL% NEQ 0 goto :FAIL
gcc -ffreestanding -nostdlib -mno-red-zone -fno-asynchronous-unwind-tables -fno-exceptions -fno-stack-protector -fno-tree-loop-distribute-patterns -c kernel.c -o kernel.o
if %ERRORLEVEL% NEQ 0 goto :FAIL
ld -T link.ld -e _start --image-base 0 -Map kernel.map -o kernel.pe kernel_entry.o kernel.o
if %ERRORLEVEL% NEQ 0 goto :FAIL
nm kernel.pe | findstr /C:"0000000000100000 T _start" >nul
if %ERRORLEVEL% NEQ 0 (
    echo [X] Error: kernel entry _start is not linked at 0x100000. Check link.ld and the ld flags.
    goto :FAIL
)
objcopy -O binary kernel.pe kernel.bin
if %ERRORLEVEL% NEQ 0 goto :FAIL
for %%F in (kernel.bin) do echo       - kernel.bin is %%~zF bytes.
echo       - Kernel binary generation completed.
echo.

echo [5/6] Deploying executable binaries to UefiDisk mapping...
if not exist "%EFI_OUTPUT_DIR%" (
    mkdir "%EFI_OUTPUT_DIR%"
)
copy /Y "bootx64.efi" "%EFI_OUTPUT_DIR%\bootx64.efi" >nul
copy /Y "kernel.bin" "%DISK_ROOT%\kernel.bin" >nul
if %ERRORLEVEL% NEQ 0 goto :FAIL
echo       - Successfully placed bootx64.efi and kernel.bin.
echo.

echo [6/6] Cleaning up local build workspace artifacts...
del "bootx64.efi" "kernel.o" "kernel_entry.o" "kernel.pe" "kernel.map" "kernel.elf" "kernel.bin" >nul 2>nul
echo       - Local staging area sanitized.
echo.

echo ===================================================
echo  SUCCESS: System fully compiled and deployed!
echo ===================================================
pause
exit /b 0

:FAIL
echo.
echo [X] Error occurred during compile! Check and fix code.
pause
exit /b 1
