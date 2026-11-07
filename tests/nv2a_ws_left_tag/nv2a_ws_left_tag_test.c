/* The widescreen left/extended-centre tags. Synthetic 2D
 * triangles go through the real executor's host forwarding path; a translator
 * stub checks the clip and the vertices at draw time. A triangle drawn in the
 * tag band arrives moved back up and clipped to the left-anchored box; the same
 * triangle untagged stays centred; at 4:3, or with the tag off, a triangle in
 * the band is left alone. The independent centre tag extends a full-width
 * clip horizontally, preserving narrower masks and vertical clipping. The
 * fight HUD's strips go through the same path.
 * Guest memory is a synthetic buffer; no game data. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "../../src/nv2a/nv2a_pgraph_d3d11.h"

#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;   /* #189 made the stack placement runtime state */
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
static uint32_t s_fence_va;                           /* the mirrored fence word, if any */
uint32_t xbox_Nv2aFenceWordVa(void) { return s_fence_va; }
int nv2a_pb_dma_walk(void) { return 1; }             /* RECOMP_PB_WALK=dma: 0x0310 is latched */
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
/* The flip hold (nv2a_flip_hold.c, linked in). No vblank moves here, so the
 * executor never holds a flip; the walk is never inside a pushbuffer CALL. */
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }

static volatile uint32_t s_pgraph_400b10;
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    return offset == 0x400B10u ? &s_pgraph_400b10 : NULL;
}

/* ---- driving the executor ---------------------------------------------- */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

extern void nv2a_ws_configure(float k, float reach);
extern void nv2a_ws_hud_heartbeat(int bottom_rows);
static uint32_t clip[4], expected[4], draws;
static float expect_y0, expect_h = 50.0f;
void pgraph_d3d11_set_surface_clip(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ clip[0] = x; clip[1] = y; clip[2] = w; clip[3] = h; }
void pgraph_d3d11_draw_ready(uint32_t mode, const PgraphReadyVertex *v, uint32_t n)
{
    uint32_t i; (void)mode;
    CHECK(n == 3); ++draws;
    for (i = 0; i < 4; i++)
        CHECK(clip[i] == expected[i]);
    if (memcmp(clip, expected, sizeof clip))
        printf("draw %u: clip %u %u %u %u, expected %u %u %u %u\n", draws, clip[0], clip[1], clip[2], clip[3],
               expected[0], expected[1], expected[2], expected[3]);
    if (n >= 3)
        CHECK(v[0].y == expect_y0 && v[1].y == expect_y0 && v[2].y == expect_y0 + expect_h);
}

/* One 2D triangle at y (y, y, y + 50), x 150..200: wholly left of the middle,
 * below the fight HUD's top strip. */
static void triangle(float y)
{
    putf(0x2000, 150); putf(0x2004, y);         putf(0x2008, 0);
    putf(0x200C, 200); putf(0x2010, y);         putf(0x2014, 0);
    putf(0x2018, 170); putf(0x201C, y + 50.0f); putf(0x2020, 0);
    m(0x17FC, 5); m(0x1810, 2u << 24); m(0x17FC, 0);
}

/* A 2D batch with this extent, as the fight HUD draws them. */
static void box(float x0, float x1, float y0, float y1)
{
    putf(0x2000, x0); putf(0x2004, y0); putf(0x2008, 0);
    putf(0x200C, x1); putf(0x2010, y0); putf(0x2014, 0);
    putf(0x2018, x0); putf(0x201C, y1); putf(0x2020, 0);
    expect_y0 = y0;
    expect_h = y1 - y0;
    m(0x17FC, 5); m(0x1810, 2u << 24); m(0x17FC, 0);
    expect_h = 50.0f;
}

/* The fight HUD's strips at this aspect. Extents are the combo call-out's
 * ("1ST COMBO MOVE": a digit, a suffix and text, three batches) and the pause
 * panel's, measured in Def Jam. `left` and `right` are the clip's x when
 * anchored; every clip is `width` wide. */
