#include "efi.h"
#include "memory.h"   // int_to_str

#define SCREEN_MAIN           0
#define SCREEN_SETTINGS       1
#define SCREEN_TIMEOUT        2
#define SCREEN_THEMES         3
#define SCREEN_POWER_OPTIONS  4


// Where kernel.bin is loaded and how much room it gets (including .bss).
// Keep in sync with KERNEL_BASE / KERNEL_MAX_SIZE in link.ld.
#define KERNEL_LOAD_ADDRESS 0x00100000ULL
#define KERNEL_MAX_PAGES    256                      // 256 pages = 1 MB

// Block until a key is available, sleeping instead of spinning.
static void wait_key(EFI_SYSTEM_TABLE *SystemTable, EFI_INPUT_KEY *Key) {
    UINTN index;
    while (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, Key) != EFI_SUCCESS) {
        SystemTable->BootServices->WaitForEvent(1, &SystemTable->ConIn->WaitForKey, &index);
    }
}

static void print_u64(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut, UINT64 value) {
    CHAR16 buf[32];
    int_to_str(value, buf, 32);
    ConOut->OutputString(ConOut, buf);
}

// "10 seconds until..." / " 9 seconds until..." / " 1 second until..."
static void print_countdown(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut, int countdown) {
    CHAR16 num[8];
    int_to_str((UINT64)countdown, num, 8);
    if (countdown < 10) ConOut->OutputString(ConOut, L" ");
    ConOut->OutputString(ConOut, num);
    ConOut->OutputString(ConOut, countdown == 1 ? L" second until selected entry is executed...\r\n"
                                                : L" seconds until selected entry is executed...\r\n");
}

// Helper function to dynamically load and execute kernel.bin from disk with full diagnostics

