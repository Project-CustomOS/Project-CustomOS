// ============================================================================
// commands.c - every CustomOS command, plus the command table.
//
// kernel.c (the interpreter) never needs to change for a new command: write
// cmd_xxx() here and add one line to the table at the bottom of this file.
// ============================================================================

#include "shell.h"
#include "cpu_info.h"
#include "memory.h"
#include "path.h"

static char fileContent[4096];

// ============================================================================
// 1. STRING HELPERS
// ============================================================================

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
    put(sh, L"\r\nCustomOS " OS_VERSION L" - Compiled on " OS_BUILD L"\r\n");
}

static void cmd_systeminfo(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    scprint(sh->st, L"\r\n=== CustomOS System Information ===\r\n", 0x0B);
    put(sh, L" OS Version : CustomOS " OS_VERSION L"\r\n");
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

void command_not_found(SHELL *sh, const CHAR16 *line) {
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
// 6b. SHARED HELPERS FOR THE NEWER COMMANDS
// ============================================================================

static void drain_keys(SHELL *sh) {
    EFI_INPUT_KEY k;
    while (sh->st->ConIn->ReadKeyStroke(sh->st->ConIn, &k) == EFI_SUCCESS) { }
}

// Blocks (sleeping, not spinning) until a key is pressed.
static void wait_key(SHELL *sh, EFI_INPUT_KEY *key) {
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn = sh->st->ConIn;
    UINTN index;
    while (ConIn->ReadKeyStroke(ConIn, key) != EFI_SUCCESS) {
        sh->st->BootServices->WaitForEvent(1, &ConIn->WaitForKey, &index);
    }
}

// Case-insensitive string compare (0 = equal).
static int str_cmp_nocase(const CHAR16 *a, const CHAR16 *b) {
    while (*a && fold_case(*a) == fold_case(*b)) { a++; b++; }
    return (int)fold_case(*a) - (int)fold_case(*b);
}

// ---- Pager: prints lines and pauses with "-- More --" at the bottom of the screen
typedef struct {
    SHELL  *sh;
    int     lines;    // screen rows used since the last pause
    int     page;     // rows per page
    int     cols;
    BOOLEAN quit;
} PAGER;

static void pager_init(PAGER *p, SHELL *sh) {
    UINTN cols = 80, rows = 25;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = sh->st->ConOut;
    if (out->Mode && out->QueryMode(out, (UINTN)out->Mode->Mode, &cols, &rows) != EFI_SUCCESS) { cols = 80; rows = 25; }
    p->sh = sh;
    p->lines = 0;
    p->quit = FALSE;
    p->cols = (cols >= 20) ? (int)cols : 80;
    p->page = (rows > 6) ? (int)rows - 2 : 4;
}

// Prints `text` and a line break. Returns FALSE once the user has quit; callers
// should stop producing output then. Lines longer than the screen are counted
// as the rows they really take.
static BOOLEAN pager_line(PAGER *p, const CHAR16 *text) {
    SHELL *sh = p->sh;
    if (p->quit) return FALSE;
    // Pause only when there really is another line to show.
    if (p->lines >= p->page) {
        EFI_INPUT_KEY key;
        put(sh, L"-- More -- (any key: next page, Q: quit)");
        drain_keys(sh);
        wait_key(sh, &key);
        put(sh, L"\r                                       \r");
        p->lines = 0;
        if (key.UnicodeChar == L'q' || key.UnicodeChar == L'Q' || key.ScanCode == SCAN_ESC) {
            p->quit = TRUE;
            return FALSE;
        }
    }
    int len = str_len(text);
    put(sh, text);
    put(sh, L"\r\n");
    p->lines += 1 + ((len > 0) ? (len - 1) / p->cols : 0);
    return TRUE;
}

// ---- Numbers and text formatting -------------------------------------------

// Decimal, or hexadecimal with a 0x prefix. Returns FALSE for anything else.
static BOOLEAN parse_u64(const CHAR16 *s, UINT64 *out) {
    UINT64 v = 0;
    int i = 0;
    if (!s[0]) return FALSE;
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) {
        i = 2;
        if (!s[i]) return FALSE;
        for (; s[i]; i++) {
            CHAR16 c = s[i];
            int d;
            if (c >= L'0' && c <= L'9') d = c - L'0';
            else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
            else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
            else return FALSE;
            if (v >> 60) return FALSE;
            v = (v << 4) | (UINT64)d;
        }
    } else {
        for (; s[i]; i++) {
            if (s[i] < L'0' || s[i] > L'9') return FALSE;
            UINT64 d = (UINT64)(s[i] - L'0');
            if (v > (0xFFFFFFFFFFFFFFFFULL - d) / 10) return FALSE;
            v = v * 10 + d;
        }
    }
    *out = v;
    return TRUE;
}

// Writes `digits` upper-case hex digits of v into out (NUL-terminated).
static void hex_str(UINT64 v, int digits, CHAR16 *out) {
    for (int i = digits - 1; i >= 0; i--) {
        int d = (int)(v & 0xF);
        out[i] = (CHAR16)(d < 10 ? L'0' + d : L'A' + d - 10);
        v >>= 4;
    }
    out[digits] = 0;
}

static void put_hex(SHELL *sh, UINT64 v, int digits) {
    CHAR16 b[20];
    if (digits > 16) digits = 16;
    hex_str(v, digits, b);
    put(sh, b);
}

// Right-aligns a number in `width` columns.
static void put_numr(SHELL *sh, UINT64 n, int width) {
    CHAR16 b[32];
    scitoa(n, b);
    for (int i = str_len(b); i < width; i++) put(sh, L" ");
    put(sh, b);
}

// ---- Wildcards -------------------------------------------------------------

static BOOLEAN has_wildcard(const CHAR16 *s) {
    for (; *s; s++) if (*s == L'*' || *s == L'?') return TRUE;
    return FALSE;
}

// Case-insensitive match where * is any run of characters and ? is any one.
static BOOLEAN wild_match(const CHAR16 *p, const CHAR16 *s) {
    const CHAR16 *star = 0, *mark = 0;
    while (*s) {
        if (*p == L'*') { star = p++; mark = s; }
        else if (*p == L'?' || fold_case(*p) == fold_case(*s)) { p++; s++; }
        else if (star) { p = star + 1; s = ++mark; }
        else return FALSE;
    }
    while (*p == L'*') p++;
    return *p == 0;
}

// Does `hay` contain `needle` (ignoring case)?
static BOOLEAN contains_nocase(const CHAR16 *hay, const CHAR16 *needle) {
    if (!*needle) return TRUE;
    for (; *hay; hay++) {
        const CHAR16 *h = hay, *n = needle;
        while (*h && *n && fold_case(*h) == fold_case(*n)) { h++; n++; }
        if (!*n) return TRUE;
    }
    return FALSE;
}

// Skips the first word of `s` (it may be quoted) and the spaces after it.
static const CHAR16 *skip_word(const CHAR16 *s) {
    BOOLEAN q = FALSE;
    while (*s == L' ') s++;
    while (*s && (q || *s != L' ')) { if (*s == L'"') q = !q; s++; }
    while (*s == L' ') s++;
    return s;
}

// ---- Folder walking --------------------------------------------------------

// Reads the next entry of an open folder, skipping "." and "..". NULL at the end.
static EFI_FILE_INFO *dir_next(EFI_FILE_PROTOCOL *dir, UINT64 *buf, UINTN bufBytes) {
    for (;;) {
        UINTN size = bufBytes;
        if (dir->Read(dir, &size, buf) != EFI_SUCCESS || size == 0) return 0;
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
        if (is_dot_name(fi->FileName)) continue;
        return fi;
    }
}

// ---- Whole-file access for the text and binary tools -----------------------

#define MAX_LOAD_SIZE  (16 * 1024 * 1024)

// Resolves `arg` against the current folder and checks it exists.
// wantFile: refuse folders. Prints the error and returns FALSE on failure.
static BOOLEAN resolve_item(SHELL *sh, const CHAR16 *arg, CHAR16 *path, ITEM_INFO *info, BOOLEAN wantFile) {
    if (!normalize_path(sh->cwd, arg, path, 256)) {
        put_err(sh, L"\r\n[X] That path is too long.\r\n");
        return FALSE;
    }
    if (!query_item(sh->device, sh->st, path, info)) {
        put_not_found(sh, arg);
        return FALSE;
    }
    if (wantFile && info->isDir) {
        put_err(sh, L"\r\n[X] That is a folder, not a file: ");
        put(sh, arg);
        put(sh, L"\r\n");
        return FALSE;
    }
    return TRUE;
}

// Reads a whole file into memory (NUL-terminated, so the bytes can also be
// treated as text). Release with free_file().
static BOOLEAN load_file(SHELL *sh, const CHAR16 *arg, UINT8 **data, UINTN *size) {
    CHAR16 path[256];
    ITEM_INFO info;
    EFI_FILE_PROTOCOL *root = 0, *f = 0;
    UINT8 *buf = 0;

    if (!resolve_item(sh, arg, path, &info, TRUE)) return FALSE;
    if (info.size > MAX_LOAD_SIZE) {
        put_err(sh, L"\r\n[X] That file is too large for this command (limit 16 MB).\r\n");
        return FALSE;
    }
    if (sh->st->BootServices->AllocatePool(EfiLoaderData, (UINTN)info.size + 2, (void **)&buf) != EFI_SUCCESS) {
        put_err(sh, L"\r\n[X] Not enough memory to read that file.\r\n");
        return FALSE;
    }
    UINTN got = (UINTN)info.size;
    BOOLEAN ok = FALSE;
    if (open_volume(sh->device, sh->st, &root)) {
        if (root->Open(root, &f, path, EFI_FILE_MODE_READ, 0) == EFI_SUCCESS) {
            UINTN total = 0;
            ok = TRUE;
            while (total < got) {
                UINTN chunk = got - total;
                if (chunk > COPY_BUF_SIZE) chunk = COPY_BUF_SIZE;
                if (f->Read(f, &chunk, buf + total) != EFI_SUCCESS) { ok = FALSE; break; }
                if (chunk == 0) break;
                total += chunk;
            }
            got = total;
            f->Close(f);
        }
        root->Close(root);
    }
    if (!ok) {
        sh->st->BootServices->FreePool(buf);
        put_err(sh, L"\r\n[X] The file could not be read.\r\n");
        return FALSE;
    }
    buf[got] = 0;
    buf[got + 1] = 0;
    *data = buf;
    *size = got;
    return TRUE;
}

static void free_file(SHELL *sh, UINT8 *data) {
    sh->st->BootServices->FreePool(data);
}

// Takes the next line of text out of `*cur` (bytes, Latin-1) as a CHAR16 string.
// Control characters become '.', tabs become spaces, a lone CR is dropped.
// A line longer than max-1 comes back in pieces. Returns FALSE at the end.
static BOOLEAN text_line(const UINT8 **cur, const UINT8 *end, CHAR16 *out, int max) {
    const UINT8 *p = *cur;
    int n = 0;
    if (p >= end) return FALSE;
    while (p < end && *p != '\n' && n < max - 1) {
        UINT8 c = *p++;
        if (c == '\r') continue;
        if (c == '\t') { out[n++] = L' '; continue; }
        out[n++] = (CHAR16)(c < 32 ? L'.' : c);
    }
    if (p < end && *p == '\n') p++;
    out[n] = 0;
    *cur = p;
    return TRUE;
}

// A buffer of text split into lines (offsets, so the lines can be sorted or counted).
static UINT64 count_lines(const UINT8 *d, UINTN size) {
    UINT64 n = 0;
    for (UINTN i = 0; i < size; i++) if (d[i] == '\n') n++;
    if (size > 0 && d[size - 1] != '\n') n++;
    return n;
}

static UINT32 crc32_bytes(const UINT8 *d, UINTN size) {
    UINT32 crc = 0xFFFFFFFFu;
    for (UINTN i = 0; i < size; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (UINT32)-(int)(crc & 1));
    }
    return ~crc;
}

// ============================================================================
// 8. FILE AND TEXT COMMANDS
// ============================================================================

// ---- touch -----------------------------------------------------------------

static void cmd_touch(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    EFI_FILE_PROTOCOL *root = 0;
    if (!open_volume(sh->device, sh->st, &root)) {
        put_err(sh, L"\r\n[X] Could not open the file system.\r\n");
        return;
    }
    put(sh, L"\r\n");
    for (int i = 1; i < argc; i++) {
        CHAR16 path[256], parent[256];
        ITEM_INFO info;
        if (!normalize_path(sh->cwd, argv[i], path, 256)) {
            put_err(sh, L"[X] That path is too long.\r\n");
            continue;
        }
        if (fs_stat(root, path, &info)) {
            put(sh, L"Already exists, left unchanged: ");
            put(sh, path);
            put(sh, L"\r\n");
            continue;
        }
        path_parent(path, parent);
        if (!fs_stat(root, parent, &info) || !info.isDir) {
            put_err(sh, L"[X] The parent folder does not exist: ");
            put(sh, parent);
            put(sh, L"\r\n");
            continue;
        }
        if (is_protected(path) && !protected_prompt(sh, FALSE, L"create")) {
            put(sh, L"Skipped: ");
            put(sh, path);
            put(sh, L"\r\n");
            continue;
        }
        EFI_FILE_PROTOCOL *f = 0;
        if (root->Open(root, &f, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0) != EFI_SUCCESS) {
            put_err(sh, L"[X] Could not create the file. The name may contain characters that are not allowed.\r\n");
            continue;
        }
        f->Close(f);
        put(sh, L"Created file: ");
        put(sh, path);
        put(sh, L"\r\n");
    }
    root->Close(root);
}

// ---- more, head, tail ------------------------------------------------------

static void cmd_more(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    UINT8 *data; UINTN size;
    if (!load_file(sh, pick_arg(argc, argv, rest), &data, &size)) return;
    PAGER pg;
    pager_init(&pg, sh);
    put(sh, L"\r\n");
    const UINT8 *cur = data, *end = data + size;
    CHAR16 line[512];
    if (size == 0) put(sh, L"(the file is empty)\r\n");
    while (text_line(&cur, end, line, 512)) {
        if (!pager_line(&pg, line)) break;
    }
    free_file(sh, data);
}

static void head_tail(SHELL *sh, int argc, CHAR16 **argv, BOOLEAN tail) {
    UINT64 n = 10;
    if (argc == 3 && (!parse_u64(argv[2], &n) || n == 0)) {
        put_err(sh, L"\r\n[X] The number of lines must be a number greater than zero.\r\n");
        return;
    }
    UINT8 *data; UINTN size;
    if (!load_file(sh, argv[1], &data, &size)) return;

    const UINT8 *cur = data, *end = data + size;
    if (tail) {
        UINT64 total = count_lines(data, size);
        UINT64 skip = (total > n) ? total - n : 0;
        while (skip > 0 && cur < end) {
            if (*cur++ == '\n') skip--;
        }
    }
    PAGER pg;
    pager_init(&pg, sh);
    put(sh, L"\r\n");
    CHAR16 line[512];
    UINT64 shown = 0;
    if (size == 0) put(sh, L"(the file is empty)\r\n");
    while ((tail || shown < n) && text_line(&cur, end, line, 512)) {
        if (!pager_line(&pg, line)) break;
        shown++;
    }
    free_file(sh, data);
}

