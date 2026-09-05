/* The bits of UEFI this loader actually uses, declared here
 *
 * Not gnu-efi, not EDK2: those are large, and what a loader needs of
 * UEFI is a handful of function tables whose layout is fixed by the
 * specification. Writing them out is a few hundred lines and it means
 * the loader has no dependency at all - which is the point.
 *
 * Two things matter and both are easy to get wrong:
 *
 * The ORDER of the members. Firmware hands over a table of function
 * pointers and we index into it by position; a member left out shifts
 * everything after it and the machine jumps into the wrong function.
 * So every entry of the boot services table is here, in order, even
 * the ones we never call - as void pointers where the shape does not
 * matter.
 *
 * The CALLING CONVENTION. UEFI uses the Microsoft one, which is not
 * the System V one the rest of this project is compiled with. That is
 * why this file is built by clang targeting x86_64-unknown-windows -
 * then it is the compiler's default and nothing here has to say it.
 */

#ifndef PROTONOS_EFI_H
#define PROTONOS_EFI_H

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef long long          INT64;
typedef UINT64             UINTN;
typedef UINT16             CHAR16;
typedef UINT64             EFI_STATUS;
typedef void              *EFI_HANDLE;

#define EFI_SUCCESS         0
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL

typedef struct { UINT32 a; UINT16 b, c; UINT8 d[8]; } EFI_GUID;

/* --- text output ---------------------------------------------------- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *,
                               CHAR16 *);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* --- memory --------------------------------------------------------- */
typedef struct {
    UINT32 Type;
    UINT32 Pad;
    UINT64 PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* The types of memory the firmware reports. Only the ones that decide
   whether a region is ours to use once boot services are gone. */
#define EfiLoaderCode          1
#define EfiLoaderData          2
#define EfiBootServicesCode    3
#define EfiBootServicesData    4
#define EfiConventionalMemory  7

#define AllocateAnyPages   0
#define AllocateMaxAddress 1
#define AllocateAddress    2
#define EfiLoaderData_Type 2

/* --- boot services -------------------------------------------------- */
/* Every entry, in order. The ones we do not call are void pointers -
   what matters is that they take up their place. */
typedef struct {
    char Hdr[24];
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (*AllocatePages)(UINTN type, UINTN memtype, UINTN pages,
                                UINT64 *memory);
    EFI_STATUS (*FreePages)(UINT64 memory, UINTN pages);
    EFI_STATUS (*GetMemoryMap)(UINTN *size, EFI_MEMORY_DESCRIPTOR *map,
                               UINTN *key, UINTN *descsize,
                               UINT32 *descver);
    EFI_STATUS (*AllocatePool)(UINTN type, UINTN size, void **buf);
    EFI_STATUS (*FreePool)(void *buf);
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE handle, EFI_GUID *guid,
                                 void **iface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE image, UINTN key);
    void *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN microseconds);
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (*LocateProtocol)(EFI_GUID *guid, void *registration,
                                 void **iface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* --- the system table ----------------------------------------------- */
typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    char Hdr[24];
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    UINT32 Pad;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* --- files ----------------------------------------------------------- */
#define EFI_FILE_MODE_READ 1

typedef struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (*Open)(struct EFI_FILE_PROTOCOL *self,
                       struct EFI_FILE_PROTOCOL **new_handle,
                       CHAR16 *name, UINT64 mode, UINT64 attributes);
    EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL *self);
    void *Delete;
    EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL *self, UINTN *size,
                       void *buffer);
    void *Write;
    EFI_STATUS (*GetPosition)(struct EFI_FILE_PROTOCOL *self, UINT64 *pos);
    EFI_STATUS (*SetPosition)(struct EFI_FILE_PROTOCOL *self, UINT64 pos);
    void *GetInfo;
    void *SetInfo;
    void *Flush;
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,
                             EFI_FILE_PROTOCOL **root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    UINT32 Revision;
    UINT32 Pad;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    UINT32 Pad2;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* --- the screen ------------------------------------------------------ */
typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    UINT32 PixelInformation[4];
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

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
                            UINT32 mode, UINTN *size,
                            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info);
    EFI_STATUS (*SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
                          UINT32 mode);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* The pixel formats worth telling apart: red first or blue first. The
   others (a bit mask, or no framebuffer at all) we refuse. */
#define PixelRedGreenBlueReserved8BitPerColor 0
#define PixelBlueGreenRedReserved8BitPerColor 1

#endif
