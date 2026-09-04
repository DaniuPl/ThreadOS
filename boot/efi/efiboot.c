/* protoboot - the ProtonOS loader for UEFI machines
 *
 * Until now this system was started by GRUB. GRUB is excellent and it
 * is somebody else's; everything else here is written from nothing, and
 * the thing that puts the kernel in memory was the last piece that was
 * not.
 *
 * What a loader has to do is short to say and long to do: find the
 * kernel and the initrd, find out what the screen is, tell the kernel
 * where the memory is, and jump. The shape of "tell the kernel" is
 * already decided and it is not ours to change - the kernel is a
 * Multiboot 2 image and expects the magic in EAX and a table of tags in
 * EBX. So this loader BUILDS that table, exactly as GRUB does, and the
 * kernel does not learn that anything changed. Not one line of it had
 * to.
 *
 * The last step is the awkward one. A UEFI application runs in long
 * mode; a Multiboot 2 kernel is entered in 32-bit protected mode. So
 * after the firmware is done with, this has to leave long mode on its
 * own - its own tables, paging off - and jump.
 *
 * Reading the kernel off the disk needs no filesystem of ours: the
 * firmware has already mounted the partition it loaded us from, and
 * hands out a file protocol. A FAT of our own will be needed for
 * WRITING one, which is the installer's problem, not this one's.
 */

#include "efi.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

