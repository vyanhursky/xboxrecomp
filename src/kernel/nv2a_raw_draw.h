/* Bounded raw vertex uploads. No retained guest pointers or content cache.
 * The executor owns ordering; the backend copies the span during the call. */
#ifndef XBOXRECOMP_NV2A_RAW_DRAW_H
#define XBOXRECOMP_NV2A_RAW_DRAW_H
#include <stdint.h>
#include <string.h>
#ifdef _MSC_VER
#define NV2A_RAW_INLINE __inline
#else
#define NV2A_RAW_INLINE inline
#endif

typedef struct {
    uint32_t offset, type, size, stride;
} Nv2aRawAttr;

/* Zero-initialize before filling: compared in full as a shader/layout key. */
typedef struct {
    uint32_t used, streams, stride[16];
    uint32_t binding[16], offset[16], type[16], size[16];
} Nv2aRawLayout;

typedef struct {
    Nv2aRawLayout layout;
    uint32_t source[16], source_stride[16], source_extent[16], first, nverts, bytes;
} Nv2aRawPlan;

typedef struct {
    Nv2aRawLayout layout;
    const void *vertices;
    uint32_t nverts, first;
} Nv2aRawDraw;

/* Bytes the native IA fetches, including padding for three short/byte
 * components. Such padding must fit inside the validated record/span. */
static NV2A_RAW_INLINE uint32_t nv2a_raw_fetch_bytes(uint32_t type, uint32_t size)
{
    if (!size || size > 4) return 0;
    switch (type) {
    case 0: case 4: case 6: return 4;
    case 2: return size * 4;
    case 1: case 5: return size <= 2 ? size * 2 : 8;
    default: return 0;
    }
}

static NV2A_RAW_INLINE uint32_t nv2a_raw_source_bytes(uint32_t type, uint32_t size)
{
    if (!nv2a_raw_fetch_bytes(type, size)) return 0;
    if (type == 1 || type == 5) return size * 2;
    if (type == 4) return size;
    return nv2a_raw_fetch_bytes(type, size);
}

/* One owned upload, with each native stream starting on a 16-byte boundary.
 * These offsets depend on vertex count and are NOT part of layout identity. */
static NV2A_RAW_INLINE uint32_t nv2a_raw_spans(const Nv2aRawLayout *raw,
                                              uint32_t nverts, uint32_t *offsets,
                                              uint32_t *sizes)
{
    uint32_t s, a, extent[16] = {0};
    uint64_t total = 0;
    if (!raw->streams || raw->streams > 16 || !nverts || nverts > 65536u) return 0;
    for (a = 0; a < 16; ++a) if (raw->used & (1u << a)) {
        uint32_t binding = raw->binding[a], n = nv2a_raw_fetch_bytes(raw->type[a], raw->size[a]);
        uint32_t align = raw->type[a] == 2 || raw->type[a] == 6 ? 4 :
                         raw->type[a] == 1 || raw->type[a] == 5 ? 2 : 1;
        if (!n || binding >= raw->streams || raw->offset[a] > 2047u ||
            raw->offset[a] % align || raw->stride[binding] % align) return 0;
        if (raw->offset[a] + n > extent[binding]) extent[binding] = raw->offset[a] + n;
    }
    for (s = 0; s < raw->streams; ++s) {
        uint64_t bytes = (uint64_t)(nverts - 1) * raw->stride[s] + extent[s];
        if (!extent[s] || !raw->stride[s] || raw->stride[s] > 2048u) return 0;
        if (offsets) offsets[s] = (uint32_t)total;
        if (sizes) sizes[s] = (uint32_t)bytes;
        total += (bytes + 15u) & ~(uint64_t)15u;
        if (total > 4u * 1024u * 1024u) return 0;
    }
    return (uint32_t)total;
}

static NV2A_RAW_INLINE uint32_t nv2a_raw_offsets(const Nv2aRawLayout *raw,
                                                uint32_t nverts, uint32_t *offsets)
{
    return nv2a_raw_spans(raw, nverts, offsets, NULL);
}

/* Does not dereference guest data. Caller must validate the entire returned
 * byte span with its guest/inline memory policy before forming a pointer. */
