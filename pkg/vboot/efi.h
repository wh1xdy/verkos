/* efi.h — a minimal, from-scratch subset of the UEFI ABI for vboot.
 *
 * Just the types + protocols vboot needs — no gnu-efi, no vendor headers. On
 * aarch64 UEFI uses the ordinary AAPCS64 calling convention (unlike x86_64,
 * which needs ms_abi), so plain C function pointers match the firmware ABI.
 */
#ifndef VERK_EFI_H
#define VERK_EFI_H
#include <stdint.h>

typedef uint64_t  UINTN;
typedef int64_t   INTN;
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef uint16_t  CHAR16;
typedef uint8_t   BOOLEAN;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef UINTN     EFI_STATUS;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;
typedef UINT64    EFI_LBA;

#define EFIAPI                              /* aarch64: standard AAPCS, no attr */
#define IN
#define OUT
#define EFIERR(n)      (0x8000000000000000ULL | (n))
#define EFI_SUCCESS            0
#define EFI_LOAD_ERROR        EFIERR(1)
#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_UNSUPPORTED       EFIERR(3)
#define EFI_BUFFER_TOO_SMALL  EFIERR(5)
#define EFI_NOT_FOUND         EFIERR(14)
#define EFI_ERROR(s)   (((INTN)(s)) < 0)

typedef struct { UINT32 Data1; UINT16 Data2, Data3; UINT8 Data4[8]; } EFI_GUID;

typedef struct {
    UINT64 Signature; UINT32 Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

/* ---- Simple Text Output ------------------------------------------------- */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, BOOLEAN);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, CHAR16*);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SETATTR)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*);
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET   Reset;
    EFI_TEXT_STRING  OutputString;
    EFI_TEXT_STRING  TestString;
    void            *QueryMode;
    void            *SetMode;
    EFI_TEXT_SETATTR SetAttribute;
    EFI_TEXT_CLEAR   ClearScreen;
    EFI_STATUS (EFIAPI *SetCursorPosition)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN, UINTN);
    EFI_STATUS (EFIAPI *EnableCursor)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, BOOLEAN);
    void            *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* text attributes: foreground | background << 4 */
#define EFI_BLACK      0x00
#define EFI_LIGHTGRAY  0x07
#define EFI_DARKGRAY   0x08
#define EFI_CYAN       0x03
#define EFI_LIGHTCYAN  0x0B
#define EFI_MAGENTA    0x05
#define EFI_WHITE      0x0F
#define EFI_BG(c)      ((c) << 4)

/* ---- Simple Text Input --------------------------------------------------- */
typedef struct { UINT16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
#define SCAN_UP     0x01
#define SCAN_DOWN   0x02
#define SCAN_ESC    0x17
struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, EFI_INPUT_KEY*);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* ---- Boot Services (only the members vboot uses; order matters!) --------- */
typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;
typedef enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData } EFI_MEMORY_TYPE;

typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    /* task priority */
    void *RaiseTPL; void *RestoreTPL;
    /* memory */
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS*);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    void *GetMemoryMap;
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, void**);
    EFI_STATUS (EFIAPI *FreePool)(void*);
    /* event & timer */
    void *CreateEvent; void *SetTimer; void *WaitForEvent; void *SignalEvent;
    void *CloseEvent; void *CheckEvent;
    /* protocol handlers */
    void *InstallProtocolInterface; void *ReinstallProtocolInterface; void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID*, void**);
    void *Reserved; void *RegisterProtocolNotify;
    EFI_STATUS (EFIAPI *LocateHandle)(UINTN, EFI_GUID*, void*, UINTN*, EFI_HANDLE*);
    void *LocateDevicePath; void *InstallConfigurationTable;
    /* image */
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE, void*, void*, UINTN, EFI_HANDLE*);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN*, CHAR16**);
    EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE, EFI_STATUS, UINTN, CHAR16*);
    void *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);
    /* misc */
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN);
    void *SetWatchdogTimer;
    /* driver support / open protocol */
    void *ConnectController; void *DisconnectController;
    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE, EFI_GUID*, void**, EFI_HANDLE, EFI_HANDLE, UINT32);
    void *CloseProtocol; void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(UINTN, EFI_GUID*, void*, UINTN*, EFI_HANDLE**);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID*, void*, void**);
    /* ... remainder unused */
} EFI_BOOT_SERVICES;

/* ---- Loaded Image ------------------------------------------------------- */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1,0x9562,0x11d2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B} }
typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;        /* the volume we were loaded from */
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;         /* kernel command line goes here ... */
    void *LoadOptions;              /* ... as a CHAR16 string */
    void *ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType, ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- Simple File System + File ------------------------------------------ */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b} }
#define EFI_FILE_MODE_READ  0x0000000000000001ULL

struct _EFI_FILE_PROTOCOL;
typedef struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct _EFI_FILE_PROTOCOL*, struct _EFI_FILE_PROTOCOL**, CHAR16*, UINT64, UINT64);
    EFI_STATUS (EFIAPI *Close)(struct _EFI_FILE_PROTOCOL*);
    EFI_STATUS (EFIAPI *Delete)(struct _EFI_FILE_PROTOCOL*);
    EFI_STATUS (EFIAPI *Read)(struct _EFI_FILE_PROTOCOL*, UINTN*, void*);
    EFI_STATUS (EFIAPI *Write)(struct _EFI_FILE_PROTOCOL*, UINTN*, void*);
    EFI_STATUS (EFIAPI *GetPosition)(struct _EFI_FILE_PROTOCOL*, UINT64*);
    EFI_STATUS (EFIAPI *SetPosition)(struct _EFI_FILE_PROTOCOL*, UINT64);
    EFI_STATUS (EFIAPI *GetInfo)(struct _EFI_FILE_PROTOCOL*, EFI_GUID*, UINTN*, void*);
    EFI_STATUS (EFIAPI *SetInfo)(struct _EFI_FILE_PROTOCOL*, EFI_GUID*, UINTN, void*);
    EFI_STATUS (EFIAPI *Flush)(struct _EFI_FILE_PROTOCOL*);
} EFI_FILE_PROTOCOL;

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor; UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle; EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle; EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle; EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries; void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif
