#include "efi.h"

// Define missing UEFI structures and GUIDs locally if not present in efi.h
#ifndef EFI_LOADED_IMAGE_PROTOCOL_GUID
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

typedef struct {
    unsigned int   Revision;
    EFI_HANDLE     ParentHandle;
    void*          SystemTable;
    EFI_HANDLE     DeviceHandle;
    void*          FilePath;
    void*          Reserved;
    unsigned int   LoadOptionsSize;
    void*          LoadOptions;
    void*          ImageBase;
    unsigned long long ImageSize;
    unsigned int   ImagePolicy;
} EFI_LOADED_IMAGE_PROTOCOL;
#endif

#define SCREEN_MAIN           0
#define SCREEN_SETTINGS       1
#define SCREEN_TIMEOUT        2
#define SCREEN_THEMES         3
#define SCREEN_POWER_OPTIONS  4

// External reference to the autonomous kernel entry point
extern void kernel_main(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut = SystemTable->ConOut;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn = SystemTable->ConIn;

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
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.5                            ║\r\n");
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
                ConOut->OutputString(ConOut, countdown == 10 ? L"10 seconds until selected entry is executed...\r\n" :
                                             countdown == 9  ? L" 9 seconds until selected entry is executed...\r\n" :
                                             countdown == 8  ? L" 8 seconds until selected entry is executed...\r\n" :
                                             countdown == 7  ? L" 7 seconds until selected entry is executed...\r\n" :
                                             countdown == 6  ? L" 6 seconds until selected entry is executed...\r\n" :
                                             countdown == 5  ? L" 5 seconds until selected entry is executed...\r\n" :
                                             countdown == 4  ? L" 4 seconds until selected entry is executed...\r\n" :
                                             countdown == 3  ? L" 3 seconds until selected entry is executed...\r\n" :
                                             countdown == 2  ? L" 2 seconds until selected entry is executed...\r\n" :
                                                               L" 1 second until selected entry is executed...\r\n");
            } else if (!timeoutActive) {
                ConOut->OutputString(ConOut, L"                                                                \r\n");
            } else {
                ConOut->ClearScreen(ConOut);
                ConOut->OutputString(ConOut, L"Booting CustomOS Kernel via Timeout...\r\n");
                
                EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
                EFI_LOADED_IMAGE_PROTOCOL *loadedImage = 0;
                SystemTable->BootServices->HandleProtocol(ImageHandle, &loadedImageGuid, (void**)&loadedImage);
                
                kernel_main(loadedImage ? loadedImage->DeviceHandle : ImageHandle, SystemTable);
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
                        ConOut->ClearScreen(ConOut);
                        ConOut->OutputString(ConOut, L"Booting CustomOS Kernel...\r\n");
                        
                        EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
                        EFI_LOADED_IMAGE_PROTOCOL *loadedImage = 0;
                        SystemTable->BootServices->HandleProtocol(ImageHandle, &loadedImageGuid, (void**)&loadedImage);
                        
                        kernel_main(loadedImage ? loadedImage->DeviceHandle : ImageHandle, SystemTable);
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
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.5                            ║\r\n");
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

            while (ConIn->ReadKeyStroke(ConIn, &Key) != EFI_SUCCESS);

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
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.5                            ║\r\n");
            ConOut->OutputString(ConOut, L"╚════════════════════════════════════════════════════════════════════════════════╝\r\n\r\n");

            ConOut->OutputString(ConOut, L"Settings > Boot Timeout\r\n\r\n");
            ConOut->OutputString(ConOut, L"Time in seconds: ");
            ConOut->OutputString(ConOut, timeoutStr);
            ConOut->OutputString(ConOut, L" _\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");
            ConOut->OutputString(ConOut, L"Press Enter to save changes and go back...\r\n");

            while (ConIn->ReadKeyStroke(ConIn, &Key) != EFI_SUCCESS);

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
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.5                            ║\r\n");
            ConOut->OutputString(ConOut, L"╚════════════════════════════════════════════════════════════════════════════════╝\r\n\r\n");

            ConOut->OutputString(ConOut, L"Settings > Themes\r\n\r\n");

            ConOut->SetAttribute(ConOut, 0x70);
            ConOut->OutputString(ConOut, L"Classic Black / White\r\n");
            ConOut->SetAttribute(ConOut, 0x07);

            ConOut->OutputString(ConOut, L"\r\nNo other themes are loaded !\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n");
            ConOut->OutputString(ConOut, L"Press Enter to save changes and go back...\r\n");

            while (ConIn->ReadKeyStroke(ConIn, &Key) != EFI_SUCCESS);

            if (Key.UnicodeChar == L'\r' || Key.UnicodeChar == L'\n' || Key.UnicodeChar == L'b' || Key.UnicodeChar == L'B' || Key.UnicodeChar == 0x08 || Key.UnicodeChar == L'\b') {
                currentScreen = SCREEN_SETTINGS;
            }
        } else if (currentScreen == SCREEN_POWER_OPTIONS) {
            ConOut->ClearScreen(ConOut);
            ConOut->SetAttribute(ConOut, 0x07);

            ConOut->OutputString(ConOut, L"╔════════════════════════════════════════════════════════════════════════════════╗\r\n");
            ConOut->OutputString(ConOut, L"║                            CustomOS Bootloader v0.5                            ║\r\n");
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

            while (ConIn->ReadKeyStroke(ConIn, &Key) != EFI_SUCCESS);

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