static EFI_GUID GUID_LOADED_IMAGE =
    { 0x5B1B31A1, 0x9562, 0x11d2,
      { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static EFI_GUID GUID_SIMPLE_FS =
    { 0x964E5B22, 0x6459, 0x11D2,
      { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static EFI_GUID GUID_GOP =
    { 0x9042A9DE, 0x23DC, 0x4A38,
      { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } };
static EFI_GUID GUID_ACPI20 =
    { 0x8868E871, 0xE4F1, 0x11D3,
      { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };
static EFI_GUID GUID_ACPI10 =
    { 0xEB9D2D30, 0x2D88, 0x11D3,
      { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

/* --- saying things -------------------------------------------------- */

static void put(const CHAR16 *s)
{
    ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
}

/* A number, because every interesting thing here is one. Written into
   a small buffer backwards, which is how a number is written. */
static void put_num(UINT64 v, int base)
{
    CHAR16 buf[24];
    int n = 0;
    if (!v)
        buf[n++] = '0';
    while (v) {
        int d = (int)(v % (UINT64)base);
        buf[n++] = (CHAR16)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (UINT64)base;
    }
    CHAR16 out[26];
    int k = 0;
    while (n)
        out[k++] = buf[--n];
    out[k] = 0;
    put(out);
}

static void fail(const CHAR16 *what, EFI_STATUS st)
{
    put(L"protoboot: ");
    put(what);
    put(L" (status 0x");
    put_num(st, 16);
    put(L")\r\n");
    for (;;)
        __asm__ volatile("hlt");
}

/* --- reading a file off the partition we were loaded from ------------ */

struct blob {
    void *data;
    UINTN size;
};

static EFI_FILE_PROTOCOL *root;

static void open_volume(EFI_HANDLE image)
{
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_STATUS st = BS->HandleProtocol(image, &GUID_LOADED_IMAGE,
                                       (void **)&li);
    if (st != EFI_SUCCESS)
        fail(L"nie wiem, skad mnie uruchomiono", st);

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    st = BS->HandleProtocol(li->DeviceHandle, &GUID_SIMPLE_FS, (void **)&fs);
    if (st != EFI_SUCCESS)
        fail(L"na tym nosniku nie ma systemu plikow", st);

    st = fs->OpenVolume(fs, &root);
    if (st != EFI_SUCCESS)
        fail(L"nie moge otworzyc woluminu", st);
}

/* The size is asked for by going to the end and asking where that is -
   which avoids EFI_FILE_INFO and the GUID and the buffer that goes
   with it, for an answer that is one number. */
static int read_file(const CHAR16 *name, struct blob *out)
{
    EFI_FILE_PROTOCOL *f;
    if (root->Open(root, &f, (CHAR16 *)name, EFI_FILE_MODE_READ, 0)
        != EFI_SUCCESS)
        return 0;

    UINT64 size = 0;
    if (f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL) != EFI_SUCCESS ||
        f->GetPosition(f, &size) != EFI_SUCCESS ||
        f->SetPosition(f, 0) != EFI_SUCCESS) {
        f->Close(f);
        return 0;
    }

    UINT64 where = 0;
    UINTN pages = (UINTN)((size + 4095) / 4096);
    /* Loader data, so that the memory map hands it to the kernel as
       something already spoken for rather than free. */
    if (BS->AllocatePages(AllocateAnyPages, EfiLoaderData_Type, pages,
                          &where) != EFI_SUCCESS) {
        f->Close(f);
        return 0;
    }

    UINTN left = (UINTN)size;
    if (f->Read(f, &left, (void *)(UINTN)where) != EFI_SUCCESS) {
        f->Close(f);
        return 0;
    }
    f->Close(f);

    out->data = (void *)(UINTN)where;
    out->size = left;
    return 1;
}

/* --- what the screen is ---------------------------------------------- */

struct screen {
    UINT64 base;
    UINT32 width, height, pitch_pixels;
    UINT32 red_shift, green_shift, blue_shift;
    int have;
};

static void find_screen(struct screen *s)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    s->have = 0;
    if (BS->LocateProtocol(&GUID_GOP, 0, (void **)&gop) != EFI_SUCCESS)
        return;

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    /* Whatever the firmware is already showing. On a laptop that is
       the panel's own resolution, which is exactly what we want and
       what asking for a particular mode would throw away. */
    if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
        s->red_shift = 0;
        s->green_shift = 8;
        s->blue_shift = 16;
    } else if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        s->blue_shift = 0;
        s->green_shift = 8;
        s->red_shift = 16;
    } else {
        return;                /* a bit mask, or no framebuffer at all */
    }

    s->base = gop->Mode->FrameBufferBase;
    s->width = info->HorizontalResolution;
    s->height = info->VerticalResolution;
    s->pitch_pixels = info->PixelsPerScanLine;
    s->have = 1;
}

/* --- the table the firmware keeps its own pointers in ---------------- */

static int same_guid(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *p = (const UINT8 *)a, *q = (const UINT8 *)b;
    for (int i = 0; i < 16; i++)
        if (p[i] != q[i])
            return 0;
    return 1;
}

/* The RSDP - where ACPI begins. The kernel finds its tables through
   this and on a UEFI machine there is no other way to it: the old
   search through low memory finds nothing there. */
static void *find_rsdp(int *is_v2)
{
    void *old = 0;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *e = &ST->ConfigurationTable[i];
        if (same_guid(&e->VendorGuid, &GUID_ACPI20)) {
            *is_v2 = 1;
            return e->VendorTable;
        }
        if (same_guid(&e->VendorGuid, &GUID_ACPI10))
            old = e->VendorTable;
    }
    *is_v2 = 0;
    return old;
}

/* --- the way in ------------------------------------------------------ */

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st;
    BS = st->BootServices;

    put(L"protoboot: ProtonOS\r\n");

    open_volume(image);

    struct blob kernel, initrd;
    if (!read_file(L"\\protonos.bin", &kernel) &&
        !read_file(L"\\EFI\\BOOT\\protonos.bin", &kernel))
        fail(L"nie znalazlem protonos.bin", 0);
    if (!read_file(L"\\initrd.img", &initrd) &&
        !read_file(L"\\EFI\\BOOT\\initrd.img", &initrd))
        fail(L"nie znalazlem initrd.img", 0);

    put(L"  jadro:  ");
    put_num(kernel.size, 10);
    put(L" B\r\n  initrd: ");
    put_num(initrd.size, 10);
    put(L" B\r\n");

    struct screen sc;
    find_screen(&sc);
    if (sc.have) {
        put(L"  ekran:  ");
        put_num(sc.width, 10);
        put(L"x");
        put_num(sc.height, 10);
        put(L", bufor 0x");
        put_num(sc.base, 16);
        put(L"\r\n");
    } else {
        put(L"  ekran:  brak bufora obrazu\r\n");
    }

    int acpi2 = 0;
    void *rsdp = find_rsdp(&acpi2);
    put(L"  ACPI:   ");
    if (rsdp) {
        put(L"0x");
        put_num((UINT64)(UINTN)rsdp, 16);
        put(acpi2 ? L" (2.0)\r\n" : L" (1.0)\r\n");
    } else {
        put(L"nie ma\r\n");
    }

    put(L"protoboot: na razie tyle - reszta w nastepnym kroku\r\n");
    for (;;)
        __asm__ volatile("hlt");
}
