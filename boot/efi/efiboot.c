/* threadboot - the ThreadOS loader for UEFI machines
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
/* What the screen says about itself (its EDID): the active one is what
   the firmware settled on, the discovered one what it read */
static EFI_GUID GUID_EDID_ACTIVE =
    { 0xBD8C1056, 0x9F36, 0x44EC,
      { 0x92, 0xA8, 0xA6, 0x33, 0x7F, 0x81, 0x79, 0x86 } };
static EFI_GUID GUID_EDID_DISCOVERED =
    { 0x1C0C34F6, 0xD380, 0x41FA,
      { 0xA0, 0x49, 0x8A, 0xD0, 0x6C, 0x1A, 0x66, 0xAA } };
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
    put(L"threadboot: ");
    put(what);
    put(L" (status 0x");
    put_num(st, 16);
    put(L")\r\n");
    for (;;)
        __asm__ volatile("hlt");
}

/* Everything the kernel is handed - its files, the table of tags, the
   way in - goes below this. Not four gigabytes, which is what a
   Multiboot 2 address can say, but the 512 MiB the kernel identity-maps
   and manages (PAGING_MANAGED_MAX, kernel/include/paging.h): anything
   above is memory it cannot reach at first. Under OVMF with a few
   hundred megabytes nothing ever landed higher; a laptop with 8 GiB
   gives the highest free pages below whatever limit it is told, and
   with four gigabytes as the limit the initrd lay at 3 GiB - and the
   kernel fell over its own files. */
#define LOW_LIMIT 0x1FFFFFFFull

/* And as LOW as possible, not merely below that line. AllocateMaxAddress
   hands out the highest free pages under the limit - and the kernel
   reaches only as far as the highest FREE memory it is told about, so
   pages the loader took near 512 MiB lie past that, reserved and
   unmapped. GRUB takes memory from the bottom; so does this: the lowest
   stretch of free memory above 16 MiB (below lie the kernel and what
   the firmware likes to keep low) that holds the request. */
static EFI_STATUS alloc_low(UINTN type, UINTN pages, UINT64 *at)
{
    static UINT8 buf[16384];
    UINTN size = sizeof(buf), key, dsize;
    UINT32 dver;
    if (BS->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)buf, &key, &dsize,
                         &dver) == EFI_SUCCESS && dsize) {
        UINT64 best = 0;
        for (UINTN off = 0; off + dsize <= size; off += dsize) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(buf + off);
            if (d->Type != EfiConventionalMemory)
                continue;
            UINT64 start = d->PhysicalStart;
            UINT64 end = start + d->NumberOfPages * 4096;
            if (start < 0x1000000)
                start = 0x1000000;
            if (end > LOW_LIMIT + 1)
                end = LOW_LIMIT + 1;
            if (start + (UINT64)pages * 4096 <= end && (!best || start < best))
                best = start;
        }
        if (best) {
            *at = best;
            if (BS->AllocatePages(AllocateAddress, type, pages, at)
                == EFI_SUCCESS)
                return EFI_SUCCESS;
        }
    }
    *at = LOW_LIMIT;
    return BS->AllocatePages(AllocateMaxAddress, type, pages, at);
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

    /* Low - see alloc_low: what the kernel can reach, rather than
       hoping the firmware happens to allocate there. Loader data, so that the memory map
       hands it to the kernel as something already spoken for. */
    UINT64 where;
    UINTN pages = (UINTN)((size + 4095) / 4096);
    if (alloc_low(EfiLoaderData_Type, pages, &where) != EFI_SUCCESS) {
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
    UINT32 mm_w, mm_h;          /* the panel's size, 0 when unknown */
    int have;
};

typedef struct {
    UINT32 SizeOfEdid;
    UINT8 *Edid;
} EFI_EDID_PROTOCOL;

/* The mode the panel was built for and its size, out of the first
   timing descriptor of its EDID. 0 when the firmware has no EDID to
   give, or it does not hold up. */
