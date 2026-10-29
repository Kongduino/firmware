// Meshtastic Font Pack (.mfp) reader, format version 2: see fontpack.h and SPEC.md
#include "fontpack.h"

#include <string.h>

#define FP_VERSION 2
#define FP_HEADER_SIZE 80
#define FP_PLANE_SIZE 8
#define FP_MAX_PLANES 17
#define FP_RANGE_SIZE 3
#define FP_METRICS_SIZE 5
#define FP_CHECKPOINT 64

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// True when [off, off + len) lies inside [0, size), without overflow
static bool inside(uint32_t off, uint64_t len, uint32_t size)
{
    return off <= size && len <= (uint64_t)(size - off);
}

// The checkpoints of n entries
static uint64_t blocks(uint32_t n)
{
    return ((uint64_t)n + FP_CHECKPOINT - 1) / FP_CHECKPOINT;
}

fp_result fp_open(fp_pack *pack, fp_read_fn read, void *ctx, uint32_t store_size)
{
    uint8_t h[FP_HEADER_SIZE];
    memset(pack, 0, sizeof(*pack));
    if (store_size < FP_HEADER_SIZE)
        return FP_ERR_FORMAT;
    if (!read(ctx, 0, h, FP_HEADER_SIZE))
        return FP_ERR_READ;
    if (memcmp(h, "MFPK", 4) != 0)
        return FP_ERR_MAGIC;
    if (rd16(h + 4) != FP_VERSION)
        return FP_ERR_VERSION;

    uint16_t header_size = rd16(h + 6);
    pack->file_size = rd32(h + 8);
    pack->crc32 = rd32(h + 12);
    uint8_t bpp = h[16];
    pack->line_height = h[17];
    pack->baseline = h[18];
    pack->max_advance = h[19];
    pack->plane_count = h[20];
    pack->metric_index_size = h[21];
    pack->metrics_count = rd16(h + 22);
    pack->glyph_count = rd32(h + 24);
    pack->range_count = rd32(h + 28);
    pack->planes_offset = rd32(h + 32);
    pack->ranges_offset = rd32(h + 36);
    pack->range_checkpoints_offset = rd32(h + 40);
    pack->metrics_offset = rd32(h + 44);
    pack->glyphs_offset = rd32(h + 48);
    pack->glyph_checkpoints_offset = rd32(h + 52);
    pack->bitmaps_offset = rd32(h + 56);
    pack->bitmaps_size = rd32(h + 60);
    pack->meta_offset = rd32(h + 64);
    pack->meta_size = rd32(h + 68);

    uint32_t size = pack->file_size;
    if (header_size < FP_HEADER_SIZE || size < header_size || size > store_size || bpp != 1 || pack->line_height == 0 ||
        pack->plane_count == 0 || pack->plane_count > FP_MAX_PLANES ||
        (pack->metric_index_size != 1 && pack->metric_index_size != 2) || pack->metrics_count == 0 ||
        (pack->metric_index_size == 1 && pack->metrics_count > 256) || pack->glyph_count == 0 || pack->range_count == 0)
        return FP_ERR_FORMAT;
    if (!inside(pack->planes_offset, (uint64_t)pack->plane_count * FP_PLANE_SIZE, size) ||
        !inside(pack->ranges_offset, (uint64_t)pack->range_count * FP_RANGE_SIZE, size) ||
        !inside(pack->range_checkpoints_offset, blocks(pack->range_count) * 4, size) ||
        !inside(pack->metrics_offset, (uint64_t)pack->metrics_count * FP_METRICS_SIZE, size) ||
        !inside(pack->glyphs_offset, (uint64_t)pack->glyph_count * pack->metric_index_size, size) ||
        !inside(pack->glyph_checkpoints_offset, blocks(pack->glyph_count) * 4, size) ||
        !inside(pack->bitmaps_offset, pack->bitmaps_size, size) || !inside(pack->meta_offset, pack->meta_size, size))
        return FP_ERR_FORMAT;

    pack->read = read;
    pack->ctx = ctx;
    return FP_OK;
}

