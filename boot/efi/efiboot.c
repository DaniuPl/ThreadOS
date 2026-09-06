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
 *
 * It loads the 64-bit kernel and nothing else. A machine with UEFI is
 * a 64-bit machine; the 32-bit kernel exists so that one set of
 * sources gets compiled twice and the second compilation says what the
 * first one hid, which is a good reason to keep it and not a reason to
 * boot it on a laptop.
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
        fail(L"I do not know where I was started from", st);

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    st = BS->HandleProtocol(li->DeviceHandle, &GUID_SIMPLE_FS, (void **)&fs);
    if (st != EFI_SUCCESS)
        fail(L"this medium has no filesystem", st);

    st = fs->OpenVolume(fs, &root);
    if (st != EFI_SUCCESS)
        fail(L"I cannot open the volume", st);
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

    /* Below four gigabytes, because a Multiboot 2 module is named by a
       thirty-two bit address and there is nowhere to put a larger one.
       AllocateMaxAddress means "at or below this", so the request says
       what the format can express rather than hoping the firmware
       happens to allocate low. Loader data, so that the memory map
       hands it to the kernel as something already spoken for. */
    UINT64 where = 0xFFFFFFFFull;
    UINTN pages = (UINTN)((size + 4095) / 4096);
    if (BS->AllocatePages(AllocateMaxAddress, EfiLoaderData_Type, pages,
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

/* --- the table the kernel is told everything through ------------------
 *
 * Multiboot 2: a length, then tags one after another, each padded up to
 * eight bytes, ending with a tag of type zero. The kernel already knows
 * how to read this - GRUB was handing it one - so building it is the
 * whole of "talking to the kernel" and nothing in the kernel changes.
 */
#define MB2_MAGIC        0x36d76289u
#define TAG_END          0
#define TAG_CMDLINE      1
#define TAG_BOOTLOADER   2
#define TAG_MODULE       3
#define TAG_MMAP         6
#define TAG_FRAMEBUF     8
#define TAG_RSDP_OLD     14
#define TAG_RSDP_NEW     15

static UINT8 *mbi;                 /* the structure being built */
static UINTN mbi_at;               /* how far into it we are */
static UINTN mbi_room;

static void *tag_open(UINT32 type, UINTN size)
{
    if (mbi_at + size + 8 > mbi_room)
        fail(L"the table for the kernel does not fit", 0);
    UINT8 *p = mbi + mbi_at;
    *(UINT32 *)p = type;
    *(UINT32 *)(p + 4) = (UINT32)size;
    mbi_at += (size + 7) & ~(UINTN)7;   /* tags sit on eight bytes */
    return p;
}

/* A module: an address, an end and a name. The name is what the kernel
   tells them apart by - taking them in order would mean the loader and
   the kernel agreeing on a sequence, which is the kind of agreement
   that goes wrong quietly. */
static void tag_module(UINT64 start, UINT64 size, const char *name)
{
    UINTN n = 0;
    while (name[n])
        n++;
    UINT8 *p = (UINT8 *)tag_open(TAG_MODULE, 8 + 4 + 4 + n + 1);
    *(UINT32 *)(p + 8) = (UINT32)start;
    *(UINT32 *)(p + 12) = (UINT32)(start + size);
    for (UINTN i = 0; i <= n; i++)
        p[16 + i] = (UINT8)name[i];
}

static void copy_bytes(void *dst, const void *src, UINTN n)
{
    UINT8 *d = dst;
    const UINT8 *s = src;
    while (n--)
        *d++ = *s++;
}

static void tag_string(UINT32 type, const char *s)
{
    UINTN n = 0;
    while (s[n])
        n++;
    UINT8 *p = tag_open(type, 8 + n + 1);
    copy_bytes(p + 8, s, n + 1);
}

/* --- the kernel image ------------------------------------------------- */

/* Just enough ELF64 to put the segments where they say they go. The
   kernel is an ELF the loader has to place itself: the program headers
   say what to copy, where to, and how much to leave as zeros after it
   (that is the .bss - space the file does not carry). */
struct elf64_hdr {
    UINT8 ident[16];
    UINT16 type, machine;
    UINT32 version;
    UINT64 entry, phoff, shoff;
    UINT32 flags;
    UINT16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    UINT32 type, flags;
    UINT64 offset, vaddr, paddr, filesz, memsz, align;
};

#define PT_LOAD 1

static UINT64 load_elf(struct blob *img)
{
    const UINT8 *base = img->data;
    const struct elf64_hdr *eh = (const struct elf64_hdr *)base;

    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2)
        fail(L"this is not an ELF64", 0);

    for (UINT16 i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)
            (base + eh->phoff + (UINTN)i * eh->phentsize);
        if (ph->type != PT_LOAD || !ph->memsz)
            continue;

        /* The place the segment asks for has to be taken from the
           firmware first, or it may be holding something of its own
           there and we would be writing over it. */
        UINT64 start = ph->paddr & ~(UINT64)4095;
        UINTN pages = (UINTN)((ph->paddr + ph->memsz - start + 4095) / 4096);
        UINT64 got = start;
        if (BS->AllocatePages(AllocateAddress, EfiLoaderData_Type, pages,
                              &got) != EFI_SUCCESS)
            fail(L"I cannot take memory for the kernel", ph->paddr);

        copy_bytes((void *)(UINTN)ph->paddr, base + ph->offset,
                   (UINTN)ph->filesz);
        UINT8 *tail = (UINT8 *)(UINTN)(ph->paddr + ph->filesz);
        for (UINT64 n = ph->memsz - ph->filesz; n; n--)
            *tail++ = 0;
    }
    return eh->entry;
}

