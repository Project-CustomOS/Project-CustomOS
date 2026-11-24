#ifndef CPU_INFO_H
#define CPU_INFO_H

// Renamed from cpuid.h: a project header with the same name as GCC's own
// <cpuid.h> breaks as soon as the project directory is added with -I.

#include "efi.h"
#include <cpuid.h>

// Fills `buffer` (at most maxLen CHAR16s, always NUL-terminated) with the CPU
// brand string, e.g. "QEMU Virtual CPU version 2.5+".
static inline void get_cpu_brand_string(CHAR16 *buffer, UINTN maxLen) {
    unsigned int regs[4];
    unsigned int words[13];            // 12 words of brand string + 1 for a guaranteed NUL
    UINTN out = 0;

    if (maxLen == 0) return;
    buffer[0] = 0;

    for (int k = 0; k < 13; k++) words[k] = 0;

    __cpuid(0x80000000, regs[0], regs[1], regs[2], regs[3]);
    if (regs[0] >= 0x80000004) {
        __cpuid(0x80000002, words[0], words[1],  words[2],  words[3]);
        __cpuid(0x80000003, words[4], words[5],  words[6],  words[7]);
        __cpuid(0x80000004, words[8], words[9],  words[10], words[11]);

        const char *brand = (const char *)words;
        UINTN i = 0;

        // Some CPUs pad the start of the string with spaces.
        while (brand[i] == ' ') i++;

        for (; brand[i] != '\0' && out < maxLen - 1; i++) {
            buffer[out++] = (CHAR16)(unsigned char)brand[i];
        }
        buffer[out] = 0;
    }

    if (out == 0) {
        const CHAR16 *fallback = L"x86_64 Compatible Processor";
        UINTN i = 0;
        while (fallback[i] && i < maxLen - 1) {
            buffer[i] = fallback[i];
            i++;
        }
        buffer[i] = 0;
    }
}

#endif