fp_result fp_verify(const fp_pack *pack)
{
    uint8_t buf[256];
    uint32_t crc = 0;
    for (uint32_t off = 16; off < pack->file_size;) {
        uint32_t n = pack->file_size - off;
        if (n > sizeof(buf))
            n = sizeof(buf);
        if (!pack->read(pack->ctx, off, buf, n))
            return FP_ERR_READ;
        crc = fp_crc32(crc, buf, n);
        off += n;
    }
    return crc == pack->crc32 ? FP_OK : FP_ERR_CRC;
}

uint32_t fp_metrics_size(const fp_pack *pack)
{
    return (uint32_t)pack->metrics_count * FP_METRICS_SIZE;
}

bool fp_load_metrics(fp_pack *pack, uint8_t *buf, uint32_t buf_size)
{
    uint32_t n = fp_metrics_size(pack);
    if (pack->read == NULL || buf == NULL || buf_size < n || !pack->read(pack->ctx, pack->metrics_offset, buf, n))
        return false;
    pack->metrics = buf;
    return true;
}

// Entry i of the metrics table
static bool read_metrics(const fp_pack *pack, uint32_t i, uint8_t m[FP_METRICS_SIZE])
{
    if (i >= pack->metrics_count)
        return false;
    if (pack->metrics) {
        memcpy(m, pack->metrics + i * FP_METRICS_SIZE, FP_METRICS_SIZE);
        return true;
    }
    return pack->read(pack->ctx, pack->metrics_offset + i * FP_METRICS_SIZE, m, FP_METRICS_SIZE);
}

static uint32_t metrics_bitmap_size(const uint8_t m[FP_METRICS_SIZE])
{
    return ((uint32_t)m[1] * m[2] + 7) / 8;
}

// The ranges of a plane: [*first, *end) in the range table
static bool find_plane(const fp_pack *pack, uint8_t plane, uint32_t *first, uint32_t *end)
{
    uint8_t p[FP_MAX_PLANES * FP_PLANE_SIZE];
    if (!pack->read(pack->ctx, pack->planes_offset, p, (uint32_t)pack->plane_count * FP_PLANE_SIZE))
        return false;
    for (uint32_t i = 0; i < pack->plane_count; i++) {
        if (p[i * FP_PLANE_SIZE] != plane)
            continue;
        *first = rd32(p + i * FP_PLANE_SIZE + 4);
        *end = (i + 1 < pack->plane_count) ? rd32(p + (i + 1) * FP_PLANE_SIZE + 4) : pack->range_count;
        return *first <= *end && *end <= pack->range_count;
    }
    return false;
}

