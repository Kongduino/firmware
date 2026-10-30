// Font pack storage in an nRF52840's QSPI NOR flash, for boards that define FONTPACK_FLASH_QSPI (and PIN_QSPI_*) along
// with the slot layout FONTPACK_FLASH_BASE, FONTPACK_FLASH_SLOT_SIZE and FONTPACK_FLASH_SLOTS.
#include "graphics/fontpack/FontPacks.h"

#if HAS_FONTPACKS && defined(ARCH_NRF52) && defined(FONTPACK_FLASH_QSPI)

#include "graphics/fontpack/RawFlash.h"
#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <nrfx_qspi.h>

// The core's nrfx_config.h doesn't set it; 6 is a priority the SoftDevice leaves to the application
#ifndef NRFX_QSPI_DEFAULT_CONFIG_IRQ_PRIORITY
#define NRFX_QSPI_DEFAULT_CONFIG_IRQ_PRIORITY 6
#endif

#ifndef FONTPACK_FLASH_QSPI_SIZE
#define FONTPACK_FLASH_QSPI_SIZE (4UL * 1024 * 1024)
#endif

namespace graphics
{

namespace
{

// Standard SPI NOR commands
constexpr uint8_t CMD_READ_JEDEC = 0x9F;
constexpr uint8_t CMD_READ_SR1 = 0x05;
constexpr uint8_t CMD_READ_SR2 = 0x35;
constexpr uint8_t CMD_WRITE_ENABLE = 0x06;
constexpr uint8_t CMD_WRITE_SR = 0x01;
constexpr uint8_t CMD_DEEP_POWER_DOWN = 0xB9;
constexpr uint8_t CMD_RELEASE_POWER_DOWN = 0xAB;
constexpr uint32_t PAGE_SIZE = 256;
constexpr uint32_t SECTOR_SIZE = 4096;
constexpr uint32_t BUSY_TIMEOUT_MS = 1000;

class QspiRawFlash : public RawFlash
{
  public:
    uint32_t size() const override { return FONTPACK_FLASH_QSPI_SIZE; }
    uint32_t eraseSize() const override { return SECTOR_SIZE; }

    bool read(uint32_t addr, void *buf, uint32_t len) override
    {
        if (!wake())
            return false;
        uint8_t *out = static_cast<uint8_t *>(buf);
        while (len > 0) {
            // EasyDMA needs a word-aligned RAM buffer, address and length: read through the bounce buffer
            const uint32_t aligned = addr & ~3UL, lead = addr - aligned;
            const uint32_t n = std::min<uint32_t>(len, sizeof(bounce) - lead);
            const uint32_t span = (lead + n + 3) & ~3UL;
            if (nrfx_qspi_read(bounce, span, aligned) != NRFX_SUCCESS)
                return false;
            memcpy(out, reinterpret_cast<uint8_t *>(bounce) + lead, n);
            out += n, addr += n, len -= n;
        }
        return true;
    }

    bool write(uint32_t addr, const void *buf, uint32_t len) override
    {
        if (!wake())
            return false;
        const uint8_t *in = static_cast<const uint8_t *>(buf);
        while (len > 0) {
            // One page program at a time, padded to whole words with 0xFF (which leaves erased bytes as they are)
            const uint32_t aligned = addr & ~3UL, lead = addr - aligned;
            const uint32_t pageLeft = PAGE_SIZE - (aligned % PAGE_SIZE);
            const uint32_t n = std::min<uint32_t>(len, pageLeft - lead);
            const uint32_t span = (lead + n + 3) & ~3UL;
            memset(bounce, 0xFF, span);
            memcpy(reinterpret_cast<uint8_t *>(bounce) + lead, in, n);
            if (nrfx_qspi_write(bounce, span, aligned) != NRFX_SUCCESS || !waitReady())
                return false;
            in += n, addr += n, len -= n;
        }
        return true;
    }

    bool erase(uint32_t addr, uint32_t len) override
    {
        if (addr % SECTOR_SIZE || len % SECTOR_SIZE || !wake())
            return false;
        for (uint32_t a = addr; a < addr + len; a += SECTOR_SIZE) {
            if (nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_4KB, a) != NRFX_SUCCESS || !waitReady())
                return false;
        }
        return true;
    }