static void cmd_head(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) { (void)rest; head_tail(sh, argc, argv, FALSE); }
static void cmd_tail(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) { (void)rest; head_tail(sh, argc, argv, TRUE); }

// ---- count -----------------------------------------------------------------

static void cmd_count(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    UINT8 *data; UINTN size;
    if (!load_file(sh, pick_arg(argc, argv, rest), &data, &size)) return;
    UINT64 words = 0;
    BOOLEAN inWord = FALSE;
    for (UINTN i = 0; i < size; i++) {
        UINT8 c = data[i];
        BOOLEAN space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (!space && !inWord) words++;
        inWord = !space;
    }
    put(sh, L"\r\n  Lines: "); put_num(sh, count_lines(data, size));
    put(sh, L"\r\n  Words: "); put_num(sh, words);
    put(sh, L"\r\n  Bytes: "); put_num(sh, size);
    put(sh, L"\r\n");
    free_file(sh, data);
}

// ---- sort ------------------------------------------------------------------

typedef struct { const UINT8 *p; UINT32 len; } LINE_REF;

static int line_cmp(const LINE_REF *a, const LINE_REF *b) {
    UINT32 n = a->len < b->len ? a->len : b->len;
    for (UINT32 i = 0; i < n; i++) {
        CHAR16 x = fold_case(a->p[i]), y = fold_case(b->p[i]);
        if (x != y) return (int)x - (int)y;
    }
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    return 0;
}

static void cmd_sort(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    BOOLEAN reverse = FALSE;
    if (argc == 3) {
        if (str_cmp_nocase(argv[2], L"reverse") != 0) {
            put_err(sh, L"\r\n[X] The only option is: reverse\r\n");
            return;
        }
        reverse = TRUE;
    }
    UINT8 *data; UINTN size;
    if (!load_file(sh, argv[1], &data, &size)) return;

    UINT64 n = count_lines(data, size);
    if (n == 0) { put(sh, L"\r\n(the file is empty)\r\n"); free_file(sh, data); return; }
    if (n > 100000) {
        put_err(sh, L"\r\n[X] Too many lines to sort (limit 100000).\r\n");
        free_file(sh, data);
        return;
    }
    LINE_REF *lines = 0;
    if (sh->st->BootServices->AllocatePool(EfiLoaderData, (UINTN)n * sizeof(LINE_REF), (void **)&lines) != EFI_SUCCESS) {
        put_err(sh, L"\r\n[X] Not enough memory to sort that file.\r\n");
        free_file(sh, data);
        return;
    }
    UINTN count = 0, start = 0;
    for (UINTN i = 0; i <= size && count < n; i++) {
        if (i == size || data[i] == '\n') {
            if (i == size && start == size) break;
            UINTN len = i - start;
            if (len > 0 && data[start + len - 1] == '\r') len--;
            lines[count].p = data + start;
            lines[count].len = (UINT32)len;
            count++;
            start = i + 1;
        }
    }
    // Shell sort: no recursion, no extra memory.
    for (UINTN gap = count / 2; gap > 0; gap /= 2) {
        for (UINTN i = gap; i < count; i++) {
            LINE_REF t = lines[i];
            UINTN j = i;
            while (j >= gap && line_cmp(&lines[j - gap], &t) > 0) { lines[j] = lines[j - gap]; j -= gap; }
            lines[j] = t;
        }
    }
    PAGER pg;
    pager_init(&pg, sh);
    put(sh, L"\r\n");
    for (UINTN k = 0; k < count; k++) {
        const LINE_REF *r = &lines[reverse ? count - 1 - k : k];
        const UINT8 *cur = r->p, *end = r->p + r->len;
        CHAR16 line[512];
        BOOLEAN any = FALSE;
        while (text_line(&cur, end, line, 512)) { any = TRUE; if (!pager_line(&pg, line)) goto out; }
        if (!any && !pager_line(&pg, L"")) break;
    }
out:
    sh->st->BootServices->FreePool(lines);
    free_file(sh, data);
}

// ---- compare ---------------------------------------------------------------

static void cmd_compare(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)rest;
    UINT8 *a, *b; UINTN sa, sb;
    if (!load_file(sh, argv[1], &a, &sa)) return;
    if (!load_file(sh, argv[2], &b, &sb)) { free_file(sh, a); return; }

    UINTN common = sa < sb ? sa : sb, first = common;
    UINT64 diffs = 0;
    for (UINTN i = 0; i < common; i++) {
        if (a[i] != b[i]) { if (diffs == 0) first = i; diffs++; }
    }
    if (diffs == 0 && sa == sb) {
        put(sh, L"\r\nThe files are identical (");
        put_num(sh, sa);
        put(sh, L" bytes).\r\n");
    } else {
        put(sh, L"\r\nThe files are different.\r\n");
        put(sh, L"  Sizes: "); put_num(sh, sa); put(sh, L" and "); put_num(sh, sb); put(sh, L" bytes\r\n");
        if (diffs > 0) {
            put(sh, L"  First difference at byte "); put_num(sh, first);
            put(sh, L" (0x"); put_hex(sh, first, 8); put(sh, L"): 0x");
            put_hex(sh, a[first], 2); put(sh, L" and 0x"); put_hex(sh, b[first], 2);
            put(sh, L"\r\n  Differing bytes in the shared part: "); put_num(sh, diffs);
            put(sh, L"\r\n");
        } else {
            put(sh, L"  The shorter file matches the start of the longer one.\r\n");
        }
    }
    free_file(sh, a);
    free_file(sh, b);
}

// ---- checksum --------------------------------------------------------------

static void cmd_checksum(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    UINT8 *data; UINTN size;
    const CHAR16 *arg = pick_arg(argc, argv, rest);
    if (!load_file(sh, arg, &data, &size)) return;
    put(sh, L"\r\nCRC-32: ");
    put_hex(sh, crc32_bytes(data, size), 8);
    put(sh, L"   ("); put_num(sh, size); put(sh, L" bytes)  "); put(sh, arg);
    put(sh, L"\r\n");
    free_file(sh, data);
}

// ---- hexdump ---------------------------------------------------------------

static void cmd_hexdump(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    UINT64 offset = 0, length = 0xFFFFFFFFFFFFFFFFULL;
    if (argc >= 3 && !parse_u64(argv[2], &offset)) {
        put_err(sh, L"\r\n[X] The offset must be a number (decimal, or hex such as 0x100).\r\n");
        return;
    }
    if (argc >= 4 && !parse_u64(argv[3], &length)) {
        put_err(sh, L"\r\n[X] The length must be a number (decimal, or hex such as 0x100).\r\n");
        return;
    }
    UINT8 *data; UINTN size;
    if (!load_file(sh, argv[1], &data, &size)) return;
    if (size == 0) { put(sh, L"\r\n(the file is empty)\r\n"); free_file(sh, data); return; }
    if (offset >= size) {
        put_err(sh, L"\r\n[X] The offset is past the end of the file (");
        put_num(sh, size);
        put(sh, L" bytes).\r\n");
        free_file(sh, data);
        return;
    }
    UINT64 end = (length > size - offset) ? size : offset + length;

    PAGER pg;
    pager_init(&pg, sh);
    put(sh, L"\r\n");
    for (UINT64 pos = offset; pos < end; pos += 16) {
        CHAR16 line[96];
        int n = 0;
        CHAR16 tmp[12];
        hex_str(pos, 8, tmp);
        for (int i = 0; i < 8; i++) line[n++] = tmp[i];
        line[n++] = L' '; line[n++] = L' ';
        for (int i = 0; i < 16; i++) {
            if (pos + i < end) {
                hex_str(data[pos + i], 2, tmp);
                line[n++] = tmp[0]; line[n++] = tmp[1];
            } else {
                line[n++] = L' '; line[n++] = L' ';
            }
            line[n++] = L' ';
            if (i == 7) line[n++] = L' ';
        }
        line[n++] = L'|';
        for (int i = 0; i < 16 && pos + i < end; i++) {
            UINT8 c = data[pos + i];
            line[n++] = (c >= 32 && c < 127) ? c : L'.';
        }
        line[n++] = L'|';
        line[n] = 0;
        if (!pager_line(&pg, line)) break;
    }
    free_file(sh, data);
}

// ---- find ------------------------------------------------------------------

typedef struct {
    PAGER   pg;
    const CHAR16 *pattern;   // already has wildcards
    UINT64  found;
} FIND_CTX;

