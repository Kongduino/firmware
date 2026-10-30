#include "graphics/fontpack/FlashSlots.h"
#include "graphics/fontpack/FontPacks.h"

#if HAS_FONTPACKS

#include "graphics/fontpack/fontpack.h"
#include <cstddef>
#include <cstring>

namespace graphics
{

namespace
{

// The slot header, little-endian, at the start of the slot (the rest of its 4 KB stays erased)
constexpr char kMagic[4] = {'M', 'F', 'P', 'S'};
constexpr uint16_t kVersion = 1;
struct __attribute__((packed)) Header {
    char magic[4];
    uint16_t version;
    uint16_t reserved;
    uint32_t seq;
    uint32_t size;
    char name[FlashSlots::kNameSize];
    uint32_t crc; // CRC-32 of the bytes above
};

uint32_t headerCrc(const Header &h)
{
    return fp_crc32(0, reinterpret_cast<const uint8_t *>(&h), offsetof(Header, crc));
}

} // namespace

bool FlashSlots::info(int slot, Info &out)
{
    if (slot < 0 || slot >= slotCount)
        return false;
    Header h;
    if (!flash.read(base + (uint32_t)slot * slotSize, &h, sizeof(h)))
        return false;
    if (memcmp(h.magic, kMagic, 4) != 0 || h.version != kVersion || h.crc != headerCrc(h) || h.size == 0 ||
        h.size > maxPackSize())
        return false;
    memcpy(out.name, h.name, kNameSize);
    out.name[kNameSize - 1] = '\0';
    out.size = h.size;
    out.seq = h.seq;
    return true;
}

int FlashSlots::find(const char *name)
{
    int best = -1;
    uint32_t bestSeq = 0;
    for (int i = 0; i < slotCount; i++) {
        Info in;
        if (info(i, in) && strcmp(in.name, name) == 0 && (best < 0 || in.seq > bestSeq))
            best = i, bestSeq = in.seq;
    }
    return best;
}

int FlashSlots::findFree()
{
    for (int i = 0; i < slotCount; i++) {
        Info in;
        if (!info(i, in))
            return i;
    }
    return -1;
}

bool FlashSlots::begin(int slot)
{
    if (slot < 0 || slot >= slotCount)
        return false;
    const uint32_t start = base + (uint32_t)slot * slotSize;
    // The header occupies whole erase units (kHeaderSize is a multiple of every erase size we support)
    if (!flash.erase(start, kHeaderSize))
        return false;
    erasedTo = start + kHeaderSize;
    return true;
}

bool FlashSlots::write(int slot, uint32_t offset, const void *data, uint32_t len)
{
    if (slot < 0 || slot >= slotCount || offset > maxPackSize() || len > maxPackSize() - offset)
        return false;
    const uint32_t addr = packAddr(slot) + offset;
    const uint32_t unit = flash.eraseSize();
    while (erasedTo < addr + len) {
        if (!flash.erase(erasedTo, unit))
            return false;
        erasedTo += unit;
    }
    return flash.write(addr, data, len);
}

bool FlashSlots::commit(int slot, const char *name, uint32_t size)
{
    if (slot < 0 || slot >= slotCount || size == 0 || size > maxPackSize())
        return false;
    uint32_t seq = 0;
    for (int i = 0; i < slotCount; i++) {
        Info in;
        if (info(i, in) && in.seq > seq)
            seq = in.seq;
    }
    Header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, kMagic, 4);
    h.version = kVersion;
    h.seq = seq + 1;
    h.size = size;
    strncpy(h.name, name, kNameSize - 1);
    h.crc = headerCrc(h);
    return flash.write(base + (uint32_t)slot * slotSize, &h, sizeof(h));
}

bool FlashSlots::clear(int slot)
{
    if (slot < 0 || slot >= slotCount)
        return false;
    return flash.erase(base + (uint32_t)slot * slotSize, kHeaderSize);
}

bool SlotFontStore::read(uint32_t offset, void *buf, uint32_t len)
{
    if (offset > packSize || len > packSize - offset)
        return false;
    return len == 0 || flash.read(addr + offset, buf, len);
}

} // namespace graphics

#endif
