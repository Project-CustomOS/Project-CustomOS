#ifndef SHELL_H
#define SHELL_H

// ============================================================================
// shell.h - the contract between kernel.c (the command-line interpreter) and
// commands.c (every command). kernel.c never needs to change when a command is
// added: write the command in commands.c and add one line to its table.
// ============================================================================

#include "efi.h"

#define PROMPT_MAX 40

// The one place the version number lives (about, version, systeminfo, the banner).
#define OS_VERSION   L"0.2.0"
#define OS_BUILD     L"11/24/2026"

// Everything a command needs to know about the running shell.
typedef struct {
    EFI_HANDLE          device;      // boot device (holds the file system)
    EFI_SYSTEM_TABLE   *st;
    unsigned long long *color;       // current text colour
    CHAR16             *cwd;         // current folder (cd changes it)
    CHAR16              prompt[PROMPT_MAX];   // text before the folder in the prompt
    EFI_TIME            startTime;   // wall-clock time when the kernel started
    BOOLEAN             haveStartTime;
} SHELL;

typedef void (*CMD_HANDLER)(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest);

typedef struct {
    const CHAR16 *name;
    const CHAR16 *label;       // shown as "Usage: ..."
    const CHAR16 *category;    // heading used by "help"
    const CHAR16 *details;     // first line is the one-line summary
    CMD_HANDLER   handler;
    int           minArgs;     // counted after the command name
    int           maxArgs;     // -1 = no limit
} CMD;

// ---- provided by kernel.c --------------------------------------------------
void *memset(void *dest, int c, unsigned long long n);
void *memcpy(void *dest, const void *src, unsigned long long n);
void scprint(EFI_SYSTEM_TABLE *SystemTable, const unsigned short *String, unsigned long long Color);
int  scstrcmp(const unsigned short *s1, const unsigned short *s2);
int  scstrncmp(const unsigned short *s1, const unsigned short *s2, unsigned long long n);
void scitoa(unsigned long long num, unsigned short *str);
void strip_whitespace(unsigned short *str);
int  shell_history_count(void);
const unsigned short *shell_history_get(int index);   // 0 = oldest
void shell_history_clear(void);

// ---- provided by commands.c ------------------------------------------------
const CMD *find_command(const CHAR16 *name);
void command_not_found(SHELL *sh, const CHAR16 *line);

#endif