static void find_walk(EFI_FILE_PROTOCOL *root, const CHAR16 *dirPath, FIND_CTX *c, int depth) {
    EFI_FILE_PROTOCOL *dir = 0;
    UINT64 buf[128];
    if (root->Open(root, &dir, (CHAR16 *)dirPath, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) return;
    EFI_FILE_INFO *fi;
    while (!c->pg.quit && (fi = dir_next(dir, buf, sizeof buf)) != 0) {
        CHAR16 child[256];
        BOOLEAN isDir = (fi->Attribute & EFI_FILE_DIRECTORY) ? TRUE : FALSE;
        if (!path_join(dirPath, fi->FileName, child, 256)) continue;
        if (wild_match(c->pattern, fi->FileName)) {
            CHAR16 shown[260];
            str_set(shown, 260, child);
            if (isDir) str_cat(shown, 260, L"\\");
            c->found++;
            if (!pager_line(&c->pg, shown)) break;
        }
        if (isDir && depth < MAX_TREE_DEPTH) find_walk(root, child, c, depth + 1);
    }
    dir->Close(dir);
}

static void cmd_find(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    CHAR16 pattern[260], start[256];
    ITEM_INFO info;
    EFI_FILE_PROTOCOL *root = 0;

    if (has_wildcard(argv[1])) str_set(pattern, 260, argv[1]);
    else { str_set(pattern, 260, L"*"); str_cat(pattern, 260, argv[1]); str_cat(pattern, 260, L"*"); }

    if (argc == 3) {
        if (!normalize_path(sh->cwd, argv[2], start, 256)) { put_err(sh, L"\r\n[X] That path is too long.\r\n"); return; }
    } else {
        str_set(start, 256, sh->cwd);
    }
    if (!open_volume(sh->device, sh->st, &root)) { put_err(sh, L"\r\n[X] Could not open the file system.\r\n"); return; }
    if (!fs_stat(root, start, &info) || !info.isDir) {
        put_err(sh, L"\r\n[X] Not a folder: ");
        put(sh, start);
        put(sh, L"\r\n");
        root->Close(root);
        return;
    }
    FIND_CTX ctx;
    ctx.pattern = pattern;
    ctx.found = 0;
    pager_init(&ctx.pg, sh);
    put(sh, L"\r\n");
    find_walk(root, start, &ctx, 0);
    root->Close(root);
    if (!ctx.pg.quit) {
        if (ctx.found == 0) put(sh, L"Nothing found.\r\n");
        else { put_count(sh, ctx.found, L"match", L"matches"); put(sh, L"\r\n"); }
    }
}

// ---- tree ------------------------------------------------------------------

typedef struct {
    PAGER   pg;
    UINT64  files;
    UINT64  dirs;
} TREE_CTX;

static void tree_walk(EFI_FILE_PROTOCOL *root, const CHAR16 *dirPath, CHAR16 *prefix, TREE_CTX *c, int depth) {
    EFI_FILE_PROTOCOL *dir = 0;
    UINT64 buf[128];
    CHAR16 curName[256], nextName[256];
    BOOLEAN curDir = FALSE, nextDir = FALSE;
    if (root->Open(root, &dir, (CHAR16 *)dirPath, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) return;

    EFI_FILE_INFO *fi = dir_next(dir, buf, sizeof buf);
    BOOLEAN have = FALSE;
    if (fi) { str_set(curName, 256, fi->FileName); curDir = (fi->Attribute & EFI_FILE_DIRECTORY) ? TRUE : FALSE; have = TRUE; }

    while (have && !c->pg.quit) {
        BOOLEAN haveNext = FALSE;
        fi = dir_next(dir, buf, sizeof buf);
        if (fi) { str_set(nextName, 256, fi->FileName); nextDir = (fi->Attribute & EFI_FILE_DIRECTORY) ? TRUE : FALSE; haveNext = TRUE; }

        CHAR16 line[300];
        str_set(line, 300, prefix);
        str_cat(line, 300, haveNext ? L"|-- " : L"\\-- ");
        str_cat(line, 300, curName);
        if (curDir) str_cat(line, 300, L"\\");
        if (curDir) c->dirs++; else c->files++;
        if (!pager_line(&c->pg, line)) break;

        if (curDir) {
            if (depth >= MAX_TREE_DEPTH) {
                CHAR16 more[120];
                str_set(more, 120, prefix);
                str_cat(more, 120, haveNext ? L"|   ..." : L"    ...");
                if (!pager_line(&c->pg, more)) break;
            } else {
                CHAR16 child[256], sub[100];
                if (path_join(dirPath, curName, child, 256)) {
                    str_set(sub, 100, prefix);
                    str_cat(sub, 100, haveNext ? L"|   " : L"    ");
                    tree_walk(root, child, sub, c, depth + 1);
                }
            }
        }
        if (haveNext) { str_set(curName, 256, nextName); curDir = nextDir; }
        have = haveNext;
    }
    dir->Close(dir);
}

static void cmd_tree(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    CHAR16 start[256];
    ITEM_INFO info;
    EFI_FILE_PROTOCOL *root = 0;
    const CHAR16 *arg = pick_arg(argc, argv, rest);

    if (argc == 1) str_set(start, 256, sh->cwd);
    else if (!normalize_path(sh->cwd, arg, start, 256)) { put_err(sh, L"\r\n[X] That path is too long.\r\n"); return; }

    if (!open_volume(sh->device, sh->st, &root)) { put_err(sh, L"\r\n[X] Could not open the file system.\r\n"); return; }
    if (!fs_stat(root, start, &info) || !info.isDir) {
        put_err(sh, L"\r\n[X] Not a folder: ");
        put(sh, start);
        put(sh, L"\r\n");
        root->Close(root);
        return;
    }
    TREE_CTX ctx;
    ctx.files = ctx.dirs = 0;
    pager_init(&ctx.pg, sh);
    put(sh, L"\r\n");
    if (!pager_line(&ctx.pg, start)) { root->Close(root); return; }
    CHAR16 prefix[100];
    prefix[0] = 0;
    tree_walk(root, start, prefix, &ctx, 0);
    root->Close(root);
    if (!ctx.pg.quit) {
        put(sh, L"\r\n");
        put_count(sh, ctx.dirs, L"folder", L"folders");
        put(sh, L", ");
        put_count(sh, ctx.files, L"file", L"files");
        put(sh, L"\r\n");
    }
}

// ---- search (text inside files) --------------------------------------------

typedef struct {
    PAGER   pg;
    const CHAR16 *needle;
    UINT64  hits;
} SEARCH_CTX;

// Returns FALSE when the user quit the pager.
static BOOLEAN search_one(SHELL *sh, SEARCH_CTX *c, const CHAR16 *shownName, const CHAR16 *arg, BOOLEAN prefixName) {
    UINT8 *data; UINTN size;
    if (!load_file(sh, arg, &data, &size)) return TRUE;
    const UINT8 *cur = data, *end = data + size;
    CHAR16 line[512], out[800];
    UINT64 lineNo = 0;
    BOOLEAN keepGoing = TRUE;
    for (;;) {
        BOOLEAN continued = (cur > data && cur[-1] != '\n');      // the rest of a line that was too long for one piece
        if (!text_line(&cur, end, line, 512)) break;
        if (!continued) lineNo++;
        if (!contains_nocase(line, c->needle)) continue;
        c->hits++;
        CHAR16 num[32];
        scitoa(lineNo, num);
        out[0] = 0;
        if (prefixName) { str_cat(out, 800, shownName); str_cat(out, 800, L":"); }
        str_cat(out, 800, num);
        str_cat(out, 800, L": ");
        str_cat(out, 800, line);
        if (!pager_line(&c->pg, out)) { keepGoing = FALSE; break; }
    }
    free_file(sh, data);
    return keepGoing;
}

static void cmd_search(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    SEARCH_CTX ctx;
    ctx.needle = argv[1];
    ctx.hits = 0;

    if (argc == 3) {
        CHAR16 path[256];
        ITEM_INFO info;
        if (!resolve_item(sh, argv[2], path, &info, TRUE)) return;
        pager_init(&ctx.pg, sh);
        put(sh, L"\r\n");
        search_one(sh, &ctx, argv[2], argv[2], FALSE);
    } else {
        // No file given: look in every file of the current folder.
        EFI_FILE_PROTOCOL *root = 0, *dir = 0;
        UINT64 buf[128];
        if (!open_volume(sh->device, sh->st, &root)) { put_err(sh, L"\r\n[X] Could not open the file system.\r\n"); return; }
        if (root->Open(root, &dir, sh->cwd, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) { root->Close(root); put_err(sh, L"\r\n[X] Could not open the current folder.\r\n"); return; }
        pager_init(&ctx.pg, sh);
        put(sh, L"\r\n");
        EFI_FILE_INFO *fi;
        while ((fi = dir_next(dir, buf, sizeof buf)) != 0) {
            if (fi->Attribute & EFI_FILE_DIRECTORY) continue;
            CHAR16 name[256];
            str_set(name, 256, fi->FileName);
            if (!search_one(sh, &ctx, name, name, TRUE)) break;
        }
        dir->Close(dir);
        root->Close(root);
    }
    if (!ctx.pg.quit) {
        if (ctx.hits == 0) put(sh, L"No matching lines.\r\n");
        else { put_count(sh, ctx.hits, L"matching line", L"matching lines"); put(sh, L"\r\n"); }
    }
}

// ---- write and append ------------------------------------------------------

// Checks the target of write/append and asks for confirmation where needed.
// Returns FALSE (after printing why) if nothing should be written.
static BOOLEAN prepare_write(SHELL *sh, const CHAR16 *arg, CHAR16 *path, BOOLEAN append) {
    CHAR16 parent[256];
    ITEM_INFO info;
    EFI_FILE_PROTOCOL *root = 0;
    BOOLEAN exists, ok = FALSE;

    if (!normalize_path(sh->cwd, arg, path, 256)) { put_err(sh, L"\r\n[X] That path is too long.\r\n"); return FALSE; }
    if (!open_volume(sh->device, sh->st, &root)) { put_err(sh, L"\r\n[X] Could not open the file system.\r\n"); return FALSE; }

    exists = fs_stat(root, path, &info);
    if (exists && info.isDir) {
        put_err(sh, L"\r\n[X] That is a folder, not a file: ");
        put(sh, arg);
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
        if (!protected_prompt(sh, FALSE, append ? L"add to" : L"overwrite")) { put_cancelled(sh); goto done; }
    } else if (exists && !append) {
        if (!ask_yes_no(sh, L"That file already exists. Replace its contents? [Y] or [N]:", 0x0E)) { put_cancelled(sh); goto done; }
    }
    ok = TRUE;
done:
    root->Close(root);
    return ok;
}

// Writes `text` plus a line break. append=FALSE replaces the file.
static BOOLEAN write_text(SHELL *sh, const CHAR16 *path, const CHAR16 *text, BOOLEAN append) {
    EFI_FILE_PROTOCOL *root = 0, *f = 0;
    ITEM_INFO info;
    UINTN len = (UINTN)str_len(text);
    UINT8 *buf = 0;
    BOOLEAN ok = FALSE;

    if (sh->st->BootServices->AllocatePool(EfiLoaderData, len + 2, (void **)&buf) != EFI_SUCCESS) return FALSE;
    for (UINTN i = 0; i < len; i++) buf[i] = (text[i] < 256) ? (UINT8)text[i] : (UINT8)'?';
    buf[len] = '\r';
    buf[len + 1] = '\n';

    if (open_volume(sh->device, sh->st, &root)) {
        BOOLEAN exists = fs_stat(root, path, &info);
        BOOLEAN proceed = TRUE;
        if (exists && !append) proceed = (fs_delete(root, path) == EFI_SUCCESS);   // CREATE does not truncate
        if (proceed && root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0) == EFI_SUCCESS) {
            UINTN n = len + 2;
            ok = TRUE;
            if (append && exists && f->SetPosition(f, info.size) != EFI_SUCCESS) ok = FALSE;
            if (ok && (f->Write(f, &n, buf) != EFI_SUCCESS || n != len + 2)) ok = FALSE;
            f->Close(f);
        }
        root->Close(root);
    }
    sh->st->BootServices->FreePool(buf);
    return ok;
}

static void write_or_append(SHELL *sh, const CHAR16 *rest, BOOLEAN append, const CHAR16 *fileArg) {
    CHAR16 path[256];
    const CHAR16 *text = skip_word(rest);
    if (!prepare_write(sh, fileArg, path, append)) return;
    if (!write_text(sh, path, text, append)) {
        put_err(sh, L"\r\n[X] Could not write to the file.\r\n");
        return;
    }
    put(sh, append ? L"\r\nAdded a line to: " : L"\r\nWrote: ");
    put(sh, path);
    put(sh, L"\r\n");
}

static void cmd_write(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) { (void)argc; write_or_append(sh, rest, FALSE, argv[1]); }
static void cmd_append(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) { (void)argc; write_or_append(sh, rest, TRUE, argv[1]); }

// ---- attributes ------------------------------------------------------------

static void put_yes_no(SHELL *sh, const CHAR16 *label, BOOLEAN v) {
    put(sh, label);
    put(sh, v ? L"yes\r\n" : L"no\r\n");
}

static void cmd_attributes(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    CHAR16 path[256];
    ITEM_INFO info;
    UINT64 add = 0, remove = 0;
    EFI_GUID infoGuid = EFI_FILE_INFO_GUID;

    for (int i = 2; i < argc; i++) {
        const CHAR16 *a = argv[i];
        UINT64 bit = 0;
        if ((a[0] == L'+' || a[0] == L'-') && a[1] && !a[2]) {
            switch (fold_case(a[1])) {
                case L'R': bit = EFI_FILE_READ_ONLY; break;
                case L'H': bit = EFI_FILE_HIDDEN;    break;
                case L'S': bit = EFI_FILE_SYSTEM;    break;
                case L'A': bit = EFI_FILE_ARCHIVE;   break;
            }
        }
        if (!bit) {
            put_err(sh, L"\r\n[X] Not understood: ");
            put(sh, a);
            put(sh, L"\r\n    Use +r/-r (read-only), +h/-h (hidden), +s/-s (system), +a/-a (archive).\r\n");
            return;
        }
        if (a[0] == L'+') add |= bit; else remove |= bit;
    }
    if (!resolve_item(sh, argv[1], path, &info, FALSE)) return;

    EFI_FILE_PROTOCOL *root = 0, *f = 0;
    UINT64 buf[128];
    UINTN size;
    BOOLEAN changing = (add | remove) != 0;
    if (!open_volume(sh->device, sh->st, &root)) { put_err(sh, L"\r\n[X] Could not open the file system.\r\n"); return; }

    if (changing && is_protected(path) && !protected_prompt(sh, info.isDir, L"change the attributes of")) {
        put_cancelled(sh);
        root->Close(root);
        return;
    }
    UINT64 mode = EFI_FILE_MODE_READ | (changing ? EFI_FILE_MODE_WRITE : 0);
    if (root->Open(root, &f, path, mode, 0) != EFI_SUCCESS) {
        put_err(sh, L"\r\n[X] Could not open it for changing (the disk may be read-only).\r\n");
        root->Close(root);
        return;
    }
    size = sizeof buf;
    if (f->GetInfo(f, &infoGuid, &size, buf) != EFI_SUCCESS) {
        put_err(sh, L"\r\n[X] Could not read the attributes.\r\n");
        goto done;
    }
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
    if (changing) {
        fi->Attribute = (fi->Attribute & ~remove) | add;
        if (f->SetInfo(f, &infoGuid, (UINTN)fi->Size, fi) != EFI_SUCCESS) {
            put_err(sh, L"\r\n[X] The attributes could not be changed.\r\n");
            goto done;
        }
        size = sizeof buf;
        if (f->GetInfo(f, &infoGuid, &size, buf) != EFI_SUCCESS) goto done;
    }
    put(sh, L"\r\nAttributes of ");
    put(sh, path);
    put(sh, L":\r\n");
    put_yes_no(sh, L"  Read-only: ", (fi->Attribute & EFI_FILE_READ_ONLY) != 0);
    put_yes_no(sh, L"  Hidden:    ", (fi->Attribute & EFI_FILE_HIDDEN) != 0);
    put_yes_no(sh, L"  System:    ", (fi->Attribute & EFI_FILE_SYSTEM) != 0);
    put_yes_no(sh, L"  Archive:   ", (fi->Attribute & EFI_FILE_ARCHIVE) != 0);
    put_yes_no(sh, L"  Folder:    ", (fi->Attribute & EFI_FILE_DIRECTORY) != 0);
done:
    f->Close(f);
    root->Close(root);
}

// ============================================================================
// 9. SHELL COMMANDS: version, date, time, pause, sleep, prompt, history
// ============================================================================

static void put_2d(SHELL *sh, UINT64 n) {
    if (n < 10) put(sh, L"0");
    put_num(sh, n);
}

// Days since 1970-01-01 for a calendar date (proleptic Gregorian).
static long long days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static BOOLEAN is_leap_year(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in_month(int y, int m) {
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && is_leap_year(y)) ? 29 : dm[m - 1];
}

// 0 = Sunday ... 6 = Saturday
static int weekday_of(int y, int m, int d) {
    long long z = days_from_civil(y, m, d);
    int w = (int)((z + 4) % 7);
    return w < 0 ? w + 7 : w;
}

static const CHAR16 *const day_names[7] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };
static const CHAR16 *const month_names[12] = { L"January", L"February", L"March", L"April", L"May", L"June",
                                               L"July", L"August", L"September", L"October", L"November", L"December" };

static BOOLEAN get_clock(SHELL *sh, EFI_TIME *t) {
    if (sh->st->RuntimeServices->GetTime(t, NULL) != EFI_SUCCESS) {
        put_err(sh, L"\r\n[X] The clock is not available on this computer.\r\n");
        return FALSE;
    }
    return TRUE;
}

// Reads up to three numbers separated by - / or : from `s` ("2026-11-23", "14:30:05").
// Returns how many numbers were found, or -1 if the text is not of that form.
static int parse_parts(const CHAR16 *s, UINT64 *v, int maxParts) {
    int n = 0;
    while (*s) {
        if (n >= maxParts) return -1;
        if (*s < L'0' || *s > L'9') return -1;
        UINT64 x = 0;
        int digits = 0;
        while (*s >= L'0' && *s <= L'9') { x = x * 10 + (UINT64)(*s - L'0'); s++; if (++digits > 6) return -1; }
        v[n++] = x;
        if (*s == L'-' || *s == L'/' || *s == L':') { s++; if (!*s) return -1; }
        else if (*s) return -1;
    }
    return n;
}

static void cmd_version(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    UINT32 rev = sh->st->Hdr.Revision;
    put(sh, L"\r\nCustomOS version " OS_VERSION L"\r\n");
    put(sh, L"Built on " OS_BUILD L" for x86_64 UEFI\r\n");
    put(sh, L"UEFI firmware: ");
    put(sh, sh->st->FirmwareVendor);
    put(sh, L", UEFI specification ");
    put_num(sh, rev >> 16);
    put(sh, L".");
    put_num(sh, (rev & 0xFFFF) / 10);
    if ((rev & 0xFFFF) % 10) { put(sh, L"."); put_num(sh, (rev & 0xFFFF) % 10); }
    put(sh, L"\r\n");
}

static void cmd_date(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    EFI_TIME t;
    if (!get_clock(sh, &t)) return;

    if (argc == 2) {
        UINT64 v[3];
        if (parse_parts(argv[1], v, 3) != 3 || v[0] < 1980 || v[0] > 2099 || v[1] < 1 || v[1] > 12 ||
            v[2] < 1 || (int)v[2] > days_in_month((int)v[0], (int)v[1])) {
            put_err(sh, L"\r\n[X] That is not a valid date. Use year-month-day, for example 2026-11-23.\r\n");
            return;
        }
        t.Year = (UINT16)v[0]; t.Month = (UINT8)v[1]; t.Day = (UINT8)v[2];
        if (sh->st->RuntimeServices->SetTime(&t) != EFI_SUCCESS) {
            put_err(sh, L"\r\n[X] The clock could not be changed.\r\n");
            return;
        }
        put(sh, L"\r\nDate set.\r\n");
    }
    put(sh, L"\r\n");
    put(sh, day_names[weekday_of(t.Year, t.Month, t.Day)]);
    put(sh, L", ");
    put_num(sh, t.Day);
    put(sh, L" ");
    put(sh, month_names[(t.Month >= 1 && t.Month <= 12) ? t.Month - 1 : 0]);
    put(sh, L" ");
    put_num(sh, t.Year);
    put(sh, L"  (");
    put_num(sh, t.Year); put(sh, L"-"); put_2d(sh, t.Month); put(sh, L"-"); put_2d(sh, t.Day);
    put(sh, L")\r\n");
}

static void cmd_time(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    EFI_TIME t;
    if (!get_clock(sh, &t)) return;

    if (argc == 2) {
        UINT64 v[3] = { 0, 0, 0 };
        int n = parse_parts(argv[1], v, 3);
        if (n < 2 || v[0] > 23 || v[1] > 59 || v[2] > 59) {
            put_err(sh, L"\r\n[X] That is not a valid time. Use hours:minutes or hours:minutes:seconds, for example 14:30.\r\n");
            return;
        }
        t.Hour = (UINT8)v[0]; t.Minute = (UINT8)v[1]; t.Second = (UINT8)v[2]; t.Nanosecond = 0;
        if (sh->st->RuntimeServices->SetTime(&t) != EFI_SUCCESS) {
            put_err(sh, L"\r\n[X] The clock could not be changed.\r\n");
            return;
        }
        put(sh, L"\r\nTime set.\r\n");
    }
    put(sh, L"\r\n");
    put_2d(sh, t.Hour); put(sh, L":"); put_2d(sh, t.Minute); put(sh, L":"); put_2d(sh, t.Second);
    put(sh, L"\r\n");
}

static void cmd_pause(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argv;
    EFI_INPUT_KEY key;
    put(sh, L"\r\n");
    put(sh, argc > 1 ? rest : L"Press any key to continue...");
    drain_keys(sh);
    wait_key(sh, &key);
    put(sh, L"\r\n");
}

static void cmd_sleep(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)rest;
    UINT64 secs;
    if (!parse_u64(argv[1], &secs) || secs == 0 || secs > 3600) {
        put_err(sh, L"\r\n[X] Give a number of seconds from 1 to 3600.\r\n");
        return;
    }
    put(sh, L"\r\nWaiting ");
    put_count(sh, secs, L"second", L"seconds");
    put(sh, L"... (Esc cancels)\r\n");
    drain_keys(sh);
    for (UINT64 i = 0; i < secs * 10; i++) {
        EFI_INPUT_KEY key;
        sh->st->BootServices->Stall(100000);   // 100 ms
        if (sh->st->ConIn->ReadKeyStroke(sh->st->ConIn, &key) == EFI_SUCCESS && key.ScanCode == SCAN_ESC) {
            put(sh, L"Cancelled.\r\n");
            return;
        }
    }
}

