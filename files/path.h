#ifndef PATH_H
#define PATH_H

#include "efi.h"

// Joins `base` (the current directory) and `target` into a clean absolute
// path: backslash separators only, "." removed, ".." resolved, no trailing
// slash, and "\" for the root. An absolute `target` (leading / or \) ignores
// `base`. Returns FALSE if the result would not fit in `maxLen` CHAR16s.
static inline BOOLEAN normalize_path(const CHAR16 *base, const CHAR16 *target, CHAR16 *out, int maxLen) {
    CHAR16 tmp[512];
    int t = 0;
    BOOLEAN absolute = (target[0] == L'/' || target[0] == L'\\');

    if (!absolute) {
        for (int i = 0; base[i] && t < 255; i++) tmp[t++] = base[i];
        tmp[t++] = L'\\';
    }
    for (int i = 0; target[i]; i++) {
        if (t >= 510) return FALSE;
        tmp[t++] = (target[i] == L'/') ? L'\\' : target[i];
    }
    tmp[t] = 0;

    int len = 0;
    int i = 0;
    out[0] = 0;

    while (tmp[i]) {
        while (tmp[i] == L'\\') i++;
        if (!tmp[i]) break;

        int start = i;
        while (tmp[i] && tmp[i] != L'\\') i++;
        int clen = i - start;

        if (clen == 1 && tmp[start] == L'.') continue;

        if (clen == 2 && tmp[start] == L'.' && tmp[start + 1] == L'.') {
            while (len > 0 && out[len - 1] != L'\\') len--;
            if (len > 0) len--;            // drop the separator too
            out[len] = 0;
            continue;
        }

        if (len + 1 + clen >= maxLen) return FALSE;
        out[len++] = L'\\';
        for (int k = 0; k < clen; k++) out[len++] = tmp[start + k];
        out[len] = 0;
    }

    if (len == 0) {
        out[0] = L'\\';
        out[1] = 0;
    }
    return TRUE;
}

#endif
