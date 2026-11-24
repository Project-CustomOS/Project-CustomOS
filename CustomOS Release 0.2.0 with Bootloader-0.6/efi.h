#ifndef EFI_H
#define EFI_H

// ============================================================================
// 1. BASE DEFINITIONS, MACROS, AND TYPES
// ============================================================================

typedef unsigned short CHAR16;

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
typedef int INT32;

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

#ifndef EFI_PHYSICAL_ADDRESS_DEFINED
#define EFI_PHYSICAL_ADDRESS_DEFINED
typedef UINT64 EFI_PHYSICAL_ADDRESS;
#endif

#ifndef EFI_ALLOCATE_TYPE_DEFINED
#define EFI_ALLOCATE_TYPE_DEFINED
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;
#endif

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

// Calling convention. UEFI on x86-64 always uses the Microsoft x64 ABI.
// MinGW-w64 already defaults to it, so EFIAPI stays empty there (exactly as
// before). Any other GCC/Clang target gets the explicit attribute so the same
// headers keep working if the toolchain ever changes.
#ifndef EFIAPI
  #if defined(__MINGW32__) || defined(__MINGW64__) || defined(_WIN64)
    #define EFIAPI
  #elif defined(__GNUC__) && defined(__x86_64__)
    #define EFIAPI __attribute__((ms_abi))
  #else
    #define EFIAPI
  #endif
#endif

#define EFI_SUCCESS 0
#define EFI_ERR     0x8000000000000000ULL
#define EFI_UNSUPPORTED       (EFI_ERR | 3)
#define EFI_BUFFER_TOO_SMALL  (EFI_ERR | 5)
#define EFI_NOT_READY         (EFI_ERR | 6)
#define EFI_OUT_OF_RESOURCES  (EFI_ERR | 9)
#define EFI_NOT_FOUND         (EFI_ERR | 14)

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
#define SCAN_ESC       0x17

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

typedef EFI_STATUS (EFIAPI *EFI_TEXT_TEST_STRING) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN ModeNumber,
    UINTN *Columns,
    UINTN *Rows
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_MODE) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN ModeNumber
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION) (
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN Column,
    UINTN Row
);

typedef struct {
    INT32   MaxMode;
    INT32   Mode;
    INT32   Attribute;
    INT32   CursorColumn;
    INT32   CursorRow;
    BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET Reset;
    EFI_TEXT_STRING OutputString;
    EFI_TEXT_TEST_STRING TestString;
    EFI_TEXT_QUERY_MODE QueryMode;
    EFI_TEXT_SET_MODE SetMode;
    EFI_TEXT_SET_ATTRIBUTE SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR EnableCursor;
    EFI_SIMPLE_TEXT_OUTPUT_MODE *Mode;
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

#define EFI_FILE_DIRECTORY   0x0000000000000010ULL

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
    EFI_STATUS (EFIAPI *GetPosition)(struct _EFI_FILE_PROTOCOL *This, UINT64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(struct _EFI_FILE_PROTOCOL *This, UINT64 Position);
    EFI_STATUS (EFIAPI *GetInfo)(struct _EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN *BufferSize, VOID *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(struct _EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN BufferSize, VOID *Buffer);
    EFI_STATUS (EFIAPI *Flush)(struct _EFI_FILE_PROTOCOL *This);
} EFI_FILE_PROTOCOL;

#define EFI_FILE_SYSTEM_INFO_GUID \
    { 0x09576e93, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT64  Size;
    BOOLEAN ReadOnly;
    UINT64  VolumeSize;
    UINT64  FreeSpace;
    UINT32  BlockSize;
    CHAR16  VolumeLabel[1];
} EFI_FILE_SYSTEM_INFO;

#define EFI_FILE_READ_ONLY   0x0000000000000001ULL
#define EFI_FILE_HIDDEN      0x0000000000000002ULL
#define EFI_FILE_SYSTEM      0x0000000000000004ULL
#define EFI_FILE_ARCHIVE     0x0000000000000020ULL

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    CHAR16 FileName[256];
    UINT64 FileSize;
    BOOLEAN IsFolder;
} OS_FILE_ENTRY;

// ============================================================================
// 4b. LOADED IMAGE PROTOCOL (one shared definition for bootloader and kernel)
// ============================================================================
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

typedef struct {
    UINT32           Revision;
    EFI_HANDLE       ParentHandle;
    VOID            *SystemTable;
    EFI_HANDLE       DeviceHandle;
    VOID            *FilePath;
    VOID            *Reserved;
    UINT32           LoadOptionsSize;
    VOID            *LoadOptions;
    VOID            *ImageBase;
    UINT64           ImageSize;
    UINT32           ImageCodeType;
    UINT32           ImageDataType;
    VOID            *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

// ============================================================================
// 5. BOOT AND RUNTIME SERVICES TABLES
// ============================================================================
#define EVT_TIMER        0x80000000
#define TPL_APPLICATION  4

#define TimerCancel      0
#define TimerPeriodic    1
#define TimerRelative    2

typedef VOID (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, VOID *Context);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType,
    UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory,
    UINTN Pages
);

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

typedef EFI_STATUS (EFIAPI *EFI_CREATE_EVENT)(
    UINT32 Type,
    UINTN NotifyTpl,
    EFI_EVENT_NOTIFY NotifyFunction,
    VOID *NotifyContext,
    EFI_EVENT *Event
);

typedef EFI_STATUS (EFIAPI *EFI_SET_TIMER)(
    EFI_EVENT Event,
    UINT32 Type,
    UINT64 TriggerTime
);

typedef EFI_STATUS (EFIAPI *EFI_WAIT_FOR_EVENT)(
    UINTN NumberOfEvents,
    EFI_EVENT *Event,
    UINTN *Index
);

typedef EFI_STATUS (EFIAPI *EFI_CLOSE_EVENT)(
    EFI_EVENT Event
);

typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    UINTN Timeout,
    UINT64 WatchdogCode,
    UINTN DataSize,
    CHAR16 *WatchdogData
);

typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *RaiseTPL;
    VOID *RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages;
    EFI_FREE_PAGES FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;
    EFI_CREATE_EVENT CreateEvent;
    EFI_SET_TIMER SetTimer;
    EFI_WAIT_FOR_EVENT WaitForEvent;
    VOID *SignalEvent;
    EFI_CLOSE_EVENT CloseEvent;
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
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
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
    EFI_STATUS (EFIAPI *GetTime)(EFI_TIME *Time, VOID *Capabilities);
    EFI_STATUS (EFIAPI *SetTime)(EFI_TIME *Time);
    VOID *GetWakeupTime;
    VOID *SetWakeupTime;
    VOID *SetVirtualAddressMap;
    VOID *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 *Attributes, UINTN *DataSize, VOID *Data);
    EFI_STATUS (EFIAPI *GetNextVariableName)(UINTN *VariableNameSize, CHAR16 *VariableName, EFI_GUID *VendorGuid);
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
// 5b. CONFIGURATION TABLES AND WELL-KNOWN GUIDS
// ============================================================================
typedef struct {
    EFI_GUID VendorGuid;
    VOID    *VendorTable;
} EFI_CONFIGURATION_TABLE;

#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }
#define EFI_SMBIOS_TABLE_GUID \
    { 0xeb9d2d31, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }
#define EFI_SMBIOS3_TABLE_GUID \
    { 0xf2fd1544, 0x9794, 0x4a2c, { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } }
#define EFI_GLOBAL_VARIABLE_GUID \
    { 0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } }

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
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif // EFI_H
