/*
 * mmio_trap.c -- see mmio_trap.h.
 */
#if !defined(_WIN32)

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "mmio_trap.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define MMIO_TRAP_MAX 32
#define DEVICE_PAGE   4096u

typedef struct {
    uintptr_t          addr, len;        /* the device range */
    uintptr_t          span, span_len;   /* the host pages closed for it */
    uint8_t           *open;             /* second view of the span, or NULL */
    void              *dev;
    mmio_trap_read_fn  rd;
    mmio_trap_write_fn wr;
} mmio_trap;

/* Read by the signal handler without a lock: an entry is complete before the
 * count that publishes it moves, and a removed entry only has its len zeroed. */
static mmio_trap        g_traps[MMIO_TRAP_MAX];
static volatile int     g_trap_count;
static volatile int     g_installed;
static volatile uint64_t g_neighbours;
static struct sigaction g_prev_segv, g_prev_bus;

uint64_t mmio_trap_neighbour_count(void) { return g_neighbours; }

/* ── A64 loads and stores ──────────────────────────────────────────────── */

static int64_t sext(uint32_t v, int bits)
{
    return (int64_t)((uint64_t)v << (64 - bits)) >> (64 - bits);
}

int mmio_a64_decode(uint32_t insn, mmio_a64_access *a)
{
    memset(a, 0, sizeof *a);
    a->rt  = (int)(insn & 31);
    a->rn  = (int)((insn >> 5) & 31);
    a->rt2 = -1;

    if (insn & (1u << 26))                 /* SIMD and FP registers */
        return 0;

    /* Single register: unsigned offset, unscaled, pre/post-indexed and
     * register-offset forms share the size and opc fields. */
    if ((insn & 0x3A000000u) == 0x38000000u) {
        uint32_t opc = (insn >> 22) & 3;
        int scaled   = (insn >> 24) & 1;

        a->size = 1 << (insn >> 30);
        if (opc == 0) {
            a->is_load = 0;
        } else {
            a->is_load = 1;
            if (opc >= 2) {
                if (a->size == 8 || (opc == 3 && a->size == 4))
                    return 0;              /* prefetch, or unallocated */
                a->sign_extend = 1;
                a->dest_64 = (opc == 2);
            }
        }
        if (scaled)
            return 1;
        if (insn & (1u << 21))             /* register offset; atomics excluded */
            return ((insn >> 10) & 3) == 2;
        switch ((insn >> 10) & 3) {
        case 0:  return 1;                 /* unscaled */
        case 2:  return 0;                 /* unprivileged */
        default:                           /* 1 post-index, 3 pre-index */
            a->writeback = 1;
            a->wb_delta  = sext((insn >> 12) & 0x1FF, 9);
            return 1;
        }
    }

    /* Pair: post-index, signed offset, pre-index. */
    if ((insn & 0x3A000000u) == 0x28000000u) {
        uint32_t opc  = insn >> 30;
        uint32_t mode = (insn >> 23) & 3;

        if (opc == 3 || mode == 0)         /* unallocated; no-allocate hint */
            return 0;
        a->is_load = (insn >> 22) & 1;
        a->rt2     = (int)((insn >> 10) & 31);
        a->size    = (opc == 2) ? 8 : 4;
        if (opc == 1) {                    /* LDPSW */
            if (!a->is_load)
                return 0;
            a->sign_extend = 1;
            a->dest_64 = 1;
        }
        if (mode != 2) {
            a->writeback = 1;
            a->wb_delta  = sext((insn >> 15) & 0x7F, 7) * a->size;
        }
        return 1;
    }
    return 0;
}

/* ── Register state out of a ucontext ──────────────────────────────────── */

#if defined(__aarch64__)

#if defined(__APPLE__)
#define UC_X(uc, n) ((uc)->uc_mcontext->__ss.__x[n])
#define UC_FP(uc)   ((uc)->uc_mcontext->__ss.__fp)
#define UC_LR(uc)   ((uc)->uc_mcontext->__ss.__lr)
#define UC_SP(uc)   ((uc)->uc_mcontext->__ss.__sp)
#define UC_PC(uc)   ((uc)->uc_mcontext->__ss.__pc)
#else
#define UC_X(uc, n) ((uc)->uc_mcontext.regs[n])
#define UC_FP(uc)   ((uc)->uc_mcontext.regs[29])
#define UC_LR(uc)   ((uc)->uc_mcontext.regs[30])
#define UC_SP(uc)   ((uc)->uc_mcontext.sp)
#define UC_PC(uc)   ((uc)->uc_mcontext.pc)
#endif

