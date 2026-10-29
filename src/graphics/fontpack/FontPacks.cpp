#include "graphics/fontpack/FontPacks.h"

#if HAS_FONTPACKS

#include "SPILock.h"
#include "graphics/fontpack/FileFontStore.h"
#include "graphics/fontpack/fontpack.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <strings.h>

#ifndef FONTPACK_MAX_PACKS
#define FONTPACK_MAX_PACKS 4
#endif
// Glyphs kept in RAM (direct-mapped by code point), and the largest bitmap a cache entry holds (32 bytes: 16x16)
#ifndef FONTPACK_CACHE_SIZE
#define FONTPACK_CACHE_SIZE 64
#endif
#ifndef FONTPACK_CACHE_BITMAP
#define FONTPACK_CACHE_BITMAP 32
#endif
// A pack's metrics table (5 bytes per distinct glyph metrics) is kept in RAM up to this size; larger ones are read from
// the pack
#ifndef FONTPACK_MAX_METRICS_RAM
#define FONTPACK_MAX_METRICS_RAM 4096
#endif
// Larger glyphs are read on demand into a stack buffer this size (48x48); glyphs larger still are drawn as a box
#define FONTPACK_MAX_BITMAP 288

namespace graphics
{
namespace FontPacks
{

namespace
{

struct Pack {
    FileFontStore store;
    fp_pack pack;
    uint8_t *metrics = nullptr; // the pack's metrics table in RAM, when small enough
    ~Pack() { free(metrics); }
};

// Plain data: the cache is allocated with calloc (a failed allocation must not abort the device)
struct CacheEntry {
    uint32_t cp;      // UINT32_MAX: empty
    int8_t packIndex; // -1: no pack has this code point
    bool bitmapCached;
    fp_glyph glyph;
    uint8_t bitmap[FONTPACK_CACHE_BITMAP];
};

void clearEntry(CacheEntry &e, uint32_t cp)
{
    memset(&e, 0, sizeof(e));
    e.cp = cp;
    e.packIndex = -1;
}

std::unique_ptr<Pack> packs[FONTPACK_MAX_PACKS];
int packCount = 0;
bool loaded = false;
std::atomic<bool> reloadRequested{false};
int maxLineHeight = 0;
// Allocated only when a pack is installed: no RAM is used otherwise
CacheEntry *cache = nullptr;

bool storeRead(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    return static_cast<FontStore *>(ctx)->read(off, buf, len);
}

const char *resultName(fp_result r)
{
    switch (r) {
    case FP_OK:
        return "OK";
    case FP_ERR_READ:
        return "read error";
    case FP_ERR_MAGIC:
        return "not a font pack";
    case FP_ERR_VERSION:
        return "unknown format version";
    case FP_ERR_FORMAT:
        return "malformed";
    case FP_ERR_CRC:
        return "CRC mismatch";
    }
    return "?";
}

bool endsWith(const char *s, const char *suffix)
{
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

void loadPacks()
{
    loaded = true;
    packCount = 0;
    maxLineHeight = 0;
    free(cache);
    cache = nullptr;

    std::vector<meshtastic_FileInfo> files;
    {
        concurrency::LockGuard g(spiLock);
        ensureDirLocked();
        files = getFiles(kFontDir, 0, FONTPACK_MAX_PACKS * 2);
    }
    for (const auto &info : files) {
        if (packCount >= FONTPACK_MAX_PACKS)
            break;
        // ESP32 lists full paths, the other platforms bare names
        const char *slash = strrchr(info.file_name, '/');
        const char *name = slash ? slash + 1 : info.file_name;
        if (!endsWith(name, ".mfp"))
            continue;
        char path[64];
        snprintf(path, sizeof(path), "%s/%s", kFontDir, name);

        std::unique_ptr<Pack> p(new Pack());
        if (!p->store.open(path)) {
            LOG_WARN("FontPacks: can't open %s", path);
            continue;
        }
        const uint32_t start = millis();
        fp_result r = fp_open(&p->pack, storeRead, &p->store, p->store.size());
        if (r == FP_OK)
            r = fp_verify(&p->pack);
        if (r != FP_OK) {
            LOG_WARN("FontPacks: %s not used: %s", path, resultName(r));
            continue;
        }
        // Each lookup adds up the metrics of up to 63 glyphs: from RAM, not from the file, when it can
        const uint32_t metricsSize = fp_metrics_size(&p->pack);
        if (metricsSize <= FONTPACK_MAX_METRICS_RAM) {
            p->metrics = static_cast<uint8_t *>(malloc(metricsSize));
            if (p->metrics && !fp_load_metrics(&p->pack, p->metrics, metricsSize)) {
                free(p->metrics);
                p->metrics = nullptr;
            }
        }
        LOG_INFO("FontPacks: %s: %u glyphs, %u px lines, metrics %s, checked in %u ms", path, (unsigned)p->pack.glyph_count,
                 (unsigned)p->pack.line_height, p->metrics ? "in RAM" : "read from the pack", (unsigned)(millis() - start));
        if (p->pack.line_height > maxLineHeight)
            maxLineHeight = p->pack.line_height;
        packs[packCount++] = std::move(p);
    }
    if (packCount == 0) {
        LOG_DEBUG("FontPacks: none installed in %s", kFontDir);
        return;
    }
    cache = static_cast<CacheEntry *>(calloc(FONTPACK_CACHE_SIZE, sizeof(CacheEntry)));
    if (!cache) {
        LOG_WARN("FontPacks: no memory for the glyph cache, packs not used");
        for (auto &p : packs)
            p.reset();
        packCount = 0;
        maxLineHeight = 0;
        return;
    }
    for (int i = 0; i < FONTPACK_CACHE_SIZE; i++)
        clearEntry(cache[i], UINT32_MAX);
}

/// The cache entry of a code point, looked up in the packs (in order) when it isn't cached
CacheEntry &lookup(uint32_t cp)
{
    CacheEntry &e = cache[cp % FONTPACK_CACHE_SIZE];
    if (e.cp == cp)
        return e;
    clearEntry(e, cp);
    for (int i = 0; i < packCount; i++) {
        if (fp_find(&packs[i]->pack, cp, &e.glyph)) {
            e.packIndex = i;
            if (fp_bitmap_size(&e.glyph) <= sizeof(e.bitmap))
                e.bitmapCached = fp_read_bitmap(&packs[i]->pack, &e.glyph, e.bitmap, sizeof(e.bitmap));
            break;
        }
    }
    return e;
}

int tofuWidth()
{
    return maxLineHeight > 0 ? maxLineHeight * 2 / 3 : 8;
}

} // namespace

bool available()
{
    if (reloadRequested.exchange(false))
        reload();
    else if (!loaded)
        loadPacks();
    return packCount > 0;
}

void reload()
{
    for (auto &p : packs)
        p.reset();
    loadPacks();
}

void requestReload()
{
    reloadRequested = true;
}

bool isPackPath(const char *path)
{
    const size_t n = strlen(kFontDir);
    return path && strncmp(path, kFontDir, n) == 0 && path[n] == '/' && !strchr(path + n + 1, '/') &&
           endsWith(path + n + 1, ".mfp") && strlen(path + n + 1) > 4;
}

void ensureDirLocked()
{
    if (!FSCom.exists(kFontDir))
        FSCom.mkdir(kFontDir);
}

bool builtinCovers(uint32_t cp)
{
    if (cp < 0x100) // ASCII and Latin-1: every built-in font
        return true;
#if defined(OLED_RU) || defined(OLED_UA)
    if (cp >= 0x400 && cp <= 0x45F)
        return true;
#endif
#if defined(OLED_GR)
    if (cp >= 0x370 && cp <= 0x3FF)
        return true;
#endif
#if defined(OLED_PL) || defined(OLED_CS)
    if (cp >= 0x100 && cp <= 0x17F)
        return true;
#endif
    return false;
}

int advance(uint32_t cp)
{
    if (!available())
        return 0;
    const CacheEntry &e = lookup(cp);
    return e.packIndex >= 0 ? e.glyph.advance : tofuWidth();
}

int lineHeight()
{
    return available() ? maxLineHeight : 0;
}

int draw(OLEDDisplay *display, int x, int y, int fontHeight, uint32_t cp)
{
    if (!available())
        return 0;
    const CacheEntry &e = lookup(cp);
    // The pack's line box, centred on the built-in font's line
    const int lh = e.packIndex >= 0 ? packs[e.packIndex]->pack.line_height : maxLineHeight;
    const int top = y + (fontHeight - lh) / 2;

    const uint8_t *bits = nullptr;
    uint8_t buf[FONTPACK_MAX_BITMAP];
    if (e.packIndex >= 0) {
        if (e.glyph.width == 0)
            return e.glyph.advance; // blank (a space)
        if (e.bitmapCached)
            bits = e.bitmap;
        else if (fp_read_bitmap(&packs[e.packIndex]->pack, &e.glyph, buf, sizeof(buf)))
            bits = buf;
    }
    if (!bits) {
        // Tofu: an empty box from a quarter of the line to the bottom of the ink
        const int w = tofuWidth();
        const int boxTop = top + lh / 4, boxH = lh - lh / 4 - 2;
        display->drawRect(x + 1, boxTop, w - 2, boxH);
        return e.packIndex >= 0 ? e.glyph.advance : w;
    }

    const int gx = x + e.glyph.x_offset, gy = top + e.glyph.y_offset;
    for (int row = 0; row < e.glyph.height; row++)
        for (int col = 0; col < e.glyph.width; col++)
            if (fp_pixel(bits, e.glyph.width, col, row))
                display->setPixel(gx + col, gy + row);
    return e.glyph.advance;
}

uint32_t decodeUtf8(const char *s, size_t len, size_t &charLen)
{
    const char *p = s;
    const uint32_t cp = fp_utf8_next(&p, s + len);
    charLen = p - s;
    return cp;
}

} // namespace FontPacks
} // namespace graphics

#endif