static void hud_strips(uint32_t left, uint32_t right, uint32_t centred, uint32_t width)
{
    expected[2] = width;
    nv2a_ws_hud_heartbeat(0);
    expected[0] = left;
    box(48, 68, 106.5f, 142.4f);                /* P1 digit at its largest */
    box(64, 92, 108.6f, 124.0f);                /* P1 suffix */
    box(64, 196, 120.5f, 136.5f);               /* P1 text */
    expected[0] = right;
    box(448, 464, 110.0f, 142.4f);              /* P2 digit */
    box(462, 594, 124.5f, 140.5f);              /* P2 text */
    expected[0] = left;
    box(41, 297, 122.0f, 186.0f);               /* P1 struggle meter: frame */
    box(52, 183, 137.0f, 147.0f);               /* its fill bar */
    box(46.5f, 218.5f, 146.5f, 162.5f);         /* its text ("TAP ANY BUTTON") */
    expected[0] = right;
    box(343, 599, 121.0f, 185.0f);              /* P2 struggle meter: frame */
    box(455, 588, 136.0f, 146.0f);              /* its fill bar */
    expected[0] = centred;
    box(48, 68, 150.0f, 170.0f);                /* below the top strip */
    box(235.5f, 248.5f, 162.5f, 180.5f);        /* the pause panel's highest one-sided piece */
    box(125.5f, 233.5f, 196.5f, 211.5f);        /* a pause panel label */
    box(66, 199, 145.0f, 171.0f);               /* match summary rows, just after a fight */
    box(455, 554, 124.0f, 147.0f);
    box(435, 569, 141.0f, 164.0f);
    box(240, 400, 120.0f, 136.0f);              /* across the middle */
    box(48, 68, 338.5f, 370.4f);                /* no bottom strip with two fighters */

    nv2a_ws_hud_heartbeat(1);
    expected[0] = left;
    box(48, 68, 338.5f, 370.4f);                /* P3 digit at its largest */
    box(64, 92, 340.6f, 356.1f);                /* P3 suffix */
    box(64, 196, 352.5f, 368.5f);               /* P3 text */
    expected[0] = right;
    box(448, 464, 338.5f, 370.4f);              /* P4 digit */
    box(460, 488, 340.6f, 356.1f);              /* P4 suffix */
    expected[0] = left;
    box(48.5f, 183.5f, 314.0f, 330.0f);         /* P3 struggle meter: text above the HUD */
    box(41, 297, 331.0f, 395.0f);               /* its frame */
    box(138.4f, 159.9f, 335.0f, 345.0f);        /* its fill bar */
    expected[0] = right;
    box(465.5f, 590.5f, 314.0f, 330.0f);        /* P4 struggle meter: text */
    box(343, 599, 331.0f, 395.0f);              /* its frame */
    box(467.6f, 475.4f, 335.0f, 345.0f);        /* its fill bar */
    expected[0] = centred;
    box(146, 268, 330.0f, 346.0f);              /* the pause panel's lowest piece */
    box(190.5f, 223.5f, 300.0f, 316.0f);        /* a pause panel piece above it */
    nv2a_ws_hud_heartbeat(0);
}