static uint64_t reg_get(ucontext_t *uc, int r)
{
    if (r == 31) return 0;
    if (r == 30) return (uint64_t)UC_LR(uc);
    if (r == 29) return (uint64_t)UC_FP(uc);
    return (uint64_t)UC_X(uc, r);
}

static void reg_set(ucontext_t *uc, int r, uint64_t v)
{
    if (r == 31) return;
    if (r == 30)      UC_LR(uc) = v;
    else if (r == 29) UC_FP(uc) = v;
    else              UC_X(uc, r) = v;
}

static uint64_t mem_get(const uint8_t *p, int size)
{
    uint64_t v = 0;
    memcpy(&v, p, (size_t)size);
    return v;
}

/* The device whose range holds [va, va + size), if any. Two devices can sit
 * in one host page, so this is not necessarily the trap whose span faulted. */
static mmio_trap *device_at(uintptr_t va, int size)
{
    int i, n = g_trap_count;

    for (i = 0; i < n; i++) {
        mmio_trap *d = &g_traps[i];
        if (d->len && va >= d->addr && va + (uintptr_t)size <= d->addr + d->len)
            return d;
    }
    return NULL;
}

/* One element of the access at host address va, against a device if it is
 * inside one and against the open view of t's span if it is only nearby. */
static int element(mmio_trap *t, ucontext_t *uc, const mmio_a64_access *a,
                   uintptr_t va, int reg)
{
    mmio_trap *d = device_at(va, a->size);
    uint64_t v;

    if (!d) {
        if (!t->open || va < t->span
                || va + (uintptr_t)a->size > t->span + t->span_len)
            return 0;
        g_neighbours++;
    }
    if (a->is_load) {
        v = d ? d->rd(d->dev, (uint32_t)(va - d->addr), a->size)
              : mem_get(t->open + (va - t->span), a->size);
        if (a->size < 8)
            v &= (1ULL << (a->size * 8)) - 1;
        if (a->sign_extend) {
            v = (uint64_t)sext((uint32_t)v, a->size * 8);
            if (!a->dest_64)
                v &= 0xFFFFFFFFu;
        }
        reg_set(uc, reg, v);
    } else {
        v = reg_get(uc, reg);
        if (a->size < 8)
            v &= (1ULL << (a->size * 8)) - 1;
        if (d)
            d->wr(d->dev, (uint32_t)(va - d->addr), v, a->size);
        else
            memcpy(t->open + (va - t->span), &v, (size_t)a->size);
    }
    return 1;
}

static int service(mmio_trap *t, ucontext_t *uc, uintptr_t fault)
{
    mmio_a64_access a;
    uint32_t insn;
    uintptr_t va = fault;

    memcpy(&insn, (const void *)(uintptr_t)UC_PC(uc), 4);
    if (!mmio_a64_decode(insn, &a))
        return 0;
    /* For a pair the fault address may be its second element, so the address
     * comes from the base register instead. */
    if (a.rt2 >= 0) {
        uint64_t base = (a.rn == 31) ? (uint64_t)UC_SP(uc) : reg_get(uc, a.rn);
        int64_t off = sext((insn >> 15) & 0x7F, 7) * a.size;
        if (((insn >> 23) & 3) == 1)
            off = 0;                       /* post-index reads at the base */
        va = (uintptr_t)(base + (uint64_t)off);
    }
    if (!element(t, uc, &a, va, a.rt))
        return 0;
    if (a.rt2 >= 0 && !element(t, uc, &a, va + (uintptr_t)a.size, a.rt2))
        return 0;
    if (a.writeback) {
        if (a.rn == 31) UC_SP(uc) += (uint64_t)a.wb_delta;
        else            reg_set(uc, a.rn, reg_get(uc, a.rn) + (uint64_t)a.wb_delta);
    }
    UC_PC(uc) += 4;
    return 1;
}

#else  /* !__aarch64__ */

/* x86-64 hosts use mmio_decode.h's decoder once its context is not Win32's;
 * until then a fault here is not serviced and falls through to the previous
 * handler, which reports it. */
