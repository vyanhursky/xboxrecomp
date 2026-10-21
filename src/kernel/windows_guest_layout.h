/* Private Windows layout allocator. Include after the Win32 declarations.
 *
 * A free RAM view alone says nothing about base + guest MMIO addresses. In
 * particular, a RAM base near 3 GB can put the physical window over the host
 * executable at 0x140000000. Select room for the whole guest layout, then map
 * ALL required windows before loading the XBE or starting polling threads.
 *
 * VirtualFree cannot release a slice of a Win32 reservation. Release the
 * address-space probe in full, and treat placement as a transaction: ordinary
 * fixed-address mappings never replace somebody else's memory. If a competing
 * allocation takes any slice, unwind our views and select another full span.
 * No placeholder API/newer Windows SDK is required.
 */
#ifndef XBOX_WINDOWS_GUEST_LAYOUT_H
#define XBOX_WINDOWS_GUEST_LAYOUT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    void *base, *mirrors[XBOX_NUM_MIRRORS];
    void *contig, *tiled, *nv2a, *mcpx, *flash;
} XboxWindowsLayout;

static void xbox_windows_layout_release(XboxWindowsLayout *p)
{
    int i;
    for (i = 0; i < XBOX_NUM_MIRRORS; ++i)
        if (p->mirrors[i]) UnmapViewOfFile(p->mirrors[i]);
    if (p->tiled) UnmapViewOfFile(p->tiled);
    if (p->contig) UnmapViewOfFile(p->contig);
    if (p->base) UnmapViewOfFile(p->base);
    if (p->nv2a) VirtualFree(p->nv2a, 0, MEM_RELEASE);
    if (p->mcpx) VirtualFree(p->mcpx, 0, MEM_RELEASE);
    if (p->flash) VirtualFree(p->flash, 0, MEM_RELEASE);
    memset(p, 0, sizeof *p);
}

static int xbox_windows_overlap(uint64_t lo, uint64_t hi,
                                uint64_t base, uint64_t size)
{
    return lo < base + size && base < hi;
}

static int xbox_windows_layout_try(XboxWindowsLayout *p, uintptr_t base,
                                   HANDLE ram, HANDLE physical, size_t bytes,
                                   size_t tiled_bytes)
{
    DWORD error;
    int i;
    memset(p, 0, sizeof *p);
    p->base = MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, bytes, (void *)base);
    if (!p->base) goto failed;
    p->contig = MapViewOfFileEx(physical, FILE_MAP_ALL_ACCESS, 0, 0,
                               XBOX_CONTIG_SIZE, (void *)(base + XBOX_CONTIG_BASE));
    if (!p->contig) goto failed;
    p->tiled = MapViewOfFileEx(physical, FILE_MAP_ALL_ACCESS, 0, 0,
                              tiled_bytes, (void *)(base + XBOX_TILED_BASE));
    if (!p->tiled) goto failed;
    p->nv2a = VirtualAlloc((void *)(base + XBOX_NV2A_BASE), XBOX_NV2A_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p->nv2a) goto failed;
    p->mcpx = VirtualAlloc((void *)(base + XBOX_MCPX_BASE), XBOX_MCPX_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p->mcpx) goto failed;
    p->flash = VirtualAlloc((void *)(base + XBOX_FLASH_BASE), XBOX_FLASH_SIZE,
                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p->flash) goto failed;

    for (i = 0; i < XBOX_NUM_MIRRORS; ++i) {
        uint64_t lo = (uint64_t)(i + 1) * bytes, hi = lo + bytes;
        /* Specific hardware windows take precedence over generic RAM wrap.
         * At 128 MB or larger some mirrors cross these guest addresses. */
        if (xbox_windows_overlap(lo, hi, XBOX_CONTIG_BASE, XBOX_CONTIG_SIZE) ||
            xbox_windows_overlap(lo, hi, XBOX_TILED_BASE, tiled_bytes) ||
            xbox_windows_overlap(lo, hi, XBOX_NV2A_BASE, XBOX_NV2A_SIZE) ||
            xbox_windows_overlap(lo, hi, XBOX_MCPX_BASE, XBOX_MCPX_SIZE) ||
            xbox_windows_overlap(lo, hi, XBOX_FLASH_BASE, XBOX_FLASH_SIZE))
            continue;
        p->mirrors[i] = MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0,
                                       bytes, (void *)(base + (uintptr_t)lo));
        if (!p->mirrors[i]) goto failed;
    }
    return 1;

failed:
    error = GetLastError();
    xbox_windows_layout_release(p);
    SetLastError(error);
    return 0;
}

static int xbox_windows_layout_create(XboxWindowsLayout *p, HANDLE ram,
                                      HANDLE physical, size_t bytes,
                                      size_t tiled_bytes)
{
    size_t span = bytes * (size_t)(1 + XBOX_NUM_MIRRORS);
    unsigned attempt;
    /* Includes device apertures even when RAM + its mirrors need only 1.8 GB.
     * This reserves virtual address space, not 4 GB of physical memory. */
    if (span < UINT64_C(0x100000000)) span = (size_t)UINT64_C(0x100000000);
    memset(p, 0, sizeof *p);
    for (attempt = 0; attempt < 8; ++attempt) {
        void *probe = VirtualAlloc(NULL, span, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) return 0;
        if (!VirtualFree(probe, 0, MEM_RELEASE)) return 0;
        if (xbox_windows_layout_try(p, (uintptr_t)probe, ram, physical,
                                    bytes, tiled_bytes))
            return 1;
    }
    return 0;
}

#endif
