#include "efi.h"
#include "cpuid.h"
#include "memory.h"

static char fileContent[4096];

// --- Multi-Slot Command History Buffer ---
#define MAX_HISTORY 10
static unsigned short commandHistory[MAX_HISTORY][256];
static int historyCount = 0;
static int historyNavIndex = -1; // -1 means currently typing a new command

void *memset(void *dest, int c, unsigned long long n) {
    unsigned char *p = (unsigned char *)dest;
    while (n--) *p++ = (unsigned char)c;
    return dest;
}

void scprint(EFI_SYSTEM_TABLE *SystemTable, const unsigned short *String, unsigned long long Color) {
    SystemTable->ConOut->SetAttribute(SystemTable->ConOut, Color);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (unsigned short*)String);
}

int scstrcmp(const unsigned short *s1, const unsigned short *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *s1 - *s2;
}

int scstrncmp(const unsigned short *s1, const unsigned short *s2, unsigned long long n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *s1 - *s2;
}

void scitoa(unsigned long long num, unsigned short *str) {
    int i = 0;
    if (num == 0) { str[i++] = L'0'; str[i] = 0; return; }
    while (num > 0) { str[i++] = L'0' + (num % 10); num /= 10; }
    str[i] = 0;
    for (int j = 0; j < i / 2; j++) {
        unsigned short temp = str[j]; str[j] = str[i - j - 1]; str[i - j - 1] = temp;
    }
}

void strip_whitespace(unsigned short *str) {
    int len = 0;
    while (str[len]) len++;
    while (len > 0 && (str[len - 1] == L' ' || str[len - 1] == L'\r' || str[len - 1] == L'\n')) {
        str[len - 1] = 0;
        len--;
    }
}

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