static void cmd_prompt(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argv;
    if (argc == 1) {
        put(sh, L"\r\nThe prompt text is: ");
        put(sh, sh->prompt);
        put(sh, L"\r\n");
        return;
    }
    if (str_cmp_nocase(rest, L"reset") == 0) {
        str_set(sh->prompt, PROMPT_MAX, L"CustomOS:");
        put(sh, L"\r\nPrompt restored.\r\n");
        return;
    }
    if (str_len(rest) >= PROMPT_MAX) {
        put_err(sh, L"\r\n[X] The prompt text can be at most 39 characters long.\r\n");
        return;
    }
    str_set(sh->prompt, PROMPT_MAX, rest);
    put(sh, L"\r\nPrompt changed.\r\n");
}

static void cmd_history(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    if (argc == 2) {
        if (str_cmp_nocase(argv[1], L"clear") != 0) {
            put_err(sh, L"\r\n[X] The only option is: clear\r\n");
            return;
        }
        shell_history_clear();
        put(sh, L"\r\nHistory cleared.\r\n");
        return;
    }
    int n = shell_history_count();
    put(sh, L"\r\n");
    if (n == 0) { put(sh, L"No commands in the history yet.\r\n"); return; }
    for (int i = 0; i < n; i++) {
        put_numr(sh, (UINT64)(i + 1), 3);
        put(sh, L"  ");
        put(sh, (const CHAR16 *)shell_history_get(i));
        put(sh, L"\r\n");
    }
}

// ============================================================================
// 10. SYSTEM AND HARDWARE COMMANDS
// ============================================================================

// ---- small helpers ---------------------------------------------------------

static UINT16 rd16(const UINT8 *p) { return (UINT16)(p[0] | (p[1] << 8)); }
static UINT32 rd32(const UINT8 *p) { return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24); }
static UINT64 rd64(const UINT8 *p) { return (UINT64)rd32(p) | ((UINT64)rd32(p + 4) << 32); }

static BOOLEAN guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++) if (x[i] != y[i]) return FALSE;
    return TRUE;
}

// 0 when the n bytes at p equal the ASCII text s.
static int memcmp_u8(const UINT8 *p, const char *s, int n) {
    for (int i = 0; i < n; i++) if (p[i] != (UINT8)s[i]) return 1;
    return 0;
}

// Prints `n` bytes of an ASCII field (OEM ids and the like); anything odd shows as '.'.
static void put_ascii(SHELL *sh, const UINT8 *s, int n) {
    CHAR16 b[40];
    int i = 0;
    for (; i < n && i < 39 && s[i]; i++) b[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : L'.';
    while (i > 0 && b[i - 1] == L' ') i--;
    b[i] = 0;
    put(sh, b);
}

// Pads `s` with spaces to `width` columns and prints it.
static void put_pad(SHELL *sh, const CHAR16 *s, int width) {
    put(sh, s);
    for (int i = str_len(s); i < width; i++) put(sh, L" ");
}

static void put_size(SHELL *sh, UINT64 bytes) {
    CHAR16 b[40];
    format_memory_size(bytes, b, 40);
    put(sh, b);
}

static void put_size_pad(SHELL *sh, UINT64 bytes, int width) {
    CHAR16 b[40];
    format_memory_size(bytes, b, 40);
    for (int i = str_len(b); i < width; i++) put(sh, L" ");
    put(sh, b);
}

// ---- memory and memmap -----------------------------------------------------

static BOOLEAN load_memmap(SHELL *sh, EFI_MEMORY_DESCRIPTOR **map, UINTN *bytes, UINTN *dsize) {
    EFI_BOOT_SERVICES *bs = sh->st->BootServices;
    UINTN size = 0, key, ds = 0;
    UINT32 ver;
    EFI_MEMORY_DESCRIPTOR *m = 0;

    if (bs->GetMemoryMap(&size, 0, &key, &ds, &ver) != EFI_BUFFER_TOO_SMALL || ds == 0) goto fail;
    size += 8 * ds;                          // allocating the buffer can add map entries
    if (bs->AllocatePool(EfiLoaderData, size, (void **)&m) != EFI_SUCCESS) goto fail;
    if (bs->GetMemoryMap(&size, m, &key, &ds, &ver) != EFI_SUCCESS) { bs->FreePool(m); goto fail; }
    *map = m; *bytes = size; *dsize = ds;
    return TRUE;
fail:
    put_err(sh, L"\r\n[X] The firmware would not give out its memory map.\r\n");
    return FALSE;
}

static void cmd_memory(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    EFI_MEMORY_DESCRIPTOR *map; UINTN bytes, ds;
    UINT64 freeB = 0, loader = 0, bootsvc = 0, runtime = 0, acpi = 0, reserved = 0, other = 0;

    if (!load_memmap(sh, &map, &bytes, &ds)) return;
    for (UINTN i = 0; i < bytes / ds; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * ds);
        UINT64 b = d->NumberOfPages * 4096;
        switch (d->Type) {
            case EfiConventionalMemory: freeB += b; break;
            case EfiLoaderCode: case EfiLoaderData: loader += b; break;
            case EfiBootServicesCode: case EfiBootServicesData: bootsvc += b; break;
            case EfiRuntimeServicesCode: case EfiRuntimeServicesData: runtime += b; break;
            case EfiACPIReclaimMemory: case EfiACPIMemoryNVS: acpi += b; break;
            case EfiReservedMemoryType: reserved += b; break;
            default: other += b; break;
        }
    }
    sh->st->BootServices->FreePool(map);

    put(sh, L"\r\n  Free memory          : "); put_size_pad(sh, freeB, 10);
    put(sh, L"\r\n  CustomOS and loader  : "); put_size_pad(sh, loader, 10);
    put(sh, L"\r\n  Firmware (boot)      : "); put_size_pad(sh, bootsvc, 10);
    put(sh, L"\r\n  Firmware (runtime)   : "); put_size_pad(sh, runtime, 10);
    put(sh, L"\r\n  ACPI tables          : "); put_size_pad(sh, acpi, 10);
    put(sh, L"\r\n  ----------------------------------\r\n  Total RAM            : ");
    put_size_pad(sh, freeB + loader + bootsvc + runtime + acpi, 10);
    put(sh, L"\r\n\r\n  Not counted as RAM   : ");
    put_size_pad(sh, reserved + other, 10);
    put(sh, L"  (reserved and hardware address space)\r\n");
}

static const CHAR16 *const memtype_names[14] = {
    L"Reserved", L"LoaderCode", L"LoaderData", L"BootCode", L"BootData", L"RuntimeCode", L"RuntimeData",
    L"Free", L"Unusable", L"ACPIReclaim", L"ACPINVS", L"MMIO", L"MMIOPorts", L"PalCode" };

static void cmd_memmap(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    EFI_MEMORY_DESCRIPTOR *map; UINTN bytes, ds;
    if (!load_memmap(sh, &map, &bytes, &ds)) return;

    PAGER pg;
    pager_init(&pg, sh);
    put(sh, L"\r\n");
    pager_line(&pg, L"Type         Start                 Pages  Size");
    for (UINTN i = 0; i < bytes / ds; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * ds);
        CHAR16 line[120], tmp[40];
        const CHAR16 *name = (d->Type < 14) ? memtype_names[d->Type] : L"Other";
        str_set(line, 120, name);
        while (str_len(line) < 13) str_cat(line, 120, L" ");
        str_cat(line, 120, L"0x");
        hex_str(d->PhysicalStart, 16, tmp);
        str_cat(line, 120, tmp);
        str_cat(line, 120, L"  ");
        scitoa(d->NumberOfPages, tmp);
        for (int k = str_len(tmp); k < 9; k++) str_cat(line, 120, L" ");
        str_cat(line, 120, tmp);
        str_cat(line, 120, L"  ");
        format_memory_size(d->NumberOfPages * 4096, tmp, 40);
        str_cat(line, 120, tmp);
        if (!pager_line(&pg, line)) break;
    }
    sh->st->BootServices->FreePool(map);
}

// ---- uptime ----------------------------------------------------------------

static long long seconds_of(const EFI_TIME *t) {
    return days_from_civil(t->Year, t->Month, t->Day) * 86400LL + t->Hour * 3600LL + t->Minute * 60LL + t->Second;
}

static void cmd_uptime(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    EFI_TIME now;
    if (!sh->haveStartTime) {
        put_err(sh, L"\r\n[X] The clock was not available when CustomOS started, so the uptime is unknown.\r\n");
        return;
    }
    if (!get_clock(sh, &now)) return;
    long long secs = seconds_of(&now) - seconds_of(&sh->startTime);
    if (secs < 0) {
        put_err(sh, L"\r\n[X] The clock was set back since CustomOS started, so the uptime is unknown.\r\n");
        return;
    }
    put(sh, L"\r\nStarted at ");
    put_num(sh, sh->startTime.Year); put(sh, L"-"); put_2d(sh, sh->startTime.Month); put(sh, L"-"); put_2d(sh, sh->startTime.Day);
    put(sh, L" ");
    put_2d(sh, sh->startTime.Hour); put(sh, L":"); put_2d(sh, sh->startTime.Minute); put(sh, L":"); put_2d(sh, sh->startTime.Second);
    put(sh, L"\r\nRunning for ");
    UINT64 d = (UINT64)(secs / 86400), h = (UINT64)(secs % 86400 / 3600), m = (UINT64)(secs % 3600 / 60), s = (UINT64)(secs % 60);
    if (d) { put_count(sh, d, L"day", L"days"); put(sh, L", "); }
    if (d || h) { put_count(sh, h, L"hour", L"hours"); put(sh, L", "); }
    if (d || h || m) { put_count(sh, m, L"minute", L"minutes"); put(sh, L", "); }
    put_count(sh, s, L"second", L"seconds");
    put(sh, L"\r\n");
}

// ---- cpuinfo ---------------------------------------------------------------

static void cmd_cpuinfo(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    unsigned int a, b, c, d;
    char vendor[13];
    CHAR16 brand[64];

    __cpuid(0, a, b, c, d);
    unsigned int maxLeaf = a;
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;

    get_cpu_brand_string(brand, 64);
    put(sh, L"\r\n  Name        : "); put(sh, brand);
    put(sh, L"\r\n  Vendor      : "); put_ascii(sh, (const UINT8 *)vendor, 12);

    unsigned int ecx1 = 0, edx1 = 0, ebx7 = 0;
    if (maxLeaf >= 1) {
        __cpuid(1, a, b, c, d);
        ecx1 = c; edx1 = d;
        unsigned int family = (a >> 8) & 0xF, model = (a >> 4) & 0xF, stepping = a & 0xF;
        if (family == 0xF) family += (a >> 20) & 0xFF;
        if ((((a >> 8) & 0xF) == 0xF) || (((a >> 8) & 0xF) == 6)) model += ((a >> 16) & 0xF) << 4;
        put(sh, L"\r\n  Family      : "); put_num(sh, family);
        put(sh, L"   Model: "); put_num(sh, model);
        put(sh, L"   Stepping: "); put_num(sh, stepping);
        put(sh, L"\r\n  Logical CPUs: ");
        put_num(sh, (edx1 & (1u << 28)) ? ((b >> 16) & 0xFF) : 1);
        put(sh, L"  (in this package, as reported by CPUID)");
    }
    if (maxLeaf >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        ebx7 = b;
    }
    put(sh, L"\r\n  Features    :");
    static const struct { int reg; unsigned bit; const CHAR16 *name; } feats[] = {
        { 0, 25, L"sse" }, { 0, 26, L"sse2" }, { 1, 0, L"sse3" }, { 1, 9, L"ssse3" }, { 1, 19, L"sse4.1" },
        { 1, 20, L"sse4.2" }, { 1, 23, L"popcnt" }, { 1, 25, L"aes" }, { 1, 28, L"avx" }, { 2, 5, L"avx2" },
        { 1, 21, L"x2apic" }, { 0, 24, L"fxsr" } };
    int shown = 0;
    for (UINTN i = 0; i < sizeof(feats) / sizeof(feats[0]); i++) {
        unsigned int v = feats[i].reg == 0 ? edx1 : feats[i].reg == 1 ? ecx1 : ebx7;
        if (v & (1u << feats[i].bit)) { put(sh, L" "); put(sh, feats[i].name); shown++; }
    }
    if (!shown) put(sh, L" (none reported)");
    put(sh, L"\r\n  Virtual machine: ");
    put(sh, (ecx1 & (1u << 31)) ? L"yes (a hypervisor is present)" : L"no");
    put(sh, L"\r\n");
}

// ---- diskinfo --------------------------------------------------------------

