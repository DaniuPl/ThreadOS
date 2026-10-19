#ifndef THREADOS_USER_PCIINFO_H
#define THREADOS_USER_PCIINFO_H

#include "uapi.h"

struct pci_info {
    uint32_t bus;
    uint32_t slot;
    uint32_t function;
    uint32_t vendor;
    uint32_t device;
    uint32_t class_code;
};

static inline int sys_pci_info(int index, struct pci_info *out)
{
#ifdef __x86_64__
    return (int)syscall64(SYS64_PCI_INFO, index, (int64_t)(uintptr_t)out);
#else
    int32_t result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(SYS_PCI_INFO), "b"(index), "c"(out)
                     : "memory");
    return result;
#endif
}

#endif
