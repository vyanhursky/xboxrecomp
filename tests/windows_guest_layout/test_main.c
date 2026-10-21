/* No game data: exercise the Windows placement algorithm using real Win32
 * mappings on Windows, and the same primitives' POSIX implementation elsewhere.
 * Fail each mapping in turn to check rollback, then introduce a real collision.
 */
#include "platform/xbox_winnt.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XBOX_CONTIG_BASE 0x80000000u
#define XBOX_CONTIG_SIZE (64u * 1024u * 1024u)
#define XBOX_TILED_BASE 0xF0000000u
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)

static int mapping_calls, fail_call, fail_all, fail_probe;
static int live_views, live_allocations, failures;

static void check(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

static void *checked_map(HANDLE h, DWORD access, DWORD hi, DWORD lo,
                          SIZE_T size, void *base)
{
    void *p;
    if (++mapping_calls == fail_call || fail_all) { SetLastError(487); return NULL; }
    p = MapViewOfFileEx(h, access, hi, lo, size, base);
    if (p) ++live_views;
    return p;
}

static void *checked_alloc(void *base, SIZE_T size, DWORD type, DWORD protect)
{
    void *p;
    if (!base && fail_probe) { SetLastError(8); return NULL; }
    if (base && (++mapping_calls == fail_call || fail_all)) { SetLastError(487); return NULL; }
    p = VirtualAlloc(base, size, type, protect);
    if (p) ++live_allocations;
    return p;
}

static BOOL checked_unmap(const void *p)
{
    BOOL ok = UnmapViewOfFile(p);
    check(ok, "view is released with UnmapViewOfFile");
    if (ok) --live_views;
    return ok;
}

static BOOL checked_free(void *p, SIZE_T size, DWORD type)
{
    BOOL ok;
    check(size == 0 && type == MEM_RELEASE, "reservation released in full");
    ok = VirtualFree(p, size, type);
    check(ok, "private allocation released with VirtualFree");
    if (ok) --live_allocations;
    return ok;
}

#define MapViewOfFileEx checked_map
#define VirtualAlloc checked_alloc
#define UnmapViewOfFile checked_unmap
#define VirtualFree checked_free
#include "windows_guest_layout.h"
#undef MapViewOfFileEx
#undef VirtualAlloc
#undef UnmapViewOfFile
#undef VirtualFree

static uintptr_t candidate(void)
{
    void *p = VirtualAlloc(NULL, (SIZE_T)UINT64_C(0x100000000),
                            MEM_RESERVE, PAGE_NOACCESS);
    check(p != NULL, "find a full guest span");
    if (!p) exit(2);
    check(VirtualFree(p, 0, MEM_RELEASE), "release address probe");
    return (uintptr_t)p;
}

static void check_aliases(XboxWindowsLayout *p, int expected_mirrors)
{
    int i, mirrors = 0;
    unsigned char *ram = p->base, *physical = p->contig, *tiled = p->tiled;
    ram[0x70000] = 0x5A;
    for (i = 0; i < XBOX_NUM_MIRRORS; ++i) {
        if (!p->mirrors[i]) continue;
        ++mirrors;
        check(((unsigned char *)p->mirrors[i])[0x70000] == 0x5A, "RAM mirror aliases base");
        ((unsigned char *)p->mirrors[i])[0x70000] = 0xA5;
        check(ram[0x70000] == 0xA5, "mirror writes reach base");
        ram[0x70000] = 0x5A;
    }
    check(mirrors == expected_mirrors, "every non-overlapping RAM mirror exists");
    physical[0x1000] = 0x67;
    check(tiled[0x1000] == 0x67, "tiled view aliases physical memory");
    tiled[0x1000] = 0x89;
    check(physical[0x1000] == 0x89, "tiled writes reach physical memory");
    check(ram[0x1000] == 0, "physical memory is separate from the guest image");
    check((uintptr_t)p->mcpx == (uintptr_t)p->base + XBOX_MCPX_BASE,
           "64-bit translated device pointer");
    ((volatile unsigned char *)p->mcpx)[0x400000] = 0xAB;
    check(((volatile unsigned char *)p->mcpx)[0x400000] == 0xAB, "AC97 page is accessible");
}

int main(void)
{
    const size_t bytes = XBOX_TOTAL_RAM;
    HANDLE ram = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                     0, (DWORD)(bytes * 2), NULL);
    HANDLE physical = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                          0, XBOX_CONTIG_SIZE, NULL);
    XboxWindowsLayout p, zero = {0};
    int i;
    if (!ram || !physical) return 2;

    /* The original bug: accepting RAM whose translated MCPX window is taken.
     * A real blocker must survive rollback byte-for-byte. */
    {
        uintptr_t base = candidate();
        unsigned char *blocker = VirtualAlloc((void *)(base + XBOX_MCPX_BASE),
                                  0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        check(blocker != NULL, "place collision in candidate MCPX window");
        if (!blocker) return 2;
        blocker[0] = 0xCD;
        check(!xbox_windows_layout_try(&p, base, ram, physical, bytes, bytes),
               "reject incomplete candidate despite free RAM");
        check(blocker[0] == 0xCD, "rollback preserves foreign mapping");
        check(!live_views && !live_allocations, "collision rolls back all owned windows");
        check(!memcmp(&p, &zero, sizeof p), "failed transaction publishes no pointers");
        check(VirtualFree(blocker, 0, MEM_RELEASE), "release collision fixture");
    }

    /* Base + physical + tiled + three devices + 28 mirrors. */
    for (i = 1; i <= 6 + XBOX_NUM_MIRRORS; ++i) {
        fail_call = i;
        mapping_calls = 0;
        check(!xbox_windows_layout_try(&p, candidate(), ram, physical, bytes, bytes),
               "injected mapping failure rejects transaction");
        check(GetLastError() == 487, "preserve placement failure through cleanup");
        check(!live_views && !live_allocations, "injected failure releases every owned mapping");
        check(!memcmp(&p, &zero, sizeof p), "injected failure clears result");
    }

    /* One collision during placement is retried; all aliases really work.
     * A second create/release checks teardown, including the mapped physical view. */
    for (i = 0; i < 2; ++i) {
        fail_call = i ? 0 : 5;
        mapping_calls = 0;
        check(xbox_windows_layout_create(&p, ram, physical, bytes, bytes), "complete layout with retry");
        if (!p.base) return 2;
        check_aliases(&p, XBOX_NUM_MIRRORS);
        xbox_windows_layout_release(&p);
        check(!live_views && !live_allocations, "successful layout teardown releases all windows");
    }
    fail_call = 0;
    check(xbox_windows_layout_create(&p, ram, physical, bytes * 2, bytes), "128 MB layout skips hardware-overlapping mirrors");
    if (p.base) {
        check_aliases(&p, XBOX_NUM_MIRRORS - 1);
        xbox_windows_layout_release(&p);
    }
    check(!live_views && !live_allocations, "128 MB teardown releases all windows");

    fail_all = 1;
    mapping_calls = 0;
    check(!xbox_windows_layout_create(&p, ram, physical, bytes, bytes), "exhausted retries fail cleanly");
    check(mapping_calls == 8 && !p.base, "retry limit publishes no incomplete layout");
    check(!live_views && !live_allocations, "exhausted retries leave no allocations");
    fail_all = 0;
    fail_probe = 1;
    check(!xbox_windows_layout_create(&p, ram, physical, bytes, bytes), "span reservation failure is reported");
    check(GetLastError() == 8 && !p.base, "reservation error survives without published pointers");
    check(!live_views && !live_allocations, "reservation failure leaves no allocations");
    CloseHandle(physical);
    CloseHandle(ram);
    if (!failures) puts("PASS collision, 34 rollback points, retries/exhaustion, reservation failure, aliases, re-init and 128 MB layout");
    return failures ? 1 : 0;
}
