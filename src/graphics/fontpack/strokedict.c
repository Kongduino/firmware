// Meshtastic Stroke Dictionary (.msd) reader, format version 1: see strokedict.h and STROKES.md
#include "strokedict.h"
#include "fontpack.h" // fp_crc32

#include <string.h>

#define SD_VERSION 1
#define SD_HEADER_SIZE 48
#define SD_CHECKPOINT 64
#define SD_MAX_TOP_DEPTH 4
#define SD_MAX_TOP_K 64
#define SD_UNRANKED 0xFFFF
// The longest entry: n, 63 strokes in 24 bytes, a code point, a rank
#define SD_MAX_ENTRY (1 + 24 + 3 + 2)

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t rd32(const uint8_t *p)
{
    return rd24(p) | ((uint32_t)p[3] << 24);
}

// A code point a candidate can be: not 0, not a surrogate, at most U+10FFFF
static bool valid_cp(uint32_t cp)
{
    return cp != 0 && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

// True when [off, off + len) lies inside [0, size), without overflow
static bool inside(uint32_t off, uint64_t len, uint32_t size)
{
    return off <= size && len <= (uint64_t)(size - off);
}

// Top-table slots for 1 to depth strokes: 5 + 25 + ...
static uint32_t top_slots(uint8_t depth)
{
    uint32_t n = 0, p = 1;
    for (uint8_t d = 1; d <= depth; d++) {
        p *= 5;
        n += p;
    }
    return n;
}

sd_result sd_open(sd_dict *dict, sd_read_fn read, void *ctx, uint32_t store_size)
{
    uint8_t h[SD_HEADER_SIZE];
    memset(dict, 0, sizeof(*dict));
    if (store_size < SD_HEADER_SIZE || !read(ctx, 0, h, sizeof(h)))
        return SD_ERR_READ;
    if (memcmp(h, "MSDK", 4) != 0)
        return SD_ERR_MAGIC;
    if (rd16(h + 4) != SD_VERSION)
        return SD_ERR_VERSION;
    dict->read = read;
    dict->ctx = ctx;
    dict->file_size = rd32(h + 8);
    dict->crc32 = rd32(h + 12);
    dict->entry_count = rd32(h + 16);
    dict->entries_offset = rd32(h + 20);
    dict->entries_size = rd32(h + 24);
    dict->checkpoints_offset = rd32(h + 28);
    dict->top_offset = rd32(h + 32);
    dict->top_depth = h[36];
    dict->top_k = h[37];
    dict->max_strokes = h[38];
    dict->meta_offset = rd32(h + 40);
    dict->meta_size = rd32(h + 44);

    uint32_t size = dict->file_size;
    uint64_t checkpoints = ((uint64_t)dict->entry_count + SD_CHECKPOINT - 1) / SD_CHECKPOINT;
    if (rd16(h + 6) < SD_HEADER_SIZE || size > store_size || size < rd16(h + 6) || dict->entry_count == 0 ||
        dict->top_depth > SD_MAX_TOP_DEPTH || dict->top_k == 0 || dict->top_k > SD_MAX_TOP_K || dict->max_strokes == 0 ||
        dict->max_strokes > SD_MAX_STROKES || (uint64_t)dict->entry_count * 7 > dict->entries_size ||
        !inside(dict->entries_offset, dict->entries_size, size) ||
        !inside(dict->checkpoints_offset, checkpoints * 4, size) ||
        !inside(dict->top_offset, (uint64_t)top_slots(dict->top_depth) * dict->top_k * 3, size) ||
        !inside(dict->meta_offset, dict->meta_size, size))
        return SD_ERR_FORMAT;
    return SD_OK;
}

sd_result sd_verify(const sd_dict *dict)
{
    uint8_t buf[256];
    uint32_t crc = 0;
    for (uint32_t off = 16; off < dict->file_size;) {
        uint32_t n = dict->file_size - off;
        if (n > sizeof(buf))
            n = sizeof(buf);
        if (!dict->read(dict->ctx, off, buf, n))
            return SD_ERR_READ;
        crc = fp_crc32(crc, buf, n);
        off += n;
    }
    return crc == dict->crc32 ? SD_OK : SD_ERR_CRC;
}

uint8_t sd_stroke_value(char c)
{
    switch (c) {
    case 'h':
    case 'H':
    case '1':
        return 1;
    case 's':
    case 'S':
    case '2':
        return 2;
    case 'p':
    case 'P':
    case '3':
        return 3;
    case 'n':
    case 'N':
    case '4':
        return 4;
    case 'z':
    case 'Z':
    case '5':
        return 5;
    case '?':
    case '*':
    case '6':
        return SD_STROKE_ANY;
    default:
        return 0;
    }
}

typedef struct {
    uint8_t n;
    uint8_t strokes[SD_MAX_STROKES];
    uint32_t cp;
    uint16_t rank;
    uint32_t size; // bytes the entry takes
} entry;

// Decodes an entry from len bytes; false when it's malformed or doesn't fit
static bool parse_entry(const uint8_t *buf, uint32_t len, entry *e)
{
    if (len == 0)
        return false;
    e->n = buf[0];
    if (e->n == 0 || e->n > SD_MAX_STROKES)
        return false;
    uint32_t packed = (3u * e->n + 7) / 8;
    e->size = 1 + packed + 3 + 2;
    if (e->size > len)
        return false;
    for (uint8_t i = 0; i < e->n; i++) {
        uint32_t bit = 3u * i; // first stroke in the top bits of the first byte
        uint32_t word = ((uint32_t)buf[1 + bit / 8] << 8) | (bit / 8 + 1 < packed ? buf[2 + bit / 8] : 0);
        e->strokes[i] = (uint8_t)((word >> (13 - bit % 8)) & 7);
        if (e->strokes[i] == 0 || e->strokes[i] > SD_STROKE_MAX)
            return false;
    }
    e->cp = rd24(buf + 1 + packed);
    e->rank = rd16(buf + 4 + packed);
    return valid_cp(e->cp);
}

// Reads the entry at off (relative to the entries); false when it's unreadable or malformed
static bool read_entry(const sd_dict *dict, uint32_t off, entry *e)
{
    uint8_t buf[SD_MAX_ENTRY];
    if (off >= dict->entries_size)
        return false;
    uint32_t avail = dict->entries_size - off;
    uint32_t len = avail < sizeof(buf) ? avail : sizeof(buf);
    return dict->read(dict->ctx, dict->entries_offset + off, buf, len) && parse_entry(buf, len, e);
}

// Reads entries in order through a buffer: a scan reads 256 bytes at a time, not one entry
typedef struct {
    const sd_dict *dict;
    uint32_t start; // offset of buf in the entries
    uint32_t len;   // bytes in buf
    uint8_t buf[256];
} scanner;

static bool scan_entry(scanner *s, uint32_t off, entry *e)
{
    const sd_dict *dict = s->dict;
    if (off >= dict->entries_size)
        return false;
    bool in_buf = off >= s->start && off < s->start + s->len;
    bool whole = in_buf && (off - s->start + SD_MAX_ENTRY <= s->len || s->start + s->len == dict->entries_size);
    if (!whole) {
        uint32_t avail = dict->entries_size - off;
        s->len = avail < sizeof(s->buf) ? avail : sizeof(s->buf);
        s->start = off;
        if (!dict->read(dict->ctx, dict->entries_offset + off, s->buf, s->len)) {
            s->len = 0;
            return false;
        }
    }
    return parse_entry(s->buf + (off - s->start), s->start + s->len - off, e);
}

// True when the entry matches the whole input, wildcards included
static bool matches(const entry *e, const uint8_t *strokes, int n)
{
    if (e->n < n)
        return false;
    for (int i = 0; i < n; i++)
        if (strokes[i] != SD_STROKE_ANY && strokes[i] != e->strokes[i])
            return false;
    return true;
}

// < 0: the entry comes before every match of the prefix; 0: it matches; > 0: it comes after
static int compare(const entry *e, const uint8_t *prefix, int n)
{
    for (int i = 0; i < n; i++) {
        if (i >= e->n)
            return -1; // a shorter sequence the prefix starts with comes first
        if (e->strokes[i] != prefix[i])
            return e->strokes[i] < prefix[i] ? -1 : 1;
    }
    return 0;
}

typedef struct {
    uint32_t cp;
    uint16_t rank;
} candidate;

// Inserts a candidate into the best `limit` of list (sorted by rank, then code point), each code point once
static void keep_best(candidate *list, int *count, int limit, uint32_t cp, uint16_t rank)
{
    for (int i = 0; i < *count; i++) {
        if (list[i].cp == cp) {
            if (rank >= list[i].rank)
                return;
            memmove(&list[i], &list[i + 1], (size_t)(*count - i - 1) * sizeof(*list));
            (*count)--;
            break;
        }
    }
    int at = *count;
    while (at > 0 && (rank < list[at - 1].rank || (rank == list[at - 1].rank && cp < list[at - 1].cp)))
        at--;
    if (at >= limit)
        return;
    int moved = (*count < limit ? *count : limit - 1) - at;
    memmove(&list[at + 1], &list[at], (size_t)moved * sizeof(*list));
    list[at].cp = cp;
    list[at].rank = rank;
    if (*count < limit)
        (*count)++;
}

// The offset of the first entry at or after the prefix (entries_size when there's none)
static bool first_at_or_after(const sd_dict *dict, scanner *s, const uint8_t *prefix, int n, uint32_t *found)
{
    uint32_t blocks = (dict->entry_count + SD_CHECKPOINT - 1) / SD_CHECKPOINT;
    // The last checkpoint whose entry comes before the prefix: the first match is in its block or the next one
    uint32_t lo = 0, hi = blocks; // invariant: checkpoints before lo come before the prefix
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint8_t b[4];
        entry e;
        if (!dict->read(dict->ctx, dict->checkpoints_offset + mid * 4, b, 4) || !read_entry(dict, rd32(b), &e))
            return false;
        if (compare(&e, prefix, n) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    uint32_t off = 0, index = 0;
    if (lo > 0) {
        uint8_t b[4];
        if (!dict->read(dict->ctx, dict->checkpoints_offset + (lo - 1) * 4, b, 4))
            return false;
        off = rd32(b);
        index = (lo - 1) * SD_CHECKPOINT;
    }
    for (; index < dict->entry_count; index++) {
        entry e;
        if (!scan_entry(s, off, &e))
            return false;
        if (compare(&e, prefix, n) >= 0)
            break;
        off += e.size;
    }
    *found = index < dict->entry_count ? off : dict->entries_size;
    return true;
}

int sd_lookup(const sd_dict *dict, const uint8_t *strokes, int n, int skip, uint32_t *out, int max, uint32_t *total)
{
    if (total)
        *total = 0;
    if (n < 1 || n > SD_MAX_STROKES || skip < 0 || max < 1 || skip + max > SD_MAX_CANDIDATES)
        return 0;
    int fixed = n; // the strokes before the first wildcard
    for (int i = 0; i < n; i++) {
        if (strokes[i] == SD_STROKE_ANY && fixed == n)
            fixed = i;
        else if (strokes[i] < 1 || strokes[i] > SD_STROKE_ANY)
            return 0;
    }

    if (fixed == n && n <= dict->top_depth) {
        uint32_t slot = top_slots((uint8_t)(n - 1));
        uint32_t p = 1;
        for (int i = n - 1; i >= 0; i--, p *= 5)
            slot += (uint32_t)(strokes[i] - 1) * p;
        uint8_t buf[SD_MAX_TOP_K * 3];
        if (!dict->read(dict->ctx, dict->top_offset + slot * dict->top_k * 3, buf, dict->top_k * 3u))
            return 0;
        int written = 0, have = 0;
        for (int i = 0; i < dict->top_k; i++) {
            uint32_t cp = rd24(buf + i * 3);
            if (!valid_cp(cp))
                break; // 0 pads the slot; anything else invalid ends it too
            have++;
            if (i >= skip && written < max)
                out[written++] = cp;
        }
        if (total)
            *total = (uint32_t)have;
        return written;
    }

    // The entries that start with the strokes before the first wildcard (all of them for a leading wildcard), and
    // among them those that match the whole input
    scanner s = {dict, 0, 0, {0}};
    uint32_t off = 0;
    if (fixed > 0 && !first_at_or_after(dict, &s, strokes, fixed, &off))
        return 0;
    candidate best[SD_MAX_CANDIDATES];
    int count = 0, limit = skip + max;
    uint32_t found = 0;
    while (off < dict->entries_size) {
        entry e;
        if (!scan_entry(&s, off, &e) || compare(&e, strokes, fixed) != 0)
            break;
        if (matches(&e, strokes, n)) {
            found++;
            keep_best(best, &count, limit, e.cp, e.rank);
        }
        off += e.size;
    }
    if (total)
        *total = found;
    int written = 0;
    for (int i = skip; i < count; i++)
        out[written++] = best[i].cp;
    return written;
}