// The glyph index of a code point
static bool find_index(const fp_pack *pack, uint32_t cp, uint32_t *index)
{
    if (cp > 0x10FFFF)
        return false;
    uint32_t lo, hi;
    if (!find_plane(pack, (uint8_t)(cp >> 16), &lo, &hi))
        return false;

    // Binary search for the last range of the plane with first <= low
    const uint16_t low = (uint16_t)(cp & 0xFFFF);
    uint8_t r[FP_RANGE_SIZE];
    uint32_t found = 0;
    uint16_t first = 0;
    uint8_t countMinusOne = 0;
    bool any = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (!pack->read(pack->ctx, pack->ranges_offset + mid * FP_RANGE_SIZE, r, FP_RANGE_SIZE))
            return false;
        if (rd16(r) <= low) {
            found = mid, first = rd16(r), countMinusOne = r[2], any = true;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (!any || (uint32_t)(low - first) > countMinusOne)
        return false;

    // The range's block checkpoint, plus the code points of the ranges before it in the block
    uint32_t block = found / FP_CHECKPOINT;
    uint8_t c[4];
    if (!pack->read(pack->ctx, pack->range_checkpoints_offset + block * 4, c, 4))
        return false;
    uint64_t sum = rd32(c);
    uint32_t before = found - block * FP_CHECKPOINT;
    if (before > 0) {
        uint8_t b[(FP_CHECKPOINT - 1) * FP_RANGE_SIZE];
        if (!pack->read(pack->ctx, pack->ranges_offset + block * FP_CHECKPOINT * FP_RANGE_SIZE, b, before * FP_RANGE_SIZE))
            return false;
        for (uint32_t i = 0; i < before; i++)
            sum += (uint32_t)b[i * FP_RANGE_SIZE + 2] + 1;
    }
    sum += (uint32_t)(low - first);
    if (sum >= pack->glyph_count)
        return false;
    *index = (uint32_t)sum;
    return true;
}

bool fp_find(const fp_pack *pack, uint32_t cp, fp_glyph *glyph)
{
    uint32_t index;
    if (pack->read == NULL || !find_index(pack, cp, &index))
        return false;

    // The metrics indices of the glyph's block up to the glyph, and the block's bitmap checkpoint
    const uint32_t size = pack->metric_index_size;
    uint32_t block = index / FP_CHECKPOINT;
    uint32_t k = index - block * FP_CHECKPOINT;
    uint8_t ids[FP_CHECKPOINT * 2];
    uint8_t c[4];
    if (!pack->read(pack->ctx, pack->glyphs_offset + block * FP_CHECKPOINT * size, ids, (k + 1) * size) ||
        !pack->read(pack->ctx, pack->glyph_checkpoints_offset + block * 4, c, 4))
        return false;

    // The bitmap offset: the checkpoint plus the bitmaps of the glyphs before it in the block
    uint64_t offset = rd32(c);
    uint8_t m[FP_METRICS_SIZE];
    for (uint32_t j = 0; j <= k; j++) {
        uint32_t id = size == 1 ? ids[j] : rd16(ids + j * 2);
        if (!read_metrics(pack, id, m))
            return false;
        if (j < k)
            offset += metrics_bitmap_size(m);
    }

    glyph->advance = m[0];
    glyph->width = m[1];
    glyph->height = m[2];
    glyph->x_offset = (int8_t)m[3];
    glyph->y_offset = (int8_t)m[4];
    if (glyph->width == 0 || glyph->height == 0) {
        glyph->width = glyph->height = 0;
        glyph->bitmap_offset = pack->bitmaps_offset;
        return true;
    }
    if (offset >= pack->bitmaps_size || !inside((uint32_t)offset, fp_bitmap_size(glyph), pack->bitmaps_size))
        return false;
    glyph->bitmap_offset = pack->bitmaps_offset + (uint32_t)offset;
    return true;
}

uint32_t fp_bitmap_size(const fp_glyph *glyph)
{
    return ((uint32_t)glyph->width * glyph->height + 7) / 8;
}

bool fp_read_bitmap(const fp_pack *pack, const fp_glyph *glyph, uint8_t *buf, uint32_t buf_size)
{
    uint32_t n = fp_bitmap_size(glyph);
    if (n == 0)
        return true;
    if (n > buf_size)
        return false;
    return pack->read(pack->ctx, glyph->bitmap_offset, buf, n);
}

uint32_t fp_utf8_next(const char **s, const char *end)
{
    const uint8_t *p = (const uint8_t *)*s;
    const uint8_t *e = (const uint8_t *)end;
    if (p >= e)
        return 0;
    uint8_t c = *p;
    uint32_t cp, min;
    int len;
    if (c < 0x80) {
        *s += 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F, len = 2, min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F, len = 3, min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07, len = 4, min = 0x10000;
    } else {
        *s += 1;
        return 0xFFFD;
    }
    if (e - p < len) {
        *s += 1;
        return 0xFFFD;
    }
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *s += 1;
        return 0xFFFD;
    }
    *s += len;
    return cp;
}

uint32_t fp_crc32(uint32_t crc, const uint8_t *data, uint32_t len)
{
    // Half-byte table: 64 bytes of flash instead of 1 KB
    static const uint32_t t[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                   0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                   0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return ~crc;
}
