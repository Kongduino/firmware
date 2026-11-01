// Meshtastic Stroke Dictionary (.msd) reader, format version 1 (see STROKES.md). Plain C99, no allocation: the
// dictionary stays in its storage and every lookup reads it through the read callback. Every offset is checked, so
// a corrupt or hostile dictionary gives no candidates, never an out-of-bounds read.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads len bytes at offset off of the storage into buf; false on failure
typedef bool (*sd_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t len);

typedef enum {
    SD_OK = 0,
    SD_ERR_READ,    // the storage failed
    SD_ERR_MAGIC,   // not a stroke dictionary
    SD_ERR_VERSION, // a format version this reader doesn't know
    SD_ERR_FORMAT,  // a field or a part out of bounds
    SD_ERR_CRC,     // the content doesn't match its CRC
} sd_result;

// Stroke values: 横 1, 竖 2, 撇 3, 点/捺 4, 折 5; and in an input, any one stroke (a wildcard, for a stroke the user
// isn't sure of, or learned in another order)
#define SD_STROKE_MAX 5
#define SD_STROKE_ANY 6
#define SD_MAX_STROKES 63
// The most candidates one lookup can page through (skip + max)
#define SD_MAX_CANDIDATES 64

typedef struct {
    sd_read_fn read;
    void *ctx;
    uint32_t file_size;
    uint32_t crc32;
    uint32_t entry_count;
    uint32_t entries_offset;
    uint32_t entries_size;
    uint32_t checkpoints_offset;
    uint32_t top_offset;
    uint8_t top_depth;
    uint8_t top_k;
    uint8_t max_strokes;
    uint32_t meta_offset;
    uint32_t meta_size;
} sd_dict;

// Reads and checks the header and part bounds. store_size is the size of the storage (file or region)
sd_result sd_open(sd_dict *dict, sd_read_fn read, void *ctx, uint32_t store_size);

// Checks the CRC (reads the whole dictionary, 256 bytes at a time)
sd_result sd_verify(const sd_dict *dict);

// The candidates for the first n strokes of a character (values 1 to 5, or SD_STROKE_ANY), best first: writes the
// code points from index skip, at most max of them, into out and returns how many it wrote (0 when none match or the
// input is invalid). skip + max is limited to SD_MAX_CANDIDATES. *total, when not NULL, gets the number of
// candidates there are to page through: for inputs of up to top_depth strokes without a wildcard, the top table's
// (at most top_k); for the others, the matching entries.
// A wildcard makes the reader scan every entry that starts with the strokes before it: a leading wildcard scans the
// whole dictionary
int sd_lookup(const sd_dict *dict, const uint8_t *strokes, int n, int skip, uint32_t *out, int max, uint32_t *total);

// Stroke letter (h s p n z, or 1 to 5) to value 1 to 5, a wildcard (? * 6) to SD_STROKE_ANY; 0 for anything else
uint8_t sd_stroke_value(char c);

#ifdef __cplusplus
}
#endif
