// Meshtastic Font Pack (.mfp) reader, see fontpack.h and SPEC.md
#include "fontpack.h"

#include <string.h>

#define FP_HEADER_SIZE 64
#define FP_RANGE_SIZE 12
#define FP_GLYPH_SIZE 8

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
    if (rd16(h + 4) != 1)
        return FP_ERR_VERSION;

    uint16_t header_size = rd16(h + 6);
    pack->file_size = rd32(h + 8);
    pack->crc32 = rd32(h + 12);
    uint8_t bpp = h[16];
    pack->line_height = h[17];
    pack->baseline = h[18];
    pack->max_advance = h[19];
    pack->range_count = rd16(h + 20);
    pack->glyph_count = rd32(h + 24);
    pack->ranges_offset = rd32(h + 28);
    pack->glyphs_offset = rd32(h + 32);
    pack->bitmaps_offset = rd32(h + 36);
    pack->bitmaps_size = rd32(h + 40);
    pack->meta_offset = rd32(h + 44);
    pack->meta_size = rd32(h + 48);

    uint32_t size = pack->file_size;
    if (header_size < FP_HEADER_SIZE || size < header_size || size > store_size || bpp != 1 || pack->line_height == 0 ||
        pack->range_count == 0 || pack->bitmaps_size >= (1u << 24) ||
        !inside(pack->ranges_offset, (uint64_t)pack->range_count * FP_RANGE_SIZE, size) ||
        !inside(pack->glyphs_offset, (uint64_t)pack->glyph_count * FP_GLYPH_SIZE, size) ||
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

bool fp_find(const fp_pack *pack, uint32_t cp, fp_glyph *glyph)
{
    if (pack->read == NULL)
        return false;

    // Binary search for the last range with first_cp <= cp
    uint8_t r[FP_RANGE_SIZE], best[FP_RANGE_SIZE];
    uint32_t lo = 0, hi = pack->range_count;
    bool found = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (!pack->read(pack->ctx, pack->ranges_offset + mid * FP_RANGE_SIZE, r, FP_RANGE_SIZE))
            return false;
        if (rd32(r) <= cp) {
            memcpy(best, r, FP_RANGE_SIZE);
            found = true;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (!found)
        return false;
    uint32_t first = rd32(best), last = rd32(best + 4), first_glyph = rd32(best + 8);
    if (cp < first || cp > last)
        return false;
    uint64_t index = (uint64_t)first_glyph + (cp - first);
    if (index >= pack->glyph_count)
        return false;

    uint8_t g[FP_GLYPH_SIZE];
    if (!pack->read(pack->ctx, pack->glyphs_offset + (uint32_t)index * FP_GLYPH_SIZE, g, FP_GLYPH_SIZE))
        return false;
    uint32_t rel = (uint32_t)g[0] | ((uint32_t)g[1] << 8) | ((uint32_t)g[2] << 16);
    glyph->advance = g[3];
    glyph->width = g[4];
    glyph->height = g[5];
    glyph->x_offset = (int8_t)g[6];
    glyph->y_offset = (int8_t)g[7];
    if (glyph->width == 0 || glyph->height == 0) {
        glyph->width = glyph->height = 0;
    } else if (!inside(rel, fp_bitmap_size(glyph), pack->bitmaps_size)) {
        return false;
    }
    glyph->bitmap_offset = pack->bitmaps_offset + rel;
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
