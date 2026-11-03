#ifndef MEMORY_H
#define MEMORY_H

#include "efi.h"

static inline UINT64 get_total_memory_bytes(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    UINTN memoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *memoryMap = 0;
    UINTN mapKey;
    UINTN descriptorSize;
    UINT32 descriptorVersion;

    // Get size of memory map
    status = SystemTable->BootServices->GetMemoryMap(&memoryMapSize, 0, &mapKey, &descriptorSize, &descriptorVersion);
    memoryMapSize += 2 * descriptorSize;

    status = SystemTable->BootServices->AllocatePool(EfiLoaderData, memoryMapSize, (void**)&memoryMap);
    if (status != 0) return 0;

    status = SystemTable->BootServices->GetMemoryMap(&memoryMapSize, memoryMap, &mapKey, &descriptorSize, &descriptorVersion);
    if (status != 0) {
        SystemTable->BootServices->FreePool(memoryMap);
        return 0;
    }

    UINT64 totalBytes = 0;
    UINTN entries = memoryMapSize / descriptorSize;

    for (UINTN i = 0; i < entries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR*)((unsigned char*)memoryMap + (i * descriptorSize));
        
        // Sum conventional and loader/boot memory types
        if (desc->Type == EfiConventionalMemory || 
            desc->Type == EfiBootServicesCode || 
            desc->Type == EfiBootServicesData || 
            desc->Type == EfiLoaderCode || 
            desc->Type == EfiLoaderData) {
            totalBytes += (desc->NumberOfPages * 4096);
        }
    }

    SystemTable->BootServices->FreePool(memoryMap);
    return totalBytes;
}

static inline void int_to_str(UINT64 val, CHAR16 *buf, UINTN maxLen) {
    CHAR16 temp[32];
    int i = 0;
    if (val == 0) {
        buf[0] = L'0';
        buf[1] = 0;
        return;
    }
    while (val > 0 && i < 31) {
        temp[i++] = (CHAR16)(L'0' + (val % 10));
        val /= 10;
    }
    int j = 0;
    while (i > 0 && j < maxLen - 1) {
        buf[j++] = temp[--i];
    }
    buf[j] = 0;
}

static inline void format_memory_size(UINT64 bytes, CHAR16 *buf, UINTN maxLen) {
    const CHAR16 *units[] = { L"Bytes", L"KB", L"MB", L"GB", L"TB" };
    int unitIndex = 0;
    UINT64 scaled = bytes;

    while (scaled >= 1024 && unitIndex < 4) {
        scaled /= 1024;
        unitIndex++;
    }

    CHAR16 numBuf[32];
    int_to_str(scaled, numBuf, 32);

    UINTN i = 0;
    for (int k = 0; numBuf[k] != 0 && i < maxLen - 1; k++) {
        buf[i++] = numBuf[k];
    }
    if (i < maxLen - 1) buf[i++] = L' ';

    const CHAR16 *unitStr = units[unitIndex];
    for (int k = 0; unitStr[k] != 0 && i < maxLen - 1; k++) {
        buf[i++] = unitStr[k];
    }
    buf[i] = 0;
}

#endif