static void cmd_diskinfo(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    EFI_FILE_PROTOCOL *root = 0;
    UINT64 buf[128];
    UINTN size = sizeof buf;
    EFI_GUID g = EFI_FILE_SYSTEM_INFO_GUID;

    if (!open_volume(sh->device, sh->st, &root)) { put_err(sh, L"\r\n[X] Could not open the file system.\r\n"); return; }
    if (root->GetInfo(root, &g, &size, buf) != EFI_SUCCESS) {
        root->Close(root);
        put_err(sh, L"\r\n[X] The file system would not report its size.\r\n");
        return;
    }
    EFI_FILE_SYSTEM_INFO *fs = (EFI_FILE_SYSTEM_INFO *)buf;
    UINT64 total = fs->VolumeSize, freeB = fs->FreeSpace, used = total >= freeB ? total - freeB : 0;

    put(sh, L"\r\n  Volume label : ");
    put(sh, fs->VolumeLabel[0] ? fs->VolumeLabel : L"(none)");
    put(sh, L"\r\n  Total size   : "); put_size(sh, total);
    put(sh, L"  ("); put_num(sh, total); put(sh, L" bytes)");
    put(sh, L"\r\n  Used         : "); put_size(sh, used);
    put(sh, L"\r\n  Free         : "); put_size(sh, freeB);
    if (total > 0) { put(sh, L"  ("); put_num(sh, freeB / (total / 100 ? total / 100 : 1)); put(sh, L"% free)"); }
    put(sh, L"\r\n  Block size   : "); put_num(sh, fs->BlockSize); put(sh, L" bytes");
    put(sh, L"\r\n  Access       : "); put(sh, fs->ReadOnly ? L"read-only" : L"read and write");
    put(sh, L"\r\n");
    root->Close(root);
}

// ---- devices ---------------------------------------------------------------