static int panel_native(UINT32 *w, UINT32 *h, UINT32 *mm_w, UINT32 *mm_h)
{
    EFI_EDID_PROTOCOL *ep = 0;
    if (BS->LocateProtocol(&GUID_EDID_ACTIVE, 0, (void **)&ep) != EFI_SUCCESS
        || !ep || ep->SizeOfEdid < 128 || !ep->Edid) {
        ep = 0;
        if (BS->LocateProtocol(&GUID_EDID_DISCOVERED, 0, (void **)&ep)
            != EFI_SUCCESS || !ep || ep->SizeOfEdid < 128 || !ep->Edid)
            return 0;
    }
    const UINT8 *e = ep->Edid;
    static const UINT8 magic[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    UINT8 sum = 0;
    for (int i = 0; i < 8; i++)
        if (e[i] != magic[i])
            return 0;
    for (int i = 0; i < 128; i++)
        sum = (UINT8)(sum + e[i]);
    if (sum)
        return 0;
    const UINT8 *d = e + 54;
    if (!(d[0] | d[1]))
        return 0;                         /* not a timing */
    *w = d[2] | ((UINT32)(d[4] & 0xF0) << 4);
    *h = d[5] | ((UINT32)(d[7] & 0xF0) << 4);
    *mm_w = d[12] | ((UINT32)(d[14] & 0xF0) << 4);
    *mm_h = d[13] | ((UINT32)(d[14] & 0x0F) << 8);
    if (*mm_w < 10 || *mm_h < 10) {
        *mm_w = e[21] * 10u;
        *mm_h = e[22] * 10u;
    }
    return *w && *h;
}

static int direct_colour(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i)
{
    return i->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
           i->PixelFormat == PixelBlueGreenRedReserved8BitPerColor;
}

static void find_screen(struct screen *s)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    s->have = 0;
    s->mm_w = s->mm_h = 0;
    if (BS->LocateProtocol(&GUID_GOP, 0, (void **)&gop) != EFI_SUCCESS)
        return;

    /* The panel's own mode, when the screen says which it is and the
       firmware has it on its list. Firmware does not always start in
       it - a logo at 1024x768 on a 1920x1080 panel is common - and any
       other mode the panel scales, which is a soft picture. Without an
       EDID, whatever the firmware is already showing: on a laptop that
       is usually the panel's own resolution anyway. */
    UINT32 nw, nh, mw, mh;
    if (panel_native(&nw, &nh, &mw, &mh)) {
        s->mm_w = mw;
        s->mm_h = mh;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *cur = gop->Mode->Info;
        if (cur->HorizontalResolution != nw || cur->VerticalResolution != nh) {
            for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
                EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
                UINTN size;
                if (gop->QueryMode(gop, m, &size, &mi) != EFI_SUCCESS)
                    continue;
                int fits = mi->HorizontalResolution == nw &&
                           mi->VerticalResolution == nh && direct_colour(mi);
                BS->FreePool(mi);
                if (fits) {
                    gop->SetMode(gop, m);
                    break;
                }
            }
        }
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
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
#define TAG_PANEL        0x4C4E4150u   /* ours: the panel's size, "PANL" */

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
 * address does not change when paging goes away underneath it - but
 * only if that address is below four gigabytes, which is where 32-bit
 * code has to run. Under OVMF this loader always was; a laptop's
 * firmware (InsydeH2O on a Lenovo with 8 GiB) loads it higher, and the
 * far jump then went to the address with its top cut off - into
 * nothing, a fault with nowhere to go, and the machine starting over.
 * So these bytes are COPIED into a page below four gigabytes and run
 * from there (move_trampoline): everything in them is relative to
 * where they are, except the two absolute addresses the copy patches -
 * the far pointer and the table's base.
 *
 * And PCIDE comes off first: with it set, clearing paging in long mode
 * is a general protection fault.
 */
__asm__(
".globl enter_kernel\n"
".globl tramp_end\n"
".globl tramp_far\n"
".globl tramp_prot32\n"
".globl tramp_gdt\n"
".globl tramp_gdt_base\n"
".align 16\n"
"enter_kernel:\n"                  /* rcx = entry, rdx = magic, r8 = mbi */
"    cli\n"
"    movl %ecx, %edi\n"            /* where we are going */
"    movl %edx, %esi\n"            /* the magic word */
"    movl %r8d, %ebp\n"            /* the table */
"    movq %cr4, %rax\n"
"    btrq $17, %rax\n"             /* PCIDE off */
"    movq %rax, %cr4\n"
"    lgdt gdt32_desc(%rip)\n"
"    ljmpl *far_ptr(%rip)\n"       /* into a 32-bit code segment */
".code32\n"
"tramp_prot32:\n"
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
"tramp_gdt:\n"
"    .quad 0\n"
"    .quad 0x00CF9A000000FFFF\n"   /* 32-bit code, whole address space */
"    .quad 0x00CF92000000FFFF\n"   /* 32-bit data */
"gdt32_desc:\n"
"    .word 23\n"
"tramp_gdt_base:\n"
"    .quad tramp_gdt\n"
"tramp_far:\n"
"far_ptr:\n"
"    .long tramp_prot32\n"
"    .word 8\n"
"tramp_end:\n"
);

void enter_kernel(UINT64 entry, UINT64 magic, UINT64 table);
extern char tramp_end[], tramp_far[], tramp_prot32[], tramp_gdt[],
            tramp_gdt_base[];

typedef void (*enter_fn)(UINT64 entry, UINT64 magic, UINT64 table);

/* The trampoline, copied into a page below four gigabytes (see above).
   Loader code in the memory map, so the kernel leaves the page alone. */
static enter_fn move_trampoline(void)
{
    UINT64 page;
    if (alloc_low(EfiLoaderCode, 1, &page) != EFI_SUCCESS)
        fail(L"no memory below 4 GiB for the way into the kernel", 0);
    const char *from = (const char *)enter_kernel;
    UINTN n = (UINTN)(tramp_end - from);
    if (n > 4096)
        fail(L"the way into the kernel does not fit a page", n);
    UINT8 *to = (UINT8 *)(UINTN)page;
    copy_bytes(to, from, n);
    *(UINT32 *)(to + (tramp_far - from)) =
        (UINT32)(page + (UINTN)(tramp_prot32 - from));
    *(UINT64 *)(to + (tramp_gdt_base - from)) =
        page + (UINTN)(tramp_gdt - from);
    return (enter_fn)(UINTN)page;
}

/* --- the way in ------------------------------------------------------ */

/* --- Adding the installed system to the firmware's boot menu ----------
 *
 * The installer writes \EFI\ThreadOS\BOOTX64.EFI onto the disk's EFI
 * partition, and where nobody else holds \EFI\BOOT\BOOTX64.EFI the
 * firmware finds it by itself. On a machine with Windows somebody does,
 * and then the firmware has to be TOLD: a boot entry is a variable,
 * Boot####, holding a name and a device path, and BootOrder lists the
 * entries. Plenty of firmware (Lenovo's InsydeH2O, for one) has no
 * screen for adding one by hand - so this does it.
 *
 * It runs as a mode of this loader, not as part of the kernel: setting
 * a variable is one call while the firmware's boot services are still
 * there, and something the kernel, which leaves them behind, could
 * only do by calling into the firmware from its own page tables. GRUB
 * starts it from the disc ("chainloader /boot/threadboot.efi register",
 * grub64.cfg); any loader passing the word "register" does.
 *
 * What it adds goes at the END of BootOrder: the machine keeps booting
 * what it booted before, and ThreadOS is one choice in its boot menu.
 * Run twice, it rewrites its own entry rather than adding a second.
 */

static EFI_GUID GUID_DEVICE_PATH =
    { 0x09576E91, 0x6D3F, 0x11D2,
      { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static EFI_GUID GUID_GLOBAL_VARIABLE =
    { 0x8BE4DF61, 0x93CA, 0x11D2,
      { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };

static const CHAR16 INSTALLED[] = L"\\EFI\\ThreadOS\\BOOTX64.EFI";
static const CHAR16 ENTRY_NAME[] = L"ThreadOS";

static UINTN wlen(const CHAR16 *s)
{
    UINTN n = 0;
    while (s[n])
        n++;
    return n;
}

static int has_word(const CHAR16 *s, UINTN chars, const CHAR16 *w)
{
    UINTN n = wlen(w);
    for (UINTN i = 0; i + n <= chars; i++) {
        UINTN k = 0;
        while (k < n && s[i + k] == w[k])
            k++;
        if (k == n)
            return 1;
    }
    return 0;
}

static UINTN path_len(const EFI_DEVICE_PATH *p)
{
    const UINT8 *at = (const UINT8 *)p;
    for (;;) {
        const EFI_DEVICE_PATH *n = (const EFI_DEVICE_PATH *)at;
        UINTN len = n->Length[0] | (n->Length[1] << 8);
        if (n->Type == 0x7F && n->SubType == 0xFF)
            return (UINTN)(at - (const UINT8 *)p);   /* without the end */
        if (len < 4)
            return 0;
        at += len;
    }
}

/* "Boot0003": the variable's name for entry number n */
static void boot_name(CHAR16 *out, UINT16 n)
{
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    const CHAR16 *b = L"Boot";
    for (int i = 0; i < 4; i++)
        out[i] = b[i];
    for (int i = 0; i < 4; i++)
        out[4 + i] = hex[(n >> (12 - 4 * i)) & 0xF];
    out[8] = 0;
}

/* Whether entry n is ours: its description is our name */
static int entry_is_ours(UINT16 n, UINT8 *buf, UINTN room)
{
    CHAR16 name[9];
    boot_name(name, n);
    UINTN size = room;
    UINT32 attrs;
    if (ST->RuntimeServices->GetVariable(name, &GUID_GLOBAL_VARIABLE, &attrs,
                                         &size, buf) != EFI_SUCCESS)
        return 0;
    if (size < 6 + sizeof(ENTRY_NAME))
        return 0;
    const CHAR16 *desc = (const CHAR16 *)(buf + 6);
    for (UINTN i = 0; i < sizeof(ENTRY_NAME) / 2; i++)
        if (desc[i] != ENTRY_NAME[i])
            return 0;
    return 1;
}

static void pause_and_return(void)
{
    BS->Stall(8 * 1000 * 1000);
}

static EFI_STATUS register_installed(EFI_HANDLE from)
{
    EFI_RUNTIME_SERVICES *RT = ST->RuntimeServices;
    put(L"threadboot: looking for ThreadOS installed on a disk\r\n");

    /* Every filesystem the firmware has mounted, except the one we are
       running from: the one holding \EFI\ThreadOS\BOOTX64.EFI is the
       installed system */
    UINTN count = 0;
    EFI_HANDLE *handles = 0;
    if (BS->LocateHandleBuffer(ByProtocol, &GUID_SIMPLE_FS, 0, &count,
                               &handles) != EFI_SUCCESS)
        count = 0;
    EFI_HANDLE found = 0;
    for (UINTN i = 0; i < count && !found; i++) {
        if (handles[i] == from)
            continue;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
        EFI_FILE_PROTOCOL *vol, *f;
        if (BS->HandleProtocol(handles[i], &GUID_SIMPLE_FS,
                               (void **)&fs) != EFI_SUCCESS ||
            fs->OpenVolume(fs, &vol) != EFI_SUCCESS)
            continue;
        if (vol->Open(vol, &f, (CHAR16 *)INSTALLED, EFI_FILE_MODE_READ, 0)
            == EFI_SUCCESS) {
            f->Close(f);
            found = handles[i];
        }
        vol->Close(vol);
    }
    if (!found) {
        put(L"threadboot: no disk here has \\EFI\\ThreadOS\\BOOTX64.EFI -\r\n"
            L"            install ThreadOS first\r\n");
        pause_and_return();
        return EFI_NOT_FOUND;
    }

    EFI_DEVICE_PATH *dev;
    EFI_STATUS st = BS->HandleProtocol(found, &GUID_DEVICE_PATH,
                                       (void **)&dev);
    UINTN dlen = st == EFI_SUCCESS ? path_len(dev) : 0;

    /* From the partition on, not from the PCI root: HD(n,GPT,guid)
       names the partition by its GUID, and firmware finds it wherever
       the disk happens to be plugged in - which the full path (this
       controller, this port) does not survive. It is the form
       efibootmgr writes too. */
    for (UINTN off = 0; off < dlen; ) {
        EFI_DEVICE_PATH *n = (EFI_DEVICE_PATH *)((UINT8 *)dev + off);
        UINTN len = n->Length[0] | (n->Length[1] << 8);
        if (n->Type == 4 && n->SubType == 1) {   /* a hard drive partition */
            dev = n;
            dlen -= off;
            break;
        }
        off += len;
    }
    if (!dlen) {
        put(L"threadboot: the firmware gives no path to that disk\r\n");
        pause_and_return();
        return EFI_NOT_FOUND;
    }

    /* The entry: attributes, the path's length, the name, the path -
       the disk's own path, then the file on it, then the end */
    UINTN fname = sizeof(INSTALLED);             /* with its zero */
    UINTN flen = 4 + fname;
    UINTN plen = dlen + flen + 4;
    UINTN size = 4 + 2 + sizeof(ENTRY_NAME) + plen;
    UINT8 *opt;
    if (BS->AllocatePool(EfiLoaderData_Type, size, (void **)&opt)
        != EFI_SUCCESS)
        fail(L"no memory for the boot entry", 0);
    *(UINT32 *)opt = 1;                          /* LOAD_OPTION_ACTIVE */
    *(UINT16 *)(opt + 4) = (UINT16)plen;
    copy_bytes(opt + 6, ENTRY_NAME, sizeof(ENTRY_NAME));
    UINT8 *p = opt + 6 + sizeof(ENTRY_NAME);
    copy_bytes(p, dev, dlen);
    p += dlen;
    p[0] = 4;                                    /* a media path, */
    p[1] = 4;                                    /* a file in it */
    p[2] = (UINT8)flen;
    p[3] = (UINT8)(flen >> 8);
    copy_bytes(p + 4, INSTALLED, fname);
    p += flen;
    p[0] = 0x7F;
    p[1] = 0xFF;
    p[2] = 4;
    p[3] = 0;

    /* The order there is now */
    UINT16 order[128];
    UINTN osize = sizeof(order);
    UINT32 oattrs;
    if (RT->GetVariable(L"BootOrder", &GUID_GLOBAL_VARIABLE, &oattrs,
                        &osize, order) != EFI_SUCCESS)
        osize = 0;
    UINTN n_order = osize / 2;

    /* Our own entry if there is one, else the first free number */
    static UINT8 scratch[4096];
    int num = -1;
    for (UINTN i = 0; i < n_order && num < 0; i++)
        if (entry_is_ours(order[i], scratch, sizeof(scratch)))
            num = order[i];
    for (UINT16 n = 0; num < 0 && n < 0xFFFF; n++) {
        CHAR16 name[9];
        boot_name(name, n);
        UINTN sz = sizeof(scratch);
        UINT32 a;
        if (RT->GetVariable(name, &GUID_GLOBAL_VARIABLE, &a, &sz, scratch)
            == EFI_NOT_FOUND)
            num = n;
    }

    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE |
                   EFI_VARIABLE_BOOTSERVICE_ACCESS |
                   EFI_VARIABLE_RUNTIME_ACCESS;
    CHAR16 name[9];
    boot_name(name, (UINT16)num);
    st = RT->SetVariable(name, &GUID_GLOBAL_VARIABLE, attrs, size, opt);
    if (st != EFI_SUCCESS) {
        put(L"threadboot: the firmware would not store ");
        put(name);
        put(L" (status 0x");
        put_num(st, 16);
        put(L")\r\n");
        pause_and_return();
        return st;
    }

    int listed = 0;
    for (UINTN i = 0; i < n_order; i++)
        if (order[i] == (UINT16)num)
            listed = 1;
    if (!listed && n_order < sizeof(order) / 2) {
        order[n_order++] = (UINT16)num;
        st = RT->SetVariable(L"BootOrder", &GUID_GLOBAL_VARIABLE, attrs,
                             n_order * 2, order);
        if (st != EFI_SUCCESS) {
            put(L"threadboot: the firmware would not change BootOrder"
                L" (status 0x");
            put_num(st, 16);
            put(L")\r\n");
            pause_and_return();
            return st;
        }
    }

    put(L"threadboot: added ");
    put(name);
    put(L" \"ThreadOS\" to the boot menu (last in BootOrder).\r\n"
        L"            Take the stick out, restart, and choose ThreadOS\r\n"
        L"            in the firmware's boot menu (F12 on most laptops).\r\n");
    pause_and_return();
    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st;
    BS = st->BootServices;

    put(L"threadboot: ThreadOS\r\n");

    /* Started to put the installed system into the boot menu rather
       than to boot - see register_installed */
    EFI_LOADED_IMAGE_PROTOCOL *me;
    if (BS->HandleProtocol(image, &GUID_LOADED_IMAGE, (void **)&me)
            == EFI_SUCCESS && me->LoadOptions &&
        has_word((const CHAR16 *)me->LoadOptions, me->LoadOptionsSize / 2,
                 L"register"))
        return register_installed(me->DeviceHandle);

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
    if (!read_file(L"\\threados64.bin", &kernel) &&
        !read_file(L"\\EFI\\BOOT\\threados64.bin", &kernel) &&
        !read_file(L"\\EFI\\ThreadOS\\threados64.bin", &kernel))
        fail(L"I found no threados64.bin", 0);
    if (!read_file(L"\\initrd64.img", &initrd) &&
        !read_file(L"\\EFI\\BOOT\\initrd64.img", &initrd) &&
        !read_file(L"\\EFI\\ThreadOS\\initrd64.img", &initrd))
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
        !read_file(L"\\EFI\\ThreadOS\\BOOTX64.EFI", &self))
        self.data = 0;

    /* Where the firmware put this loader: above four gigabytes is the
       case move_trampoline exists for, and on a photo of a screen that
       did not boot this is the first thing worth knowing */
    put(L"  at:     0x");
    put_num((UINT64)(UINTN)efi_main, 16);
    put(L"\r\n  kernel: ");
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
    UINT64 where;                         /* EBX holds this - so, low */
    UINTN mbi_pages = 32;                 /* 128 KiB - a large memory map */
    if (alloc_low(EfiLoaderData_Type, mbi_pages, &where) != EFI_SUCCESS)
        fail(L"no memory for the kernel's table", 0);
    mbi = (UINT8 *)(UINTN)where;
    mbi_room = mbi_pages * 4096;
    mbi_at = 8;                           /* total_size and a reserved word */

    tag_string(TAG_BOOTLOADER, "threadboot");
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

    if (sc.have && sc.mm_w && sc.mm_h) {
        UINT8 *p = tag_open(TAG_PANEL, 8 + 8);
        *(UINT32 *)(p + 8) = sc.mm_w;
        *(UINT32 *)(p + 12) = sc.mm_h;
    }

    if (rsdp) {
        /* The whole thing is copied in, because after ExitBootServices
           the firmware's own tables are not promised to stay put. */
        UINTN n = acpi2 ? 36 : 20;
        UINT8 *p = tag_open(acpi2 ? TAG_RSDP_NEW : TAG_RSDP_OLD, 8 + n);
        copy_bytes(p + 8, rsdp, n);
    }

    UINT64 entry = load_elf(&kernel);

    enter_fn go = move_trampoline();

    /* --- the memory map, and then no more firmware ----------------- */
    /* Said BEFORE the map is taken: printing may allocate, a changed
       map makes its key stale, and ExitBootServices refuses a stale
       key. OVMF never minded; real firmware does. */
    put(L"threadboot: handing the machine to the kernel\r\n");

    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    map_size += 16 * desc_size;           /* room for what our own call adds */
    UINTN map_room = map_size;

    UINT64 map_at = 0;
    if (BS->AllocatePages(AllocateAnyPages, EfiLoaderData_Type,
                          (map_size + 4095) / 4096, &map_at) != EFI_SUCCESS)
        fail(L"no memory for the memory map", 0);
    EFI_MEMORY_DESCRIPTOR *map = (EFI_MEMORY_DESCRIPTOR *)(UINTN)map_at;
    UINTN tags_before_map = mbi_at;

    /* The map, the kernel's copy of it, and the hand-over - again if the
       firmware says the map moved in between (it may, and then only
       GetMemoryMap may still be called: nothing here prints any more) */
    for (int attempt = 0;; attempt++) {
        map_size = map_room;
        EFI_STATUS st2 = BS->GetMemoryMap(&map_size, map, &map_key,
                                          &desc_size, &desc_ver);
        if (st2 != EFI_SUCCESS) {
            if (!attempt)
                fail(L"I did not get a memory map", st2);
            for (;;)
                __asm__ volatile("cli; hlt");
        }
        mbi_at = tags_before_map;

        /* The firmware itself, for the kernel to call later: the system
           table, and the memory map as the firmware wrote it - its
           runtime regions are what has to stay reachable
           (kernel64/efirt.c). GRUB passes the same two tags. */
        {
            UINT8 *p = tag_open(12, 8 + 8);         /* EFI 64-bit system table */
            *(UINT64 *)(p + 8) = (UINT64)(UINTN)ST;
            p = tag_open(17, 8 + 8 + map_size);      /* the EFI memory map */
            *(UINT32 *)(p + 8) = (UINT32)desc_size;
            *(UINT32 *)(p + 12) = desc_ver;
            copy_bytes(p + 16, map, map_size);
        }

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

        if (BS->ExitBootServices(image, map_key) == EFI_SUCCESS)
            break;
        if (attempt >= 8)
            for (;;)
                __asm__ volatile("cli; hlt");
    }

    go(entry, MB2_MAGIC, (UINT64)(UINTN)mbi);
    for (;;)
        __asm__ volatile("hlt");
}
