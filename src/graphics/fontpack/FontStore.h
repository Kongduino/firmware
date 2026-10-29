#pragma once

#include <stdint.h>

namespace graphics
{

/**
 * Where a font pack lives: a file, a flash region, an EEPROM or FRAM chip, an SD card... A device that keeps its
 * packs somewhere other than the filesystem provides its own FontStore.
 *
 * Drawing only needs random-access reads, from the screen thread. Writing (installing a pack) is not part of this
 * interface yet.
 */
class FontStore
{
  public:
    virtual ~FontStore() = default;

    /// Bytes available to read (the pack's file size, or the region's size)
    virtual uint32_t size() const = 0;

    /// Reads len bytes at offset into buf; false on failure or when the range isn't all inside the store
    virtual bool read(uint32_t offset, void *buf, uint32_t len) = 0;
};

} // namespace graphics