static NV2A_RAW_INLINE int nv2a_raw_plan(const Nv2aRawAttr attr[16], uint32_t used,
                               const uint32_t *indices, uint32_t count,
                               Nv2aRawPlan *out)
{
    Nv2aRawPlan p;
    uint32_t a, i, s, nattr = 0, lo = UINT32_MAX, hi = 0;
    uint32_t base[16] = {0}, end[16] = {0};
    if (!used || (used & ~65535u) || !indices || !count || count > 65536u || !out)
        return 0;
    memset(&p, 0, sizeof p);
    p.layout.used = used;
    for (a = 0; a < 16; ++a) if (used & (1u << a)) {
        uint32_t n = nv2a_raw_fetch_bytes(attr[a].type, attr[a].size);
        uint32_t stride = attr[a].stride;
        ++nattr;
        if (!n || !stride || stride > 2048u) return 0;
        /* Group nearby interleaved attributes; separate arrays keep their
         * own binding/stride. Never infer sharing from a guest address alone. */
        for (s = 0; s < p.layout.streams; ++s) {
            uint32_t b = attr[a].offset < base[s] ? attr[a].offset : base[s];
            uint64_t old_end = (uint64_t)base[s] - b + end[s];
            uint64_t new_end = (uint64_t)attr[a].offset - b + n;
            if (p.layout.stride[s] == stride && !((attr[a].offset - base[s]) & 3u) &&
                old_end <= stride && new_end <= stride) {
                base[s] = b;
                end[s] = (uint32_t)(old_end > new_end ? old_end : new_end);
                break;
            }
        }
        if (s == p.layout.streams) {
            if (s == 16) return 0;
            ++p.layout.streams;
            p.layout.stride[s] = stride;
            base[s] = attr[a].offset;
            end[s] = n;
        }
        p.layout.binding[a] = s;
        p.layout.type[a] = attr[a].type;
        p.layout.size[a] = attr[a].size;
    }
    for (s = 0; s < p.layout.streams; ++s) p.source_stride[s] = p.layout.stride[s];
    for (a = 0; a < 16; ++a) if (used & (1u << a)) {
        uint32_t binding = p.layout.binding[a];
        p.layout.offset[a] = attr[a].offset - base[p.layout.binding[a]];
        uint32_t source_end = p.layout.offset[a] + nv2a_raw_source_bytes(attr[a].type, attr[a].size);
        uint32_t native_end = p.layout.offset[a] + nv2a_raw_fetch_bytes(attr[a].type, attr[a].size);
        if (source_end > p.source_extent[binding]) p.source_extent[binding] = source_end;
        if (native_end > p.layout.stride[binding]) p.layout.stride[binding] = native_end;
    }
    /* Padded native strides also work on portability drivers that require
     * four-byte alignment and forbid attribute fetches beyond a record. */
    for (s = 0; s < p.layout.streams; ++s)
        p.layout.stride[s] = (p.layout.stride[s] + 3u) & ~3u;
    for (i = 0; i < count; ++i) {
        if (indices[i] < lo) lo = indices[i];
        if (indices[i] > hi) hi = indices[i];
    }
    if (hi - lo >= 65536u || (uint64_t)hi - lo + 1 > (uint64_t)count * 4u)
        return 0; /* bound sparse-range upload amplification */
    p.nverts = hi - lo + 1;
    p.bytes = nv2a_raw_offsets(&p.layout, p.nverts, NULL);
    if (!p.bytes || p.bytes > (uint64_t)count * nattr * 16u) return 0;
    for (s = 0; s < p.layout.streams; ++s) {
        uint64_t at = (uint64_t)base[s] + (uint64_t)lo * p.source_stride[s];
        if (at > UINT32_MAX) return 0;
        p.source[s] = (uint32_t)at;
    }
    p.first = lo;
    *out = p;
    return 1;
}

/* Repack only byte records that the native IA cannot consume directly;
 * no float expansion or normalization. Common SHORT3 and UBYTE3 loops use
 * fixed-size copies, not a variable-size library call for every vertex. */
static NV2A_RAW_INLINE void nv2a_raw_copy_rows(unsigned char *dst, const unsigned char *src,
                                              uint32_t count, uint32_t ds, uint32_t ss,
                                              uint32_t de, uint32_t se)
{
    uint32_t v;
    if (ds == ss) {
        uint32_t bytes = (count - 1) * ss + se;
        memcpy(dst, src, bytes);
        memset(dst + bytes, 0, de - se);
    } else if (ss == 6 && ds == 8 && se == 6 && de == 8) {
        for (v = 0; v < count; ++v) {
            memcpy(dst, src, 4); memcpy(dst + 4, src + 4, 2); dst[6] = dst[7] = 0;
            src += 6; dst += 8;
        }
    } else if (ss == 3 && ds == 4 && se == 3 && de == 4) {
        for (v = 0; v < count; ++v) {
            memcpy(dst, src, 2); dst[2] = src[2]; dst[3] = 0; src += 3; dst += 4;
        }
    } else for (v = 0; v < count; ++v) {
        memcpy(dst, src, se); memset(dst + se, 0, de - se); dst += ds; src += ss;
    }
}

/* Backend rejection gathers the SAME owned snapshot, not guest memory that
 * may have changed while textures/shaders were prepared. */
static NV2A_RAW_INLINE void nv2a_raw_gather(const Nv2aRawDraw *raw, float *dst)
{
    uint32_t vi, a, c, offsets[16];
    if (!nv2a_raw_offsets(&raw->layout, raw->nverts, offsets)) return;
    for (vi = 0; vi < raw->nverts; ++vi) for (a = 0; a < 16; ++a) if (raw->layout.used & (1u << a)) {
        uint32_t binding = raw->layout.binding[a];
        const unsigned char *p = (const unsigned char *)raw->vertices + offsets[binding] +
                                (size_t)vi * raw->layout.stride[binding] + raw->layout.offset[a];
        uint32_t type = raw->layout.type[a], n = raw->layout.size[a];
        dst[0] = dst[1] = dst[2] = 0; dst[3] = 1;
        if (type == 0) {
            dst[0] = p[2] / 255.0f; dst[1] = p[1] / 255.0f;
            dst[2] = p[0] / 255.0f; dst[3] = p[3] / 255.0f;
        } else if (type == 6) {
            uint32_t w; memcpy(&w, p, 4);
            dst[0] = (float)((int32_t)(w << 21) >> 21) / 1023.0f;
            dst[1] = (float)((int32_t)(w << 10) >> 21) / 1023.0f;
            dst[2] = (float)((int32_t)w >> 22) / 511.0f;
        } else for (c = 0; c < n; ++c) {
            if (type == 2) memcpy(dst + c, p + c * 4, 4);
            else if (type == 4) dst[c] = p[c] / 255.0f;
            else {
                int16_t s; memcpy(&s, p + c * 2, 2);
                dst[c] = type == 1 ? s / 32767.0f : (float)s;
            }
        }
        dst += 4;
    }
}
#endif
