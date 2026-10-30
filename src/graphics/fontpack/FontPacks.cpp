#include "graphics/fontpack/FontPacks.h"

#if HAS_FONTPACKS

#include "SPILock.h"
#include "concurrency/OSThread.h"
#include "graphics/fontpack/FileFontStore.h"
#include "graphics/fontpack/FlashSlots.h"
#include "graphics/fontpack/RawFlash.h"
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

/// Boards that keep their packs in a raw storage define their own fontPackFlash() (QspiRawFlash.cpp...)
__attribute__((weak)) RawFlash *fontPackFlash()
{
    return nullptr;
}

namespace FontPacks
{

namespace
{

struct Pack {
    std::unique_ptr<FontStore> store; // a file, or a flash slot
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

// Guards the packs and the cache: the screen thread draws while the API thread installs or removes packs. Taken before
// the SPI lock (the store reads take it)
concurrency::Lock &packLock()
{
    static concurrency::Lock lock;
    return lock;
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

/// The board's flash slots, when it keeps its packs in a raw storage (FONTPACK_FLASH_SLOTS)
FlashSlots *slots()
{
#if defined(FONTPACK_FLASH_SLOTS)
    if (!fontPackFlash())
        return nullptr;
    static FlashSlots s(*fontPackFlash(), FONTPACK_FLASH_BASE, FONTPACK_FLASH_SLOT_SIZE, FONTPACK_FLASH_SLOTS);
    return &s;
#else
    return nullptr;
#endif
}

// When the flash was last used (0: idle); FlashIdle lets it power down after 2 s
std::atomic<uint32_t> lastFlashUse{0};

void touchFlash()
{
    if (slots())
        lastFlashUse = millis() | 1;
}

class FlashIdle : public concurrency::OSThread
{
  public:
    FlashIdle() : OSThread("FontPackFlash") {}

  protected:
    int32_t runOnce() override;
};
FlashIdle *flashIdle = nullptr;

/// Adds a pack to the packs in use. Pack lock held
void addPack(std::unique_ptr<FontStore> store, const char *label)
{
    if (packCount >= FONTPACK_MAX_PACKS)
        return;
    std::unique_ptr<Pack> p(new Pack());
    p->store = std::move(store);
    // The CRC was checked at install (endReceive()); the reader checks every offset it reads
    fp_result r = fp_open(&p->pack, storeRead, p->store.get(), p->store->size());
    if (r != FP_OK) {
        LOG_WARN("FontPacks: %s not used: %s", label, resultName(r));
        return;
    }
    // Each lookup adds up the metrics of up to 63 glyphs: from RAM, not from the store, when it can
    const uint32_t metricsSize = fp_metrics_size(&p->pack);
    if (metricsSize <= FONTPACK_MAX_METRICS_RAM) {
        p->metrics = static_cast<uint8_t *>(malloc(metricsSize));
        if (p->metrics && !fp_load_metrics(&p->pack, p->metrics, metricsSize)) {
            free(p->metrics);
            p->metrics = nullptr;
        }
    }
    LOG_INFO("FontPacks: %s: %u glyphs, %u px lines, metrics %s", label, (unsigned)p->pack.glyph_count,
             (unsigned)p->pack.line_height, p->metrics ? "in RAM" : "read from the pack");
    if (p->pack.line_height > maxLineHeight)
        maxLineHeight = p->pack.line_height;
    packs[packCount++] = std::move(p);
}

/// Pack lock held
void loadPacks()
{
    loaded = true;
    packCount = 0;
    maxLineHeight = 0;
    free(cache);
    cache = nullptr;

    // Packs in the board's flash slots (the newest of a name)
    if (FlashSlots *s = slots()) {
        touchFlash();
        if (!flashIdle)
            flashIdle = new FlashIdle();
        for (int i = 0; i < s->count(); i++) {
            FlashSlots::Info in;
            if (!s->info(i, in) || s->find(in.name) != i)
                continue;
            char label[80];
            snprintf(label, sizeof(label), "slot %d (%s)", i, in.name);
            addPack(std::unique_ptr<FontStore>(new SlotFontStore(s->device(), s->packAddr(i), in.size)), label);
        }
    }

    // Packs in kFontDir
    std::vector<meshtastic_FileInfo> files;
    {
        concurrency::LockGuard g(spiLock);
        if (!FSCom.exists(kFontDir))
            FSCom.mkdir(kFontDir);
        files = getFiles(kFontDir, 0, FONTPACK_MAX_PACKS * 2);
    }
    for (const auto &info : files) {
        // ESP32 lists full paths, the other platforms bare names
        const char *slash = strrchr(info.file_name, '/');
        const char *name = slash ? slash + 1 : info.file_name;
        if (!endsWith(name, ".mfp"))
            continue;
        char path[64];
        snprintf(path, sizeof(path), "%s/%s", kFontDir, name);
        std::unique_ptr<FileFontStore> store(new FileFontStore());
        if (!store->open(path)) {
            LOG_WARN("FontPacks: can't open %s", path);
            continue;
        }
        addPack(std::move(store), path);
    }

    if (packCount == 0) {
        LOG_DEBUG("FontPacks: none installed");
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
    touchFlash();
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

/// Closes the packs (their files) and frees the cache; they are loaded again at their next use. Pack lock held
void closePacks()
{
    for (auto &p : packs)
        p.reset();
    packCount = 0;
    maxLineHeight = 0;
    free(cache);
    cache = nullptr;
    loaded = false;
}

/// Loads the packs when needed. Pack lock held
bool ensureLoaded()
{
    if (reloadRequested.exchange(false))
        closePacks();
    if (!loaded)
        loadPacks();
    return packCount > 0;
}

} // namespace

bool available()
{
    concurrency::LockGuard g(&packLock());
    return ensureLoaded();
}

void reload()
{
    concurrency::LockGuard g(&packLock());
    closePacks();
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

namespace
{

// The pack being received (beginReceive() to endReceive())
enum class Rx { None, File, Slot };
Rx rx = Rx::None;
char rxPackPath[80];
char rxPartPath[88];
File rxFile;
int rxSlot = -1;
uint32_t rxSize = 0;

const char *baseName(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/// Checks the pack in a store (structure and CRC)
fp_result checkPack(FontStore &store, const char *label)
{
    fp_pack pack;
    const uint32_t start = millis();
    fp_result r = fp_open(&pack, storeRead, &store, store.size());
    if (r == FP_OK)
        r = fp_verify(&pack);
    LOG_INFO("FontPacks: %s checked in %u ms: %s", label, (unsigned)(millis() - start), resultName(r));
    return r;
}

/// A received file pack: checks the partial file, then puts it in place of the pack of that name
bool installFile()
{
    {
        FileFontStore store;
        if (!store.open(rxPartPath)) {
            LOG_WARN("FontPacks: can't open %s", rxPartPath);
            return false;
        }
        if (checkPack(store, rxPartPath) != FP_OK) {
            store.close();
            concurrency::LockGuard s(spiLock);
            FSCom.remove(rxPartPath);
            return false;
        }
    }
    // Close the packs (one may be the pack being replaced), then put the new one in place
    concurrency::LockGuard g(&packLock());
    closePacks();
    bool ok;
    {
        concurrency::LockGuard s(spiLock);
        if (FSCom.exists(rxPackPath))
            FSCom.remove(rxPackPath);
        ok = FSCom.rename(rxPartPath, rxPackPath);
    }
    LOG_INFO("FontPacks: %s %s", rxPackPath, ok ? "installed" : "not installed (rename failed)");
    return ok;
}

/// A received slot pack: checks it in the flash, then makes it valid and frees the older pack of that name
bool installSlot()
{
    FlashSlots *s = slots();
    concurrency::LockGuard g(&packLock());
    touchFlash();
    char label[80];
    snprintf(label, sizeof(label), "slot %d", rxSlot);
    SlotFontStore store(s->device(), s->packAddr(rxSlot), rxSize);
    if (checkPack(store, label) != FP_OK)
        return false; // the slot stays without a header: free
    closePacks();
    const char *name = baseName(rxPackPath);
    const int old = s->find(name);
    if (!s->commit(rxSlot, name, rxSize)) {
        LOG_WARN("FontPacks: slot %d header not written", rxSlot);
        return false;
    }
    if (old >= 0 && old != rxSlot)
        s->clear(old);
    LOG_INFO("FontPacks: %s installed in slot %d (%u bytes)", name, rxSlot, (unsigned)rxSize);
    return true;
}

} // namespace

bool beginReceive(const char *packPath)
{
    abortReceive();
    snprintf(rxPackPath, sizeof(rxPackPath), "%s", packPath);
    rxSize = 0;
    if (FlashSlots *s = slots()) {
        concurrency::LockGuard g(&packLock());
        touchFlash();
        rxSlot = s->findFree();
        if (rxSlot < 0) {
            LOG_WARN("FontPacks: no free slot for %s: remove a pack first", packPath);
            return false;
        }
        if (!s->begin(rxSlot))
            return false;
        rx = Rx::Slot;
        LOG_INFO("FontPacks: receiving %s into slot %d", packPath, rxSlot);
        return true;
    }
    snprintf(rxPartPath, sizeof(rxPartPath), "%s.part", packPath);
    concurrency::LockGuard s(spiLock);
    if (!FSCom.exists(kFontDir))
        FSCom.mkdir(kFontDir);
    if (FSCom.exists(rxPartPath))
        FSCom.remove(rxPartPath);
    rxFile = FSCom.open(rxPartPath, FILE_O_WRITE);
    if (!rxFile)
        return false;
    rx = Rx::File;
    return true;
}

bool receive(const uint8_t *data, uint32_t len)
{
    bool ok = false;
    if (rx == Rx::Slot) {
        concurrency::LockGuard g(&packLock());
        touchFlash();
        ok = slots()->write(rxSlot, rxSize, data, len);
    } else if (rx == Rx::File) {
        concurrency::LockGuard s(spiLock);
        ok = rxFile.write(data, len) == len;
    }
    if (ok)
        rxSize += len;
    return ok;
}

bool endReceive()
{
    bool ok = false;
    if (rx == Rx::Slot) {
        ok = installSlot();
    } else if (rx == Rx::File) {
        {
            concurrency::LockGuard s(spiLock);
            rxFile.flush();
            rxFile.close();
        }
        ok = installFile();
    }
    rx = Rx::None;
    return ok;
}

void abortReceive()
{
    if (rx == Rx::File) {
        concurrency::LockGuard s(spiLock);
        rxFile.close();
        FSCom.remove(rxPartPath);
    }
    // A slot without a header is free: nothing to undo
    if (rx != Rx::None)
        LOG_INFO("FontPacks: receiving %s abandoned", rxPackPath);
    rx = Rx::None;
}

bool remove(const char *packPath)
{
    concurrency::LockGuard g(&packLock());
    closePacks();
    bool ok = false;
    if (FlashSlots *s = slots()) {
        touchFlash();
        for (int i = 0; i < s->count(); i++) {
            FlashSlots::Info in;
            if (s->info(i, in) && strcmp(in.name, baseName(packPath)) == 0)
                ok = s->clear(i) || ok;
        }
    }
    {
        concurrency::LockGuard s(spiLock);
        if (FSCom.exists(packPath))
            ok = FSCom.remove(packPath) || ok;
    }
    LOG_INFO("FontPacks: %s %s", packPath, ok ? "removed" : "not found");
    return ok;
}

namespace
{
int32_t FlashIdle::runOnce()
{
    const uint32_t used = lastFlashUse;
    if (used && millis() - used > 2000 && rx == Rx::None) {
        concurrency::LockGuard g(&packLock());
        if (fontPackFlash())
            fontPackFlash()->idle();
        lastFlashUse = 0;
    }
    return 1000;
}
} // namespace

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
    concurrency::LockGuard g(&packLock());
    if (!ensureLoaded())
        return 0;
    const CacheEntry &e = lookup(cp);
    return e.packIndex >= 0 ? e.glyph.advance : tofuWidth();
}

int lineHeight()
{
    concurrency::LockGuard g(&packLock());
    return ensureLoaded() ? maxLineHeight : 0;
}

int draw(OLEDDisplay *display, int x, int y, int fontHeight, uint32_t cp)
{
    concurrency::LockGuard g(&packLock());
    if (!ensureLoaded())
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