void kernel_list_directory(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, unsigned long long currentTextColor, const CHAR16 *currentDir) {
    scprint(SystemTable, L"\r\nDirectory listing of ", currentTextColor);
    scprint(SystemTable, currentDir, currentTextColor);
    scprint(SystemTable, L":\r\n\r\n", currentTextColor);

    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem = 0;
    
    if (SystemTable->BootServices->HandleProtocol(DeviceHandle, &fsGuid, (void**)&fileSystem) != EFI_SUCCESS) {
        scprint(SystemTable, L"  [X] Error: Could not access File System Protocol.\r\n", 0x0C);
        return;
    }

    EFI_FILE_PROTOCOL *rootDrive = 0;
    if (fileSystem->OpenVolume(fileSystem, &rootDrive) != EFI_SUCCESS) {
        scprint(SystemTable, L"  [X] Error: Could not open root volume.\r\n", 0x0C);
        return;
    }

    EFI_FILE_PROTOCOL *targetDir = 0;
    EFI_FILE_PROTOCOL *dirToRead = rootDrive;

    // If we are in a subfolder, open that specific directory handle
    if (currentDir[0] != L'\\' || currentDir[1] != 0) {
        if (rootDrive->Open(rootDrive, &targetDir, (CHAR16*)currentDir, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
            scprint(SystemTable, L"  [X] Error: Could not open current directory.\r\n", 0x0C);
            rootDrive->Close(rootDrive);
            return;
        }
        dirToRead = targetDir;
    }

    char infoBuffer[1024];
    unsigned long long readSize = 1024;
    UINTN fileCount = 0;
    unsigned short sizeStr[32];

    while (dirToRead->Read(dirToRead, &readSize, infoBuffer) == EFI_SUCCESS && readSize > 0) {
        EFI_FILE_INFO *fileInfo = (EFI_FILE_INFO*)infoBuffer;
        unsigned short *srcName = (unsigned short*)(infoBuffer + 80);
        
        if (srcName[0] == 0) {
            readSize = 1024;
            continue;
        }

        scprint(SystemTable, srcName, currentTextColor);
        
        int nameLen = 0;
        while (srcName[nameLen] && nameLen < 255) nameLen++;
        for (int i = nameLen; i < 20; i++) scprint(SystemTable, L" ", currentTextColor);

        if (fileInfo->Attribute & 0x0000000000000010ULL) {
            scprint(SystemTable, L"Folder          ", currentTextColor);
        } else {
            scprint(SystemTable, L"File            ", currentTextColor);
        }

        scitoa(fileInfo->FileSize, sizeStr);
        scprint(SystemTable, sizeStr, currentTextColor);
        scprint(SystemTable, L"    bytes\r\n", currentTextColor);

        fileCount++;
        readSize = 1024;
    }

    if (fileCount == 0) {
        scprint(SystemTable, L"  (No files detected in directory)\r\n", currentTextColor);
    }

    if (targetDir) {
        targetDir->Close(targetDir);
    }
    rootDrive->Close(rootDrive);
}

void read_file_directly(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, const unsigned short *FileName, unsigned long long currentTextColor) {
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem = 0;
    if (SystemTable->BootServices->HandleProtocol(DeviceHandle, &fsGuid, (void**)&fileSystem) != EFI_SUCCESS) {
        scprint(SystemTable, L"\r\n[X] Hardware Access Fault.\r\n", 0x0C);
        return;
    }

    EFI_FILE_PROTOCOL *rootDrive = 0;
    if (fileSystem->OpenVolume(fileSystem, &rootDrive) != EFI_SUCCESS) return;

    EFI_FILE_PROTOCOL *targetFile = 0;
    unsigned short cleanFileName[256];
    int fnLen = 0;
    while (FileName[fnLen] && fnLen < 255) {
        cleanFileName[fnLen] = FileName[fnLen];
        fnLen++;
    }
    cleanFileName[fnLen] = 0;
    strip_whitespace(cleanFileName);

    if (rootDrive->Open(rootDrive, &targetFile, cleanFileName, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        scprint(SystemTable, L"\r\n[X] File Not Found: Check spelling or extension.\r\n", 0x0C);
        rootDrive->Close(rootDrive);
        return;
    }

    unsigned long long readSize = 4095;
    memset(fileContent, 0, sizeof(fileContent));

    if (targetFile->Read(targetFile, &readSize, fileContent) == EFI_SUCCESS) {
        scprint(SystemTable, L"\r\n--- File Content ---\r\n", 0x0B); 
        unsigned short wideLine[2] = {0, 0};
        int charIdx = 0;
        while (fileContent[charIdx] != 0 && charIdx < 4096) {
            if (fileContent[charIdx] == '\n') {
                scprint(SystemTable, L"\r\n", currentTextColor);
            } else if (fileContent[charIdx] != '\r') {
                wideLine[0] = (unsigned short)fileContent[charIdx];
                scprint(SystemTable, wideLine, currentTextColor);
            }
            charIdx++;
        }
        scprint(SystemTable, L"\r\n--------------------\r\n", 0x0B);
    }

    targetFile->Close(targetFile);
    rootDrive->Close(rootDrive);
}

// Commands start here !!

void execute_command(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, unsigned short *cmdBuffer, unsigned long long *currentTextColor, CHAR16 *currentDir) {
    strip_whitespace(cmdBuffer);

	// List command

    if (scstrcmp(cmdBuffer, L"list") == 0) {
        kernel_list_directory(DeviceHandle, SystemTable, *currentTextColor, currentDir);
    }
	
	// Echo command
	
	else if (scstrcmp(cmdBuffer, L"echo") == 0 || scstrncmp(cmdBuffer, L"echo ", 5) == 0) {
        // Check if there is anything after "echo "
        const unsigned short *textToEcho = cmdBuffer + 4;
        while (*textToEcho == L' ') {
            textToEcho++;
        }

        if (*textToEcho == 0) {
            scprint(SystemTable, L"\r\nNothing to echo!\r\n", *currentTextColor);
        } else {
            scprint(SystemTable, L"\r\n", *currentTextColor);
            scprint(SystemTable, textToEcho, *currentTextColor);
            scprint(SystemTable, L"\r\n", *currentTextColor);
        }
    }
	
	// CD command
	
	else if (scstrncmp(cmdBuffer, L"cd ", 3) == 0 || scstrcmp(cmdBuffer, L"cd") == 0) {
        const unsigned short *targetPath = cmdBuffer + 2;
        while (*targetPath == L' ') targetPath++;

        if (*targetPath == 0) {
            scprint(SystemTable, L"\r\n", *currentTextColor);
            scprint(SystemTable, currentDir, *currentTextColor);
            scprint(SystemTable, L"\r\n", *currentTextColor);
            return;
        }

        // Build prospective path buffer
        CHAR16 newPath[256];
        int i = 0;

        // Handle absolute path vs relative path
        if (targetPath[0] == L'/' || targetPath[0] == L'\\') {
            while (targetPath[i] && i < 255) {
                newPath[i] = (targetPath[i] == L'/') ? L'\\' : targetPath[i];
                i++;
            }
            newPath[i] = 0;
        } 
        else if (targetPath[0] == L'.' && targetPath[1] == L'.') {
            // Handle parent rollback (cd ..)
            int len = 0;
            while (currentDir[len]) len++;
            
            while (len > 0 && currentDir[len - 1] != L'\\') {
                len--;
            }
            if (len > 1) {
                len--; // remove trailing slash
            }
            for (int j = 0; j < len; j++) {
                newPath[j] = currentDir[j];
            }
            newPath[len] = 0;
            if (newPath[0] == 0) {
                newPath[0] = L'\\';
                newPath[1] = 0;
            }
        } 
        else {
            // Handle relative subfolder entry (e.g., cd EFI)
            int len = 0;
            while (currentDir[len]) {
                newPath[len] = currentDir[len];
                len++;
            }
            if (len > 1 || newPath[0] != L'\\') {
                newPath[len++] = L'\\';
            }
            int j = 0;
            while (targetPath[j] && len < 255) {
                newPath[len++] = targetPath[j++];
            }
            newPath[len] = 0;
        }

        // Verify folder exists via File System Protocol before committing change
        EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem = 0;
        EFI_FILE_PROTOCOL *rootDrive = 0;
        EFI_FILE_PROTOCOL *testDir = 0;

        if (SystemTable->BootServices->HandleProtocol(DeviceHandle, &fsGuid, (void**)&fileSystem) == EFI_SUCCESS) {
            if (fileSystem->OpenVolume(fileSystem, &rootDrive) == EFI_SUCCESS) {
                if (rootDrive->Open(rootDrive, &testDir, newPath, EFI_FILE_MODE_READ, 0) == EFI_SUCCESS) {
                    // Query the exact canonical folder name from disk metadata
                    char infoBuf[512];
                    UINTN infoSize = 512;
                    EFI_GUID infoGuid = EFI_FILE_INFO_GUID;
                    
                    // Typecast GetInfo function pointer safely
                    EFI_STATUS (EFIAPI *GetInfoPtr)(EFI_FILE_PROTOCOL*, EFI_GUID*, UINTN*, VOID*) = 
                        (EFI_STATUS (EFIAPI *)(EFI_FILE_PROTOCOL*, EFI_GUID*, UINTN*, VOID*))testDir->GetInfo;

                    if (GetInfoPtr && GetInfoPtr(testDir, &infoGuid, &infoSize, infoBuf) == EFI_SUCCESS) {
                        EFI_FILE_INFO *fileInfo = (EFI_FILE_INFO*)infoBuf;
                        
                        // If it's a relative path entry, replace the leaf component with the canonical name
                        if (!(targetPath[0] == '/' || targetPath[0] == '\\') && !(targetPath[0] == '.' && targetPath[1] == '.')) {
                            // Find the last backslash in newPath and replace everything after it with fileInfo->FileName
                            int lastSlash = 0;
                            int idx = 0;
                            while (newPath[idx]) {
                                if (newPath[idx] == L'\\') lastSlash = idx;
                                idx++;
                            }
                            
                            int fIdx = 0;
                            lastSlash++; // Move past the slash
                            while (fileInfo->FileName[fIdx] && lastSlash < 255) {
                                newPath[lastSlash++] = fileInfo->FileName[fIdx++];
                            }
                            newPath[lastSlash] = 0;
                        }
                    }

                    testDir->Close(testDir);
                    
                    // Commit path change with correct capitalization
                    int cIdx = 0;
                    while (newPath[cIdx]) {
                        currentDir[cIdx] = newPath[cIdx];
                        cIdx++;
                    }
                    currentDir[cIdx] = 0;
                    scprint(SystemTable, L"\r\n", *currentTextColor);
                }
                rootDrive->Close(rootDrive);
            }
        }
    }
	
	// Type command
	
    else if (scstrncmp(cmdBuffer, L"type ", 5) == 0) {
        const unsigned short *reqFile = cmdBuffer + 5;
        read_file_directly(DeviceHandle, SystemTable, reqFile, *currentTextColor);
    }
	
	// Clear command
	
    else if (scstrcmp(cmdBuffer, L"clear") == 0) {
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    } 
	
	// Color command
	
    else if (scstrncmp(cmdBuffer, L"color ", 6) == 0) {
        const unsigned short *colorArg = cmdBuffer + 6;
        if (scstrcmp(colorArg, L"green") == 0) {
            *currentTextColor = 0x0A; 
            scprint(SystemTable, L"\r\nTheme updated to Neon Green.\r\n", *currentTextColor);
        } else if (scstrcmp(colorArg, L"cyan") == 0) {
            *currentTextColor = 0x0B; 
            scprint(SystemTable, L"\r\nTheme updated to Bright Cyan.\r\n", *currentTextColor);
        } else if (scstrcmp(colorArg, L"red") == 0) {
            *currentTextColor = 0x0C; 
            scprint(SystemTable, L"\r\nTheme updated to Warning Red.\r\n", *currentTextColor);
        } else if (scstrcmp(colorArg, L"white") == 0) {
            *currentTextColor = 0x0F; 
            scprint(SystemTable, L"\r\nTheme updated to Standard White.\r\n", *currentTextColor);
        } else {
            scprint(SystemTable, L"\r\n[X] Unknown color theme. Try: green, cyan, red, white\r\n", *currentTextColor);
        }
    }
	
	// Reboot command
	
    else if (scstrcmp(cmdBuffer, L"reboot") == 0) {
        scprint(SystemTable, L"\r\n[!] Restarting system hardware canvas channels...", 0x0E); 
        __asm__ __volatile__ (
            "movb $0xFE, %%al\n\t"
            "outb %%al, $0x64"
            : : : "al"
        );
    }
	
	// Shutdown command
	
	else if (scstrcmp(cmdBuffer, L"shutdown") == 0) {
    scprint(SystemTable, L"\r\nShutting down CustomOS...\r\n", 0x0C);
    
    // Call UEFI Runtime Services to trigger a clean shutdown
    SystemTable->RuntimeServices->ResetSystem(
        EfiResetShutdown, 
        EFI_SUCCESS, 
        0, 
        NULL
    );
}

	// About command

	else if (scstrcmp(cmdBuffer, L"about") == 0) {
    scprint(SystemTable, L"\r\nCustomOS 0.1.4 - Compiled on 10/18/2026\r\n", *currentTextColor);
}
	// Systeminfo command

    else if (scstrcmp(cmdBuffer, L"systeminfo") == 0) {
    scprint(SystemTable, L"\r\n=== CustomOS System Information ===\r\n", 0x0B);
    scprint(SystemTable, L" OS Version : CustomOS 0.1.4\r\n", *currentTextColor);
    scprint(SystemTable, L" Architecture: x86_64 (64-bit Freestanding UEFI)\r\n", *currentTextColor);
    
    // Dynamic Firmware Vendor
    scprint(SystemTable, L" Firmware Vendor: ", *currentTextColor);
    scprint(SystemTable, SystemTable->FirmwareVendor, *currentTextColor);
    scprint(SystemTable, L"\r\n", *currentTextColor);

    // Dynamic CPU Model
    CHAR16 cpuBuffer[64];
    get_cpu_brand_string(cpuBuffer, 64);
    scprint(SystemTable, L" CPU Model   : ", *currentTextColor);
    scprint(SystemTable, cpuBuffer, *currentTextColor);
    scprint(SystemTable, L"\r\n", *currentTextColor);

    scprint(SystemTable, L" Platform    : Bare-Metal / UEFI Host\r\n", *currentTextColor);

    // Dynamic RAM Calculation
    CHAR16 memString[64];
    UINT64 totalBytes = get_total_memory_bytes(SystemTable);
    format_memory_size(totalBytes, memString, 64);
    scprint(SystemTable, L" Memory      : ", *currentTextColor);
    scprint(SystemTable, memString, *currentTextColor);
    scprint(SystemTable, L" Detected\r\n", *currentTextColor);

    scprint(SystemTable, L"=====================================\r\n", 0x0B);
}
	// Help command

    else if (scstrcmp(cmdBuffer, L"help") == 0) {
        scprint(SystemTable, L"\r\nAvailable Commands: list, type [filename], color [theme], echo [text], clear, reboot, shutdown, about, systeminfo, help\r\n", *currentTextColor);
    }
	
	// Command unrecognition
	
    else if (cmdBuffer[0] != 0) {
        scprint(SystemTable, L"\r\n'", *currentTextColor);
        scprint(SystemTable, cmdBuffer, 0x0C);
        scprint(SystemTable, L"' is not recognized as an command, program, or file. Type 'help' for the list of commands.\r\n", *currentTextColor);
    }
}

void print_prompt(EFI_SYSTEM_TABLE *SystemTable, const CHAR16 *dir, unsigned long long color) {
    scprint(SystemTable, L"CustomOS:", color);
    scprint(SystemTable, dir, color); // Prints "\"
    // Since dir[0] is '\\' and dir[1] is '\0', this condition is skipped
    if (dir[0] != L'\\' || dir[1] != 0) {
        scprint(SystemTable, L"\\", color);
    }
    scprint(SystemTable, L"> ", color);
}

void kernel_main(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable) {
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    SystemTable->ConOut->EnableCursor(SystemTable->ConOut, TRUE);
    
    unsigned long long activeColor = 0x0F; 
    scprint(SystemTable, L"CustomOS (Version 0.1.4) Command Line\r\n\r\n", activeColor);

    CHAR16 currentDir[256];
    currentDir[0] = L'\\';
    currentDir[1] = 0;
	
	unsigned short cmdBuffer[256];
    memset(cmdBuffer, 0, sizeof(cmdBuffer));
    int cmdLength = 0;
    
    EFI_INPUT_KEY key;
    unsigned short echoBuffer[2];
    memset(echoBuffer, 0, sizeof(echoBuffer));

    print_prompt(SystemTable, currentDir, activeColor);
    
    // Snappy cursor blinking tick state
    unsigned long long tickCounter = 0;
    int cursorVisible = 1;

    while (1) {
        if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &key) == EFI_SUCCESS) {
            // Ensure cursor is fully visible when user starts typing
            if (!cursorVisible) {
                SystemTable->ConOut->EnableCursor(SystemTable->ConOut, TRUE);
                cursorVisible = 1;
            }

            // 1. Process Enter (Execute Command & Cache History)
            if (key.UnicodeChar == L'\r' || key.UnicodeChar == L'\n') {
                cmdBuffer[cmdLength] = 0;
                
                if (cmdLength > 0) {
                    if (historyCount < MAX_HISTORY) {
                        int hIdx = 0;
                        while (cmdBuffer[hIdx]) {
                            commandHistory[historyCount][hIdx] = cmdBuffer[hIdx];
                            hIdx++;
                        }
                        commandHistory[historyCount][hIdx] = 0;
                        historyCount++;
                    } else {
                        for (int i = 0; i < MAX_HISTORY - 1; i++) {
                            int hIdx = 0;
                            while (commandHistory[i + 1][hIdx]) {
                                commandHistory[i][hIdx] = commandHistory[i + 1][hIdx];
                                hIdx++;
                            }
                            commandHistory[i][hIdx] = 0;
                        }
                        int hIdx = 0;
                        while (cmdBuffer[hIdx]) {
                            commandHistory[MAX_HISTORY - 1][hIdx] = cmdBuffer[hIdx];
                            hIdx++;
                        }
                        commandHistory[MAX_HISTORY - 1][hIdx] = 0;
                    }
                }
                historyNavIndex = -1;

                execute_command(DeviceHandle, SystemTable, cmdBuffer, &activeColor, currentDir);
                
                int wasClear = (scstrcmp(cmdBuffer, L"clear") == 0);
                cmdLength = 0;
                memset(cmdBuffer, 0, sizeof(cmdBuffer));
                
                if (!wasClear) scprint(SystemTable, L"\r\n", activeColor);
				print_prompt(SystemTable, currentDir, activeColor);
            } 
            // 2. Process Backspace
            else if (key.UnicodeChar == 8) {
                if (cmdLength > 0) {
                    cmdLength--;
                    cmdBuffer[cmdLength] = 0;
                    scprint(SystemTable, L"\b \b", activeColor);
                }
            } 
            // 3. Process Up Arrow Key
            else if (key.ScanCode == SCAN_UP) {
                if (historyCount > 0) {
                    if (historyNavIndex == -1) {
                        historyNavIndex = historyCount - 1;
                    } else if (historyNavIndex > 0) {
                        historyNavIndex--;
                    }
                    
                    for (int i = 0; i < cmdLength; i++) {
                        scprint(SystemTable, L"\b \b", activeColor);
                    }
                    
                    int hIdx = 0;
                    while (commandHistory[historyNavIndex][hIdx]) {
                        cmdBuffer[hIdx] = commandHistory[historyNavIndex][hIdx];
                        hIdx++;
                    }
                    cmdBuffer[hIdx] = 0;
                    cmdLength = hIdx;
                    
                    scprint(SystemTable, cmdBuffer, activeColor);
                }
            }
            // 4. Process Down Arrow Key
            else if (key.ScanCode == SCAN_DOWN) {
                if (historyNavIndex != -1) {
                    for (int i = 0; i < cmdLength; i++) {
                        scprint(SystemTable, L"\b \b", activeColor);
                    }

                    historyNavIndex++;
                    if (historyNavIndex >= historyCount) {
                        historyNavIndex = -1;
                        cmdLength = 0;
                        memset(cmdBuffer, 0, sizeof(cmdBuffer));
                    } else {
                        int hIdx = 0;
                        while (commandHistory[historyNavIndex][hIdx]) {
                            cmdBuffer[hIdx] = commandHistory[historyNavIndex][hIdx];
                            hIdx++;
                        }
                        cmdBuffer[hIdx] = 0;
                        cmdLength = hIdx;
                        scprint(SystemTable, cmdBuffer, activeColor);
                    }
                }
            }
            // 5. Process Printable Characters
            else if (key.UnicodeChar >= 32 && key.UnicodeChar <= 126) {
                if (cmdLength < 255) {
                    cmdBuffer[cmdLength] = key.UnicodeChar;
                    cmdLength++; 
                    cmdBuffer[cmdLength] = 0; 
                    echoBuffer[0] = key.UnicodeChar; 
                    scprint(SystemTable, echoBuffer, activeColor);
                }
            }
        } else {
            // Snappy Idle Blink Loop (toggles native hardware cursor visibility cleanly)
            tickCounter++;
            if (tickCounter > 300000) { // Faster toggle threshold for a snappy blink
                tickCounter = 0;
                if (cursorVisible) {
                    SystemTable->ConOut->EnableCursor(SystemTable->ConOut, FALSE);
                    cursorVisible = 0;
                } else {
                    SystemTable->ConOut->EnableCursor(SystemTable->ConOut, TRUE);
                    cursorVisible = 1;
                }
            }
        }
    }
}