#include "efi.h"
#include "cpu_info.h"
#include "memory.h"
#include "path.h"

static char fileContent[4096];

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

static int str_len(const CHAR16 *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

// Replaces dst with src (truncating if it does not fit).
static void str_set(CHAR16 *dst, int max, const CHAR16 *src) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// Appends src to dst without ever writing past dst[max - 1].
static void str_cat(CHAR16 *dst, int max, const CHAR16 *src) {
    int d = str_len(dst);
    int i = 0;
    while (src[i] && d < max - 1) dst[d++] = src[i++];
    dst[d] = 0;
}

// ============================================================================
// 2. SHELL CONTEXT AND OUTPUT HELPERS
// ============================================================================

// Everything a command needs to know about the running shell.
typedef struct {
    EFI_HANDLE          device;   // boot device (holds the file system)
    EFI_SYSTEM_TABLE   *st;
    unsigned long long *color;    // current text colour
    CHAR16             *cwd;      // current folder (cd changes it)
} SHELL;

static void put(SHELL *sh, const CHAR16 *s)      { scprint(sh->st, s, *sh->color); }
static void put_err(SHELL *sh, const CHAR16 *s)  { scprint(sh->st, s, 0x0C); }

static void put_num(SHELL *sh, UINT64 n) {
    CHAR16 b[32];
    scitoa(n, b);
    put(sh, b);
}

// "1 file" / "3 files"
static void put_count(SHELL *sh, UINT64 n, const CHAR16 *one, const CHAR16 *many) {
    put_num(sh, n);
    put(sh, L" ");
    put(sh, n == 1 ? one : many);
}

// ============================================================================
// 3. PATH HELPERS (normalize_path itself lives in path.h)
// ============================================================================

static CHAR16 fold_case(CHAR16 c) {
    return (c >= L'a' && c <= L'z') ? (CHAR16)(c - 32) : c;
}

// FAT is case-insensitive, so paths are compared that way too.
static BOOLEAN path_eq(const CHAR16 *a, const CHAR16 *b) {
    while (*a && *b) {
        if (fold_case(*a) != fold_case(*b)) return FALSE;
        a++; b++;
    }
    return *a == *b;
}

// TRUE if `path` is `dir` itself or lies anywhere inside it.
static BOOLEAN path_within(const CHAR16 *path, const CHAR16 *dir) {
    int dl = str_len(dir);
    if (dl == 1 && dir[0] == L'\\') return TRUE;         // everything is inside the root
    for (int i = 0; i < dl; i++) {
        if (fold_case(path[i]) != fold_case(dir[i])) return FALSE;
    }
    return path[dl] == 0 || path[dl] == L'\\';
}

static BOOLEAN is_root_path(const CHAR16 *path) {
    return path[0] == L'\\' && path[1] == 0;
}

// Last component: "\a\b.txt" -> "b.txt"
static const CHAR16 *path_base(const CHAR16 *path) {
    const CHAR16 *base = path;
    for (const CHAR16 *p = path; *p; p++) {
        if (*p == L'\\') base = p + 1;
    }
    return base;
}

// "\a\b" -> "\a", "\a" -> "\"
static void path_parent(const CHAR16 *path, CHAR16 *out) {
    int last = 0;
    int i = 0;
    while (path[i]) {
        if (path[i] == L'\\') last = i;
        i++;
    }
    if (last == 0) {
        out[0] = L'\\';
        out[1] = 0;
        return;
    }
    for (int k = 0; k < last; k++) out[k] = path[k];
    out[last] = 0;
}

// "\a" + "b" -> "\a\b"   "\" + "b" -> "\b"
static BOOLEAN path_join(const CHAR16 *dir, const CHAR16 *name, CHAR16 *out, int max) {
    int len = 0;
    for (int i = 0; dir[i]; i++) {
        if (len >= max - 1) return FALSE;
        out[len++] = dir[i];
    }
    if (!(len == 1 && out[0] == L'\\')) {
        if (len >= max - 1) return FALSE;
        out[len++] = L'\\';
    }
    for (int i = 0; name[i]; i++) {
        if (len >= max - 1) return FALSE;
        out[len++] = name[i];
    }
    out[len] = 0;
    return TRUE;
}

static BOOLEAN is_dot_name(const CHAR16 *name) {
    return name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0));
}

// ============================================================================
// 4. FILE SYSTEM LAYER
//    Every file command goes through these, so none of them repeats the
//    open-volume / open-file / query-info boilerplate.
// ============================================================================

#define MAX_TREE_DEPTH  12            // deepest folder nesting copy/delete will follow
#define COPY_BUF_SIZE   (64 * 1024)   // chunk size used when copying file data

typedef struct {
    BOOLEAN isDir;
    UINT64  size;
    CHAR16  name[256];   // the name exactly as stored on disk (correct case)
} ITEM_INFO;

typedef struct {
    UINT64  files;
    UINT64  dirs;
    BOOLEAN tooDeep;
} XFER_STATS;

static BOOLEAN open_volume(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_PROTOCOL **root) {
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem = 0;

    if (SystemTable->BootServices->HandleProtocol(DeviceHandle, &fsGuid, (void**)&fileSystem) != EFI_SUCCESS) return FALSE;
    return fileSystem->OpenVolume(fileSystem, root) == EFI_SUCCESS;
}

