#include "E5BridgeHal.h"
#include "Throttle.h"
#include "concurrency/LockGuard.h"
#include "configuration.h"

namespace
{
// Must outlast the E5's 100 ms BUSY timeout plus a full-size frame each way
constexpr uint32_t RESPONSE_TIMEOUT_MS = 150;
constexpr int MAX_ATTEMPTS = 3;
} // namespace

E5BridgeHal::E5BridgeHal(RadioLibHal *host, E5Link *link)
    : RadioLibHal(host->GpioModeInput, host->GpioModeOutput, host->GpioLevelLow, host->GpioLevelHigh, host->GpioInterruptRising,
                  host->GpioInterruptFalling),
      host(host), link(link)
{
}

void E5BridgeHal::init()
{
    // host->init() is skipped: ArduinoHal's would start an SPI bus this HAL never uses
    concurrency::LockGuard guard(&lock);
    const uint8_t req[] = {E5_PROTO_VERSION};
    size_t respLen = 0;
    connected = transact(E5_MSG_HELLO, req, sizeof(req), respBuf, sizeof(respBuf), &respLen) && respLen >= 3 &&
                respBuf[0] == E5_PROTO_VERSION;
    if (connected)
        LOG_INFO("E5 bridge: protocol %u, firmware 0x%x", respBuf[0], respBuf[1] | respBuf[2] << 8);
    else
        LOG_ERROR("E5 bridge: no valid answer to HELLO");
}

bool E5BridgeHal::enterBootloader()
{
    concurrency::LockGuard guard(&lock);
    size_t respLen = 0;
    if (!transact(E5_MSG_BOOT, (const uint8_t *)E5_BOOT_MAGIC, E5_BOOT_MAGIC_LEN, respBuf, sizeof(respBuf), &respLen) ||
        respLen != 1 || respBuf[0] != E5_STATUS_OK)
        return false;
    connected = false;
    return true;
}

void E5BridgeHal::digitalWrite(uint32_t pin, uint32_t value)
{
    if (pin == PIN_RESET) {
        // The radio's reset is a register bit on the E5; the level sent is that of the active-low NRESET line
        concurrency::LockGuard guard(&lock);
        const uint8_t req[] = {E5_PIN_RADIO_RESET, (uint8_t)(value == GpioLevelHigh)};
        size_t respLen = 0;
        if (!transact(E5_MSG_GPIO_WRITE, req, sizeof(req), respBuf, sizeof(respBuf), &respLen) || respLen != 1 ||
            respBuf[0] != E5_STATUS_OK)
            LOG_WARN("E5 bridge: radio reset write failed");
    } else if (!isVirtualPin(pin)) {
        host->digitalWrite(pin, value);
    }
}

uint32_t E5BridgeHal::digitalRead(uint32_t pin)
{
    // The E5 waits for BUSY around every transfer, so RadioLib never has to
    if (isVirtualPin(pin))
        return GpioLevelLow;
    return host->digitalRead(pin);
}

void E5BridgeHal::spiTransfer(uint8_t *out, size_t len, uint8_t *in)
{
    concurrency::LockGuard guard(&lock);
    size_t respLen = 0;
    if (len <= E5_MAX_PAYLOAD && transact(E5_MSG_SPI_XFER, out, len, respBuf, sizeof(respBuf), &respLen) &&
        respLen == 1 + len && respBuf[0] == E5_STATUS_OK) {
        memcpy(in, &respBuf[1], len);
        return;
    }
    // RadioLib ignores this call's outcome, but SX126x::SPIparseStatus reads a 0xFF status as "chip not found"
    memset(in, 0xFF, len);
}

bool E5BridgeHal::transact(uint8_t type, const uint8_t *req, size_t reqLen, uint8_t *resp, size_t respMax, size_t *respLen)
{
    // Sequence 0 is reserved for unsolicited frames from the E5
    if (++seq == 0)
        seq = 1;
    const size_t wireLen = e5BuildWire(type, seq, req, reqLen, txFrame, txWire);

    // A retry reuses seq, so the E5 replays its cached answer instead of running a command like SetTx twice
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        link->write(txWire, wireLen);

        size_t rxLen = 0;
        uint32_t start = ::millis();
        while (Throttle::isWithinTimespanMs(start, RESPONSE_TIMEOUT_MS)) {
            const int c = link->read();
            if (c < 0) {
                host->yield();
                continue;
            }
            if (c != 0x00) {
                if (rxLen < sizeof(rxWire))
                    rxWire[rxLen++] = (uint8_t)c;
                continue;
            }

            const size_t n = e5CobsDecode(rxWire, rxLen, rxFrame);
            rxLen = 0;
            if (!e5FrameValid(rxFrame, n) || rxFrame[1] != seq)
                continue; // corrupt, stale, or unsolicited
            if (rxFrame[0] != (type | E5_MSG_RESPONSE)) {
                LOG_WARN("E5 bridge: request 0x%x rejected", type);
                return false;
            }
            *respLen = n - 4 < respMax ? n - 4 : respMax;
            memcpy(resp, &rxFrame[2], *respLen);
            return true;
        }
    }
    LOG_WARN("E5 bridge: no answer to request 0x%x", type);
    return false;
}
