#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void check_alloc_page(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void) {
    pmm_manager = &best_fit_pmm_manager;
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

//lab2 function1 YOUR CODE
/* page_init - build the Page descriptor array and register the free memory.
 *
 * The physical memory range comes from the device tree.  The kernel is linked
 * at KERNBASE but loaded at KERNEL_BEGIN_PADDR, so a kernel virtual address
 * (kva) always maps to (kva - va_pa_offset).  Only page frames [nbase, npage)
 * can be described by pages[], and the space occupied by OpenSBI, the kernel
 * image and the descriptor array itself must never enter the free area.
 */
static void page_init(void) {
    extern char end[];                  // first byte after the kernel image
    uint64_t mem_base, mem_size, mem_end;
    uintptr_t pages_end, pages_end_pa, free_start_pa;
    size_t nr_page, free_pages, i;

    // the kernel is linked at KERNBASE and loaded at KERNEL_BEGIN_PADDR
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    // physical memory range reported by the device tree
    mem_base = get_memory_base();
    mem_size = get_memory_size();
    if (mem_base == 0 || mem_size == 0) {
        panic("page_init: no valid memory information from DTB");
    }
    mem_end = mem_base + mem_size;
    if (mem_end <= mem_base) {
        panic("page_init: invalid memory range [0x%lx, 0x%lx)", mem_base, mem_end);
    }
    cprintf("physical memory: [0x%016lx, 0x%016lx), %lu MB\n",
            mem_base, mem_end, mem_size / (1024 * 1024));

    // npage is the exclusive upper bound of the manageable page number.
    // A trailing partial page is not manageable, so it is rounded down.
    npage = PPN(mem_end);
    if (npage <= nbase) {
        panic("page_init: no manageable page (npage=0x%lx, nbase=0x%lx)",
              npage, nbase);
    }

    // pages[] is placed right after the kernel image: pages is a virtual
    // address, PADDR(pages) is its physical address, and nr_page descriptors
    // cover the page frames nbase .. npage-1
    pages = (struct Page *)ROUNDUP((uintptr_t)end, PGSIZE);
    nr_page = npage - nbase;
    pages_end = (uintptr_t)pages + nr_page * sizeof(struct Page);
    pages_end_pa = PADDR(pages_end);
    if (pages_end_pa > mem_end) {
        panic("page_init: Page array does not fit in physical memory");
    }

    // reserve every page frame first; init_memmap() releases the free ones
    for (i = 0; i < nr_page; i ++) {
        struct Page *p = pages + i;
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
        SetPageReserved(p);
    }
    cprintf("npage: 0x%lx, pages: 0x%016lx (PA 0x%016lx)\n",
            npage, (uintptr_t)pages, PADDR((uintptr_t)pages));

    // the free area starts at the first complete page after the Page array,
    // so OpenSBI, the kernel image and the array itself stay reserved
    free_start_pa = ROUNDUP(pages_end_pa, PGSIZE);
    if (free_start_pa >= mem_end) {
        cprintf("page_init: no free page available\n");
        return;
    }

    free_pages = npage - PPN(free_start_pa);
    cprintf("free memory: [0x%016lx, 0x%016lx), %lu pages\n",
            free_start_pa, mem_end, free_pages);

    init_memmap(pa2page(free_start_pa), free_pages);
}

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
