#pragma once

#include <stdint.h>

namespace graphics
{

/**
 * A board's raw storage for font packs: an external NOR flash (QSPI, SPI), an EEPROM, a FRAM... The board provides it
 * (fontPackFlash()); FlashSlots keeps the packs in it. Only used from the main loop.
 */
class RawFlash
{
  public:
    virtual ~RawFlash() = default;

    /// Bytes of the device
    virtual uint32_t size() const = 0;

    /// Bytes erased at once (4096 for most NOR flash); 1 for memory written without erasing (EEPROM, FRAM)
    virtual uint32_t eraseSize() const = 0;

    virtual bool read(uint32_t addr, void *buf, uint32_t len) = 0;

    /// Writes to erased memory
    virtual bool write(uint32_t addr, const void *buf, uint32_t len) = 0;

    /// Erases whole erase units (addr and len multiples of eraseSize())
    virtual bool erase(uint32_t addr, uint32_t len) = 0;

    /// Not used for a while: the device and its interface may power down (the next access wakes them)
    virtual void idle() {}
};

/// The board's font pack storage, or nullptr when the packs are kept as files (the default)
RawFlash *fontPackFlash();

} // namespace graphics
