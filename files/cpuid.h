#ifndef CPUID_H
#define CPUID_H

#include <cpuid.h>

static inline void get_cpu_brand_string(CHAR16 *buffer, UINTN maxLen) {
    unsigned int regs[4];
    char cpuString[48] = {0};

    __cpuid(0x80000000, regs[0], regs[1], regs[2], regs[3]);
    if (regs[0] >= 0x80000004) {
        __cpuid(0x80000002, ((unsigned int*)cpuString)[0], ((unsigned int*)cpuString)[1], ((unsigned int*)cpuString)[2], ((unsigned int*)cpuString)[3]);
        __cpuid(0x80000003, ((unsigned int*)cpuString)[4], ((unsigned int*)cpuString)[5], ((unsigned int*)cpuString)[6], ((unsigned int*)cpuString)[7]);
        __cpuid(0x80000004, ((unsigned int*)cpuString)[8], ((unsigned int*)cpuString)[9], ((unsigned int*)cpuString)[10], ((unsigned int*)cpuString)[11]);
        
        for (UINTN i = 0; i < 48 && cpuString[i] != '\0'; i++) {
            buffer[i] = (CHAR16)cpuString[i];
        }
        buffer[47] = 0;
    } else {
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