/* --- leaving, and the last thing this loader does --------------------- */

/* The jump, written out because C cannot say any of it.
 *
 * A UEFI application runs in long mode; a Multiboot 2 kernel is entered
 * in 32-bit protected mode. Between those two facts sits this routine,
 * and it has to walk back out of long mode by itself: a table of its
 * own, a far jump into a 32-bit segment, paging off, the long mode bit
 * cleared, PAE off. From there it is an ordinary protected mode machine
 * and the kernel can have it.
 *
 * It works because the firmware maps memory one-to-one, so this code's
 * address does not change when paging goes away underneath it.
 */
__asm__(
".globl enter_kernel\n"
".align 16\n"
"enter_kernel:\n"                  /* rcx = entry, rdx = magic, r8 = mbi */
"    cli\n"
"    movl %ecx, %edi\n"            /* where we are going */
"    movl %edx, %esi\n"            /* the magic word */
"    movl %r8d, %ebp\n"            /* the table */
"    lgdt gdt32_desc(%rip)\n"
"    ljmpl *far_ptr(%rip)\n"       /* into a 32-bit code segment */
".code32\n"
"prot32:\n"
"    movl $0x10, %eax\n"
"    movl %eax, %ds\n"
"    movl %eax, %es\n"
"    movl %eax, %fs\n"
"    movl %eax, %gs\n"
"    movl %eax, %ss\n"
"    movl %cr0, %eax\n"            /* paging off - and with it long mode */
"    andl $0x7FFFFFFF, %eax\n"
"    movl %eax, %cr0\n"
"    movl $0xC0000080, %ecx\n"     /* EFER */
"    rdmsr\n"
"    andl $0xFFFFFEFF, %eax\n"     /* LME away */
"    wrmsr\n"
"    movl %cr4, %eax\n"
"    andl $0xFFFFFFDF, %eax\n"     /* PAE away */
"    movl %eax, %cr4\n"
"    movl %esi, %eax\n"            /* the contract: magic in EAX... */
"    movl %ebp, %ebx\n"            /* ...and the table in EBX */
"    jmp *%edi\n"
".code64\n"
".align 8\n"
"gdt32:\n"
"    .quad 0\n"
"    .quad 0x00CF9A000000FFFF\n"   /* 32-bit code, whole address space */
"    .quad 0x00CF92000000FFFF\n"   /* 32-bit data */
"gdt32_desc:\n"
"    .word 23\n"
"    .quad gdt32\n"
"far_ptr:\n"
"    .long prot32\n"
"    .word 8\n"
);

void enter_kernel(UINT64 entry, UINT64 magic, UINT64 table);

