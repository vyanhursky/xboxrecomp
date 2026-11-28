/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u   /* the contiguous window; kernel.h */
#endif
#define XBOX_CONTIG_WINDOW 0x04000000u  /* 64 MB of it */

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern int nv2a_pb_exec_constant_word(uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;
static int s_scan_enabled = -1;

/* Clears the NV2A request bits (xbox_memory_layout.c); called every 256
 * words while executing, so kickoffs are answered mid-walk. */
extern void xbox_Nv2aAckBusyBits(void);
static uint32_t s_ack_tick;

/* Either switch, read once. The answer "not set" has to be cached as well as
 * "set": this runs on every pass of the NV2A poll loop, and on the default
 * path -- neither switch set -- a cache that only remembered "set" called
 * getenv on every pass, which walks the whole environment. */
static int pb_walk_enabled(void)
{
    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = getenv("RECOMP_PB_SCAN") != NULL;
    return s_exec_enabled || s_scan_enabled;
}

/* Slot + 1 of each (subchannel, method) in s_seen. note() runs for every
 * pushbuffer word, so a linear search here was the executor's largest cost
 * (about 40% of its thread in X-Men Legends' levels). */
static uint16_t s_seen_slot[8][0x2000 / 4];

/* The inventory is printed only for RECOMP_PB_SCAN; counting every parameter
 * word of every frame for nobody was several percent of the executor thread. */
static int s_survey = -1;
#define SURVEY_ON() (s_survey > 0 || (s_survey < 0 && (s_survey = getenv("RECOMP_PB_SCAN") != NULL)))

static void note(uint32_t subch, uint32_t method)
{
    uint16_t *slot = &s_seen_slot[subch & 7][(method & 0x1FFC) / 4];

    if (*slot) {
        s_seen[*slot - 1].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        *slot = (uint16_t)s_seen_count;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1818 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "ARRAY_ELEMENT32" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1818, "INLINE_ARRAY" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* A jump or call outside RAM means the walk is reading data as commands. */
/* The last command headers decoded, so a desync can be traced back to the
 * command that caused it. */
/* Physical pushbuffer addresses (DMA_PUT/GET, JUMP and CALL targets) and the
 * contiguous window they index (upstream's DMA walk). */
#define PB_PHYS_MASK 0x0FFFFFFCu
#define PB_RAM_BYTES (64u * 1024u * 1024u)   /* XBOX_CONTIG_SIZE */
#define PB_HIST 8
static struct { uint32_t at, w; } s_hist[PB_HIST];
static unsigned s_hist_n;
/* The last jumps/calls/returns taken, and resyncs (w = 0xFFFFFFFF). */
static struct { uint32_t at, w, to; } s_flow[PB_HIST];
static unsigned s_flow_n;
static void pb_flow(uint32_t at, uint32_t w, uint32_t to)
{
    s_flow[s_flow_n % PB_HIST].at = at;
    s_flow[s_flow_n % PB_HIST].w = w;
    s_flow[s_flow_n % PB_HIST].to = to;
    s_flow_n++;
}

static int pb_bad_target(uint32_t w, uint32_t at, uint32_t target, uint32_t put)
{
    static unsigned total;
    unsigned k;
    if (target < PB_RAM_BYTES)
        return 0;
    if (++total <= 16 || total % 100 == 0) {
        fprintf(stderr, "  [PB] bad target #%u 0x%08X from word 0x%08X at 0x%08X"
                        " (PUT 0x%08X); resync\n", total, target, w, at, put);
        if (total <= 4)
            for (k = 0; k < PB_HIST && k < s_hist_n; k++) {
                unsigned i = (s_hist_n - 1 - k) % PB_HIST;
                fprintf(stderr, "  [PB]   earlier header 0x%08X at 0x%08X\n",
                        s_hist[i].w, s_hist[i].at);
            }
        if (total <= 4)
            for (k = 0; k < PB_HIST && k < s_flow_n; k++) {
                unsigned i = (s_flow_n - 1 - k) % PB_HIST;
                fprintf(stderr, "  [PB]   earlier flow 0x%08X at 0x%08X -> 0x%08X\n",
                        s_flow[i].w, s_flow[i].at, s_flow[i].to);
            }
    }
    return 1;
}
static uint32_t s_get = 0xFFFFFFFFu, s_ret;
static int      s_in_call;

/* A method whose parameters the walk has not all reached: the header said
 * `count` words and PUT came first. The DMA engine stalls at PUT and carries
 * on with the same method when PUT moves; reading the next word as a new
 * header instead would decode vertex data as commands. (D3D streams large
 * inline arrays across several kicks, so this is routine, not a desync.) */
static uint32_t s_pend_left, s_pend_i, s_pend_method, s_pend_subch;
static int      s_pend_noninc;

/* DMA_GET is the GPU's fetch pointer: nothing at or past it has been read, and
 * XDK D3D relies on exactly that. Its fence wait reads GET and, when the fence
 * is far enough on, patches a "wake me" software method into the pushbuffer at
 * the fence and sleeps on an event. If GET lagged the executor -- it used to
 * move only when the walk reached PUT -- D3D patched commands the executor had
 * already passed and slept for good. So under RECOMP_PB_EXEC the walk publishes
 * its position before every command (s_pub is the last value it wrote); a GET
 * that is not what it last wrote means the title moved it, and the walk
 * follows. */
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
#define NV2A_USER_DMA_GET 0x800044u
static uint32_t s_pub = 0xFFFFFFFFu;

/* The title moved GET itself: XDK D3D resets the ring by writing DMA_PUT and
 * DMA_GET together (CDevice::KickOff's restart path). Walking on from the old
 * position would read whatever lies between as commands. */
void nv2a_pb_resync(uint32_t get_phys)
{
    if (!pb_walk_enabled())
        return;
    pb_flow(s_get, 0xFFFFFFFFu, get_phys & PB_PHYS_MASK);
    s_get = get_phys & PB_PHYS_MASK;
    s_in_call = 0;
    s_pend_left = 0;
    s_pub = s_get;
}

/* Feed the parameters of the method in progress to the survey and the
 * executor, up to PUT. */
static void pb_consume(const uint8_t *mem, uint32_t put, uint32_t *words)
{
    while (s_pend_left && s_get < PB_RAM_BYTES && s_get != put) {
        uint32_t m = s_pend_noninc ? s_pend_method : s_pend_method + s_pend_i * 4;
        uint32_t param = *(const uint32_t *)(mem + (0x80000000u | s_get));

        note(s_pend_subch, m);
        /* Same walk, two consumers: the survey counts, the executor
         * acts. Keeping them on one decode means they can never
         * disagree about what the stream said. */
        if (s_exec_enabled)
            nv2a_pb_exec_method(s_pend_subch, m, param);
        s_get = (s_get + 4) & PB_PHYS_MASK;
        s_pend_i++;
        s_pend_left--;
        (*words)++;
    }
}

void nv2a_pb_scan(uint32_t put_phys)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t put = put_phys & PB_PHYS_MASK;
    volatile uint32_t *get_reg = NULL;
    uint32_t words = 0, jumps = 0, unknown = 0;

    if (!pb_walk_enabled())
        return;
    if (s_get == 0xFFFFFFFFu) {               /* never resynced: start at PUT */
        s_get = put, s_pend_left = 0;
        return;
    }
    if (s_exec_enabled)
        get_reg = xbox_Nv2aRegPtr(NV2A_USER_DMA_GET);

    pb_consume(mem, put, &words);             /* the method PUT stalled inside */

    while (s_get != put) {
        uint32_t at, target;
        uint32_t w;

        if (get_reg) {
            uint32_t g = *get_reg;

            if (g != s_pub) {                 /* the title moved GET mid-walk */
                pb_flow(s_get, 0xFFFFFFFFu, g & PB_PHYS_MASK);
                s_get = g & PB_PHYS_MASK;
                s_in_call = 0;
                s_pend_left = 0;
                s_pub = s_get;
                continue;
            }
            *get_reg = s_get;
            s_pub = s_get;
        }
        at = s_get;

        /* Keep answering kickoffs while executing; see xbox_Nv2aAckBusyBits. */
        if (s_exec_enabled && (++s_ack_tick & 0xFFu) == 0)
            xbox_Nv2aAckBusyBits();

        if (s_get >= PB_RAM_BYTES) {
            s_get = put, s_pend_left = 0;
            break;
        }
        w = *(const uint32_t *)(mem + (0x80000000u | s_get));
        s_hist[s_hist_n % PB_HIST].at = at;
        s_hist[s_hist_n % PB_HIST].w = w;
        s_hist_n++;

        if (++words > 0x400000u || unknown > 64) {
            s_get = put, s_pend_left = 0;                      /* lost sync, or runaway */
            break;
        }
        s_get = (s_get + 4) & PB_PHYS_MASK;

        if ((w & 0xE0000003u) == 0x20000000u) {         /* old-style jump */
            target = w & 0x1FFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { s_get = put, s_pend_left = 0; break; }
            pb_flow(at, w, target);
            s_get = target;
            jumps++;
            continue;
        }
        if ((w & 3u) == 1u) {                            /* jump */
            target = w & 0xFFFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { s_get = put, s_pend_left = 0; break; }
            pb_flow(at, w, target);
            s_get = target;
            jumps++;
            continue;
        }
        if ((w & 3u) == 2u) {                            /* call */
            target = w & 0xFFFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { s_get = put, s_pend_left = 0; break; }
            if (s_in_call) {
                /* One level only: the hardware raises a CALL error and stops
                 * the pusher. Taking it would overwrite the return address
                 * and send the first RETURN to the wrong place. */
                s_in_call = 0;
                s_get = put, s_pend_left = 0;
                break;
            }
            s_ret = s_get;
            s_in_call = 1;
            s_tot_calls++;
            pb_flow(at, w, target);
            s_get = target;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {          /* return */
            if (s_in_call) {
                pb_flow(at, w, s_ret);
                s_get = s_ret;
                s_in_call = 0;
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            /* PUT bounds the parameters too. A count that runs past it must
             * not carry s_get beyond PUT (the outer loop would then replay a
             * lap of stale ring contents), nor be forgotten (the leftover
             * parameters would be read as headers): the method stays pending
             * and the next walk finishes it. */
            s_pend_left = count;
            s_pend_i = 0;
            s_pend_method = method;
            s_pend_subch = subch;
            s_pend_noninc = noninc;
            pb_consume(mem, put, &words);
            unknown = 0;
            continue;
        }
        unknown++;
    }

    if (get_reg) {                            /* what the poll thread writes too */
        *get_reg = s_get;
        s_pub = s_get;
    }
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

/* RECOMP_PB_WALK=dma selects the DMA-engine walk above. The default is the
 * segment scan below, which is what the executor did before the walk existed.
 *
 * The walk is the more faithful of the two: it follows JUMPs and ring wraps,
 * keeps its own GET, publishes it before every command, and answers kickoffs
 * mid-walk. That fidelity changes what XDK D3D does. Told exactly how far the
 * GPU has got, D3D's fence code takes its "GPU far behind" path: it patches a
 * software method (NO_OPERATION with a value) into the pushbuffer and sleeps
 * on an event that the GPU's interrupt for that method signals. A runtime that
 * does not deliver that interrupt leaves the title asleep for good -- a title
 * that ran under the segment scan froze on the first screen whose pushbuffer
 * made the walk long enough. So the walk is opt-in, for projects that deliver
 * the GPU's software-method interrupt. */
int nv2a_pb_dma_walk(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PB_WALK");
        on = e && !strcmp(e, "dma");
    }
    return on;
}

/* The segment scan: walk commands from va to end_va. Returns the words
 * consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. RenderWare on the Xbox compiles
 * static geometry into pushbuffers and draws it with a CALL, so skipping CALLs
 * loses whole meshes. The NV2A has one level of subroutine, so a CALL inside a
 * call is not followed. A JUMP outside a call ends the segment: the ring's
 * wrap is handled by the caller, which scans the two pieces separately.
 * Addresses are physical, like PUT's, and reach guest memory through the
 * contiguous window. */
/* Whether the walk is inside a pushbuffer CALL, for the executor: a flip
 * hold is only armed at the top level (nv2a_pb_exec.c). */
static int s_in_call;

/* The GPU stops at a software method until the title has handled it.
 *
 * Direct3D relies on that. Its fixup method (Def Jam: case 13 of
 * sub_002234F0) patches commands *after* the NOP before the GPU reaches them,
 * from a record the title may reuse the moment the GPU is past; run past the
 * NOP and the patch lands late, on commands already executed, from a record
 * already recycled -- which is how D3D's own DPC came to write to 0x858B003B
 * a few seconds into the main menu. So the executor stops at the end of the
 * packet that queued one, and the caller resumes from there once the title
 * has acknowledged it. RECOMP_PB_NO_STALL=1 restores running straight on. */
static volatile long s_stall_request;

/* The pusher's one-deep subroutine: CALL saves the address after it and
 * jumps, RETURN goes back. Direct3D runs its precompiled push buffers this
 * way (RunPushBuffer), and those are what its fixups patch. Both used to be
 * skipped as if they were no-ops, so every subroutine went unexecuted --
 * whole screens of 3D, among them Def Jam's main menu. */
static uint32_t s_sub_return;
/* The last few transfers of control, for the report when the walk leaves the
 * command stream. */
static struct { uint32_t va, word; } s_hdrs[24];
static unsigned s_hdrs_n;
static struct { uint32_t from, word, to; struct { uint32_t va, word; } hdr[24]; } s_xfer[8];
static unsigned s_xfer_n;
static struct { uint32_t from, put, stop; } s_runs[8];
static unsigned s_runs_n;
/* The recent transfers and runs, for a report from the caller. */
/* The address of the newest transfer taken from the ring (not from inside a
 * subroutine): where a bad CALL or JUMP was read, for RECOMP_PB_DUMP_XFER. */
uint32_t nv2a_pb_last_ring_xfer(uint32_t lo, uint32_t hi)
{
    unsigned k;
    for (k = 0; k < 8 && k < s_xfer_n; k++) {
        unsigned i = (s_xfer_n - 1 - k) & 7u;
        if (s_xfer[i].from >= lo && s_xfer[i].from < hi)
            return s_xfer[i].from;
    }
    return 0;
}

void nv2a_pb_dump_history(void)
{
    unsigned k;
    for (k = 0; k < 8 && k < s_xfer_n; k++) {
        unsigned i = (s_xfer_n - 1 - k) & 7u;
        fprintf(stderr, "  [PB]   transfer -%u: 0x%08X at 0x%08X -> 0x%08X%c", k + 1,
                s_xfer[i].word, s_xfer[i].from, s_xfer[i].to, 10);
        {
            unsigned h;
            for (h = 0; h < 24; h++)
                if (s_xfer[i].hdr[h].va)
                    fprintf(stderr, "  [PB]      after header 0x%08X at 0x%08X (now 0x%08X)%c",
                            s_xfer[i].hdr[h].word, s_xfer[i].hdr[h].va,
                            *(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + s_xfer[i].hdr[h].va), 10);
        }
    }
    for (k = 0; k < 0 && k < s_hdrs_n; k++) {
        unsigned i = (s_hdrs_n - 1 - k) % 6u;
        fprintf(stderr, "  [PB]   header -%u: 0x%08X at 0x%08X (now 0x%08X)%c", k + 1, s_hdrs[i].word,
                s_hdrs[i].va, *(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + s_hdrs[i].va), 10);
    }
    for (k = 0; k < 8 && k < s_runs_n; k++) {
        unsigned i = (s_runs_n - 1 - k) & 7u;
        fprintf(stderr, "  [PB]   run -%u: from 0x%08X toward PUT 0x%08X, stopped 0x%08X%c", k + 1,
                s_runs[i].from, s_runs[i].put, s_runs[i].stop, 10);
    }
}
static void note_xfer(uint32_t from, uint32_t word, uint32_t to)
{
    unsigned i = s_xfer_n++ & 7u;
    s_xfer[i].from = from; s_xfer[i].word = word; s_xfer[i].to = to;
    {
        unsigned k;
        for (k = 0; k < 24; k++) {                 /* the headers before it, newest first */
            unsigned j = (s_hdrs_n - 1 - k) % 24u;
            s_xfer[i].hdr[k].va = s_hdrs_n > k ? s_hdrs[j].va : 0;
            s_xfer[i].hdr[k].word = s_hdrs_n > k ? s_hdrs[j].word : 0;
        }
    }
}
static int s_no_call = -1;      /* RECOMP_PB_NO_CALL=1: skip CALL/RETURN, as before */
static int s_sub_active;
static int s_stalled;

/* Where the main stream stands: inside a subroutine that is the word after the
 * CALL, not the subroutine's own address. Returns 1 and sets *ret when inside
 * one. Positions in Direct3D's ring are only comparable with this. */
int nv2a_pb_in_subroutine(uint32_t *ret)
{
    if (!s_sub_active)
        return 0;
    *ret = s_sub_return;
    return 1;
}

/* The caller moved the executor somewhere else (a skip to PUT, a rewind):
 * whatever subroutine it was in is no longer being run. Left set, every CALL
 * after it was refused as nested and every subroutine went unexecuted. */
void nv2a_pb_leave_subroutine(void)
{
    s_sub_active = 0;
}

void nv2a_pb_stall(void)
{
    static int off = -1;
    if (off < 0)
        off = getenv("RECOMP_PB_NO_STALL") != NULL;
    if (!off)
        s_stall_request = 1;
}

/* A word of the stream (a guest address in the contiguous window). */
uint32_t nv2a_pb_read(uint32_t va)
{
    return *(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + va);
}

/* Decode one run of the push buffer, [va, end_va), handing every method to
 * the executor. Stops at end_va or at a jump; returns where it stopped, and
 * for a jump puts the target's guest address in *jump_va (0 otherwise). */
/* A packet cut off by PUT. The pusher keeps its method, subchannel and
 * remaining count when it stops at PUT and goes on with parameters when PUT
 * moves; Direct3D kicks in the middle of long packets (a large inline vertex
 * array filled across several reservations). The walker started every run
 * afresh, took the next parameter for a header, and lost the stream -- one of
 * the ways into "not a command" and "skipping to PUT". Kept here and resumed
 * only at the exact address it stopped at; any other start discards it. */
static struct {
    uint32_t va, count, method, subch;
    int noninc;
} s_pend;

/* Where the walk is, word by word, for whoever asks how far behind it is
 * (xbox_Nv2aBacklog): a run can take many milliseconds, and its start alone
 * made a moving executor look stopped. */
volatile uint32_t g_pb_live_va;
/* Words the walk has consumed, ever: how far past a fence's release it has
 * read, for the prefetch model (xbox_Nv2aReleasePump). */
volatile uint32_t g_pb_words;

static uint32_t pb_segment(uint32_t va, uint32_t end_va, uint32_t *jump_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0, jumps = 0, unknown = 0;
    (void)mem;                                  /* reads go through nv2a_pb_read */

    *jump_va = 0;
    if (s_no_call < 0)
        s_no_call = getenv("RECOMP_PB_NO_CALL") != NULL;
    if (s_pend.count) {
        if (va != s_pend.va) {
            s_pend.count = 0;                   /* the walk moved elsewhere */
        } else {
            while (s_pend.count && va != end_va) {
                if (SURVEY_ON())
                    note(s_pend.subch, s_pend.method);
                if (s_exec_enabled)
                    nv2a_pb_exec_method(s_pend.subch, s_pend.method, nv2a_pb_read(va));
                if (!s_pend.noninc)
                    s_pend.method += 4;
                s_pend.count--;
                va += 4;
                words++;
            }
            if (s_pend.count) {
                s_pend.va = va;                 /* still cut off */
                s_tot_words += words;
                return va;
            }
            if (s_stall_request) {
                s_stall_request = 0;
                s_stalled = 1;
                s_tot_words += words;
                return va;
            }
        }
    }
    /* Never outside the contiguous window, where every push buffer lives: a
     * stray jump or a PUT that moved backwards must not walk off it. */
    while (va != end_va && words < 0x100000u
           && va >= XBOX_CONTIG_BASE && va < XBOX_CONTIG_BASE + XBOX_CONTIG_WINDOW) {
        uint32_t w = nv2a_pb_read(va);
        g_pb_live_va = va;
        va += 4;
        words++;
        if ((++g_pb_words & 255u) == 0) {
            extern void xbox_Nv2aReleasePump(int at_put);
            xbox_Nv2aReleasePump(0);
        }

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            /* JUMP, new form (target in 31:2) or old (28:2). The target is a
             * physical offset, and the push buffer is contiguous memory. */
            uint32_t target = ((w & 3u) == 1u) ? (w & 0xFFFFFFFCu) : (w & 0x1FFFFFFCu);
            if ((target & 0x0FFFFFFFu) < XBOX_CONTIG_WINDOW)
                *jump_va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            note_xfer(va - 4, w, *jump_va);
            jumps++;
            break;
        }
        if ((w & 3u) == 2u && s_no_call) {             /* CALL, ignored on request */
            continue;
        }
        if ((w & 3u) == 2u) {                          /* CALL */
            uint32_t target = w & 0xFFFFFFFCu;
            if (s_sub_active || (target & 0x0FFFFFFFu) >= XBOX_CONTIG_WINDOW) {
                static int told;
                if (told < 3 && ++told)
                    fprintf(stderr, "  [PB] CALL 0x%08X at 0x%08X %s; skipped\n", w, va - 4,
                            s_sub_active ? "inside a subroutine" : "outside memory");
                continue;
            }
            s_sub_return = va;
            s_sub_active = 1;
            *jump_va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            note_xfer(va - 4, w, *jump_va);
            jumps++;
            break;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {       /* RETURN */
            if (s_sub_active) {
                s_sub_active = 0;
                *jump_va = s_sub_return;
                note_xfer(va - 4, w, s_sub_return);
                jumps++;
                break;
            }
            continue;
        }
        /* A method header: bits 31:29 000 (increasing) or 010 (non-increasing).
         * Other top bits used to pass as increasing headers too, so a pointer
         * in data (0x80244950) was decoded as one and the walk went on. */
        if ((w & 0xE0030003u) == 0u || (w & 0xE0030003u) == 0x40000000u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            s_hdrs[s_hdrs_n % 24u].va = va - 4; s_hdrs[s_hdrs_n % 24u].word = w; s_hdrs_n++;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            uint32_t i;
            for (i = 0; i < count && va != end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                if (SURVEY_ON())
                    note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled) {
                    uint32_t param = nv2a_pb_read(va);
                    /* Shader constants, most of a fight's words, skip the
                     * general dispatch (nv2a_pb_exec_constant_word). */
                    if (!(subch == 0 && m >= 0x0B80u && m < 0x0C00u
                          && nv2a_pb_exec_constant_word(m, param)))
                        nv2a_pb_exec_method(subch, m, param);
                }
                va += 4;
                words++;
            }
            if (i < count) {                   /* cut off by PUT */
                s_pend.va = va;
                s_pend.count = count - i;
                s_pend.method = noninc ? method : method + i * 4;
                s_pend.subch = subch;
                s_pend.noninc = noninc;
                break;
            }
            if (s_stall_request) {         /* at the packet's end, never inside it */
                s_stall_request = 0;
                s_stalled = 1;
                break;
            }
            continue;
        }
        unknown++;
        {
            /* Not a header, a jump, a call or a return: the walk is no longer
             * on the command stream (misaligned, or in memory that is not a
             * push buffer). Where it happens is the lead. */
            static int told;
            if (told < 20 && ++told)
                fprintf(stderr, "  [PB] not a command: 0x%08X at 0x%08X%s\n", w, va - 4,
                        s_sub_active ? " (in a subroutine)" : "");
            if (told <= 3) {
                unsigned k;
                for (k = 0; k < 24 && k < s_hdrs_n; k++) {
                    unsigned j = (s_hdrs_n - 1 - k) % 24u;
                    fprintf(stderr, "  [PB]   header -%u: 0x%08X at 0x%08X (now 0x%08X)\n", k + 1,
                            s_hdrs[j].word, s_hdrs[j].va, nv2a_pb_read(s_hdrs[j].va));
                }
                fprintf(stderr, "  [PB]   run from 0x%08X to 0x%08X\n",
                        s_runs[(s_runs_n - 1) & 7u].from, s_runs[(s_runs_n - 1) & 7u].put);
                if (told == 1) {
                    /* The 0x100 bytes before the word that was not a command, and a few after. */
                    uint32_t a = va - 4 - 0x100u, j;
                    for (j = 0; j < 72 && a >= XBOX_CONTIG_BASE
                                && a + 72 * 4 < XBOX_CONTIG_BASE + XBOX_CONTIG_WINDOW; j += 8)
                        fprintf(stderr, "  [PB]   %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                                a + j * 4, nv2a_pb_read(a + j * 4), nv2a_pb_read(a + j * 4 + 4),
                                nv2a_pb_read(a + j * 4 + 8), nv2a_pb_read(a + j * 4 + 12),
                                nv2a_pb_read(a + j * 4 + 16), nv2a_pb_read(a + j * 4 + 20),
                                nv2a_pb_read(a + j * 4 + 24), nv2a_pb_read(a + j * 4 + 28));
                }
                for (k = 0; k < 8 && k < s_xfer_n; k++) {
                    unsigned i = (s_xfer_n - 1 - k) & 7u;
                    fprintf(stderr, "  [PB]   transfer -%u: 0x%08X at 0x%08X -> 0x%08X\n",
                            k + 1, s_xfer[i].word, s_xfer[i].from, s_xfer[i].to);
                }
                for (k = 0; k < 8 && k < s_runs_n; k++) {
                    unsigned i = (s_runs_n - 1 - k) & 7u;
                    fprintf(stderr, "  [PB]   run -%u: from 0x%08X toward PUT 0x%08X, stopped 0x%08X\n",
                            k + 1, s_runs[i].from, s_runs[i].put, s_runs[i].stop);
                }
            }
        }
    }

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
    return va;
}

/* Run the push buffer from where the GPU last stopped to PUT, following jumps,
 * as the pusher does.
 *
 * The scan above stops at a jump, and its caller only ran it while PUT moved
 * forward. Direct3D wraps its push buffer with a jump back to the start once
 * it fills, so everything it wrote after a wrap was never executed -- and the
 * first thing it writes there is the NOP(5) whose software method releases its
 * wait for space. Def Jam stopped for good at about frame 99 in some runs,
 * waiting on that event, once its first screen filled the buffer.
 *
 * Returns where the run stopped and sets *stalled when that was a software
 * method (see nv2a_pb_stall); otherwise it ran to PUT, or as far as it could. */
uint32_t nv2a_pb_run(uint32_t get_va, uint32_t put_va, int *stalled)
{
    uint32_t va = get_va, jump;
    int hops = 0;

    *stalled = 0;
    if (!pb_walk_enabled() || get_va == put_va)
        return put_va;
    {
        /* The flip hold samples the vblank at the start of a walk
         * (nv2a_flip_hold.h); a no-op unless RECOMP_FLIP_PACING is on. */
        extern void nv2a_pb_exec_walk_begin(void);
        nv2a_pb_exec_walk_begin();
    }
    {
        unsigned i = s_runs_n++ & 7u;
        s_runs[i].from = get_va; s_runs[i].put = put_va; s_runs[i].stop = 0;
    }
    while (va != put_va && hops < 4096) {           /* a CALL and a RETURN are two */
        uint32_t end = put_va;
        /* A run up to PUT when PUT is ahead; otherwise PUT is behind a jump,
         * so run until that jump (bounded, as the scan always was). */
        if (put_va < va)
            end = va + 0x400000u;
        s_stalled = 0;
        va = pb_segment(va, end, &jump);
        if (s_stalled) {
            *stalled = 1;
            s_runs[(s_runs_n - 1) & 7u].stop = va;
            return va;
        }
        if (!jump)
            break;
        va = jump;
        hops++;
    }
    return va;
}

/* Whether the executor is inside a pushbuffer subroutine (the flip hold does
 * not pace a flip met there): upstream's DMA walk, or the fork's runner. */
int nv2a_pb_scan_in_call(void)
{
    return s_in_call || s_sub_active;
}
