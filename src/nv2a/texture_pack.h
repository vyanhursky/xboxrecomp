#ifndef RECOMP_TEXTURE_PACK_H
#define RECOMP_TEXTURE_PACK_H
#include <stdint.h>
#include "../d3d/d3d8_xbox.h"

/* Source dimensions/stride remain guest properties. Replacements never alter them. */
typedef struct RecompTextureSource {
    uint32_t address, format, width, height, row_texels;
    const void *data;
    uint32_t bytes, stride, row_bytes, rows;
    const void *palette;
    uint32_t palette_entries;
} RecompTextureSource;
typedef int (*RecompTextureSample)(void *user, uint32_t x, uint32_t y, uint32_t *argb);
/* Render-thread-only, address-scoped host textures. Register copies the BGRA
 * pixels and builds independent-channel mips; 1x–8x, at most 2048px. Owner must
 * clear before freeing/reusing its guest source. Clear retires GPU objects until
 * the next bind/shutdown, and does not affect ordinary content-addressed packs.
 * At most 32 sources / 256 MiB of host pixels, sharing the pack GPU cache budget.
 * Returns 0 on invalid input, duplicate address or budget/allocation failure. */
int texture_pack_memory_register(uint32_t owner,uint32_t address,uint32_t format,
        uint32_t native_width,uint32_t native_height,const uint8_t *bgra,uint32_t width,uint32_t height);
void texture_pack_memory_clear(uint32_t owner);
int texture_pack_active(void);
int texture_pack_hash(const RecompTextureSource *source, char out[65]);
int texture_pack_bind(IDirect3DDevice8 *device, unsigned stage,
                      const RecompTextureSource *source, RecompTextureSample sample, void *user);
void texture_pack_shutdown(IDirect3DDevice8 *device);
/* Widescreen side slabs ([wide] in a manifest) of the source texture_pack_bind last saw on
 * `stage`: their shifts in source texels (up to max), 0 when it has none. wide_bind sets slab
 * `slab` on the stage; 0 while it is still loading or was rejected. */
int texture_pack_wide_slabs(unsigned stage, int *dt, unsigned max);
int texture_pack_wide_bind(IDirect3DDevice8 *device, unsigned stage, unsigned slab);
/* Optional application policy for side replays. Return nonzero to replay the central draw's
 * current texture instead of a manifest slab. The callback runs on the render thread; register
 * it before rendering starts. No filter preserves normal pack behavior. source_binding includes
 * the original-texture fallback while an asynchronous replacement is loading. */
typedef int (*RecompWideReplayFilter)(const char *source_id, int offset, void *user);
void texture_pack_set_wide_replay_filter(RecompWideReplayFilter filter, void *user);
int texture_pack_wide_bind_source(IDirect3DDevice8 *device, unsigned stage, unsigned slab,
                                  IDirect3DBaseTexture8 *source_binding);
/* With a widescreen picture, an anchor's inner: slab is bound in place of its replacement. */
void texture_pack_set_widescreen(int on);
/* 1 when the source last bound on `stage` is a [wide] stretch overlay. */
int texture_pack_wide_stretch(unsigned stage);
/* The source id texture_pack_bind last saw on a stage ("" when none), for diagnostics. */
const char *texture_pack_stage_id(unsigned stage);
/* Replacements resident now and the budget they share (cache_mb), in bytes; 0 without a pack. */
void texture_pack_stats(uint64_t *resident_bytes, uint64_t *budget_bytes);
#endif