    void idle() override
    {
        if (!active)
            return;
        command(CMD_DEEP_POWER_DOWN, nullptr, 0, nullptr, 0);
        nrfx_qspi_uninit();
        // nRF52840 anomaly 122: the QSPI keeps drawing current after it is disabled
        *(volatile uint32_t *)0x40029054UL = 1;
        active = false;
    }

  private:
    uint32_t bounce[PAGE_SIZE / 4]; // word-aligned, in RAM
    bool active = false;
    bool checked = false;

    /// A custom instruction: opcode, then tx bytes, then rx bytes (at most 8 in all)
    bool command(uint8_t opcode, const uint8_t *tx, uint8_t txLen, uint8_t *rx, uint8_t rxLen)
    {
        uint8_t data[8] = {0};
        if (txLen + rxLen > sizeof(data))
            return false;
        if (tx)
            memcpy(data, tx, txLen);
        nrf_qspi_cinstr_conf_t c = NRFX_QSPI_DEFAULT_CINSTR(opcode, (nrf_qspi_cinstr_len_t)(1 + txLen + rxLen));
        c.io2_level = true; // keep WP# and HOLD# high
        c.io3_level = true;
        if (nrfx_qspi_cinstr_xfer(&c, data, data) != NRFX_SUCCESS)
            return false;
        if (rx)
            memcpy(rx, data + txLen, rxLen);
        return true;
    }

    bool waitReady()
    {
        const uint32_t start = millis();
        while (nrfx_qspi_mem_busy_check() == NRFX_ERROR_BUSY) {
            if (millis() - start > BUSY_TIMEOUT_MS) {
                LOG_WARN("FontPacks: QSPI flash busy timeout");
                return false;
            }
        }
        return true;
    }

    /// Starts the QSPI and wakes the chip from deep power-down, if needed
    bool wake()
    {
        if (active)
            return true;
        auto pin = [](uint32_t arduinoPin) { return static_cast<uint8_t>(g_ADigitalPinMap[arduinoPin]); };
        nrfx_qspi_config_t cfg = NRFX_QSPI_DEFAULT_CONFIG(pin(PIN_QSPI_SCK), pin(PIN_QSPI_CS), pin(PIN_QSPI_IO0),
                                                          pin(PIN_QSPI_IO1), pin(PIN_QSPI_IO2), pin(PIN_QSPI_IO3));
        // Single-line read and page program: no quad-enable bit needed. 16 MHz
        cfg.prot_if.readoc = NRF_QSPI_READOC_FASTREAD;
        cfg.prot_if.writeoc = NRF_QSPI_WRITEOC_PP;
        cfg.phy_if.sck_freq = NRF_QSPI_FREQ_DIV2;
        if (nrfx_qspi_init(&cfg, nullptr, nullptr) != NRFX_SUCCESS) {
            LOG_WARN("FontPacks: QSPI init failed");
            return false;
        }
        active = true;
        command(CMD_RELEASE_POWER_DOWN, nullptr, 0, nullptr, 0);
        delayMicroseconds(50);
        if (!checked) {
            checked = true;
            uint8_t id[3] = {0}, sr1 = 0, sr2 = 0;
            command(CMD_READ_JEDEC, nullptr, 0, id, 3);
            command(CMD_READ_SR1, nullptr, 0, &sr1, 1);
            command(CMD_READ_SR2, nullptr, 0, &sr2, 1);
            LOG_INFO("FontPacks: QSPI flash JEDEC %02x %02x %02x, SR1 %02x SR2 %02x", id[0], id[1], id[2], sr1, sr2);
            // Block protection (SR1 BP bits, SR2 CMP) would make writes fail silently: clear it
            if ((sr1 & 0x7C) || (sr2 & 0x40)) {
                const uint8_t sr[2] = {(uint8_t)(sr1 & 0x83), (uint8_t)(sr2 & ~0x40)};
                command(CMD_WRITE_ENABLE, nullptr, 0, nullptr, 0);
                command(CMD_WRITE_SR, sr, 2, nullptr, 0);
                waitReady();
                LOG_INFO("FontPacks: QSPI flash block protection cleared");
            }
        }
        return true;
    }
};

QspiRawFlash qspiFlash;

} // namespace

RawFlash *fontPackFlash()
{
    return &qspiFlash;
}

} // namespace graphics

#endif
