#pragma once

#include "graphics/fontpack/FontStore.h"
#include "graphics/fontpack/RawFlash.h"

namespace graphics
{

/**
 * Font packs in a board's raw storage (RawFlash), one per slot. A slot starts with a 4 KB header (name, size, install
 * counter, CRC of the header), followed by the pack. The header is written last, after the pack was checked, so a slot
 * without a valid header is free: an interrupted or refused install leaves nothing behind.
 */
class FlashSlots
{
  public:
    static constexpr uint32_t kHeaderSize = 4096;
    static constexpr int kNameSize = 64;

    struct Info {
        char name[kNameSize]; // e.g. "cjk16.mfp"
        uint32_t size;        // bytes of the pack
        uint32_t seq;         // install counter: the higher, the newer
    };

    FlashSlots(RawFlash &flash, uint32_t base, uint32_t slotSize, int slotCount)
        : flash(flash), base(base), slotSize(slotSize), slotCount(slotCount)
    {
    }

    int count() const { return slotCount; }
    uint32_t maxPackSize() const { return slotSize - kHeaderSize; }
    uint32_t packAddr(int slot) const { return base + (uint32_t)slot * slotSize + kHeaderSize; }

    /// The slot's pack, when its header is valid
    bool info(int slot, Info &out);

    /// The slot holding a pack of that name (the newest), or -1
    int find(const char *name);

    /// A slot without a valid header, or -1
    int findFree();

    /// Starts writing a pack into a free slot (erases its header)
    bool begin(int slot);

    /// Writes the next bytes of the pack, erasing the memory ahead as needed
    bool write(int slot, uint32_t offset, const void *data, uint32_t len);

    /// Makes the slot's pack valid: writes its header (with the next install counter)
    bool commit(int slot, const char *name, uint32_t size);

    /// Frees a slot (erases its header)
    bool clear(int slot);

    RawFlash &device() { return flash; }

  private:
    RawFlash &flash;
    uint32_t base, slotSize;
    int slotCount;
    uint32_t erasedTo = 0; // during a write: the memory below this address (in the slot) is erased
};

/// A pack in a flash slot
class SlotFontStore : public FontStore
{
  public:
    SlotFontStore(RawFlash &flash, uint32_t addr, uint32_t size) : flash(flash), addr(addr), packSize(size) {}
    uint32_t size() const override { return packSize; }
    bool read(uint32_t offset, void *buf, uint32_t len) override;

  private:
    RawFlash &flash;
    uint32_t addr, packSize;
};

} // namespace graphics