static int service(mmio_trap *t, ucontext_t *uc, uintptr_t fault)
{ (void)t; (void)uc; (void)fault; return 0; }

#endif

/* ── Signal plumbing ───────────────────────────────────────────────────── */

static void chain(const struct sigaction *prev, int sig, siginfo_t *si, void *ctx)
{
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) { prev->sa_sigaction(sig, si, ctx); return; }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    /* Default action: put it back and let the instruction fault again. */
    signal(sig, SIG_DFL);
}

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    uintptr_t fault = (uintptr_t)si->si_addr;
    int saved = errno, i, n = g_trap_count;

    for (i = 0; i < n; i++) {
        mmio_trap *t = &g_traps[i];
        if (t->len && fault >= t->span && fault < t->span + t->span_len) {
            if (service(t, (ucontext_t *)ctx, fault)) {
                errno = saved;
                return;
            }
            break;
        }
    }
    chain(sig == SIGBUS ? &g_prev_bus : &g_prev_segv, sig, si, ctx);
    errno = saved;
}

static int install(void)
{
    struct sigaction sa;

    if (__atomic_exchange_n(&g_installed, 1, __ATOMIC_SEQ_CST))
        return 0;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    /* Darwin reports a protection fault as SIGBUS, Linux as SIGSEGV. */
    if (sigaction(SIGSEGV, &sa, &g_prev_segv) || sigaction(SIGBUS, &sa, &g_prev_bus))
        return -1;
    return 0;
}

/* A second mapping of the same pages, so the handler can reach memory the
 * guest's view has closed. */
static uint8_t *open_view(uintptr_t span, uintptr_t span_len)
{
#if defined(__APPLE__)
    mach_vm_address_t at = 0;
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &at, span_len, 0,
                                     VM_FLAGS_ANYWHERE, mach_task_self(), span,
                                     FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS)
        return NULL;
    if (mprotect((void *)(uintptr_t)at, span_len, PROT_READ | PROT_WRITE)) {
        munmap((void *)(uintptr_t)at, span_len);
        return NULL;
    }
    return (uint8_t *)(uintptr_t)at;
#else
    /* Linux can only alias a shared mapping this way (mremap with a zero old
     * size); a private one fails and the caller reports it. */
    void *p = mremap((void *)span, 0, span_len, MREMAP_MAYMOVE);
    return p == MAP_FAILED ? NULL : (uint8_t *)p;
#endif
}

int mmio_trap_add(void *addr, size_t len, void *dev,
                  mmio_trap_read_fn rd, mmio_trap_write_fn wr)
{
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t a = (uintptr_t)addr;
    mmio_trap *t;
    int n = g_trap_count;

    if (!rd || !wr || !len || (a | len) % DEVICE_PAGE || n >= MMIO_TRAP_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (install())
        return -1;

    t = &g_traps[n];
    t->addr = a;
    t->len  = len;
    t->dev  = dev;
    t->rd   = rd;
    t->wr   = wr;
    t->span = a & ~(page - 1);
    t->span_len = ((a + len + page - 1) & ~(page - 1)) - t->span;
    t->open = NULL;
    if (t->span != a || t->span_len != len) {
        t->open = open_view(t->span, t->span_len);
        if (!t->open) {
            t->len = 0;
            errno = ENOTSUP;
            return -1;
        }
    }
    __atomic_store_n(&g_trap_count, n + 1, __ATOMIC_SEQ_CST);
    if (mprotect((void *)t->span, t->span_len, PROT_NONE)) {
        t->len = 0;
        return -1;
    }
    return 0;
}

int mmio_trap_remove(void *addr)
{
    int i, n = g_trap_count;

    for (i = 0; i < n; i++) {
        mmio_trap *t = &g_traps[i];
        if (t->len && t->addr == (uintptr_t)addr) {
            int j;
            if (mprotect((void *)t->span, t->span_len, PROT_READ | PROT_WRITE))
                return -1;
            t->len = 0;
            if (t->open)
                munmap(t->open, t->span_len);
            t->open = NULL;
            /* A device that shares a host page with this one stays closed. */
            for (j = 0; j < n; j++)
                if (g_traps[j].len)
                    mprotect((void *)g_traps[j].span, g_traps[j].span_len, PROT_NONE);
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

#endif /* !_WIN32 */