// Looks up `path` (absolute, from normalize_path) on an already open volume and
// reports whether it is a folder, its size and its real on-disk name.
static BOOLEAN fs_stat(EFI_FILE_PROTOCOL *root, const CHAR16 *path, ITEM_INFO *info) {
    EFI_FILE_PROTOCOL *handle = 0;
    UINT64 infoBuf[128];                    // 1 KB, 8-byte aligned
    UINTN infoSize = sizeof(infoBuf);
    EFI_GUID infoGuid = EFI_FILE_INFO_GUID;
    BOOLEAN ok = FALSE;

    if (root->Open(root, &handle, (CHAR16*)path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) return FALSE;

    if (handle->GetInfo(handle, &infoGuid, &infoSize, infoBuf) == EFI_SUCCESS) {
        EFI_FILE_INFO *fileInfo = (EFI_FILE_INFO*)infoBuf;
        int n = 0;
        info->isDir = (fileInfo->Attribute & EFI_FILE_DIRECTORY) ? TRUE : FALSE;
        info->size  = fileInfo->FileSize;
        while (fileInfo->FileName[n] && n < 255) {
            info->name[n] = fileInfo->FileName[n];
            n++;
        }
        info->name[n] = 0;
        ok = TRUE;
    }
    handle->Close(handle);
    return ok;
}

// Same as fs_stat, but opens and closes the volume itself.
static BOOLEAN query_item(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, const CHAR16 *path, ITEM_INFO *info) {
    EFI_FILE_PROTOCOL *rootDrive = 0;
    BOOLEAN ok;

    if (!open_volume(DeviceHandle, SystemTable, &rootDrive)) return FALSE;
    ok = fs_stat(rootDrive, path, info);
    rootDrive->Close(rootDrive);
    return ok;
}

static EFI_STATUS fs_make_dir(EFI_FILE_PROTOCOL *root, const CHAR16 *path) {
    EFI_FILE_PROTOCOL *handle = 0;
    EFI_STATUS status = root->Open(root, &handle, (CHAR16*)path,
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                   EFI_FILE_DIRECTORY);
    if (status == EFI_SUCCESS) handle->Close(handle);
    return status;
}

// Deletes one file or one EMPTY folder. Returns EFI_SUCCESS only if it is gone.
static EFI_STATUS fs_delete(EFI_FILE_PROTOCOL *root, const CHAR16 *path) {
    EFI_FILE_PROTOCOL *handle = 0;
    EFI_STATUS status = root->Open(root, &handle, (CHAR16*)path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (status != EFI_SUCCESS) return status;
    return handle->Delete(handle);   // also closes the handle; a warning code means "not deleted"
}

// Finds the first entry inside a folder (skipping "." and ".."). Used by the
// delete loop, which re-reads the folder after each removal.
static BOOLEAN fs_first_child(EFI_FILE_PROTOCOL *root, const CHAR16 *dirPath, CHAR16 *name, int nameMax, BOOLEAN *isDir) {
    EFI_FILE_PROTOCOL *dir = 0;
    UINT64 entryBuf[128];
    BOOLEAN found = FALSE;

    if (root->Open(root, &dir, (CHAR16*)dirPath, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) return FALSE;

    for (;;) {
        UINTN size = sizeof(entryBuf);
        if (dir->Read(dir, &size, entryBuf) != EFI_SUCCESS || size == 0) break;

        EFI_FILE_INFO *fi = (EFI_FILE_INFO*)entryBuf;
        if (is_dot_name(fi->FileName)) continue;

        int n = 0;
        while (fi->FileName[n] && n < nameMax - 1) {
            name[n] = fi->FileName[n];
            n++;
        }
        name[n] = 0;
        *isDir = (fi->Attribute & EFI_FILE_DIRECTORY) ? TRUE : FALSE;
        found = TRUE;
        break;
    }
    dir->Close(dir);
    return found;
}

// How many files and folders are inside `path` (not counting `path` itself).
static UINT64 count_items(EFI_FILE_PROTOCOL *root, const CHAR16 *path, int depth) {
    EFI_FILE_PROTOCOL *dir = 0;
    UINT64 entryBuf[128];
    UINT64 total = 0;

    if (depth > MAX_TREE_DEPTH) return 0;
    if (root->Open(root, &dir, (CHAR16*)path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) return 0;

    for (;;) {
        UINTN size = sizeof(entryBuf);
        if (dir->Read(dir, &size, entryBuf) != EFI_SUCCESS || size == 0) break;

        EFI_FILE_INFO *fi = (EFI_FILE_INFO*)entryBuf;
        if (is_dot_name(fi->FileName)) continue;

        total++;
        if (fi->Attribute & EFI_FILE_DIRECTORY) {
            CHAR16 child[256];
            if (path_join(path, fi->FileName, child, 256)) {
                total += count_items(root, child, depth + 1);
            }
        }
    }
    dir->Close(dir);
    return total;
}

// Copies the data of one file. An existing destination file is replaced.
static BOOLEAN copy_file(EFI_FILE_PROTOCOL *root, const CHAR16 *src, const CHAR16 *dst, UINT8 *buf, UINTN bufSize) {
    EFI_FILE_PROTOCOL *in = 0;
    EFI_FILE_PROTOCOL *out = 0;
    ITEM_INFO existing;
    BOOLEAN ok = TRUE;

    if (root->Open(root, &in, (CHAR16*)src, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) return FALSE;

    // Opening with CREATE does not truncate, so remove an old file first.
    if (fs_stat(root, dst, &existing)) {
        if (existing.isDir || fs_delete(root, dst) != EFI_SUCCESS) {
            in->Close(in);
            return FALSE;
        }
    }

    if (root->Open(root, &out, (CHAR16*)dst, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0) != EFI_SUCCESS) {
        in->Close(in);
        return FALSE;
    }

    for (;;) {
        UINTN got = bufSize;
        if (in->Read(in, &got, buf) != EFI_SUCCESS) { ok = FALSE; break; }
        if (got == 0) break;

        UINTN wrote = got;
        if (out->Write(out, &wrote, buf) != EFI_SUCCESS || wrote != got) { ok = FALSE; break; }
    }

    out->Close(out);   // closing flushes the data to disk
    in->Close(in);
    return ok;
}

// Copies a file or a whole folder tree. Existing folders are merged into.
static BOOLEAN copy_tree(EFI_FILE_PROTOCOL *root, const CHAR16 *src, const CHAR16 *dst, BOOLEAN srcIsDir,
                         XFER_STATS *stats, int depth, UINT8 *buf, UINTN bufSize) {
    if (!srcIsDir) {
        if (!copy_file(root, src, dst, buf, bufSize)) return FALSE;
        stats->files++;
        return TRUE;
    }

    if (depth > MAX_TREE_DEPTH) {
        stats->tooDeep = TRUE;
        return FALSE;
    }

    ITEM_INFO existing;
    if (fs_stat(root, dst, &existing)) {
        if (!existing.isDir) return FALSE;                  // a file is in the way
    } else if (fs_make_dir(root, dst) != EFI_SUCCESS) {
        return FALSE;
    }
    stats->dirs++;

    EFI_FILE_PROTOCOL *dir = 0;
    UINT64 entryBuf[128];
    BOOLEAN ok = TRUE;

    if (root->Open(root, &dir, (CHAR16*)src, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) return FALSE;

    for (;;) {
        UINTN size = sizeof(entryBuf);
        if (dir->Read(dir, &size, entryBuf) != EFI_SUCCESS || size == 0) break;

        EFI_FILE_INFO *fi = (EFI_FILE_INFO*)entryBuf;
        if (is_dot_name(fi->FileName)) continue;

        CHAR16 childSrc[256];
        CHAR16 childDst[256];
        if (!path_join(src, fi->FileName, childSrc, 256) || !path_join(dst, fi->FileName, childDst, 256)) {
            ok = FALSE;
            break;
        }
        if (!copy_tree(root, childSrc, childDst, (fi->Attribute & EFI_FILE_DIRECTORY) ? TRUE : FALSE,
                       stats, depth + 1, buf, bufSize)) {
            ok = FALSE;
            break;
        }
    }
    dir->Close(dir);
    return ok;
}

// Deletes a file, or a folder together with everything inside it.
static BOOLEAN delete_tree(EFI_FILE_PROTOCOL *root, const CHAR16 *path, BOOLEAN isDir, XFER_STATS *stats, int depth) {
    if (isDir) {
        if (depth > MAX_TREE_DEPTH) {
            stats->tooDeep = TRUE;
            return FALSE;
        }

        CHAR16 name[256];
        CHAR16 child[256];
        BOOLEAN childIsDir = FALSE;

        while (fs_first_child(root, path, name, 256, &childIsDir)) {
            if (!path_join(path, name, child, 256)) return FALSE;
            if (!delete_tree(root, child, childIsDir, stats, depth + 1)) return FALSE;
        }
    }

    if (fs_delete(root, path) != EFI_SUCCESS) return FALSE;
    if (isDir) stats->dirs++; else stats->files++;
    return TRUE;
}

// ============================================================================
// 5. PROTECTED SYSTEM FILES AND CONFIRMATION PROMPTS
// ============================================================================

// Anything at or below one of these paths is a protected system item.
static const CHAR16 *const PROTECTED_PATHS[] = {
    L"\\EFI",
    L"\\kernel.bin",
    L"\\NvVars"
};
#define PROTECTED_COUNT (sizeof(PROTECTED_PATHS) / sizeof(PROTECTED_PATHS[0]))

static BOOLEAN is_protected(const CHAR16 *path) {
    for (UINTN i = 0; i < PROTECTED_COUNT; i++) {
        if (path_within(path, PROTECTED_PATHS[i])) return TRUE;
    }
    return FALSE;
}

// Waits for a single Y or N key press (any other key is ignored, so a stray
// Enter can never answer the question).
static BOOLEAN ask_yes_no(SHELL *sh, const CHAR16 *question, unsigned long long color) {
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn = sh->st->ConIn;
    EFI_INPUT_KEY key;
    UINTN index;

    scprint(sh->st, L"\r\n", *sh->color);
    scprint(sh->st, question, color);
    scprint(sh->st, L" ", color);

    // Throw away anything typed ahead so it cannot answer for the user.
    while (ConIn->ReadKeyStroke(ConIn, &key) == EFI_SUCCESS) { }

    for (;;) {
        while (ConIn->ReadKeyStroke(ConIn, &key) != EFI_SUCCESS) {
            sh->st->BootServices->WaitForEvent(1, &ConIn->WaitForKey, &index);
        }

        CHAR16 answer[2];
        answer[1] = 0;
        if (key.UnicodeChar == L'y' || key.UnicodeChar == L'Y') {
            answer[0] = key.UnicodeChar;
            scprint(sh->st, answer, color);
            scprint(sh->st, L"\r\n", *sh->color);
            return TRUE;
        }
        if (key.UnicodeChar == L'n' || key.UnicodeChar == L'N') {
            answer[0] = key.UnicodeChar;
            scprint(sh->st, answer, color);
            scprint(sh->st, L"\r\n", *sh->color);
            return FALSE;
        }
    }
}

// "This is a protected system file! Modifying it may cause your system to break.
//  Are you sure you want to <phrase> this file? [Y] or [N]:"
static BOOLEAN protected_prompt(SHELL *sh, BOOLEAN isFolder, const CHAR16 *phrase) {
    CHAR16 question[400];
    const CHAR16 *kind = isFolder ? L"folder" : L"file";

    question[0] = 0;
    str_cat(question, 400, L"This is a protected system ");
    str_cat(question, 400, kind);
    str_cat(question, 400, L"! Modifying it may cause your system to break. Are you sure you want to ");
    str_cat(question, 400, phrase);
    str_cat(question, 400, L" this ");
    str_cat(question, 400, kind);
    str_cat(question, 400, L"? [Y] or [N]:");
    return ask_yes_no(sh, question, 0x0C);
}

static void put_cancelled(SHELL *sh) {
    put(sh, L"\r\nOperation cancelled. Nothing was changed.\r\n");
}

// ============================================================================
// 6. COMMANDS
//    Each command is  void cmd_xxx(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest)
//    argv[0] is the command name, argv[1..] are the arguments (quotes removed),
//    `rest` is the raw text after the command name (used by echo, cd, type, color).
// ============================================================================

// If the user typed one (possibly quoted) argument use it, otherwise use the raw
// text so folder and file names containing spaces keep working without quotes.
static const CHAR16 *pick_arg(int argc, CHAR16 **argv, const CHAR16 *rest) {
    return (argc == 2) ? argv[1] : rest;
}

void kernel_list_directory(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, unsigned long long currentTextColor, const CHAR16 *currentDir) {
    scprint(SystemTable, L"\r\nDirectory listing of ", currentTextColor);
    scprint(SystemTable, currentDir, currentTextColor);
    scprint(SystemTable, L":\r\n\r\n", currentTextColor);

    EFI_FILE_PROTOCOL *rootDrive = 0;
    if (!open_volume(DeviceHandle, SystemTable, &rootDrive)) {
        scprint(SystemTable, L"  [X] Error: Could not open root volume.\r\n", 0x0C);
        return;
    }

    EFI_FILE_PROTOCOL *targetDir = 0;
    EFI_FILE_PROTOCOL *dirToRead = rootDrive;

    if (currentDir[0] != L'\\' || currentDir[1] != 0) {
        if (rootDrive->Open(rootDrive, &targetDir, (CHAR16*)currentDir, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
            scprint(SystemTable, L"  [X] Error: Could not open current directory.\r\n", 0x0C);
            rootDrive->Close(rootDrive);
            return;
        }
        dirToRead = targetDir;
    }

    UINT64 infoBuffer[128];                 // 1 KB, 8-byte aligned
    UINTN readSize = sizeof(infoBuffer);
    UINTN fileCount = 0;
    unsigned short sizeStr[32];

    while (dirToRead->Read(dirToRead, &readSize, infoBuffer) == EFI_SUCCESS && readSize > 0) {
        EFI_FILE_INFO *fileInfo = (EFI_FILE_INFO*)infoBuffer;
        unsigned short *srcName = fileInfo->FileName;

        if (srcName[0] == 0) {
            readSize = sizeof(infoBuffer);
            continue;
        }

        unsigned short safeName[256];
        int nIdx = 0;
        while (srcName[nIdx] && nIdx < 255) {
            safeName[nIdx] = srcName[nIdx];
            nIdx++;
        }
        safeName[nIdx] = 0;

        scprint(SystemTable, safeName, currentTextColor);

        for (int i = nIdx; i < 20; i++) scprint(SystemTable, L" ", currentTextColor);

        if (fileInfo->Attribute & EFI_FILE_DIRECTORY) {
            scprint(SystemTable, L"Folder          ", currentTextColor);
        } else {
            scprint(SystemTable, L"File            ", currentTextColor);
        }

        scitoa(fileInfo->FileSize, sizeStr);
        scprint(SystemTable, sizeStr, currentTextColor);
        scprint(SystemTable, L"    bytes\r\n", currentTextColor);

        fileCount++;
        readSize = sizeof(infoBuffer);
    }

    if (fileCount == 0) {
        scprint(SystemTable, L"  (No files detected in directory)\r\n", currentTextColor);
    }

    if (targetDir) {
        targetDir->Close(targetDir);
    }
    rootDrive->Close(rootDrive);
}

// Prints a text file. `FileName` is resolved against the current directory.
void read_file_directly(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, const CHAR16 *currentDir, const unsigned short *FileName, unsigned long long currentTextColor) {
    unsigned short cleanFileName[256];
    int fnLen = 0;
    while (FileName[fnLen] && fnLen < 255) {
        cleanFileName[fnLen] = FileName[fnLen];
        fnLen++;
    }
    cleanFileName[fnLen] = 0;
    strip_whitespace(cleanFileName);

    CHAR16 fullPath[256];
    ITEM_INFO info;

    if (!normalize_path(currentDir, cleanFileName, fullPath, 256) ||
        !query_item(DeviceHandle, SystemTable, fullPath, &info)) {
        scprint(SystemTable, L"\r\n[X] File Not Found: Check spelling or extension.\r\n", 0x0C);
        return;
    }
    if (info.isDir) {
        scprint(SystemTable, L"\r\n[X] That is a folder. Use the cd command to open it.\r\n", 0x0C);
        return;
    }

    EFI_FILE_PROTOCOL *rootDrive = 0;
    EFI_FILE_PROTOCOL *targetFile = 0;

    if (!open_volume(DeviceHandle, SystemTable, &rootDrive)) {
        scprint(SystemTable, L"\r\n[X] Hardware Access Fault.\r\n", 0x0C);
        return;
    }

    if (rootDrive->Open(rootDrive, &targetFile, fullPath, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        scprint(SystemTable, L"\r\n[X] File Not Found: Check spelling or extension.\r\n", 0x0C);
        rootDrive->Close(rootDrive);
        return;
    }

    UINTN readSize = sizeof(fileContent) - 1;
    memset(fileContent, 0, sizeof(fileContent));

    if (targetFile->Read(targetFile, &readSize, fileContent) == EFI_SUCCESS) {
        scprint(SystemTable, L"\r\n--- File Content ---\r\n", 0x0B);

        unsigned short chunk[65];
        int n = 0;
        for (UINTN i = 0; i < readSize; i++) {
            unsigned char c = (unsigned char)fileContent[i];   // unsigned: bytes above 127 must not sign-extend

            if (c == '\n') {
                chunk[n] = 0; scprint(SystemTable, chunk, currentTextColor); n = 0;
                scprint(SystemTable, L"\r\n", currentTextColor);
            } else if (c != '\r') {
                if (c < 32 && c != '\t') c = '.';              // keep control bytes from confusing the console
                chunk[n++] = c;
                if (n == 64) {
                    chunk[n] = 0; scprint(SystemTable, chunk, currentTextColor); n = 0;
                }
            }
        }
        if (n > 0) {
            chunk[n] = 0; scprint(SystemTable, chunk, currentTextColor);
        }
        scprint(SystemTable, L"\r\n--------------------\r\n", 0x0B);

        if (info.size > readSize) {
            scprint(SystemTable, L"[!] File is larger than 4095 bytes; only the beginning is shown.\r\n", 0x0E);
        }
    }

    targetFile->Close(targetFile);
    rootDrive->Close(rootDrive);
}

static void cmd_list(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    kernel_list_directory(sh->device, sh->st, *sh->color, sh->cwd);
}

static void cmd_echo(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv;
    if (*rest == 0) {
        put(sh, L"\r\nNothing to echo!\r\n");
    } else {
        put(sh, L"\r\n");
        put(sh, rest);
        put(sh, L"\r\n");
    }
}

static void cmd_cd(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    const CHAR16 *targetPath = pick_arg(argc, argv, rest);

    if (*targetPath == 0) {
        put(sh, L"\r\n");
        put(sh, sh->cwd);
        put(sh, L"\r\n");
        return;
    }

    CHAR16 newPath[256];
    if (!normalize_path(sh->cwd, targetPath, newPath, 256)) {
        put_err(sh, L"\r\n[X] That path is too long.\r\n");
        return;
    }

    // Anything other than the root must exist and must be a folder.
    if (!is_root_path(newPath)) {
        ITEM_INFO info;
        if (!query_item(sh->device, sh->st, newPath, &info)) {
            put_err(sh, L"\r\n[X] Folder not found: ");
            put(sh, targetPath);
            put(sh, L"\r\n");
            return;
        }
        if (!info.isDir) {
            put_err(sh, L"\r\n[X] Not a folder: ");
            put(sh, targetPath);
            put(sh, L"\r\n");
            return;
        }

        // FAT is case-insensitive: swap the last component for the name as
        // stored on disk so the prompt always shows the real capitalisation.
        int lastSlash = 0;
        for (int idx = 0; newPath[idx]; idx++) {
            if (newPath[idx] == L'\\') lastSlash = idx;
        }
        int nameLen = str_len(info.name);
        if (nameLen > 0 && lastSlash + 1 + nameLen < 256) {
            for (int k = 0; k < nameLen; k++) newPath[lastSlash + 1 + k] = info.name[k];
            newPath[lastSlash + 1 + nameLen] = 0;
        }
    }

    int cIdx = 0;
    while (newPath[cIdx]) {
        sh->cwd[cIdx] = newPath[cIdx];
        cIdx++;
    }
    sh->cwd[cIdx] = 0;
    put(sh, L"\r\n");
}

static void cmd_type(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    read_file_directly(sh->device, sh->st, sh->cwd, pick_arg(argc, argv, rest), *sh->color);
}

static void cmd_clear(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    sh->st->ConOut->ClearScreen(sh->st->ConOut);
}

static void cmd_color(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv;
    if (scstrcmp(rest, L"green") == 0) {
        *sh->color = 0x0A;
        put(sh, L"\r\nTheme updated to Neon Green.\r\n");
    } else if (scstrcmp(rest, L"cyan") == 0) {
        *sh->color = 0x0B;
        put(sh, L"\r\nTheme updated to Bright Cyan.\r\n");
    } else if (scstrcmp(rest, L"red") == 0) {
        *sh->color = 0x0C;
        put(sh, L"\r\nTheme updated to Warning Red.\r\n");
    } else if (scstrcmp(rest, L"white") == 0) {
        *sh->color = 0x0F;
        put(sh, L"\r\nTheme updated to Standard White.\r\n");
    } else {
        put(sh, L"\r\n[X] Unknown color theme. Try: green, cyan, red, white\r\n");
    }
}

static void cmd_reboot(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    scprint(sh->st, L"\r\n[!] Restarting system hardware canvas channels...", 0x0E);
    // Ask the firmware to reset. Poking port 0x64 only works on machines with
    // a legacy keyboard controller, which many UEFI systems don't have.
    sh->st->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
}

static void cmd_shutdown(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    scprint(sh->st, L"\r\nShutting down CustomOS...\r\n", 0x0C);
    sh->st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
}

static void cmd_about(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    put(sh, L"\r\nCustomOS 0.1.6 - Compiled on 11/10/2026\r\n");
}

static void cmd_systeminfo(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    scprint(sh->st, L"\r\n=== CustomOS System Information ===\r\n", 0x0B);
    put(sh, L" OS Version : CustomOS 0.1.6\r\n");
    put(sh, L" Architecture: x86_64 (64-bit Freestanding UEFI)\r\n");

    put(sh, L" Firmware Vendor: ");
    put(sh, sh->st->FirmwareVendor);
    put(sh, L"\r\n");

    CHAR16 cpuBuffer[64];
    get_cpu_brand_string(cpuBuffer, 64);
    put(sh, L" CPU Model   : ");
    put(sh, cpuBuffer);
    put(sh, L"\r\n");

    put(sh, L" Platform    : Bare-Metal / UEFI Host\r\n");

    CHAR16 memString[64];
    UINT64 totalBytes = get_total_memory_bytes(sh->st);
    format_memory_size(totalBytes, memString, 64);
    put(sh, L" Memory      : ");
    put(sh, memString);
    put(sh, L" Detected\r\n");

    scprint(sh->st, L"=====================================\r\n", 0x0B);
}

// ---- File operations: del, mkdir, copy, move, rename -----------------------

static void put_not_found(SHELL *sh, const CHAR16 *what) {
    put_err(sh, L"\r\n[X] Not found: ");
    put(sh, what);
    put(sh, L"\r\n");
}

static void put_inside_folder(SHELL *sh) {
    put_err(sh, L"\r\n[X] You are inside that folder (or one of its subfolders). Use cd to leave it first.\r\n");
}

static void cmd_del(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)rest;
    CHAR16 path[256];
    ITEM_INFO info;
    EFI_FILE_PROTOCOL *root = 0;

    if (!normalize_path(sh->cwd, argv[1], path, 256)) {
        put_err(sh, L"\r\n[X] That path is too long.\r\n");
        return;
    }
    if (!open_volume(sh->device, sh->st, &root)) {
        put_err(sh, L"\r\n[X] Could not open the file system.\r\n");
        return;
    }

    if (!fs_stat(root, path, &info)) {
        put_not_found(sh, argv[1]);
        goto done;
    }
    if (is_root_path(path)) {
        put_err(sh, L"\r\n[X] The root folder cannot be deleted.\r\n");
        goto done;
    }
    if (path_within(sh->cwd, path)) {
        put_inside_folder(sh);
        goto done;
    }

    if (is_protected(path)) {
        if (!protected_prompt(sh, info.isDir, L"delete")) {
            put_cancelled(sh);
            goto done;
        }
    } else if (info.isDir) {
        UINT64 items = count_items(root, path, 0);
        if (items > 0) {
            CHAR16 question[200];
            CHAR16 num[32];
            scitoa(items, num);
            question[0] = 0;
            str_cat(question, 200, L"This folder contains ");
            str_cat(question, 200, num);
            str_cat(question, 200, items == 1 ? L" item. Delete all of it? [Y] or [N]:" : L" items. Delete all of it? [Y] or [N]:");
            if (!ask_yes_no(sh, question, *sh->color)) {
                put_cancelled(sh);
                goto done;
            }
        }
    }

    {
        XFER_STATS stats = { 0, 0, FALSE };
        if (!delete_tree(root, path, info.isDir, &stats, 0)) {
            put_err(sh, stats.tooDeep ? L"\r\n[X] That folder structure is too deeply nested to delete.\r\n"
                                      : L"\r\n[X] Could not delete everything. Some items may be in use or read-only.\r\n");
            if (stats.files + stats.dirs > 0) {
                put(sh, L"    Removed before the error: ");
                put_count(sh, stats.files, L"file", L"files");
                put(sh, L", ");
                put_count(sh, stats.dirs, L"folder", L"folders");
                put(sh, L"\r\n");
            }
            goto done;
        }

        put(sh, L"\r\nDeleted: ");
        put(sh, path);
        if (info.isDir) {
            put(sh, L" (");
            put_count(sh, stats.files, L"file", L"files");
            put(sh, L", ");
            put_count(sh, stats.dirs, L"folder", L"folders");
            put(sh, L" removed)");
        }
        put(sh, L"\r\n");
    }

done:
    root->Close(root);
}

static void cmd_mkdir(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    const CHAR16 *arg = pick_arg(argc, argv, rest);
    CHAR16 path[256];
    CHAR16 parent[256];
    ITEM_INFO info;
    EFI_FILE_PROTOCOL *root = 0;

    if (!normalize_path(sh->cwd, arg, path, 256)) {
        put_err(sh, L"\r\n[X] That path is too long.\r\n");
        return;
    }
    if (!open_volume(sh->device, sh->st, &root)) {
        put_err(sh, L"\r\n[X] Could not open the file system.\r\n");
        return;
    }

    if (fs_stat(root, path, &info)) {
        put_err(sh, L"\r\n[X] Already exists: ");
        put(sh, path);
        put(sh, L"\r\n");
        goto done;
    }

    path_parent(path, parent);
    if (!fs_stat(root, parent, &info) || !info.isDir) {
        put_err(sh, L"\r\n[X] The parent folder does not exist: ");
        put(sh, parent);
        put(sh, L"\r\n");
        goto done;
    }

    if (is_protected(path)) {
        if (!protected_prompt(sh, TRUE, L"add to")) {
            put_cancelled(sh);
            goto done;
        }
    }

    if (fs_make_dir(root, path) != EFI_SUCCESS) {
        put_err(sh, L"\r\n[X] Could not create the folder. The name may contain characters that are not allowed.\r\n");
        goto done;
    }
    put(sh, L"\r\nCreated folder: ");
    put(sh, path);
    put(sh, L"\r\n");

done:
    root->Close(root);
}

#define XFER_COPY    0
#define XFER_MOVE    1
#define XFER_RENAME  2

// Shared engine for copy, move and rename: copies `src` to `final`, and for
// move/rename removes the original afterwards (only if the copy fully worked).
static void transfer(SHELL *sh, EFI_FILE_PROTOCOL *root, const CHAR16 *src, const ITEM_INFO *srcInfo,
                     const CHAR16 *final, int mode) {
    ITEM_INFO fin;
    BOOLEAN finExists;
    const CHAR16 *verb  = (mode == XFER_COPY) ? L"copy"   : (mode == XFER_MOVE) ? L"move"   : L"rename";
    const CHAR16 *past  = (mode == XFER_COPY) ? L"copied" : (mode == XFER_MOVE) ? L"moved"  : L"renamed";

    if (is_root_path(src)) {
        put_err(sh, L"\r\n[X] The root folder cannot be ");
        put_err(sh, past);
        put_err(sh, L".\r\n");
        return;
    }
    if (path_eq(src, final)) {
        put_err(sh, L"\r\n[X] The source and the destination are the same.\r\n");
        return;
    }
    if (srcInfo->isDir && path_within(final, src)) {
        put_err(sh, L"\r\n[X] A folder cannot be placed inside itself.\r\n");
        return;
    }
    if (mode != XFER_COPY && path_within(sh->cwd, src)) {
        put_inside_folder(sh);
        return;
    }

    finExists = fs_stat(root, final, &fin);

    if (finExists && srcInfo->isDir != fin.isDir) {
        put_err(sh, srcInfo->isDir ? L"\r\n[X] A file with that name already exists in the destination.\r\n"
                                   : L"\r\n[X] A folder with that name already exists in the destination.\r\n");
        return;
    }
    if (finExists && mode == XFER_RENAME) {
        put_err(sh, L"\r\n[X] Something with that name already exists.\r\n");
        return;
    }

    // --- Confirmations -------------------------------------------------------
    BOOLEAN destCovered = FALSE;   // TRUE once the user already agreed to touch the destination

    if (is_protected(src)) {
        if (!protected_prompt(sh, srcInfo->isDir, verb)) { put_cancelled(sh); return; }
    } else if (is_protected(final)) {
        const CHAR16 *phrase;
        BOOLEAN kindIsFolder;
        if (finExists) {
            kindIsFolder = fin.isDir;
            phrase = fin.isDir ? L"merge into" : L"overwrite";
        } else {
            kindIsFolder = TRUE;
            phrase = (mode == XFER_COPY) ? L"copy into" : L"move into";
        }
        if (!protected_prompt(sh, kindIsFolder, phrase)) { put_cancelled(sh); return; }
        destCovered = TRUE;
    }

    if (finExists && !destCovered) {
        if (fin.isDir) {
            if (!ask_yes_no(sh, L"A folder with that name already exists in the destination. Merge into it and overwrite files with the same name? [Y] or [N]:", *sh->color)) {
                put_cancelled(sh);
                return;
            }
        } else {
            if (!ask_yes_no(sh, L"A file with that name already exists in the destination. Overwrite it? [Y] or [N]:", *sh->color)) {
                put_cancelled(sh);
                return;
            }
        }
    }

    // --- Do the work ---------------------------------------------------------
    UINT8 *buf = 0;
    if (sh->st->BootServices->AllocatePool(EfiLoaderData, COPY_BUF_SIZE, (VOID**)&buf) != EFI_SUCCESS || !buf) {
        put_err(sh, L"\r\n[X] Not enough memory to copy.\r\n");
        return;
    }

    XFER_STATS stats = { 0, 0, FALSE };
    BOOLEAN ok = copy_tree(root, src, final, srcInfo->isDir, &stats, 0, buf, COPY_BUF_SIZE);
    sh->st->BootServices->FreePool(buf);

    if (!ok) {
        put_err(sh, stats.tooDeep ? L"\r\n[X] That folder structure is too deeply nested to copy.\r\n"
                                  : L"\r\n[X] The copy failed (disk full, read-only, or an invalid name).\r\n");
        if (mode != XFER_COPY) put(sh, L"    The original was left untouched.\r\n");
        return;
    }

    if (mode != XFER_COPY) {
        XFER_STATS removed = { 0, 0, FALSE };
        if (!delete_tree(root, src, srcInfo->isDir, &removed, 0)) {
            scprint(sh->st, L"\r\n[!] Copied to ", 0x0E);
            scprint(sh->st, final, 0x0E);
            scprint(sh->st, L", but the original could not be fully removed.\r\n", 0x0E);
            return;
        }
    }

    if (mode == XFER_RENAME) {
        put(sh, L"\r\nRenamed: ");
        put(sh, src);
        put(sh, L" -> ");
        put(sh, path_base(final));
        put(sh, L"\r\n");
        return;
    }

    put(sh, mode == XFER_COPY ? L"\r\nCopied " : L"\r\nMoved ");
    put_count(sh, stats.files, L"file", L"files");
    if (srcInfo->isDir) {
        put(sh, L" and ");
        put_count(sh, stats.dirs, L"folder", L"folders");
    }
    put(sh, L" to ");
    put(sh, final);
    put(sh, L"\r\n");
}

// copy / move: <source> <destination folder>
static void cmd_copy_move(SHELL *sh, CHAR16 **argv, int mode) {
    CHAR16 srcPath[256];
    CHAR16 dstPath[256];
    CHAR16 finalPath[256];
    CHAR16 parent[256];
    ITEM_INFO srcInfo;
    ITEM_INFO dstInfo;
    EFI_FILE_PROTOCOL *root = 0;

    if (!normalize_path(sh->cwd, argv[1], srcPath, 256) || !normalize_path(sh->cwd, argv[2], dstPath, 256)) {
        put_err(sh, L"\r\n[X] That path is too long.\r\n");
        return;
    }
    if (!open_volume(sh->device, sh->st, &root)) {
        put_err(sh, L"\r\n[X] Could not open the file system.\r\n");
        return;
    }

    if (!fs_stat(root, srcPath, &srcInfo)) {
        put_not_found(sh, argv[1]);
        goto done;
    }
    if (is_root_path(srcPath)) {
        put_err(sh, mode == XFER_COPY ? L"\r\n[X] The root folder cannot be copied.\r\n"
                                      : L"\r\n[X] The root folder cannot be moved.\r\n");
        goto done;
    }

    if (fs_stat(root, dstPath, &dstInfo)) {
        if (dstInfo.isDir) {
            // Into an existing folder, keeping the original name.
            if (!path_join(dstPath, path_base(srcPath), finalPath, 256)) {
                put_err(sh, L"\r\n[X] That path is too long.\r\n");
                goto done;
            }
        } else {
            if (srcInfo.isDir) {
                put_err(sh, L"\r\n[X] A folder cannot be copied onto a file.\r\n");
                goto done;
            }
            // An existing file: it becomes the target to overwrite.
            str_set(finalPath, 256, dstPath);
        }
    } else {
        // Not there yet: the last part is the new name, its folder must exist.
        path_parent(dstPath, parent);
        if (!fs_stat(root, parent, &dstInfo) || !dstInfo.isDir) {
            put_err(sh, L"\r\n[X] Destination folder not found: ");
            put(sh, parent);
            put(sh, L"\r\n");
            goto done;
        }
        str_set(finalPath, 256, dstPath);
    }

    transfer(sh, root, srcPath, &srcInfo, finalPath, mode);

done:
    root->Close(root);
}

static void cmd_copy(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)rest;
    cmd_copy_move(sh, argv, XFER_COPY);
}

static void cmd_move(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)rest;
    cmd_copy_move(sh, argv, XFER_MOVE);
}

// rename: <file or folder> <new name>   (the new name stays in the same folder)
static void cmd_rename(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)rest;
    CHAR16 srcPath[256];
    CHAR16 parent[256];
    CHAR16 finalPath[256];
    ITEM_INFO srcInfo;
    EFI_FILE_PROTOCOL *root = 0;
    const CHAR16 *newName = argv[2];

    for (int i = 0; newName[i]; i++) {
        if (newName[i] == L'\\' || newName[i] == L'/') {
            put_err(sh, L"\r\n[X] The new name cannot contain a folder path. Use move to put it somewhere else.\r\n");
            return;
        }
    }
    if (newName[0] == 0 || is_dot_name(newName)) {
        put_err(sh, L"\r\n[X] That is not a valid name.\r\n");
        return;
    }
    if (!normalize_path(sh->cwd, argv[1], srcPath, 256)) {
        put_err(sh, L"\r\n[X] That path is too long.\r\n");
        return;
    }
    if (!open_volume(sh->device, sh->st, &root)) {
        put_err(sh, L"\r\n[X] Could not open the file system.\r\n");
        return;
    }

    if (!fs_stat(root, srcPath, &srcInfo)) {
        put_not_found(sh, argv[1]);
        goto done;
    }
    if (is_root_path(srcPath)) {
        put_err(sh, L"\r\n[X] The root folder cannot be renamed.\r\n");
        goto done;
    }

    path_parent(srcPath, parent);
    if (!path_join(parent, newName, finalPath, 256)) {
        put_err(sh, L"\r\n[X] That name is too long.\r\n");
        goto done;
    }

    transfer(sh, root, srcPath, &srcInfo, finalPath, XFER_RENAME);

done:
    root->Close(root);
}

// ---- Anything that is not a command ----------------------------------------

static void cmd_unknown(SHELL *sh, const CHAR16 *line) {
    BOOLEAN itemExists = FALSE;
    BOOLEAN isDirectory = FALSE;
    CHAR16 fullPath[256];
    ITEM_INFO info;

    if (normalize_path(sh->cwd, line, fullPath, 256) &&
        query_item(sh->device, sh->st, fullPath, &info)) {
        itemExists = TRUE;
        isDirectory = info.isDir;
    }

    if (itemExists) {
        if (isDirectory) {
            put_err(sh, L"\r\nUse the cd command to navigate to folders.\r\n");
        } else {
            put_err(sh, L"\r\nNo program to open this type of file.\r\n");
        }
    } else {
        put(sh, L"\r\n'");
        put_err(sh, line);
        put(sh, L"' is not recognized as a command, program, or file. Type 'help' for the list of commands.\r\n");
    }
}

// ============================================================================
// 7. COMMAND TABLE
//    To add a command: write a cmd_xxx function above and add one line here.
//    `label` is what "help" lists and what a wrong call prints as Usage.
//    minArgs/maxArgs count the arguments after the command name (-1 = no limit).
// ============================================================================

typedef void (*CMD_HANDLER)(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest);

typedef struct {
    const CHAR16 *name;
    const CHAR16 *label;
    const CHAR16 *details;
    CMD_HANDLER   handler;
    int           minArgs;
    int           maxArgs;
} CMD;

static void cmd_help(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest);

static const CMD commands[] = {
    { L"cd",         L"cd [folder, dir, or ..]",
      L"Changes the current folder. Use .. to go up, or a full path such as \\EFI\\BOOT.\r\nWith no folder it shows the current one.",
      cmd_cd, 0, -1 },
    { L"list",       L"list",
      L"Lists the files and folders in the current folder.",
      cmd_list, 0, 0 },
    { L"type",       L"type [filename]",
      L"Shows the contents of a text file (the first 4095 bytes).",
      cmd_type, 1, -1 },
    { L"copy",       L"copy [source] [destination]",
      L"Copies a file or folder into the destination folder, keeping its name.\r\nIf the destination does not exist yet, it becomes the new name.\r\nExample: copy \\kernel.bin \\EFI",
      cmd_copy, 2, 2 },
    { L"move",       L"move [source] [destination]",
      L"Moves a file or folder into the destination folder.\r\nExample: move \\EFI\\kernel.bin \\",
      cmd_move, 2, 2 },
    { L"del",        L"del [file or folder]",
      L"Deletes a file, or a folder and everything inside it.\r\nExamples: del testing.txt   or   del \\EFI\\BOOT\\bootx64.efi",
      cmd_del, 1, 1 },
    { L"mkdir",      L"mkdir [folder]",
      L"Creates a new folder. The folder that will contain it must already exist.",
      cmd_mkdir, 1, -1 },
    { L"rename",     L"rename [item] [new name]",
      L"Gives a file or folder a new name, keeping it in the same folder.\r\nExample: rename old.txt new.txt",
      cmd_rename, 2, 2 },
    { L"color",      L"color [theme]",
      L"Changes the text colour. Themes: green, cyan, red, white.",
      cmd_color, 1, -1 },
    { L"echo",       L"echo [text]",
      L"Prints the text you give it.",
      cmd_echo, 0, -1 },
    { L"clear",      L"clear",
      L"Clears the screen.",
      cmd_clear, 0, 0 },
    { L"reboot",     L"reboot",
      L"Restarts the computer.",
      cmd_reboot, 0, 0 },
    { L"shutdown",   L"shutdown",
      L"Turns the computer off.",
      cmd_shutdown, 0, 0 },
    { L"about",      L"about",
      L"Shows the CustomOS version and build date.",
      cmd_about, 0, 0 },
    { L"systeminfo", L"systeminfo",
      L"Shows the CPU, memory and firmware information.",
      cmd_systeminfo, 0, 0 },
    { L"help",       L"help [command]",
      L"Lists all commands. Use help followed by a command name for details.",
      cmd_help, 0, 1 }
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static const CMD *find_command(const CHAR16 *name) {
    for (UINTN i = 0; i < COMMAND_COUNT; i++) {
        if (scstrcmp(name, commands[i].name) == 0) return &commands[i];
    }
    return 0;
}

static void cmd_help(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;

    if (argc == 1) {
        put(sh, L"\r\nAvailable Commands: ");
        for (UINTN i = 0; i < COMMAND_COUNT; i++) {
            put(sh, commands[i].label);
            if (i + 1 < COMMAND_COUNT) put(sh, L", ");
        }
        put(sh, L"\r\n");
        return;
    }

    const CMD *cmd = find_command(argv[1]);
    if (!cmd) {
        put_err(sh, L"\r\n[X] No help available. Unknown command: ");
        put(sh, argv[1]);
        put(sh, L"\r\n");
        return;
    }
    put(sh, L"\r\nUsage: ");
    put(sh, cmd->label);
    put(sh, L"\r\n");
    put(sh, cmd->details);
    put(sh, L"\r\n");
}

// ============================================================================
// 8. ARGUMENT PARSER AND COMMAND DISPATCH
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

void execute_command(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, unsigned short *cmdBuffer, unsigned long long *currentTextColor, CHAR16 *currentDir) {
    strip_whitespace(cmdBuffer);
    if (cmdBuffer[0] == 0) return;

    SHELL sh;
    sh.device = DeviceHandle;
    sh.st     = SystemTable;
    sh.color  = currentTextColor;
    sh.cwd    = currentDir;

    CHAR16 argBuf[264];
    CHAR16 *argv[MAX_ARGS];
    int argc = tokenize(cmdBuffer, argBuf, argv, MAX_ARGS);

    const CMD *cmd = find_command(argv[0]);
    if (!cmd) {
        cmd_unknown(&sh, cmdBuffer);
        return;
    }

    // The raw text after the command name, for commands that want it as typed.
    const CHAR16 *rest = cmdBuffer;
    while (*rest == L' ') rest++;
    while (*rest && *rest != L' ') rest++;
    while (*rest == L' ') rest++;

    int nargs = argc - 1;
    if (nargs < cmd->minArgs || (cmd->maxArgs >= 0 && nargs > cmd->maxArgs)) {
        put(&sh, L"\r\nUsage: ");
        put(&sh, cmd->label);
        put(&sh, L"\r\n");
        if (cmd->maxArgs > 0 && nargs > cmd->maxArgs) {
            put(&sh, L"Names that contain spaces must be wrapped in quotes.\r\n");
        }
        return;
    }

    cmd->handler(&sh, argc, argv, rest);
}

void print_prompt(EFI_SYSTEM_TABLE *SystemTable, const CHAR16 *dir, unsigned long long color) {
    scprint(SystemTable, L"CustomOS:", color);
    scprint(SystemTable, dir, color);
    if (dir[0] != L'\\' || dir[1] != 0) {
        scprint(SystemTable, L"\\", color);
    }
    scprint(SystemTable, L"> ", color);
}

// ============================================================================
// 9. COMMAND HISTORY
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

// ============================================================================
// 10. KERNEL ENTRY AND MAIN LOOP
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
    scprint(SystemTable, L"CustomOS (Version 0.1.6) Command Line\r\n\r\n", activeColor);

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
