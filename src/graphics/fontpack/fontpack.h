// Meshtastic Font Pack (.mfp) reader, see SPEC.md. Plain C99, no allocation: the pack stays in its
// storage and every lookup reads it through the read callback. Every offset is checked, so a corrupt
// or hostile pack gives missing glyphs, never an out-of-bounds read.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads len bytes at offset off of the storage into buf; false on failure
typedef bool (*fp_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t len);

typedef enum {
    FP_OK = 0,
    FP_ERR_READ,    // the storage failed
    FP_ERR_MAGIC,   // not a font pack
    FP_ERR_VERSION, // a format version this reader doesn't know
    FP_ERR_FORMAT,  // a field or a table out of bounds
    FP_ERR_CRC,     // the content doesn't match its CRC
} fp_result;

typedef struct {
    fp_read_fn read;
    void *ctx;
    uint32_t file_size;
    uint32_t crc32;
    uint8_t line_height;
    uint8_t baseline;
    uint8_t max_advance;
    uint16_t range_count;
    uint32_t glyph_count;
    uint32_t ranges_offset;
    uint32_t glyphs_offset;
    uint32_t bitmaps_offset;
    uint32_t bitmaps_size;
    uint32_t meta_offset;
    uint32_t meta_size;
} fp_pack;

typedef struct {
    uint32_t bitmap_offset; // absolute, in the storage
    uint8_t advance;
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    int8_t y_offset;
} fp_glyph;

// Reads and checks the header and table bounds. store_size is the size of the storage (file or region)
fp_result fp_open(fp_pack *pack, fp_read_fn read, void *ctx, uint32_t store_size);

// Checks the CRC (reads the whole pack, 256 bytes at a time)
fp_result fp_verify(const fp_pack *pack);

// The glyph of a code point; false when the pack doesn't have it
bool fp_find(const fp_pack *pack, uint32_t cp, fp_glyph *glyph);

// The bytes a glyph's bitmap takes: ceil(width * height / 8)
uint32_t fp_bitmap_size(const fp_glyph *glyph);

// Reads a glyph's bitmap into buf (at least fp_bitmap_size bytes)
bool fp_read_bitmap(const fp_pack *pack, const fp_glyph *glyph, uint8_t *buf, uint32_t buf_size);

// Pixel (x, y) of a glyph bitmap
static inline bool fp_pixel(const uint8_t *bits, uint8_t width, int x, int y)
{
    uint32_t n = (uint32_t)y * width + (uint32_t)x;
    return (bits[n >> 3] & (0x80 >> (n & 7))) != 0;
}

// Decodes the next code point of a UTF-8 string and advances *s (never past end). An invalid sequence
// (truncated, overlong, a surrogate, above U+10FFFF) gives U+FFFD and advances by one byte
uint32_t fp_utf8_next(const char **s, const char *end);

// CRC-32 (IEEE, as zlib), to chain over chunks: start with crc = 0
uint32_t fp_crc32(uint32_t crc, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