/* --- the way in ------------------------------------------------------ */

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st;
    BS = st->BootServices;

    put(L"protoboot: ProtonOS\r\n");

    open_volume(image);

    /* The 64-bit kernel, and only that one.
     *
     * This machine is a UEFI machine, which means it is a 64-bit
     * machine; the 32-bit kernel is a thing this project keeps for its
     * own reasons - one set of sources compiled twice catches what one
     * compilation hides - and not a thing to boot on a laptop. Loading
     * it here would mean an ELF32 loader next to the ELF64 one for a
     * case nobody has, and generality nobody asked for is how a loader
     * becomes hard to trust. GRUB still boots it for anyone who wants
     * that. */
    struct blob kernel, initrd;
    if (!read_file(L"\\protonos64.bin", &kernel) &&
        !read_file(L"\\EFI\\BOOT\\protonos64.bin", &kernel) &&
        !read_file(L"\\EFI\\ProtonOS\\protonos64.bin", &kernel))
        fail(L"I found no protonos64.bin", 0);
    if (!read_file(L"\\initrd64.img", &initrd) &&
        !read_file(L"\\EFI\\BOOT\\initrd64.img", &initrd) &&
        !read_file(L"\\EFI\\ProtonOS\\initrd64.img", &initrd))
        fail(L"I found no initrd64.img", 0);

    /* And the loader itself, as a file.
     *
     * Not because it is needed to boot - it is already running - but
     * because the system that is about to start may be asked to
     * install itself onto another disk, and then these three files are
     * exactly what has to be written there. A system that boots from a
     * stick has them within reach and nowhere else to get them from:
     * the kernel in memory is loaded segments, not the file it came
     * from. Missing is not fatal; it only means installing is not on
     * offer this time. */
    struct blob self;
    self.data = 0;
    self.size = 0;
    if (!read_file(L"\\EFI\\BOOT\\BOOTX64.EFI", &self) &&
        !read_file(L"\\BOOTX64.EFI", &self) &&
        !read_file(L"\\EFI\\ProtonOS\\BOOTX64.EFI", &self))
        self.data = 0;

    put(L"  kernel: ");
    put_num(kernel.size, 10);
    put(L" B\r\n  initrd: ");
    put_num(initrd.size, 10);
    put(L" B\r\n  loader: ");
    if (self.data) {
        put_num(self.size, 10);
        put(L" B\r\n");
    } else {
        put(L"I did not find myself - installing will not be possible\r\n");
    }

    struct screen sc;
    find_screen(&sc);
    if (sc.have) {
        put(L"  screen: ");
        put_num(sc.width, 10);
        put(L"x");
        put_num(sc.height, 10);
        put(L", buffer 0x");
        put_num(sc.base, 16);
        put(L"\r\n");
    } else {
        put(L"  screen: no framebuffer\r\n");
    }

    int acpi2 = 0;
    void *rsdp = find_rsdp(&acpi2);
    put(L"  ACPI:   ");
    if (rsdp) {
        put(L"0x");
        put_num((UINT64)(UINTN)rsdp, 16);
        put(acpi2 ? L" (2.0)\r\n" : L" (1.0)\r\n");
    } else {
        put(L"none\r\n");
    }

    /* --- the table the kernel reads ------------------------------- */
    /* Generously sized and taken now: after the memory map is fetched
       nothing may allocate, or the key that ExitBootServices checks
       goes stale. */
    UINT64 where = 0xFFFFFFFFull;         /* EBX holds this - so, low */
    UINTN mbi_pages = 32;                 /* 128 KiB - a large memory map */
    if (BS->AllocatePages(AllocateMaxAddress, EfiLoaderData_Type, mbi_pages,
                          &where) != EFI_SUCCESS)
        fail(L"no memory for the kernel's table", 0);
    mbi = (UINT8 *)(UINTN)where;
    mbi_room = mbi_pages * 4096;
    mbi_at = 8;                           /* total_size and a reserved word */

    tag_string(TAG_BOOTLOADER, "protoboot");
    tag_string(TAG_CMDLINE, "");

    /* The initrd comes first and is nameless, because that is the
       module the kernel has always taken by position. The two behind
       it are named, and nothing that existed before this looks at
       them. */
    tag_module((UINTN)initrd.data, initrd.size, "");
    tag_module((UINTN)kernel.data, kernel.size, "the kernel");
    if (self.data)
        tag_module((UINTN)self.data, self.size, "loader");

    if (sc.have) {
        UINT8 *p = tag_open(TAG_FRAMEBUF, 8 + 22 + 6);
        *(UINT64 *)(p + 8) = sc.base;
        *(UINT32 *)(p + 16) = sc.pitch_pixels * 4;   /* MB2 wants bytes */
        *(UINT32 *)(p + 20) = sc.width;
        *(UINT32 *)(p + 24) = sc.height;
        p[28] = 32;                       /* bits per pixel */
        p[29] = 1;                        /* direct colours, not text */
        p[30] = 0;
        p[31] = 0;
        p[32] = (UINT8)sc.red_shift;   p[33] = 8;
        p[34] = (UINT8)sc.green_shift; p[35] = 8;
        p[36] = (UINT8)sc.blue_shift;  p[37] = 8;
    }

    if (rsdp) {
        /* The whole thing is copied in, because after ExitBootServices
           the firmware's own tables are not promised to stay put. */
        UINTN n = acpi2 ? 36 : 20;
        UINT8 *p = tag_open(acpi2 ? TAG_RSDP_NEW : TAG_RSDP_OLD, 8 + n);
        copy_bytes(p + 8, rsdp, n);
    }

    UINT64 entry = load_elf(&kernel);

    /* --- the memory map, and then no more firmware ----------------- */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    map_size += 8 * desc_size;            /* room for what our own call adds */

    UINT64 map_at = 0;
    if (BS->AllocatePages(AllocateAnyPages, EfiLoaderData_Type,
                          (map_size + 4095) / 4096, &map_at) != EFI_SUCCESS)
        fail(L"no memory for the memory map", 0);
    EFI_MEMORY_DESCRIPTOR *map = (EFI_MEMORY_DESCRIPTOR *)(UINTN)map_at;

    EFI_STATUS st2 = BS->GetMemoryMap(&map_size, map, &map_key, &desc_size,
                                      &desc_ver);
    if (st2 != EFI_SUCCESS)
        fail(L"I did not get a memory map", st2);

    /* Turning the firmware's map into the kernel's.
     *
     * What matters is which regions are FREE, and the answer is not
     * "everything the firmware is not using": the kernel image, the
     * initrd and this table are all in memory the firmware calls
     * loader memory, and reporting that as free would have the kernel
     * hand out its own initrd as a spare page. So loader memory is
     * reserved, and only genuinely spare memory - and what the boot
     * services stop needing the moment we leave - is available. */
    {
        UINTN entries = map_size / desc_size;
        UINT8 *p = tag_open(TAG_MMAP, 8 + 8 + entries * 24);
        *(UINT32 *)(p + 8) = 24;          /* the size of one entry */
        *(UINT32 *)(p + 12) = 0;          /* version */
        UINT8 *e = p + 16;
        for (UINTN i = 0; i < entries; i++) {
            EFI_MEMORY_DESCRIPTOR *d =
                (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
            UINT32 kind;
            switch (d->Type) {
            case EfiConventionalMemory:
            case EfiBootServicesCode:
            case EfiBootServicesData:
                kind = 1;                 /* free */
                break;
            case 9:  kind = 3; break;     /* ACPI, reclaimable */
            case 10: kind = 4; break;     /* ACPI NVS */
            default: kind = 2; break;     /* taken, ours included */
            }
            *(UINT64 *)(e + 0) = d->PhysicalStart;
            *(UINT64 *)(e + 8) = d->NumberOfPages * 4096;
            *(UINT32 *)(e + 16) = kind;
            *(UINT32 *)(e + 20) = 0;
            e += 24;
        }
    }

    tag_open(TAG_END, 8);
    *(UINT32 *)mbi = (UINT32)mbi_at;
    *(UINT32 *)(mbi + 4) = 0;

    put(L"protoboot: handing the machine to the kernel\r\n");

    if (BS->ExitBootServices(image, map_key) != EFI_SUCCESS) {
        /* The map may have moved under us; take it again and go. */
        map_size = mbi_pages * 0;
        BS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
        if (BS->ExitBootServices(image, map_key) != EFI_SUCCESS)
            fail(L"the firmware will not hand the machine over", 0);
    }

    enter_kernel(entry, MB2_MAGIC, (UINT64)(UINTN)mbi);
    for (;;)
        __asm__ volatile("hlt");
}