static void cmd_devices(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    static const struct { EFI_GUID guid; const CHAR16 *name; } kinds[] = {
        { { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }, L"Disks and partitions (block devices)" },
        { { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }, L"File systems" },
        { { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }, L"Graphics outputs" },
        { { 0x2f707ebb, 0x4a1a, 0x11d4, { 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }, L"PCI root bridges" },
        { { 0x4cf5b200, 0x68b8, 0x4ca5, { 0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a } }, L"PCI devices" },
        { { 0x2b2f68d6, 0x0cd2, 0x44cf, { 0x8e, 0x8b, 0xbb, 0xa2, 0x0b, 0x1b, 0x5b, 0x75 } }, L"USB devices" },
        { { 0xa19832b9, 0xac25, 0x11d3, { 0x9a, 0x2d, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }, L"Network interfaces" },
        { { 0xbb25cf6f, 0xf1d4, 0x11d2, { 0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0xfd } }, L"Serial ports" },
        { { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }, L"Mice (relative pointers)" },
        { { 0x8d59d32b, 0xc655, 0x4ae9, { 0x9b, 0x15, 0xf2, 0x59, 0x04, 0x99, 0x2a, 0x43 } }, L"Touch / absolute pointers" } };

    put(sh, L"\r\nDevices the firmware has drivers for:\r\n");
    for (UINTN i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        UINTN n = 0;
        EFI_HANDLE *handles = 0;
        EFI_GUID g = kinds[i].guid;
        if (sh->st->BootServices->LocateHandleBuffer(ByProtocol, &g, 0, &n, &handles) == EFI_SUCCESS) {
            if (handles) sh->st->BootServices->FreePool(handles);
        } else {
            n = 0;
        }
        put(sh, L"  ");
        put_pad(sh, kinds[i].name, 38);
        put_numr(sh, n, 3);
        put(sh, L"\r\n");
    }
}

// ---- pci -------------------------------------------------------------------

#ifdef CUSTOMOS_HOST_TEST
UINT32 host_pci_read32(int bus, int dev, int fn, int off);
static UINT32 pci_read32(int bus, int dev, int fn, int off) { return host_pci_read32(bus, dev, fn, off); }
#else
static inline void port_out32(UINT16 port, UINT32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline UINT32 port_in32(UINT16 port) { UINT32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static UINT32 pci_read32(int bus, int dev, int fn, int off) {
    port_out32(0xCF8, 0x80000000u | ((UINT32)bus << 16) | ((UINT32)dev << 11) | ((UINT32)fn << 8) | ((UINT32)off & 0xFC));
    return port_in32(0xCFC);
}
#endif

static const CHAR16 *pci_vendor_name(UINT16 v) {
    switch (v) {
        case 0x8086: return L"Intel";
        case 0x1234: return L"QEMU";
        case 0x1AF4: return L"Red Hat (virtio)";
        case 0x1B36: return L"Red Hat (QEMU)";
        case 0x1022: return L"AMD";
        case 0x1002: return L"AMD/ATI";
        case 0x10DE: return L"NVIDIA";
        case 0x10EC: return L"Realtek";
        case 0x15AD: return L"VMware";
        case 0x80EE: return L"VirtualBox";
        case 0x106B: return L"Apple";
        case 0x14E4: return L"Broadcom";
        case 0x1B21: return L"ASMedia";
        case 0x1106: return L"VIA";
        default:     return 0;
    }
}

static const CHAR16 *pci_class_name(UINT32 cls) {      // cls = base<<16 | sub<<8 | prog-if
    switch (cls >> 8) {
        case 0x0000: return L"Unclassified device";
        case 0x0100: return L"SCSI controller";
        case 0x0101: return L"IDE controller";
        case 0x0105: return L"ATA controller";
        case 0x0106: return L"SATA controller";
        case 0x0107: return L"SAS controller";
        case 0x0108: return L"NVMe controller";
        case 0x0200: return L"Ethernet controller";
        case 0x0280: return L"Network controller";
        case 0x0300: return L"VGA display controller";
        case 0x0380: return L"Display controller";
        case 0x0401: return L"Audio device";
        case 0x0403: return L"Audio controller";
        case 0x0600: return L"Host bridge";
        case 0x0601: return L"ISA bridge";
        case 0x0604: return L"PCI bridge";
        case 0x0680: return L"Other bridge";
        case 0x0C03: return L"USB controller";
        case 0x0C05: return L"SMBus controller";
    }
    switch (cls >> 16) {
        case 0x01: return L"Storage controller";
        case 0x02: return L"Network controller";
        case 0x03: return L"Display controller";
        case 0x04: return L"Multimedia device";
        case 0x05: return L"Memory controller";
        case 0x06: return L"Bridge";
        case 0x07: return L"Communication controller";
        case 0x08: return L"System peripheral";
        case 0x09: return L"Input device";
        case 0x0C: return L"Serial bus controller";
        default:   return L"Other device";
    }
}

static void cmd_pci(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    PAGER pg;
    UINT64 found = 0;
    pager_init(&pg, sh);
    put(sh, L"\r\n");
    pager_line(&pg, L"Bus:Dev.Fn  Vendor:Device  Maker             Kind");
    for (int bus = 0; bus < 256 && !pg.quit; bus++) {
        for (int dev = 0; dev < 32 && !pg.quit; dev++) {
            UINT32 id0 = pci_read32(bus, dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            int nfn = (((pci_read32(bus, dev, 0, 0x0C) >> 16) & 0x80) != 0) ? 8 : 1;
            for (int fn = 0; fn < nfn; fn++) {
                UINT32 id = (fn == 0) ? id0 : pci_read32(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                UINT32 cls = pci_read32(bus, dev, fn, 8) >> 8;
                CHAR16 line[120], tmp[20];
                hex_str((UINT64)bus, 2, tmp);  str_set(line, 120, tmp);
                str_cat(line, 120, L":");
                hex_str((UINT64)dev, 2, tmp);  str_cat(line, 120, tmp);
                str_cat(line, 120, L".");
                hex_str((UINT64)fn, 1, tmp);   str_cat(line, 120, tmp);
                str_cat(line, 120, L"     ");
                hex_str(id & 0xFFFF, 4, tmp);  str_cat(line, 120, tmp);
                str_cat(line, 120, L":");
                hex_str(id >> 16, 4, tmp);     str_cat(line, 120, tmp);
                str_cat(line, 120, L"    ");
                const CHAR16 *maker = pci_vendor_name((UINT16)(id & 0xFFFF));
                str_cat(line, 120, maker ? maker : L"");
                for (int k = str_len(line); k < 48; k++) str_cat(line, 120, L" ");
                str_cat(line, 120, pci_class_name(cls));
                found++;
                if (!pager_line(&pg, line)) break;
            }
        }
    }
    if (!pg.quit) {
        put(sh, L"\r\n");
        put_count(sh, found, L"PCI device", L"PCI devices");
        put(sh, L" found.\r\n");
    }
}

// ---- acpi ------------------------------------------------------------------

static const void *find_config_table(SHELL *sh, const EFI_GUID *guid) {
    EFI_CONFIGURATION_TABLE *t = sh->st->ConfigurationTable;
    for (UINTN i = 0; t && i < sh->st->NumberOfTableEntries; i++) {
        if (guid_eq(&t[i].VendorGuid, guid)) return t[i].VendorTable;
    }
    return 0;
}

static UINT8 sum_bytes(const UINT8 *p, UINT32 n) {
    UINT8 s = 0;
    for (UINT32 i = 0; i < n; i++) s = (UINT8)(s + p[i]);
    return s;
}

static void put_hex_addr(SHELL *sh, UINT64 a) { put(sh, L"0x"); put_hex(sh, a, 16); }

static void cmd_acpi(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    EFI_GUID g20 = EFI_ACPI_20_TABLE_GUID, g10 = EFI_ACPI_10_TABLE_GUID;
    const UINT8 *rsdp = (const UINT8 *)find_config_table(sh, &g20);
    BOOLEAN v2 = rsdp != 0;
    if (!rsdp) rsdp = (const UINT8 *)find_config_table(sh, &g10);
    if (!rsdp) { put_err(sh, L"\r\n[X] The firmware did not provide ACPI tables.\r\n"); return; }
    if (memcmp_u8(rsdp, "RSD PTR ", 8) != 0) { put_err(sh, L"\r\n[X] The ACPI root pointer is damaged (wrong signature).\r\n"); return; }

    UINT8 rev = rsdp[15];
    UINT64 xsdt = (rev >= 2) ? rd64(rsdp + 24) : 0;
    UINT32 rsdt = rd32(rsdp + 16);
    put(sh, L"\r\nACPI root pointer at "); put_hex_addr(sh, (UINT64)(UINTN)rsdp);
    put(sh, L"  revision "); put_num(sh, rev);
    put(sh, L" ("); put(sh, v2 ? L"ACPI 2.0 or newer" : L"ACPI 1.0"); put(sh, L")  OEM: ");
    put_ascii(sh, rsdp + 9, 6);
    put(sh, L"\r\n");

    const UINT8 *root; BOOLEAN wide;
    if (xsdt) { root = (const UINT8 *)(UINTN)xsdt; wide = TRUE; put(sh, L"Table list (XSDT) at "); put_hex_addr(sh, xsdt); }
    else      { root = (const UINT8 *)(UINTN)rsdt; wide = FALSE; put(sh, L"Table list (RSDT) at "); put_hex_addr(sh, rsdt); }
    put(sh, L"\r\n");
    if (!root || memcmp_u8(root, wide ? "XSDT" : "RSDT", 4) != 0) { put_err(sh, L"[X] The table list is missing or damaged.\r\n"); return; }

    UINT32 len = rd32(root + 4);
    if (len < 36 || len > 0x100000) { put_err(sh, L"[X] The table list has an impossible size.\r\n"); return; }
    UINT32 count = (len - 36) / (wide ? 8 : 4);

    PAGER pg;
    pager_init(&pg, sh);
    pager_line(&pg, L"");
    pager_line(&pg, L"Table  Length  Rev  Checksum  OEM");
    for (UINT32 i = 0; i < count && i < 256; i++) {
        UINT64 addr = wide ? rd64(root + 36 + i * 8) : rd32(root + 36 + i * 4);
        const UINT8 *t = (const UINT8 *)(UINTN)addr;
        CHAR16 line[100], tmp[40];
        if (!t) continue;
        UINT32 tlen = rd32(t + 4);
        int n = 0;
        for (int k = 0; k < 4; k++) line[n++] = (t[k] >= 32 && t[k] < 127) ? t[k] : L'.';
        line[n] = 0;
        str_cat(line, 100, L"   ");
        scitoa(tlen, tmp);
        for (int k = str_len(tmp); k < 6; k++) str_cat(line, 100, L" ");
        str_cat(line, 100, tmp);
        str_cat(line, 100, L"  ");
        scitoa(t[8], tmp);
        for (int k = str_len(tmp); k < 3; k++) str_cat(line, 100, L" ");
        str_cat(line, 100, tmp);
        str_cat(line, 100, L"  ");
        str_cat(line, 100, (tlen >= 36 && tlen <= 0x1000000 && sum_bytes(t, tlen) == 0) ? L"OK       " : L"BAD      ");
        str_cat(line, 100, L"  ");
        n = str_len(line);
        for (int k = 0; k < 6 && t[10 + k]; k++) line[n++] = (t[10 + k] >= 32 && t[10 + k] < 127) ? t[10 + k] : L'.';
        line[n] = 0;
        if (!pager_line(&pg, line)) break;
    }
}

// ---- smbios ----------------------------------------------------------------

// Returns string number `idx` (1-based) from the text area of an SMBIOS structure.
static void smbios_string(const UINT8 *s, const UINT8 *tableEnd, int idx, CHAR16 *out, int max) {
    const UINT8 *p = s + s[1];
    out[0] = 0;
    if (idx <= 0) return;
    for (int cur = 1; p < tableEnd && *p; cur++) {
        if (cur == idx) {
            int n = 0;
            while (p < tableEnd && *p && n < max - 1) { out[n++] = (*p >= 32 && *p < 127) ? *p : L'.'; p++; }
            out[n] = 0;
            return;
        }
        while (p < tableEnd && *p) p++;
        p++;
    }
}

static const UINT8 *smbios_next(const UINT8 *s, const UINT8 *tableEnd) {
    const UINT8 *p = s + s[1];
    while (p + 1 < tableEnd && !(p[0] == 0 && p[1] == 0)) p++;
    return p + 2;
}

static void put_field(SHELL *sh, const CHAR16 *label, const UINT8 *s, const UINT8 *end, int offset) {
    CHAR16 b[80];
    smbios_string(s, end, s[offset], b, 80);
    put(sh, label);
    put(sh, b[0] ? b : L"-");
}

static const CHAR16 *memory_type_name(UINT8 t) {
    switch (t) {
        case 0x12: return L"DDR";
        case 0x13: return L"DDR2";
        case 0x18: return L"DDR3";
        case 0x1A: return L"DDR4";
        case 0x1B: return L"LPDDR";
        case 0x1E: return L"LPDDR4";
        case 0x22: return L"DDR5";
        case 0x23: return L"LPDDR5";
        case 0x0F: return L"SDRAM";
        default:   return L"";
    }
}

static void cmd_smbios(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    EFI_GUID g3 = EFI_SMBIOS3_TABLE_GUID, g2 = EFI_SMBIOS_TABLE_GUID;
    const UINT8 *ep = (const UINT8 *)find_config_table(sh, &g3);
    const UINT8 *table; UINT32 tlen; int major, minor;

    if (ep && memcmp_u8(ep, "_SM3_", 5) == 0) {
        major = ep[7]; minor = ep[8];
        tlen = rd32(ep + 12);
        table = (const UINT8 *)(UINTN)rd64(ep + 16);
    } else {
        ep = (const UINT8 *)find_config_table(sh, &g2);
        if (!ep || memcmp_u8(ep, "_SM_", 4) != 0) {
            put_err(sh, L"\r\n[X] The firmware did not provide SMBIOS information.\r\n");
            return;
        }
        major = ep[6]; minor = ep[7];
        tlen = rd16(ep + 22);
        table = (const UINT8 *)(UINTN)rd32(ep + 24);
    }
    if (!table || tlen < 4 || tlen > 0x100000) { put_err(sh, L"\r\n[X] The SMBIOS table is missing or damaged.\r\n"); return; }
    const UINT8 *end = table + tlen;

    put(sh, L"\r\nSMBIOS "); put_num(sh, (UINT64)major); put(sh, L"."); put_num(sh, (UINT64)minor); put(sh, L"\r\n");
    int dimms = 0, cpus = 0;
    for (const UINT8 *s = table; s + 4 <= end && s[1] >= 4 && s + s[1] <= end && s[0] != 127; s = smbios_next(s, end)) {
        switch (s[0]) {
            case 0:
                if (s[1] >= 9) {
                    put_field(sh, L"  BIOS     : ", s, end, 4); put_field(sh, L", version ", s, end, 5);
                    put_field(sh, L", dated ", s, end, 8); put(sh, L"\r\n");
                }
                break;
            case 1:
                if (s[1] >= 8) {
                    put_field(sh, L"  System   : ", s, end, 4); put_field(sh, L" ", s, end, 5);
                    put_field(sh, L" (version ", s, end, 6); put(sh, L")\r\n");
                }
                break;
            case 2:
                if (s[1] >= 8) {
                    put_field(sh, L"  Board    : ", s, end, 4); put_field(sh, L" ", s, end, 5); put(sh, L"\r\n");
                }
                break;
            case 4:
                if (s[1] >= 0x18 && cpus < 16) {
                    cpus++;
                    put_field(sh, L"  Processor: ", s, end, 0x10);
                    UINT16 mhz = rd16(s + 0x16);
                    if (mhz) { put(sh, L", "); put_num(sh, mhz); put(sh, L" MHz"); }
                    put(sh, L"\r\n");
                }
                break;
            case 17:
                if (s[1] >= 0x15 && dimms < 32) {
                    UINT16 sz = rd16(s + 0x0C);
                    UINT64 bytes = 0;
                    if (sz == 0 || sz == 0xFFFF) break;                    // empty slot / unknown
                    if (sz == 0x7FFF && s[1] >= 0x20) bytes = (UINT64)rd32(s + 0x1C) * 1024 * 1024;
                    else if (sz & 0x8000) bytes = (UINT64)(sz & 0x7FFF) * 1024;
                    else bytes = (UINT64)sz * 1024 * 1024;
                    dimms++;
                    put_field(sh, L"  Memory   : ", s, end, 0x10);
                    put(sh, L": "); put_size(sh, bytes);
                    const CHAR16 *tn = memory_type_name(s[0x12]);
                    if (tn[0]) { put(sh, L" "); put(sh, tn); }
                    if (s[1] >= 0x17) { UINT16 sp = rd16(s + 0x15); if (sp) { put(sh, L" "); put_num(sh, sp); put(sh, L" MT/s"); } }
                    put(sh, L"\r\n");
                }
                break;
        }
    }
}

// ---- bootorder and efivars -------------------------------------------------

static EFI_STATUS get_var(SHELL *sh, const CHAR16 *name, EFI_GUID *g, void *buf, UINTN *size, UINT32 *attr) {
    return sh->st->RuntimeServices->GetVariable((CHAR16 *)name, g, attr, size, buf);
}

static void cmd_bootorder(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    EFI_GUID g = EFI_GLOBAL_VARIABLE_GUID;
    UINT64 store[128];
    UINT16 order[64];
    UINTN size = sizeof order;
    UINT16 current = 0xFFFF;
    UINTN csz = sizeof current;

    if (get_var(sh, L"BootOrder", &g, order, &size, 0) != EFI_SUCCESS || size < 2) {
        put_err(sh, L"\r\n[X] No boot order is stored in the firmware.\r\n");
        return;
    }
    if (get_var(sh, L"BootCurrent", &g, &current, &csz, 0) != EFI_SUCCESS || csz != 2) current = 0xFFFF;

    put(sh, L"\r\nBoot order (the firmware tries these from the top):\r\n");
    for (UINTN i = 0; i < size / 2; i++) {
        CHAR16 name[12], hex[8];
        str_set(name, 12, L"Boot");
        hex_str(order[i], 4, hex);
        str_cat(name, 12, hex);
        UINTN osz = sizeof store;
        put(sh, L"  "); put_num(sh, (UINT64)(i + 1)); put(sh, L". "); put(sh, name); put(sh, L"  ");
        if (get_var(sh, name, &g, store, &osz, 0) == EFI_SUCCESS && osz >= 8) {
            // EFI_LOAD_OPTION: attributes(4), file path list length(2), then the description text.
            const UINT8 *p = (const UINT8 *)store;
            const CHAR16 *desc = (const CHAR16 *)(p + 6);
            UINTN maxChars = (osz - 6) / 2, n = 0;
            while (n < maxChars && desc[n]) n++;
            if (n < maxChars) put(sh, desc); else put(sh, L"(unreadable name)");
        } else {
            put(sh, L"(entry not found)");
        }
        if (order[i] == current) put(sh, L"   <- started from this one");
        put(sh, L"\r\n");
    }
}

static void cmd_efivars(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    CHAR16 name[128];
    EFI_GUID g;
    PAGER pg;
    UINT64 shown = 0, total = 0;

    name[0] = 0;
    memset(&g, 0, sizeof g);
    pager_init(&pg, sh);
    put(sh, L"\r\n");
    pager_line(&pg, L"Name                         Size  Flags     Owner");
    for (;;) {
        UINTN nsz = sizeof name;
        EFI_STATUS st = sh->st->RuntimeServices->GetNextVariableName(&nsz, name, &g);
        if (st != EFI_SUCCESS) break;
        total++;
        if (argc == 2 && !contains_nocase(name, argv[1])) continue;

        UINTN vsz = 0;
        UINT32 attr = 0;
        EFI_STATUS rs = sh->st->RuntimeServices->GetVariable(name, &g, &attr, &vsz, 0);
        CHAR16 line[140], tmp[20];
        str_set(line, 140, name);
        if (str_len(line) > 26) line[26] = 0;
        while (str_len(line) < 26) str_cat(line, 140, L" ");
        scitoa(rs == EFI_BUFFER_TOO_SMALL || rs == EFI_SUCCESS ? vsz : 0, tmp);
        for (int k = str_len(tmp); k < 7; k++) str_cat(line, 140, L" ");
        str_cat(line, 140, tmp);
        str_cat(line, 140, L"  ");
        str_cat(line, 140, (attr & 1) ? L"NV " : L"-- ");
        str_cat(line, 140, (attr & 2) ? L"BS " : L"-- ");
        str_cat(line, 140, (attr & 4) ? L"RT " : L"-- ");
        str_cat(line, 140, L" ");
        hex_str(g.Data1, 8, tmp);
        str_cat(line, 140, tmp);
        shown++;
        if (!pager_line(&pg, line)) break;
    }
    if (!pg.quit) {
        put(sh, L"\r\n");
        put_count(sh, shown, L"variable", L"variables");
        if (argc == 2) { put(sh, L" match (of "); put_num(sh, total); put(sh, L")"); }
        put(sh, L".\r\nFlags: NV kept after power off, BS available during boot, RT available at runtime.\r\n");
    }
}

// ---- screeninfo and textmode -----------------------------------------------

static void cmd_screeninfo(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv; (void)rest;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = sh->st->ConOut;
    UINTN cols = 0, rows = 0;

    put(sh, L"\r\n  Text screen    : ");
    if (out->Mode && out->QueryMode(out, (UINTN)out->Mode->Mode, &cols, &rows) == EFI_SUCCESS) {
        put_num(sh, cols); put(sh, L" columns x "); put_num(sh, rows); put(sh, L" rows");
        put(sh, L"  (mode "); put_num(sh, (UINT64)out->Mode->Mode); put(sh, L" of "); put_num(sh, (UINT64)out->Mode->MaxMode); put(sh, L")");
    } else {
        put(sh, L"unknown");
    }

    EFI_GUID gg = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    put(sh, L"\r\n  Graphics       : ");
    if (sh->st->BootServices->LocateProtocol(&gg, 0, (void **)&gop) != EFI_SUCCESS || !gop || !gop->Mode || !gop->Mode->Info) {
        put(sh, L"not available\r\n");
        return;
    }
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    put_num(sh, info->HorizontalResolution); put(sh, L" x "); put_num(sh, info->VerticalResolution);
    put(sh, L" pixels  (mode "); put_num(sh, gop->Mode->Mode); put(sh, L" of "); put_num(sh, gop->Mode->MaxMode); put(sh, L")");
    put(sh, L"\r\n  Pixel format   : ");
    switch (info->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor: put(sh, L"32-bit, red-green-blue"); break;
        case PixelBlueGreenRedReserved8BitPerColor: put(sh, L"32-bit, blue-green-red"); break;
        case PixelBitMask:                          put(sh, L"custom bit masks"); break;
        case PixelBltOnly:                          put(sh, L"no direct frame buffer (blit only)"); break;
        default:                                    put(sh, L"unknown"); break;
    }
    put(sh, L"\r\n  Frame buffer   : ");
    if (info->PixelFormat == PixelBltOnly || gop->Mode->FrameBufferBase == 0) {
        put(sh, L"none");
    } else {
        put_hex_addr(sh, gop->Mode->FrameBufferBase);
        put(sh, L", "); put_size(sh, gop->Mode->FrameBufferSize);
    }
    put(sh, L"\r\n");
}

static void cmd_textmode(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = sh->st->ConOut;
    UINTN cols, rows;

    if (!out->Mode) { put_err(sh, L"\r\n[X] The firmware does not report its text modes.\r\n"); return; }

    if (argc == 1) {
        put(sh, L"\r\nText modes (textmode [number] switches):\r\n");
        for (INT32 m = 0; m < out->Mode->MaxMode; m++) {
            put(sh, L"  ");
            put_num(sh, (UINT64)m);
            put(sh, L": ");
            if (out->QueryMode(out, (UINTN)m, &cols, &rows) == EFI_SUCCESS) {
                put_num(sh, cols); put(sh, L" x "); put_num(sh, rows);
            } else {
                put(sh, L"(not supported here)");
            }
            if (m == out->Mode->Mode) put(sh, L"   <- current");
            put(sh, L"\r\n");
        }
        return;
    }

    UINT64 want;
    if (!parse_u64(argv[1], &want) || want >= (UINT64)out->Mode->MaxMode || out->QueryMode(out, (UINTN)want, &cols, &rows) != EFI_SUCCESS) {
        put_err(sh, L"\r\n[X] That is not a text mode this screen supports. Type textmode to see the list.\r\n");
        return;
    }
    if (out->SetMode(out, (UINTN)want) != EFI_SUCCESS) {
        put_err(sh, L"\r\n[X] The firmware refused to switch to that mode.\r\n");
        return;
    }
    put(sh, L"\r\nText mode ");
    put_num(sh, want); put(sh, L" is active: "); put_num(sh, cols); put(sh, L" x "); put_num(sh, rows);
    put(sh, L"\r\n");
}

// ============================================================================
// 11. TOOLS: calc, calendar, random, banner, beep, upper, lower, reverse, length
// ============================================================================

// ---- calc ------------------------------------------------------------------
// Whole-number arithmetic: + - * / % ( ) and the bit operators | ^ & << >> ~,
// with the usual precedence. Numbers are decimal or 0x hex; results are 64-bit.

typedef struct {
    const CHAR16 *p;
    const CHAR16 *err;        // first error found (the rest of the parse is abandoned)
    const CHAR16 *errAt;
    int           depth;
} CALC;

static void calc_fail(CALC *c, const CHAR16 *msg) {
    if (!c->err) { c->err = msg; c->errAt = c->p; }
}

static void calc_skip(CALC *c) { while (*c->p == L' ') c->p++; }

static long long calc_binary(CALC *c, int level);

static long long calc_unary(CALC *c) {
    calc_skip(c);
    if (c->err) return 0;
    CHAR16 ch = *c->p;
    if (ch == L'-') {
        c->p++;
        long long v = calc_unary(c);
        if (v == (long long)(1ULL << 63)) { calc_fail(c, L"The result is too large for 64 bits."); return 0; }
        return -v;
    }
    if (ch == L'+') { c->p++; return calc_unary(c); }
    if (ch == L'~') { c->p++; return ~calc_unary(c); }
    if (ch == L'(') {
        if (++c->depth > 32) { calc_fail(c, L"Too many nested parentheses."); return 0; }
        c->p++;
        long long v = calc_binary(c, 0);
        calc_skip(c);
        if (c->err) return 0;
        if (*c->p != L')') { calc_fail(c, L"Missing closing parenthesis at:"); return 0; }
        c->p++;
        c->depth--;
        return v;
    }
    if (ch >= L'0' && ch <= L'9') {
        UINT64 v = 0;
        if (ch == L'0' && (c->p[1] == L'x' || c->p[1] == L'X')) {
            const CHAR16 *start = c->p;
            c->p += 2;
            int digits = 0;
            for (;;) {
                CHAR16 d = *c->p;
                int n;
                if (d >= L'0' && d <= L'9') n = d - L'0';
                else if (d >= L'a' && d <= L'f') n = d - L'a' + 10;
                else if (d >= L'A' && d <= L'F') n = d - L'A' + 10;
                else break;
                if (++digits > 16) { c->p = start; calc_fail(c, L"That number is too large for 64 bits:"); return 0; }
                v = (v << 4) | (UINT64)n;
                c->p++;
            }
            if (digits == 0) { c->p = start; calc_fail(c, L"Not a number:"); return 0; }
            return (long long)v;
        }
        const CHAR16 *start = c->p;
        while (*c->p >= L'0' && *c->p <= L'9') {
            UINT64 d = (UINT64)(*c->p - L'0');
            if (v > (0x7FFFFFFFFFFFFFFFULL - d) / 10) { c->p = start; calc_fail(c, L"That number is too large for 64 bits:"); return 0; }
            v = v * 10 + d;
            c->p++;
        }
        return (long long)v;
    }
    calc_fail(c, *c->p ? L"Expected a number or ( at:" : L"The expression ended too early.");
    return 0;
}

// Reads the operator of this precedence level at c->p. Returns its code or 0.
static int calc_op(CALC *c, int level) {
    calc_skip(c);
    CHAR16 a = c->p[0], b = c->p[1];
    switch (level) {
        case 0: if (a == L'|') { c->p += 1; return '|'; } break;
        case 1: if (a == L'^') { c->p += 1; return '^'; } break;
        case 2: if (a == L'&') { c->p += 1; return '&'; } break;
        case 3: if (a == L'<' && b == L'<') { c->p += 2; return 'l'; }
                if (a == L'>' && b == L'>') { c->p += 2; return 'r'; } break;
        case 4: if (a == L'+') { c->p += 1; return '+'; }
                if (a == L'-') { c->p += 1; return '-'; } break;
        case 5: if (a == L'*') { c->p += 1; return '*'; }
                if (a == L'/') { c->p += 1; return '/'; }
                if (a == L'%') { c->p += 1; return '%'; } break;
    }
    return 0;
}

static long long calc_binary(CALC *c, int level) {
    if (level > 5) return calc_unary(c);
    long long v = calc_binary(c, level + 1);
    int op;
    while (!c->err && (op = calc_op(c, level)) != 0) {
        const CHAR16 *opAt = c->p;
        long long w = calc_binary(c, level + 1);
        if (c->err) return 0;
        long long r = 0;
        switch (op) {
            case '|': r = v | w; break;
            case '^': r = v ^ w; break;
            case '&': r = v & w; break;
            case 'l': case 'r':
                if (w < 0 || w > 63) { c->p = opAt; calc_fail(c, L"Shift by 0 to 63 places only."); return 0; }
                r = (op == 'l') ? (long long)((UINT64)v << w) : (v >> w);
                break;
            case '+': if (__builtin_add_overflow(v, w, &r)) { calc_fail(c, L"The result is too large for 64 bits."); return 0; } break;
            case '-': if (__builtin_sub_overflow(v, w, &r)) { calc_fail(c, L"The result is too large for 64 bits."); return 0; } break;
            case '*': if (__builtin_mul_overflow(v, w, &r)) { calc_fail(c, L"The result is too large for 64 bits."); return 0; } break;
            case '/':
            case '%':
                if (w == 0) { c->p = opAt; calc_fail(c, L"Division by zero."); return 0; }
                if (w == -1) {
                    if (op == '/') { if (v == (long long)(1ULL << 63)) { calc_fail(c, L"The result is too large for 64 bits."); return 0; } r = -v; }
                    else r = 0;
                } else {
                    r = (op == '/') ? v / w : v % w;
                }
                break;
        }
        v = r;
    }
    return v;
}

static void cmd_calc(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv;
    CALC c;
    c.p = rest; c.err = 0; c.errAt = rest; c.depth = 0;
    long long v = calc_binary(&c, 0);
    if (!c.err) {
        calc_skip(&c);
        if (*c.p) calc_fail(&c, L"Not understood at:");
    }
    if (c.err) {
        put_err(sh, L"\r\n[X] ");
        put(sh, c.err);
        if (c.errAt && *c.errAt) { put(sh, L" "); put(sh, c.errAt); }
        put(sh, L"\r\n");
        return;
    }
    put(sh, L"\r\n= ");
    if (v < 0) { put(sh, L"-"); put_num(sh, 0 - (UINT64)v); } else put_num(sh, (UINT64)v);
    CHAR16 hex[20];
    hex_str((UINT64)v, 16, hex);
    const CHAR16 *h = hex;
    while (h[0] == L'0' && h[1]) h++;
    put(sh, L"   (0x"); put(sh, h); put(sh, L")\r\n");
}

// ---- calendar --------------------------------------------------------------

static void cmd_calendar(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    UINT64 month, year;
    if (argc == 1 || argc == 2) {
        EFI_TIME now;
        if (!get_clock(sh, &now)) return;
        year = now.Year;
        month = now.Month;
        if (argc == 2 && !parse_u64(argv[1], &month)) month = 0;
    } else {
        if (!parse_u64(argv[1], &month)) month = 0;
        if (!parse_u64(argv[2], &year)) year = 0;
    }
    if (month < 1 || month > 12 || year < 1 || year > 9999) {
        put_err(sh, L"\r\n[X] Give a month from 1 to 12 and, if you like, a year from 1 to 9999. Example: calendar 12 2026\r\n");
        return;
    }
    int m = (int)month, y = (int)year;
    CHAR16 title[40];
    str_set(title, 40, month_names[m - 1]);
    str_cat(title, 40, L" ");
    CHAR16 yb[16];
    scitoa(year, yb);
    str_cat(title, 40, yb);
    put(sh, L"\r\n");
    for (int i = 0; i < (20 - str_len(title)) / 2; i++) put(sh, L" ");
    put(sh, title);
    put(sh, L"\r\nSu Mo Tu We Th Fr Sa\r\n");
    int first = weekday_of(y, m, 1), days = days_in_month(y, m);
    for (int i = 0; i < first; i++) put(sh, L"   ");
    for (int d = 1; d <= days; d++) {
        put_numr(sh, (UINT64)d, 2);
        if ((first + d) % 7 == 0 || d == days) put(sh, L"\r\n"); else put(sh, L" ");
    }
}

// ---- random ----------------------------------------------------------------

static UINT64 rng_state;

static UINT64 rng_next(void) {
    if (rng_state == 0) {
        unsigned int lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        rng_state = (((UINT64)hi << 32) | lo) ^ 0x9E3779B97F4A7C15ULL;
        if (rng_state == 0) rng_state = 0x9E3779B97F4A7C15ULL;
    }
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static void cmd_random(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    UINT64 lo = 1, hi = 100;
    BOOLEAN ok = TRUE;
    if (argc == 2) ok = parse_u64(argv[1], &hi) && hi >= 1;
    else if (argc == 3) ok = parse_u64(argv[1], &lo) && parse_u64(argv[2], &hi) && lo <= hi;
    if (!ok) {
        put_err(sh, L"\r\n[X] Give one whole number of 1 or more, or two numbers with the lowest first: random [lowest] [highest]\r\n");
        return;
    }
    UINT64 span = hi - lo + 1, r = rng_next();
    if (span != 0) {
        UINT64 threshold = (0 - span) % span;       // throw away the biased part of the range
        while (r < threshold) r = rng_next();
        r = r % span;
    }
    put(sh, L"\r\n");
    put_num(sh, lo + r);
    put(sh, L"\r\n");
}

// ---- banner ----------------------------------------------------------------

static const struct { CHAR16 ch; UINT8 row[5]; } banner_font[] = {
    { L'A', { 0x0E, 0x11, 0x1F, 0x11, 0x11 } }, { L'B', { 0x1E, 0x11, 0x1E, 0x11, 0x1E } },
    { L'C', { 0x0F, 0x10, 0x10, 0x10, 0x0F } }, { L'D', { 0x1E, 0x11, 0x11, 0x11, 0x1E } },
    { L'E', { 0x1F, 0x10, 0x1E, 0x10, 0x1F } }, { L'F', { 0x1F, 0x10, 0x1E, 0x10, 0x10 } },
    { L'G', { 0x0F, 0x10, 0x13, 0x11, 0x0F } }, { L'H', { 0x11, 0x11, 0x1F, 0x11, 0x11 } },
    { L'I', { 0x1F, 0x04, 0x04, 0x04, 0x1F } }, { L'J', { 0x07, 0x02, 0x02, 0x12, 0x0C } },
    { L'K', { 0x11, 0x12, 0x1C, 0x12, 0x11 } }, { L'L', { 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { L'M', { 0x11, 0x1B, 0x15, 0x11, 0x11 } }, { L'N', { 0x11, 0x19, 0x15, 0x13, 0x11 } },
    { L'O', { 0x0E, 0x11, 0x11, 0x11, 0x0E } }, { L'P', { 0x1E, 0x11, 0x1E, 0x10, 0x10 } },
    { L'Q', { 0x0E, 0x11, 0x15, 0x12, 0x0D } }, { L'R', { 0x1E, 0x11, 0x1E, 0x12, 0x11 } },
    { L'S', { 0x0F, 0x10, 0x0E, 0x01, 0x1E } }, { L'T', { 0x1F, 0x04, 0x04, 0x04, 0x04 } },
    { L'U', { 0x11, 0x11, 0x11, 0x11, 0x0E } }, { L'V', { 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { L'W', { 0x11, 0x11, 0x15, 0x1B, 0x11 } }, { L'X', { 0x11, 0x0A, 0x04, 0x0A, 0x11 } },
    { L'Y', { 0x11, 0x0A, 0x04, 0x04, 0x04 } }, { L'Z', { 0x1F, 0x02, 0x04, 0x08, 0x1F } },
    { L'0', { 0x0E, 0x13, 0x15, 0x19, 0x0E } }, { L'1', { 0x04, 0x0C, 0x04, 0x04, 0x0E } },
    { L'2', { 0x0E, 0x11, 0x06, 0x08, 0x1F } }, { L'3', { 0x1E, 0x01, 0x0E, 0x01, 0x1E } },
    { L'4', { 0x12, 0x12, 0x1F, 0x02, 0x02 } }, { L'5', { 0x1F, 0x10, 0x1E, 0x01, 0x1E } },
    { L'6', { 0x0E, 0x10, 0x1E, 0x11, 0x0E } }, { L'7', { 0x1F, 0x01, 0x02, 0x04, 0x04 } },
    { L'8', { 0x0E, 0x11, 0x0E, 0x11, 0x0E } }, { L'9', { 0x0E, 0x11, 0x0F, 0x01, 0x0E } },
    { L' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } }, { L'!', { 0x04, 0x04, 0x04, 0x00, 0x04 } },
    { L'?', { 0x0E, 0x11, 0x06, 0x00, 0x04 } }, { L'.', { 0x00, 0x00, 0x00, 0x00, 0x04 } },
    { L'-', { 0x00, 0x00, 0x1F, 0x00, 0x00 } }, { L':', { 0x00, 0x04, 0x00, 0x04, 0x00 } } };

static const UINT8 *banner_glyph(CHAR16 ch) {
    ch = fold_case(ch);
    for (UINTN i = 0; i < sizeof(banner_font) / sizeof(banner_font[0]); i++) {
        if (banner_font[i].ch == ch) return banner_font[i].row;
    }
    return banner_glyph(L'?');
}

static void cmd_banner(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv;
    UINTN cols = 80, rows = 25;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = sh->st->ConOut;
    if (out->Mode && out->QueryMode(out, (UINTN)out->Mode->Mode, &cols, &rows) != EFI_SUCCESS) cols = 80;
    int len = str_len(rest);
    int maxChars = (int)((cols + 1) / 6);
    if (len > maxChars) {
        put_err(sh, L"\r\n[X] That text is too long for the screen. At most ");
        put_num(sh, (UINT64)maxChars);
        put(sh, L" characters fit.\r\n");
        return;
    }
    put(sh, L"\r\n");
    for (int r = 0; r < 5; r++) {
        CHAR16 line[100];
        int n = 0;
        for (int i = 0; i < len; i++) {
            UINT8 bits = banner_glyph(rest[i])[r];
            for (int b = 4; b >= 0; b--) line[n++] = (bits >> b) & 1 ? L'#' : L' ';
            line[n++] = L' ';
        }
        while (n > 0 && line[n - 1] == L' ') n--;
        line[n] = 0;
        put(sh, line);
        put(sh, L"\r\n");
    }
}

// ---- beep ------------------------------------------------------------------

#ifdef CUSTOMOS_HOST_TEST
void host_port_out8(UINT16 port, UINT8 v);
UINT8 host_port_in8(UINT16 port);
static void port_out8(UINT16 port, UINT8 v) { host_port_out8(port, v); }
static UINT8 port_in8(UINT16 port) { return host_port_in8(port); }
#else
static inline void port_out8(UINT16 port, UINT8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline UINT8 port_in8(UINT16 port) { UINT8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
#endif

static void cmd_beep(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;
    UINT64 freq = 880, ms = 200;
    if ((argc >= 2 && !parse_u64(argv[1], &freq)) || (argc >= 3 && !parse_u64(argv[2], &ms)) ||
        freq < 37 || freq > 20000 || ms < 1 || ms > 5000) {
        put_err(sh, L"\r\n[X] The pitch must be 37 to 20000 Hz and the length 1 to 5000 milliseconds.\r\n");
        return;
    }
    UINT32 divisor = 1193182u / (UINT32)freq;
    port_out8(0x43, 0xB6);                       // timer channel 2, square wave
    port_out8(0x42, (UINT8)(divisor & 0xFF));
    port_out8(0x42, (UINT8)(divisor >> 8));
    UINT8 old = port_in8(0x61);
    port_out8(0x61, (UINT8)(old | 3));           // speaker on
    sh->st->BootServices->Stall((UINTN)ms * 1000);
    port_out8(0x61, (UINT8)(old & 0xFC));        // speaker off
    put(sh, L"\r\nBeep.\r\n");
}

// ---- upper, lower, reverse, length -----------------------------------------

static void cmd_upper(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv;
    CHAR16 b[260];
    int n = 0;
    for (; rest[n] && n < 259; n++) b[n] = (rest[n] >= L'a' && rest[n] <= L'z') ? (CHAR16)(rest[n] - 32) : rest[n];
    b[n] = 0;
    put(sh, L"\r\n"); put(sh, b); put(sh, L"\r\n");
}

static void cmd_lower(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv;
    CHAR16 b[260];
    int n = 0;
    for (; rest[n] && n < 259; n++) b[n] = (rest[n] >= L'A' && rest[n] <= L'Z') ? (CHAR16)(rest[n] + 32) : rest[n];
    b[n] = 0;
    put(sh, L"\r\n"); put(sh, b); put(sh, L"\r\n");
}

static void cmd_reverse(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv;
    CHAR16 b[260];
    int len = str_len(rest);
    if (len > 259) len = 259;
    for (int i = 0; i < len; i++) b[i] = rest[len - 1 - i];
    b[len] = 0;
    put(sh, L"\r\n"); put(sh, b); put(sh, L"\r\n");
}

static void cmd_length(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)argc; (void)argv;
    put(sh, L"\r\n");
    put_count(sh, (UINT64)str_len(rest), L"character", L"characters");
    put(sh, L"\r\n");
}

// ============================================================================
// 7. COMMAND TABLE
//    To add a command: write a cmd_xxx function above and add one line here.
//    `label` is what a wrong call prints as Usage. The first line of `details`
//    is the one-line summary that "help <category>" shows.
//    minArgs/maxArgs count the arguments after the command name (-1 = no limit).
// ============================================================================

static void cmd_help(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest);

#define CAT_FILES  L"Files"
#define CAT_SHELL  L"Shell"
#define CAT_SYSTEM L"System"
#define CAT_TOOLS  L"Tools"

static const CMD commands[] = {
    // ---- Files ----
    { L"cd",         L"cd [folder, dir, or ..]", CAT_FILES,
      L"Changes the current folder. Use .. to go up, or a full path such as \\EFI\\BOOT.\r\nWith no folder it shows the current one.",
      cmd_cd, 0, -1 },
    { L"list",       L"list", CAT_FILES,
      L"Lists the files and folders in the current folder.",
      cmd_list, 0, 0 },
    { L"type",       L"type [filename]", CAT_FILES,
      L"Shows the contents of a text file (the first 4095 bytes).",
      cmd_type, 1, -1 },
    { L"copy",       L"copy [source] [destination]", CAT_FILES,
      L"Copies a file or folder into the destination folder, keeping its name.\r\nIf the destination does not exist yet, it becomes the new name.\r\nExample: copy \\kernel.bin \\EFI",
      cmd_copy, 2, 2 },
    { L"move",       L"move [source] [destination]", CAT_FILES,
      L"Moves a file or folder into the destination folder.\r\nExample: move \\EFI\\kernel.bin \\",
      cmd_move, 2, 2 },
    { L"del",        L"del [file or folder]", CAT_FILES,
      L"Deletes a file, or a folder and everything inside it.\r\nExamples: del testing.txt   or   del \\EFI\\BOOT\\bootx64.efi",
      cmd_del, 1, 1 },
    { L"mkdir",      L"mkdir [folder]", CAT_FILES,
      L"Creates a new folder. The folder that will contain it must already exist.",
      cmd_mkdir, 1, -1 },
    { L"rename",     L"rename [item] [new name]", CAT_FILES,
      L"Gives a file or folder a new name, keeping it in the same folder.\r\nExample: rename old.txt new.txt",
      cmd_rename, 2, 2 },
    { L"touch",      L"touch [file] ...", CAT_FILES,
      L"Creates empty files. Files that already exist are left alone.",
      cmd_touch, 1, -1 },
    { L"more",       L"more [file]", CAT_FILES,
      L"Shows a whole text file one screen at a time.",
      cmd_more, 1, -1 },
    { L"head",       L"head [file] [lines]", CAT_FILES,
      L"Shows the first lines of a text file (10 unless you give a number).",
      cmd_head, 1, 2 },
    { L"tail",       L"tail [file] [lines]", CAT_FILES,
      L"Shows the last lines of a text file (10 unless you give a number).",
      cmd_tail, 1, 2 },
    { L"count",      L"count [file]", CAT_FILES,
      L"Counts the lines, words and bytes in a file.",
      cmd_count, 1, -1 },
    { L"sort",       L"sort [file] [reverse]", CAT_FILES,
      L"Shows the lines of a text file in alphabetical order (add reverse for Z to A).\r\nThe file itself is not changed.",
      cmd_sort, 1, 2 },
    { L"compare",    L"compare [file1] [file2]", CAT_FILES,
      L"Tells you whether two files are identical, and where they first differ.",
      cmd_compare, 2, 2 },
    { L"checksum",   L"checksum [file]", CAT_FILES,
      L"Shows the CRC-32 checksum of a file, to check a copy is intact.",
      cmd_checksum, 1, -1 },
    { L"hexdump",    L"hexdump [file] [offset] [length]", CAT_FILES,
      L"Shows the bytes of a file in hex and as text, 16 per line.\r\nOffset and length are decimal or hex (0x100). Example: hexdump \\kernel.bin 0 256",
      cmd_hexdump, 1, 3 },
    { L"find",       L"find [name] [folder]", CAT_FILES,
      L"Finds files and folders by name in a folder and everything below it.\r\nUse * and ? as wildcards; without them it matches names containing the text.\r\nExamples: find boot   or   find *.efi \\EFI",
      cmd_find, 1, 2 },
    { L"search",     L"search [text] [file]", CAT_FILES,
      L"Shows the lines that contain the text, with their line numbers (case is ignored).\r\nWithout a file it searches every file in the current folder.",
      cmd_search, 1, 2 },
    { L"tree",       L"tree [folder]", CAT_FILES,
      L"Draws the folders and files below a folder as a tree.",
      cmd_tree, 0, -1 },
    { L"write",      L"write [file] [text]", CAT_FILES,
      L"Writes the text as the only line of a file, replacing what was there.\r\nExample: write notes.txt Buy milk",
      cmd_write, 2, -1 },
    { L"append",     L"append [file] [text]", CAT_FILES,
      L"Adds the text as a new line at the end of a file (creating it if needed).",
      cmd_append, 2, -1 },
    { L"attributes", L"attributes [item] [+r -r +h -h +s -s +a -a]", CAT_FILES,
      L"Shows a file's attributes, or changes them: r read-only, h hidden, s system, a archive.\r\nExample: attributes notes.txt +r",
      cmd_attributes, 1, 5 },
    // ---- Shell ----
    { L"color",      L"color [theme]", CAT_SHELL,
      L"Changes the text colour. Themes: green, cyan, red, white.",
      cmd_color, 1, -1 },
    { L"echo",       L"echo [text]", CAT_SHELL,
      L"Prints the text you give it.",
      cmd_echo, 0, -1 },
    { L"clear",      L"clear", CAT_SHELL,
      L"Clears the screen.",
      cmd_clear, 0, 0 },
    { L"help",       L"help [command or group]", CAT_SHELL,
      L"Lists the commands by group. help followed by a command name gives details;\r\nhelp followed by a group name (files, shell, system, tools) lists that group.",
      cmd_help, 0, 1 },
    { L"version",    L"version", CAT_SHELL,
      L"Shows the CustomOS version, build date and the firmware version.",
      cmd_version, 0, 0 },
    { L"date",       L"date [year-month-day]", CAT_SHELL,
      L"Shows today's date. Give a date such as 2026-11-23 to set the clock.",
      cmd_date, 0, 1 },
    { L"time",       L"time [hours:minutes[:seconds]]", CAT_SHELL,
      L"Shows the time. Give a time such as 14:30 to set the clock.",
      cmd_time, 0, 1 },
    { L"pause",      L"pause [message]", CAT_SHELL,
      L"Waits until a key is pressed, showing the message (or Press any key to continue...).",
      cmd_pause, 0, -1 },
    { L"sleep",      L"sleep [seconds]", CAT_SHELL,
      L"Waits for 1 to 3600 seconds. Esc cancels.",
      cmd_sleep, 1, 1 },
    { L"prompt",     L"prompt [text | reset]", CAT_SHELL,
      L"Changes the text shown at the start of the prompt (up to 39 characters).\r\nWith no text it shows the current one; prompt reset restores the default.\r\nExample: prompt MyPC:",
      cmd_prompt, 0, -1 },
    { L"history",    L"history [clear]", CAT_SHELL,
      L"Lists the commands you typed recently (the up arrow recalls them). history clear forgets them.",
      cmd_history, 0, 1 },
    { L"reboot",     L"reboot", CAT_SHELL,
      L"Restarts the computer.",
      cmd_reboot, 0, 0 },
    { L"shutdown",   L"shutdown", CAT_SHELL,
      L"Turns the computer off.",
      cmd_shutdown, 0, 0 },
    { L"about",      L"about", CAT_SHELL,
      L"Shows the CustomOS version and build date.",
      cmd_about, 0, 0 },
    // ---- System ----
    { L"systeminfo", L"systeminfo", CAT_SYSTEM,
      L"Shows the CPU, memory and firmware information.",
      cmd_systeminfo, 0, 0 },
    { L"memory",     L"memory", CAT_SYSTEM,
      L"Shows how the computer's memory is used: free, firmware, ACPI and total RAM.",
      cmd_memory, 0, 0 },
    { L"memmap",     L"memmap", CAT_SYSTEM,
      L"Lists every region of the firmware's memory map (type, address, size).",
      cmd_memmap, 0, 0 },
    { L"uptime",     L"uptime", CAT_SYSTEM,
      L"Shows how long CustomOS has been running.",
      cmd_uptime, 0, 0 },
    { L"cpuinfo",    L"cpuinfo", CAT_SYSTEM,
      L"Shows the processor's name, vendor, model and the features it supports.",
      cmd_cpuinfo, 0, 0 },
    { L"diskinfo",   L"diskinfo", CAT_SYSTEM,
      L"Shows the size, free space and label of the disk CustomOS started from.",
      cmd_diskinfo, 0, 0 },
    { L"devices",    L"devices", CAT_SYSTEM,
      L"Counts the disks, screens, network cards and other devices the firmware found.",
      cmd_devices, 0, 0 },
    { L"pci",        L"pci", CAT_SYSTEM,
      L"Lists the devices on the PCI bus with their vendor and device numbers.",
      cmd_pci, 0, 0 },
    { L"acpi",       L"acpi", CAT_SYSTEM,
      L"Lists the ACPI tables the firmware provides, with a checksum check of each.",
      cmd_acpi, 0, 0 },
    { L"smbios",     L"smbios", CAT_SYSTEM,
      L"Shows what the firmware knows about the machine: BIOS, system, board, processor, memory.",
      cmd_smbios, 0, 0 },
    { L"bootorder",  L"bootorder", CAT_SYSTEM,
      L"Shows the firmware's list of boot options, and which one started this system.",
      cmd_bootorder, 0, 0 },
    { L"efivars",    L"efivars [text]", CAT_SYSTEM,
      L"Lists the firmware's stored variables with size and flags. Give some text to filter by name.",
      cmd_efivars, 0, 1 },
    { L"screeninfo", L"screeninfo", CAT_SYSTEM,
      L"Shows the text screen size and the graphics mode (resolution, frame buffer).",
      cmd_screeninfo, 0, 0 },
    { L"textmode",   L"textmode [number]", CAT_SYSTEM,
      L"Lists the text screen sizes the firmware offers. textmode 1 switches to mode 1.",
      cmd_textmode, 0, 1 },
    // ---- Tools ----
    { L"calc",       L"calc [expression]", CAT_TOOLS,
      L"Calculates with whole numbers: + - * / % ( ) and the bit operators | ^ & << >> ~.\r\nNumbers can be decimal or hex (0x1F). Example: calc (2 + 3) * 0x10",
      cmd_calc, 1, -1 },
    { L"calendar",   L"calendar [month] [year]", CAT_TOOLS,
      L"Shows a calendar for a month. With no numbers it shows the current month.\r\nExample: calendar 12 2026",
      cmd_calendar, 0, 2 },
    { L"random",     L"random [highest]  or  random [lowest] [highest]", CAT_TOOLS,
      L"Picks a random whole number: 1 to 100 by default, or within the range you give.",
      cmd_random, 0, 2 },
    { L"banner",     L"banner [text]", CAT_TOOLS,
      L"Writes the text in big letters (letters, digits, and ! ? . - : only).",
      cmd_banner, 1, -1 },
    { L"beep",       L"beep [pitch in Hz] [milliseconds]", CAT_TOOLS,
      L"Plays a tone on the PC speaker (880 Hz for 200 ms by default). Some machines and\r\nvirtual machines have no speaker.",
      cmd_beep, 0, 2 },
    { L"upper",      L"upper [text]", CAT_TOOLS,
      L"Prints the text in capital letters.",
      cmd_upper, 1, -1 },
    { L"lower",      L"lower [text]", CAT_TOOLS,
      L"Prints the text in small letters.",
      cmd_lower, 1, -1 },
    { L"reverse",    L"reverse [text]", CAT_TOOLS,
      L"Prints the text backwards.",
      cmd_reverse, 1, -1 },
    { L"length",     L"length [text]", CAT_TOOLS,
      L"Counts the characters in the text.",
      cmd_length, 1, -1 }
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

const CMD *find_command(const CHAR16 *name) {
    for (UINTN i = 0; i < COMMAND_COUNT; i++) {
        if (scstrcmp(name, commands[i].name) == 0) return &commands[i];
    }
    return 0;
}

static const CHAR16 *const category_order[] = { CAT_FILES, CAT_SHELL, CAT_SYSTEM, CAT_TOOLS };
#define CATEGORY_COUNT (sizeof(category_order) / sizeof(category_order[0]))

// Prints the first line of `details` (the summary) padded after the name.
static void help_summary_line(CHAR16 *out, int max, const CMD *c) {
    int n = 0;
    for (; c->name[n] && n < max - 1; n++) out[n] = c->name[n];
    while (n < 12 && n < max - 1) out[n++] = L' ';
    for (int i = 0; c->details[i] && c->details[i] != L'\r' && c->details[i] != L'\n' && n < max - 1; i++) out[n++] = c->details[i];
    out[n] = 0;
}

static void cmd_help(SHELL *sh, int argc, CHAR16 **argv, const CHAR16 *rest) {
    (void)rest;

    if (argc == 1) {
        put(sh, L"\r\nAvailable commands:\r\n");
        for (UINTN c = 0; c < CATEGORY_COUNT; c++) {
            BOOLEAN anyHere = FALSE;
            for (UINTN i = 0; i < COMMAND_COUNT; i++) if (scstrcmp(commands[i].category, category_order[c]) == 0) anyHere = TRUE;
            if (!anyHere) continue;
            // Group name, then the command names word-wrapped at 78 columns.
            CHAR16 line[96];
            str_set(line, 96, L"  ");
            str_cat(line, 96, category_order[c]);
            str_cat(line, 96, L":");
            while (str_len(line) < 10) str_cat(line, 96, L" ");
            BOOLEAN first = TRUE;
            for (UINTN i = 0; i < COMMAND_COUNT; i++) {
                if (scstrcmp(commands[i].category, category_order[c]) != 0) continue;
                int need = str_len(commands[i].name) + (first ? 0 : 2);
                if (!first && str_len(line) + need > 78) {
                    str_cat(line, 96, L",");
                    put(sh, line);
                    put(sh, L"\r\n");
                    str_set(line, 96, L"          ");
                    first = TRUE;
                }
                if (!first) str_cat(line, 96, L", ");
                str_cat(line, 96, commands[i].name);
                first = FALSE;
            }
            put(sh, line);
            put(sh, L"\r\n");
        }
        put(sh, L"\r\nhelp [command] shows details, help [group] lists a whole group with summaries.\r\n");
        return;
    }

    const CMD *cmd = find_command(argv[1]);
    if (cmd) {
        put(sh, L"\r\nUsage: ");
        put(sh, cmd->label);
        put(sh, L"\r\n");
        put(sh, cmd->details);
        put(sh, L"\r\n");
        return;
    }

    for (UINTN c = 0; c < CATEGORY_COUNT; c++) {
        if (str_cmp_nocase(argv[1], category_order[c]) != 0) continue;
        PAGER pg;
        pager_init(&pg, sh);
        put(sh, L"\r\n");
        for (UINTN i = 0; i < COMMAND_COUNT; i++) {
            if (scstrcmp(commands[i].category, category_order[c]) != 0) continue;
            CHAR16 line[200];
            help_summary_line(line, 200, &commands[i]);
            if (!pager_line(&pg, line)) break;
        }
        return;
    }

    put_err(sh, L"\r\n[X] No help available. Unknown command: ");
    put(sh, argv[1]);
    put(sh, L"\r\n");
}
