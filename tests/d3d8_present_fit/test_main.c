/* Where the picture goes in the window: d3d8_present_fit. */

#include "d3d8_present.h"

#include <stdio.h>

static int g_failed;

static void expect(const char *name, D3D8PresentRect r,
                   unsigned x, unsigned y, unsigned w, unsigned h)
{
    if (r.x != x || r.y != y || r.width != w || r.height != h) {
        printf("FAIL %s: got %u,%u %ux%u, want %u,%u %ux%u\n", name,
               r.x, r.y, r.width, r.height, x, y, w, h);
        g_failed++;
    }
}

int main(void)
{
    /* The same shape fills the window. */
    expect("4:3 in 640x480", d3d8_present_fit(4, 3, 640, 480, 1), 0, 0, 640, 480);
    expect("4:3 in 1280x960", d3d8_present_fit(4, 3, 1280, 960, 1), 0, 0, 1280, 960);
    /* Wider windows get bars at the sides. */
    expect("4:3 in 1920x1080", d3d8_present_fit(4, 3, 1920, 1080, 1), 240, 0, 1440, 1080);
    expect("4:3 in 2560x1440", d3d8_present_fit(4, 3, 2560, 1440, 1), 320, 0, 1920, 1440);
    expect("4:3 in 3840x2160", d3d8_present_fit(4, 3, 3840, 2160, 1), 480, 0, 2880, 2160);
    expect("4:3 in 1280x800", d3d8_present_fit(4, 3, 1280, 800, 1), 106, 0, 1067, 800);
    /* Taller windows get bars above and below. */
    expect("4:3 in 600x800", d3d8_present_fit(4, 3, 600, 800, 1), 0, 175, 600, 450);
    expect("16:9 in 1280x960", d3d8_present_fit(16, 9, 1280, 960, 1), 0, 120, 1280, 720);
    /* Stretch, and degenerate input, use the whole window. */
    expect("stretch", d3d8_present_fit(4, 3, 1920, 1080, 0), 0, 0, 1920, 1080);
    expect("zero ratio", d3d8_present_fit(0, 3, 1920, 1080, 1), 0, 0, 1920, 1080);
    expect("empty window", d3d8_present_fit(4, 3, 0, 0, 1), 0, 0, 0, 0);
    /* Never empty, never outside the window. */
    expect("sliver", d3d8_present_fit(4, 3, 1000, 1, 1), 499, 0, 1, 1);
    expect("column", d3d8_present_fit(4, 3, 1, 1000, 1), 0, 499, 1, 1);

    if (g_failed) { printf("%d case(s) failed\n", g_failed); return 1; }
    printf("d3d8_present_fit: all cases passed\n");
    return 0;
}
