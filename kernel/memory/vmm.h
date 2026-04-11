#ifndef VMM_H
#define VMM_H

#include <boot/boot.h>
#include <klib/klib.h>
#include <memory/pmm.h>

typedef uint64_t virt_addr_t;

void vmm_init(struct boot_info* boot_info);

#endif