EFI_STATUS boot_kernel_bin(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut = SystemTable->ConOut;
    ConOut->ClearScreen(ConOut);
    ConOut->SetAttribute(ConOut, 0x0F);

    ConOut->OutputString(ConOut, L"[INIT] Starting CustomOS Kernel Loader...\r\n");

    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID infoGuid = EFI_FILE_INFO_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem = 0;
    EFI_FILE_PROTOCOL *rootDrive = 0;
    EFI_FILE_PROTOCOL *kernelFile = 0;
    EFI_PHYSICAL_ADDRESS kernelBuffer = KERNEL_LOAD_ADDRESS;
    UINT64 infoBuf[128];                              // 1 KB, 8-byte aligned
    UINTN infoSize = sizeof(infoBuf);
    UINT64 kernelSize = 0;
    UINTN readSize = 0;
    EFI_STATUS Status;

    // Step 1: Get the Loaded Image Protocol to find the actual boot device handle
    ConOut->OutputString(ConOut, L"[..] Locating Loaded Image Protocol... ");
    Status = SystemTable->BootServices->HandleProtocol(ImageHandle, &loadedImageGuid, (void**)&loadedImage);
    if (Status != EFI_SUCCESS) {
        ConOut->OutputString(ConOut, L"[FAILED]\r\n[X] Fatal: Could not locate Loaded Image Protocol.\r\n");
        goto HALT_SYSTEM;
    }
    ConOut->OutputString(ConOut, L"[OK]\r\n");

    // Step 2: Access File System Protocol using the boot device handle
    ConOut->OutputString(ConOut, L"[..] Locating Simple File System Protocol... ");
    Status = SystemTable->BootServices->HandleProtocol(loadedImage->DeviceHandle, &fsGuid, (void**)&fileSystem);
    if (Status != EFI_SUCCESS) {
        ConOut->OutputString(ConOut, L"[FAILED]\r\n[X] Fatal: Could not access File System Protocol.\r\n");
        goto HALT_SYSTEM;
    }
    ConOut->OutputString(ConOut, L"[OK]\r\n");

    // Step 3: Open Volume
    ConOut->OutputString(ConOut, L"[..] Opening storage volume root... ");
    Status = fileSystem->OpenVolume(fileSystem, &rootDrive);
    if (Status != EFI_SUCCESS) {
        ConOut->OutputString(ConOut, L"[FAILED]\r\n[X] Fatal: Could not open root volume.\r\n");
        goto HALT_SYSTEM;
    }
    ConOut->OutputString(ConOut, L"[OK]\r\n");

    // Step 3.5: Open kernel.bin from the root volume
    ConOut->OutputString(ConOut, L"[..] Opening kernel.bin from disk... ");
    Status = rootDrive->Open(rootDrive, &kernelFile, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    if (Status != EFI_SUCCESS) {
        ConOut->OutputString(ConOut, L"[FAILED]\r\n[X] Fatal: Could not find or open 'kernel.bin'.\r\n");
        rootDrive->Close(rootDrive);
        goto HALT_SYSTEM;
    }
    ConOut->OutputString(ConOut, L"[OK]\r\n");

    // Step 3.6: Ask the file system how big kernel.bin really is, so a kernel
    // that outgrows the memory window is reported instead of silently truncated.
    ConOut->OutputString(ConOut, L"[..] Checking kernel.bin size... ");
    Status = kernelFile->GetInfo(kernelFile, &infoGuid, &infoSize, infoBuf);
    if (Status != EFI_SUCCESS) {
        ConOut->OutputString(ConOut, L"[FAILED]\r\n[X] Fatal: Could not read kernel.bin file information.\r\n");
        kernelFile->Close(kernelFile);
        rootDrive->Close(rootDrive);
        goto HALT_SYSTEM;
    }
    kernelSize = ((EFI_FILE_INFO*)infoBuf)->FileSize;
    if (kernelSize == 0 || kernelSize > (UINT64)KERNEL_MAX_PAGES * 4096ULL) {
        ConOut->OutputString(ConOut, L"[FAILED]\r\n[X] Fatal: kernel.bin is empty or larger than the 1024 KB kernel memory window.\r\n");
        kernelFile->Close(kernelFile);
        rootDrive->Close(rootDrive);
        goto HALT_SYSTEM;
    }
    ConOut->OutputString(ConOut, L"[OK] (");
    print_u64(ConOut, kernelSize);
    ConOut->OutputString(ConOut, L" bytes)\r\n");

    // Step 4: Allocate memory precisely at 0x100000 (1 MB) to match link.ld.
    // The window also has to hold the kernel's .bss, which is not stored in the file.
    ConOut->OutputString(ConOut, L"[..] Allocating ");
    print_u64(ConOut, KERNEL_MAX_PAGES);
    ConOut->OutputString(ConOut, L" pages (");
    print_u64(ConOut, (UINT64)KERNEL_MAX_PAGES * 4);
    ConOut->OutputString(ConOut, L" KB) at memory address 0x100000... ");

    Status = SystemTable->BootServices->AllocatePages(
        AllocateAddress,
        EfiLoaderCode,                                // the kernel is executable code
        KERNEL_MAX_PAGES,
        &kernelBuffer
    );

    if (Status != EFI_SUCCESS) {
        ConOut->OutputString(ConOut, L"[FAILED]\r\n[X] Fatal: Memory allocation at 0x100000 failed.\r\n");
        kernelFile->Close(kernelFile);
        rootDrive->Close(rootDrive);
        goto HALT_SYSTEM;
    }
    ConOut->OutputString(ConOut, L"[OK]\r\n");

    // Step 5: Read kernel binary into memory
    ConOut->OutputString(ConOut, L"[..] Reading kernel.bin binary contents into memory... ");
    readSize = (UINTN)kernelSize;
    Status = kernelFile->Read(kernelFile, &readSize, (VOID*)kernelBuffer);
    if (Status != EFI_SUCCESS || readSize != (UINTN)kernelSize) {
        ConOut->OutputString(ConOut, L"[FAILED]\r\n[X] Fatal: Failed to read file data from kernel.bin.\r\n");
        kernelFile->Close(kernelFile);
        rootDrive->Close(rootDrive);
        goto HALT_SYSTEM;
    }
    ConOut->OutputString(ConOut, L"[OK] (Read ");
    print_u64(ConOut, readSize);
    ConOut->OutputString(ConOut, L" bytes successfully)\r\n");

    // Step 6: Clean up file handles
    kernelFile->Close(kernelFile);
    rootDrive->Close(rootDrive);

    ConOut->OutputString(ConOut, L"[I] Kernel payload successfully staged at 0x100000.\r\n");
    ConOut->OutputString(ConOut, L"[I] Transferring execution control to kernel entry point...\r\n\r\n");
    SystemTable->BootServices->Stall(750000); // 0.75 sec delay for readability

    // Step 7: Jump to raw binary entry point (reusing loadedImage from top of function)
    typedef EFI_STATUS (EFIAPI *KERNEL_ENTRY)(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable);
    KERNEL_ENTRY kernel_entry = (KERNEL_ENTRY)kernelBuffer;

    // Execute kernel
    return kernel_entry(loadedImage ? loadedImage->DeviceHandle : ImageHandle, SystemTable);

HALT_SYSTEM:
    ConOut->SetAttribute(ConOut, 0x0C); // Warning Red
    ConOut->OutputString(ConOut, L"\r\n[!] System halted due to fatal bootloader error. Please restart virtual machine.\r\n");
    ConOut->SetAttribute(ConOut, 0x07);

    // Infinite halt loop to prevent falling back to BIOS/UiApp
    while (1) {
        __asm__ __volatile__ ("cli; hlt");
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut = SystemTable->ConOut;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn = SystemTable->ConIn;

    // Firmware arms a 5 minute watchdog for boot images; 0 turns it off.
    SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    ConOut->ClearScreen(ConOut);
    ConOut->EnableCursor(ConOut, FALSE);

    int currentScreen = SCREEN_MAIN;
    int mainSelection = 0;
    int settingsSelection = 0;
    int powerSelection = 0;
    int bootTimeoutSeconds = 10;
    int countdown = 10;
    BOOLEAN timeoutActive = TRUE;

    CHAR16 timeoutStr[4] = L"10";
    int timeoutStrLen = 2;

    EFI_INPUT_KEY Key;

    while (1) {
        if (currentScreen == SCREEN_MAIN) {
            ConOut->ClearScreen(ConOut);
            ConOut->SetAttribute(ConOut, 0x07);

            ConOut->OutputString(ConOut, L"╔════════════════════════════════════════════════════════════════════════════════╗\r\n");
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.6                            ║\r\n");
            ConOut->OutputString(ConOut, L"╚════════════════════════════════════════════════════════════════════════════════╝\r\n\r\n");

            ConOut->OutputString(ConOut, L"Main Menu\r\n\r\n");

            if (mainSelection == 0) {
                ConOut->SetAttribute(ConOut, 0x70);
                ConOut->OutputString(ConOut, L" < Boot CustomOS > ");
            } else {
                ConOut->SetAttribute(ConOut, 0x07);
                ConOut->OutputString(ConOut, L" < Boot CustomOS > ");
            }
            ConOut->SetAttribute(ConOut, 0x07);
            ConOut->OutputString(ConOut, L"\r\n\r\n");

            if (mainSelection == 1) {
                ConOut->SetAttribute(ConOut, 0x70);
                ConOut->OutputString(ConOut, L" < Settings > ");
            } else {
                ConOut->SetAttribute(ConOut, 0x07);
                ConOut->OutputString(ConOut, L" < Settings > ");
            }
            ConOut->SetAttribute(ConOut, 0x07);
            ConOut->OutputString(ConOut, L"\r\n\r\n");

            if (mainSelection == 2) {
                ConOut->SetAttribute(ConOut, 0x70);
                ConOut->OutputString(ConOut, L" < Power Options > ");
            } else {
                ConOut->SetAttribute(ConOut, 0x07);
                ConOut->OutputString(ConOut, L" < Power Options > ");
            }
            ConOut->SetAttribute(ConOut, 0x07);
            ConOut->OutputString(ConOut, L"\r\n\r\n\r\n\r\n\r\n");

            ConOut->OutputString(ConOut, L"Press Enter to execute selected entry...\r\n");

            if (timeoutActive && countdown > 0) {
                print_countdown(ConOut, countdown);
            } else if (!timeoutActive) {
                ConOut->OutputString(ConOut, L"                                                                \r\n");
            } else {
                boot_kernel_bin(ImageHandle, SystemTable);
                break;
            }

            BOOLEAN keyPressed = FALSE;
            for (int i = 0; i < 10; i++) {
                Status = ConIn->ReadKeyStroke(ConIn, &Key);
                if (Status == EFI_SUCCESS) {
                    keyPressed = TRUE;
                    break;
                }
                SystemTable->BootServices->Stall(100000);
            }

            if (keyPressed) {
                timeoutActive = FALSE;

                if (Key.ScanCode == SCAN_UP) {
                    if (mainSelection > 0) mainSelection--;
                } else if (Key.ScanCode == SCAN_DOWN) {
                    if (mainSelection < 2) mainSelection++;
                } else if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
                    if (mainSelection == 0) {
                        boot_kernel_bin(ImageHandle, SystemTable);
                        break;
                    } else if (mainSelection == 1) {
                        currentScreen = SCREEN_SETTINGS;
                        settingsSelection = 0;
                    } else if (mainSelection == 2) {
                        currentScreen = SCREEN_POWER_OPTIONS;
                        powerSelection = 0;
                    }
                }
            } else {
                if (timeoutActive && countdown > 0) {
                    countdown--;
                }
            }

        } else if (currentScreen == SCREEN_SETTINGS) {
            ConOut->ClearScreen(ConOut);
            ConOut->SetAttribute(ConOut, 0x07);

            ConOut->OutputString(ConOut, L"╔════════════════════════════════════════════════════════════════════════════════╗\r\n");
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.6                            ║\r\n");
            ConOut->OutputString(ConOut, L"╚════════════════════════════════════════════════════════════════════════════════╝\r\n\r\n");

            ConOut->OutputString(ConOut, L"Settings\r\n\r\n");

            if (settingsSelection == 0) {
                ConOut->SetAttribute(ConOut, 0x70);
                ConOut->OutputString(ConOut, L"Boot Timeout: ");
                ConOut->OutputString(ConOut, timeoutStr);
                ConOut->OutputString(ConOut, L" seconds");
            } else {
                ConOut->SetAttribute(ConOut, 0x07);
                ConOut->OutputString(ConOut, L"Boot Timeout: ");
                ConOut->OutputString(ConOut, timeoutStr);
                ConOut->OutputString(ConOut, L" seconds");
            }
            ConOut->SetAttribute(ConOut, 0x07);
            ConOut->OutputString(ConOut, L"\r\n\r\n");

            if (settingsSelection == 1) {
                ConOut->SetAttribute(ConOut, 0x70);
                ConOut->OutputString(ConOut, L"Theme: Classic Black / White");
            } else {
                ConOut->SetAttribute(ConOut, 0x07);
                ConOut->OutputString(ConOut, L"Theme: Classic Black / White");
            }
            ConOut->SetAttribute(ConOut, 0x07);
            ConOut->OutputString(ConOut, L"\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");

            ConOut->OutputString(ConOut, L"Press B or Backspace to go back to the main menu...\r\n");

            wait_key(SystemTable, &Key);

            if (Key.ScanCode == SCAN_UP) {
                if (settingsSelection > 0) settingsSelection--;
            } else if (Key.ScanCode == SCAN_DOWN) {
                if (settingsSelection < 1) settingsSelection++;
            } else if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
                if (settingsSelection == 0) {
                    currentScreen = SCREEN_TIMEOUT;
                } else if (settingsSelection == 1) {
                    currentScreen = SCREEN_THEMES;
                }
            } else if (Key.UnicodeChar == L'b' || Key.UnicodeChar == L'B' || Key.UnicodeChar == 0x08 || Key.UnicodeChar == L'\b') {
                currentScreen = SCREEN_MAIN;
            }

        } else if (currentScreen == SCREEN_TIMEOUT) {
            ConOut->ClearScreen(ConOut);
            ConOut->SetAttribute(ConOut, 0x07);

            ConOut->OutputString(ConOut, L"╔════════════════════════════════════════════════════════════════════════════════╗\r\n");
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.6                            ║\r\n");
            ConOut->OutputString(ConOut, L"╚════════════════════════════════════════════════════════════════════════════════╝\r\n\r\n");

            ConOut->OutputString(ConOut, L"Settings > Boot Timeout\r\n\r\n");
            ConOut->OutputString(ConOut, L"Time in seconds: ");
            ConOut->OutputString(ConOut, timeoutStr);
            ConOut->OutputString(ConOut, L" _\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");
            ConOut->OutputString(ConOut, L"Press Enter to save changes and go back...\r\n");

            wait_key(SystemTable, &Key);

            if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
                int val = 0;
                for (int i = 0; i < timeoutStrLen; i++) {
                    if (timeoutStr[i] >= L'0' && timeoutStr[i] <= L'9') {
                        val = val * 10 + (timeoutStr[i] - L'0');
                    }
                }
                if (val > 0 && val <= 99) {
                    bootTimeoutSeconds = val;
                    countdown = bootTimeoutSeconds;
                    timeoutActive = TRUE;
                }
                currentScreen = SCREEN_SETTINGS;
            } else if (Key.UnicodeChar == 0x08 || Key.UnicodeChar == L'\b') {
                if (timeoutStrLen > 0) {
                    timeoutStrLen--;
                    timeoutStr[timeoutStrLen] = L'\0';
                }
            } else if (Key.UnicodeChar >= L'0' && Key.UnicodeChar <= L'9') {
                if (timeoutStrLen < 2) {
                    timeoutStr[timeoutStrLen] = Key.UnicodeChar;
                    timeoutStrLen++;
                    timeoutStr[timeoutStrLen] = L'\0';
                }
            }

        } else if (currentScreen == SCREEN_THEMES) {
            ConOut->ClearScreen(ConOut);
            ConOut->SetAttribute(ConOut, 0x07);

            ConOut->OutputString(ConOut, L"╔════════════════════════════════════════════════════════════════════════════════╗\r\n");
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.6                            ║\r\n");
            ConOut->OutputString(ConOut, L"╚════════════════════════════════════════════════════════════════════════════════╝\r\n\r\n");

            ConOut->OutputString(ConOut, L"Settings > Themes\r\n\r\n");

            ConOut->SetAttribute(ConOut, 0x70);
            ConOut->OutputString(ConOut, L"Classic Black / White\r\n");
            ConOut->SetAttribute(ConOut, 0x07);

            ConOut->OutputString(ConOut, L"\r\nNo other themes are loaded !\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");
            ConOut->OutputString(ConOut, L"Press Enter to save changes and go back...\r\n");

            wait_key(SystemTable, &Key);

            if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n' || Key.UnicodeChar == L'b' || Key.UnicodeChar == L'B' || Key.UnicodeChar == 0x08 || Key.UnicodeChar == L'\b') {
                currentScreen = SCREEN_SETTINGS;
            }
        } else if (currentScreen == SCREEN_POWER_OPTIONS) {
            ConOut->ClearScreen(ConOut);
            ConOut->SetAttribute(ConOut, 0x07);

            ConOut->OutputString(ConOut, L"╔════════════════════════════════════════════════════════════════════════════════╗\r\n");
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.6                            ║\r\n");
            ConOut->OutputString(ConOut, L"╚════════════════════════════════════════════════════════════════════════════════╝\r\n\r\n");

            ConOut->OutputString(ConOut, L"Power Options\r\n\r\n");

            if (powerSelection == 0) {
                ConOut->SetAttribute(ConOut, 0x70);
                ConOut->OutputString(ConOut, L" < Shut Down > ");
            } else {
                ConOut->SetAttribute(ConOut, 0x07);
                ConOut->OutputString(ConOut, L" < Shut Down > ");
            }
            ConOut->SetAttribute(ConOut, 0x07);
            ConOut->OutputString(ConOut, L"\r\n\r\n");

            if (powerSelection == 1) {
                ConOut->SetAttribute(ConOut, 0x70);
                ConOut->OutputString(ConOut, L" < Reboot > ");
            } else {
                ConOut->SetAttribute(ConOut, 0x07);
                ConOut->OutputString(ConOut, L" < Reboot > ");
            }
            ConOut->SetAttribute(ConOut, 0x07);
            ConOut->OutputString(ConOut, L"\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");

            ConOut->OutputString(ConOut, L"Press Enter to execute selected entry...\r\n");

            wait_key(SystemTable, &Key);

            if (Key.ScanCode == SCAN_UP) {
                if (powerSelection > 0) powerSelection--;
            } else if (Key.ScanCode == SCAN_DOWN) {
                if (powerSelection < 1) powerSelection++;
            } else if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n') {
                if (powerSelection == 0) {
                    SystemTable->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
                } else if (powerSelection == 1) {
                    SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                }
            } else if (Key.UnicodeChar == L'b' || Key.UnicodeChar == L'B' || Key.UnicodeChar == 0x08 || Key.UnicodeChar == L'\b') {
                currentScreen = SCREEN_MAIN;
            }
        }
    }

    return EFI_SUCCESS;
}