int main(void)
{
    uint32_t i;
#ifdef _WIN32
    _putenv_s("RECOMP_PB_D3D11", "1");
#else
    setenv("RECOMP_PB_D3D11", "1", 1);
#endif
    m(0x1E94, 0);                               /* pretransformed positions */
    for (i = 0; i < 3; i++)
        put32(0x3000 + 4 * i, 0xFFFFFFFFu);
    m(0x1720, 0x2000); m(0x1760, 2 | (3 << 4) | (12 << 8));
    m(0x172C, 0x3000); m(0x176C, 0 | (4 << 4) | (4 << 8));
    m(0x020C, 5120); m(0x0210, 0x8000);
    m(0x0208, 0x128);                           /* no AA */
    m(0x0200, 0u | (640u << 16)); m(0x0204, 0u | (480u << 16));
    expected[1] = 0; expected[3] = 480;

    /* 16:9, tag 8192: a tagged triangle comes back to y 300, anchored left. */
    nv2a_ws_configure(.75f, 1.0f);
    nv2a_ws_set_left_tag(8192.0f);
    expected[0] = 0; expected[2] = 480;         /* 640 * k from the left edge */
    expect_y0 = 300.0f;
    triangle(8192.0f + 300.0f);
    /* The same triangle untagged: centred, clipped to the 4:3 box. */
    expected[0] = 80; expected[2] = 480;
    expect_y0 = 300.0f;
    triangle(300.0f);
    /* Far below the band (a parked overlay): untouched and centred. */
    expect_y0 = 2514.0f;
    triangle(2514.0f);

    /* Tag off: a triangle in the band is drawn where it is. */
    nv2a_ws_set_left_tag(0.0f);
    expect_y0 = 8192.0f + 300.0f;
    triangle(8192.0f + 300.0f);

    /* 4:3: nothing is placed, tagged or not. */
    nv2a_ws_configure(1.0f, 1.0f);
    nv2a_ws_set_left_tag(8192.0f);
    expected[0] = 0; expected[2] = 640;
    expect_y0 = 8192.0f + 300.0f;
    triangle(8192.0f + 300.0f);

    /* Ultrawide with the HUD in a centred 16:9 box: anchored to that box. */
    nv2a_ws_configure(4.0f / 7.0f, 0.75f);      /* 21:9; reach (16/9)/(21/9) */
    expected[0] = 80; expected[2] = 366;        /* 320 * (1 - reach), as the fight HUD; 640 * k */
    expect_y0 = 300.0f;
    triangle(8192.0f + 300.0f);

    /* An independent extend tag: centred, full canvas horizontally, original
     * vertical scissor. Regular and left-tagged draws keep their own policy. */
    nv2a_ws_configure(.75f, 1.0f);
    nv2a_ws_set_extend_tag(16384.0f);
    expected[0] = 0; expected[2] = 640;
    expect_y0 = 300.0f;
    triangle(16384.0f + 300.0f);
    expected[0] = 80; expected[2] = 480;
    triangle(300.0f);
    expected[0] = 0; expected[2] = 480;
    triangle(8192.0f + 300.0f);

    /* A partial guest mask is preserved, including its vertical bounds. */
    m(0x0200, 10u | (600u << 16)); m(0x0204, 20u | (400u << 16));
    expected[0] = 88; expected[1] = 20; expected[2] = 450; expected[3] = 400;
    triangle(16384.0f + 300.0f);
    m(0x0200, 0u | (640u << 16));
    expected[0] = 0; expected[2] = 640;
    triangle(16384.0f + 300.0f);

    /* Disabled tag and original 4:3 leave coordinates/scissors alone. */
    nv2a_ws_set_extend_tag(0.0f);
    expected[0] = 80; expected[2] = 480;
    expect_y0 = 16384.0f + 300.0f;
    triangle(expect_y0);
    nv2a_ws_configure(1.0f, 1.0f);
    nv2a_ws_set_extend_tag(16384.0f);
    expected[0] = 0; expected[2] = 640;
    triangle(expect_y0);

    /* The fight HUD's strips, on no tag: at 16:9 to the screen edges, on
     * ultrawide to the edges of the centred 16:9 box. */
    nv2a_ws_set_left_tag(0.0f);
    nv2a_ws_set_extend_tag(0.0f);
    m(0x0204, 0u | (480u << 16));               /* the whole height again */
    expected[1] = 0; expected[3] = 480;
    nv2a_ws_configure(.75f, 1.0f);
    hud_strips(0, 160, 80, 480);
    nv2a_ws_configure(4.0f / 7.0f, 0.75f);
    hud_strips(80, 194, 137, 366);

    /* 4:3: nothing is placed, in a strip or not. */
    nv2a_ws_configure(1.0f, 1.0f);
    expected[0] = 0; expected[2] = 640;
    nv2a_ws_hud_heartbeat(1);
    box(48, 68, 106.5f, 142.4f);
    box(48, 68, 338.5f, 370.4f);

    CHECK(draws == 77);
    if (!s_fail)
        printf("nv2a-ws-left-tag: 77 draws checked\n");
    return s_fail;
}
