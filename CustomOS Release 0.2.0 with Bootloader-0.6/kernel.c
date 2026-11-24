// ============================================================================
// kernel.c - the CustomOS command-line interpreter.
//
// This file only reads a line, splits it into words, finds the command in the
// table that lives in commands.c and runs it. Every command is in commands.c,
// so adding or changing a command never touches this file.
// ============================================================================

#include "shell.h"

// --- Multi-Slot Command History Buffer ---
#define MAX_HISTORY 10
static unsigned short commandHistory[MAX_HISTORY][256];
static int historyCount = 0;
static int historyNavIndex = -1; // -1 means currently typing a new command

// ============================================================================
// 1. COMPILER HELPERS AND BASIC TEXT OUTPUT
// ============================================================================

// GCC may emit calls to these even in freestanding code. Build with
// -fno-tree-loop-distribute-patterns so the loops below are never turned back
// into calls to themselves.
void *memset(void *dest, int c, unsigned long long n) {
    unsigned char *p = (unsigned char *)dest;
    while (n--) *p++ = (unsigned char)c;
    return dest;
}

void *memcpy(void *dest, const void *src, unsigned long long n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
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


// ============================================================================
// 2. ARGUMENT PARSER AND COMMAND DISPATCH
// ============================================================================

#define MAX_ARGS 16

// Splits `line` into words. Words may be wrapped in double quotes to include
// spaces ("my file.txt"); the quotes themselves are removed. The words are
// copied into `buf`, which must hold at least str_len(line) + 2 characters.
static int tokenize(const CHAR16 *line, CHAR16 *buf, CHAR16 **argv, int maxArgs) {
    int argc = 0;
    int b = 0;
    const CHAR16 *p = line;

    while (*p) {
        while (*p == L' ') p++;
        if (!*p) break;

        CHAR16 *word = &buf[b];
        BOOLEAN inQuotes = FALSE;

        while (*p && (inQuotes || *p != L' ')) {
            if (*p == L'"') {
                inQuotes = !inQuotes;
                p++;
                continue;
            }
            buf[b++] = *p++;
        }
        buf[b++] = 0;

        if (argc < maxArgs) argv[argc] = word;
        argc++;
    }
    return (argc > maxArgs) ? maxArgs : argc;
}


void execute_command(SHELL *sh, unsigned short *cmdBuffer) {
    strip_whitespace(cmdBuffer);
    if (cmdBuffer[0] == 0) return;

    CHAR16 argBuf[264];
    CHAR16 *argv[MAX_ARGS];
    int argc = tokenize(cmdBuffer, argBuf, argv, MAX_ARGS);

    const CMD *cmd = find_command(argv[0]);
    if (!cmd) {
        command_not_found(sh, cmdBuffer);
        return;
    }

    // The raw text after the command name, for commands that want it as typed.
    const CHAR16 *rest = cmdBuffer;
    while (*rest == L' ') rest++;
    while (*rest && *rest != L' ') rest++;
    while (*rest == L' ') rest++;

    int nargs = argc - 1;
    if (nargs < cmd->minArgs || (cmd->maxArgs >= 0 && nargs > cmd->maxArgs)) {
        scprint(sh->st, L"\r\nUsage: ", *sh->color);
        scprint(sh->st, cmd->label, *sh->color);
        scprint(sh->st, L"\r\n", *sh->color);
        if (cmd->maxArgs > 0 && nargs > cmd->maxArgs) {
            scprint(sh->st, L"Names that contain spaces must be wrapped in quotes.\r\n", *sh->color);
        }
        return;
    }

    cmd->handler(sh, argc, argv, rest);
}

void print_prompt(SHELL *sh) {
    unsigned long long color = *sh->color;
    scprint(sh->st, sh->prompt, color);
    scprint(sh->st, sh->cwd, color);
    if (sh->cwd[0] != L'\\' || sh->cwd[1] != 0) {
        scprint(sh->st, L"\\", color);
    }
    scprint(sh->st, L"> ", color);
}

// ============================================================================
// 3. COMMAND HISTORY
// ============================================================================

static void copy_cmd(unsigned short *dst, const unsigned short *src) {
    int i = 0;
    while (src[i] && i < 255) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void history_push(const unsigned short *cmd) {
    if (historyCount >= MAX_HISTORY) {
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            copy_cmd(commandHistory[i], commandHistory[i + 1]);
        }
        historyCount = MAX_HISTORY - 1;
    }
    copy_cmd(commandHistory[historyCount], cmd);
    historyCount++;
}

int shell_history_count(void) { return historyCount; }
const unsigned short *shell_history_get(int index) {
    return (index >= 0 && index < historyCount) ? commandHistory[index] : (const unsigned short *)L"";
}
void shell_history_clear(void) { historyCount = 0; historyNavIndex = -1; }

// ============================================================================
// 4. KERNEL ENTRY AND MAIN LOOP
// ============================================================================

EFI_STATUS EFIAPI kernel_main(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;

    // Firmware arms a 5 minute watchdog before starting a boot image; if it is
    // left running the machine resets itself. 0 disables it.
    bs->SetWatchdogTimer(0, 0, 0, NULL);

    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    SystemTable->ConOut->EnableCursor(SystemTable->ConOut, TRUE);

    // Flush any lingering keystrokes from the bootloader menu
    EFI_INPUT_KEY dummyKey;
    while (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &dummyKey) == EFI_SUCCESS) {
        // Drain buffer
    }

    unsigned long long activeColor = 0x0F;
    scprint(SystemTable, L"CustomOS (Version " OS_VERSION L") Command Line\r\n\r\n", activeColor);

    CHAR16 currentDir[256];
    currentDir[0] = L'\\';
    currentDir[1] = 0;

    static SHELL sh;
    memset(&sh, 0, sizeof(sh));
    sh.device = DeviceHandle;
    sh.st     = SystemTable;
    sh.color  = &activeColor;
    sh.cwd    = currentDir;
    {
        const CHAR16 *p = L"CustomOS:";
        int i = 0;
        while (p[i]) { sh.prompt[i] = p[i]; i++; }
        sh.prompt[i] = 0;
    }
    if (SystemTable->RuntimeServices->GetTime(&sh.startTime, NULL) == EFI_SUCCESS) sh.haveStartTime = TRUE;

    unsigned short cmdBuffer[256];
    memset(cmdBuffer, 0, sizeof(cmdBuffer));
    int cmdLength = 0;

    EFI_INPUT_KEY key;
    unsigned short echoBuffer[2];
    memset(echoBuffer, 0, sizeof(echoBuffer));

    print_prompt(&sh);

    // Sleep until a key arrives or the 500 ms blink timer fires, instead of
    // spinning the CPU at 100%. If the timer can't be created we fall back to
    // short sleeps with a counter.
    EFI_EVENT timerEvent = NULL;
    BOOLEAN haveTimer = FALSE;
    if (bs->CreateEvent(EVT_TIMER, TPL_APPLICATION, NULL, NULL, &timerEvent) == EFI_SUCCESS) {
        if (bs->SetTimer(timerEvent, TimerPeriodic, 5000000ULL) == EFI_SUCCESS) {   // 500 ms in 100 ns units
            haveTimer = TRUE;
        }
    }
    EFI_EVENT waitList[2];
    waitList[0] = SystemTable->ConIn->WaitForKey;
    waitList[1] = timerEvent;

    unsigned int tickCounter = 0;
    int cursorVisible = 1;

    while (1) {
        UINTN fired = 0;
        if (haveTimer) {
            bs->WaitForEvent(2, waitList, &fired);
        } else {
            bs->Stall(10000);   // 10 ms
        }

        if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (!cursorVisible) {
                SystemTable->ConOut->EnableCursor(SystemTable->ConOut, TRUE);
                cursorVisible = 1;
            }

            // 1. Process Enter (Execute Command & Cache History)
            if (key.UnicodeChar == L'\r' || key.UnicodeChar == L'\n') {
                cmdBuffer[cmdLength] = 0;

                if (cmdLength > 0) {
                    history_push(cmdBuffer);
                }
                historyNavIndex = -1;

                execute_command(&sh, cmdBuffer);

                int wasClear = (scstrcmp(cmdBuffer, L"clear") == 0);
                cmdLength = 0;
                memset(cmdBuffer, 0, sizeof(cmdBuffer));

                if (!wasClear) scprint(SystemTable, L"\r\n", activeColor);
                print_prompt(&sh);
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

                    copy_cmd(cmdBuffer, commandHistory[historyNavIndex]);
                    cmdLength = 0;
                    while (cmdBuffer[cmdLength]) cmdLength++;

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
                        copy_cmd(cmdBuffer, commandHistory[historyNavIndex]);
                        cmdLength = 0;
                        while (cmdBuffer[cmdLength]) cmdLength++;
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
            BOOLEAN blink = haveTimer ? (fired == 1) : (++tickCounter >= 50);   // 50 x 10 ms = 500 ms
            if (blink) {
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

    return EFI_SUCCESS;
}
