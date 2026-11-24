#ifndef MEMORY_H
#define MEMORY_H

#include "efi.h"

// Total RAM the firmware reports in its memory map: free memory plus the memory
// used by the loader, boot/runtime services and ACPI tables. EfiReservedMemoryType
// is deliberately NOT counted: firmware such as OVMF uses it for address-space
// holes (e.g. the 64-bit PCI window) that can be many GB and are not RAM.
// Holes below 4 GB (legacy VGA area, PCI window) are never reported either, so
// this can read a little lower than the RAM installed.
static inline UINT64 get_total_memory_bytes(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    UINTN memoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *memoryMap = 0;
    UINTN mapKey;
    UINTN descriptorSize = 0;
    UINT32 descriptorVersion;

    // First call is expected to fail with BUFFER_TOO_SMALL and report the size.
    status = SystemTable->BootServices->GetMemoryMap(&memoryMapSize, 0, &mapKey, &descriptorSize, &descriptorVersion);
    if (status != EFI_BUFFER_TOO_SMALL || descriptorSize == 0) return 0;

    // AllocatePool itself can add entries to the map, so leave generous slack.
    memoryMapSize += 8 * descriptorSize;

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

        switch (desc->Type) {
            case EfiLoaderCode:
            case EfiLoaderData:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiRuntimeServicesCode:
            case EfiRuntimeServicesData:
            case EfiConventionalMemory:
            case EfiACPIReclaimMemory:
            case EfiACPIMemoryNVS:
                totalBytes += (desc->NumberOfPages * 4096);
                break;
            default:
                break;   // Reserved (firmware address-space holes, e.g. the PCI window),
                         // unusable, MMIO, port space, PAL code: not usable RAM
        }
    }

    SystemTable->BootServices->FreePool(memoryMap);
    return totalBytes;
}

static inline void int_to_str(UINT64 val, CHAR16 *buf, UINTN maxLen) {
    CHAR16 temp[32];
    UINTN i = 0;
    if (val == 0) {
        buf[0] = L'0';
        buf[1] = 0;
        return;
    }
    while (val > 0 && i < 31) {
        temp[i++] = (CHAR16)(L'0' + (val % 10));
        val /= 10;
    }
    UINTN j = 0;
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
