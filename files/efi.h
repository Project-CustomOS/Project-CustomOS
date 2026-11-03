#ifndef EFI_H
#define EFI_H

// ============================================================================
// 1. BASE DEFINITIONS, MACROS, AND TYPES
// ============================================================================

typedef unsigned short CHAR16;

#ifndef EFI_FILE_INFO_DEFINED
#define EFI_FILE_INFO_DEFINED

#ifndef EFI_TIME_DEFINED
#define EFI_TIME_DEFINED
typedef struct {
    unsigned short Year;
    unsigned char  Month;
    unsigned char  Day;
    unsigned char  Hour;
    unsigned char  Minute;
    unsigned char  Second;
    unsigned char  Pad1;
    unsigned int   Nanosecond;
    short          TimeZone;
    unsigned char  Daylight;
    unsigned char  Pad2;
} EFI_TIME;
#endif

typedef struct {
    unsigned long long Size;
    unsigned long long FileSize;
    unsigned long long PhysicalSize;
    EFI_TIME           CreateTime;
    EFI_TIME           LastAccessTime;
    EFI_TIME           ModificationTime;
    unsigned long long Attribute;
    CHAR16             FileName[1];
} EFI_FILE_INFO;

#endif

#ifndef VOID
typedef void VOID;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;

typedef char CHAR8;

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64) || defined(__aarch64__)
typedef signed long long INTN;
typedef unsigned long long UINTN;
#else
typedef signed long INTN;
typedef unsigned long UINTN;
#endif

typedef UINT8 BOOLEAN;
#define TRUE  1
#define FALSE 0

typedef UINTN EFI_STATUS;
typedef VOID* EFI_HANDLE;
typedef VOID* EFI_EVENT;

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    UINT64 PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryIO,
    EfiMemoryPortSpace,
    EfiPalCode
} EFI_MEMORY_TYPE;

// Set to match your environment's compilation strategy
#ifndef EFIAPI
#define EFIAPI
#endif

#define EFI_SUCCESS 0
#define EFI_ERR     0x8000000000000000ULL
#define EFI_UNSUPPORTED (EFI_ERR | 3)
#define EFI_NOT_FOUND   (EFI_ERR | 14)

// Reset Types for ResetSystem
#define EfiResetCold     0
#define EfiResetWarm     1
#define EfiResetShutdown 2

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

// Forward structure declarations
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct _EFI_FILE_PROTOCOL;
struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;

// ============================================================================
// 2. CONSOLE INPUT / OUTPUT PROTOCOLS
// ============================================================================
typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

#define SCAN_NULL      0x00
#define SCAN_UP        0x01
#define SCAN_DOWN      0x02
#define SCAN_RIGHT     0x03
#define SCAN_LEFT      0x04
#define SCAN_HOME      0x05
#define SCAN_END       0x06

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    BOOLEAN ExtendedVerification
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN Attribute
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    BOOLEAN Visible
);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET Reset;
    EFI_TEXT_STRING OutputString;
    VOID *TestString;
    VOID *QueryMode;
    VOID *SetMode;
    EFI_TEXT_SET_ATTRIBUTE SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
    VOID *SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR EnableCursor;
    VOID *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET_INPUT) (
    struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    BOOLEAN ExtendedVerification
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_READ_KEY_STROKE) (
    struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    EFI_INPUT_KEY *Key
);

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_TEXT_RESET_INPUT Reset;
    EFI_TEXT_READ_KEY_STROKE ReadKeyStroke;
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef enum {
    AllHandles,
    ByRegisterNotify,
    ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    EFI_LOCATE_SEARCH_TYPE SearchType,
    EFI_GUID *Protocol,
    VOID *SearchKey,
    UINTN *NoHandles,
    EFI_HANDLE **Buffer
);

// ============================================================================
// 3. GRAPHICS OUTPUT PROTOCOL (GOP)
// ============================================================================
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    UINT64 FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef EFI_STATUS (EFIAPI *EFI_GOP_QUERY_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber,
    UINTN *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);

typedef EFI_STATUS (EFIAPI *EFI_GOP_SET_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber
);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GOP_QUERY_MODE QueryMode;
    EFI_GOP_SET_MODE   SetMode;
    VOID*              Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

// ============================================================================
// 4. FILE SYSTEM PROTOCOLS & STRUCTURES
// ============================================================================
#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct _EFI_FILE_PROTOCOL *This, struct _EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(struct _EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(struct _EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(struct _EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
    EFI_STATUS (EFIAPI *Write)(struct _EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
    VOID *GetPosition;
    VOID *SetPosition;
    VOID *GetInfo;
    VOID *SetInfo;
    VOID *Flush;
} EFI_FILE_PROTOCOL;

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

// Unified OS File Entry structure used across bootloader and kernel
typedef struct {
    CHAR16 FileName[256];
    UINT64 FileSize;
    BOOLEAN IsFolder;
} OS_FILE_ENTRY;

// ============================================================================
// 5. BOOT AND RUNTIME SERVICES TABLES
// ============================================================================
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol,
    VOID *Registration,
    VOID **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    UINT32 *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    UINT32 PoolType,
    UINTN Size,
    VOID **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    VOID *Buffer
);

typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *RaiseTPL;
    VOID *RestoreTPL;
    VOID *AllocatePages;
    VOID *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;
    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;
    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;
    VOID *LoadImage;
    VOID *StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    VOID *ExitBootServices;
    VOID *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    VOID *SetWatchdogTimer;
    VOID *ConnectController;
    VOID *DisconnectController;
	VOID *OpenProtocol;
    VOID *CloseProtocol;
    VOID *OpenProtocolInformation;
    VOID *ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *GetTime;
    VOID *SetTime;
    VOID *GetWakeupTime;
    VOID *SetWakeupTime;
    VOID *SetVirtualAddressMap;
    VOID *ConvertPointer;
    VOID *GetVariable;
    VOID *GetNextVariableName;
    VOID *SetVariable;
    VOID *GetNextHighMonotonicCount;
    EFI_STATUS (EFIAPI *ResetSystem)(
        UINTN ResetType,
        EFI_STATUS Status,
        UINTN DataSize,
        VOID *ResetData
    );
} EFI_RUNTIME_SERVICES;

// ============================================================================
// 6. MAIN SYSTEM TABLE
// ============================================================================
typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    VOID *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif // EFI_H