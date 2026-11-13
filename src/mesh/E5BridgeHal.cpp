#include "E5BridgeHal.h"
#include "Throttle.h"
#include "concurrency/LockGuard.h"
#include "configuration.h"

namespace
{
// Must outlast the E5's 100 ms BUSY timeout plus a full-size frame each way
constexpr uint32_t RESPONSE_TIMEOUT_MS = 150;
constexpr int MAX_ATTEMPTS = 3;
constexpr uint8_t SX126X_CLEAR_IRQ_STATUS = 0x02;
} // namespace

E5BridgeHal::E5BridgeHal(RadioLibHal *host, E5Link *link, bool irqOverLink)
    : RadioLibHal(host->GpioModeInput, host->GpioModeOutput, host->GpioLevelLow, host->GpioLevelHigh, host->GpioInterruptRising,
                  host->GpioInterruptFalling),
      host(host), link(link), irqOverLink(irqOverLink)
{
}

void E5BridgeHal::init()
{
    // host->init() is skipped: ArduinoHal's would start an SPI bus this HAL never uses
    concurrency::LockGuard guard(&lock);
    const uint8_t req[] = {E5_PROTO_VERSION, (uint8_t)(irqOverLink ? E5_HELLO_IRQ_EVENTS : 0)};
    size_t respLen = 0;
    connected = transact(E5_MSG_HELLO, req, sizeof(req), respBuf, sizeof(respBuf), &respLen) && respLen >= 3 &&
                respBuf[0] == E5_PROTO_VERSION;
    if (connected)
        LOG_INFO("E5 bridge: protocol %u, firmware 0x%x, IRQ %s", respBuf[0], respBuf[1] | respBuf[2] << 8,
                 irqOverLink ? "over the link" : "on a GPIO");
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

void E5BridgeHal::pollLink()
{
    {
        concurrency::LockGuard guard(&lock);
        size_t n;
        while (readFrame(&n)) {
            // A response nobody waits for: its request timed out
        }
    }
    // Outside the lock, like a GPIO ISR: the callback may call back into this HAL
    if (irqEventPending.exchange(false)) {
        void (*cb)(void) = irqCallback.load();
        if (cb)
            cb();
    }
}

void E5BridgeHal::attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode)
{
    if (interruptNum == PIN_IRQ)
        irqCallback = interruptCb;
    else if (!isVirtualPin(interruptNum))
        host->attachInterrupt(interruptNum, interruptCb, mode);
}

void E5BridgeHal::detachInterrupt(uint32_t interruptNum)
{
    if (interruptNum == PIN_IRQ)
        irqCallback = nullptr;
    else if (!isVirtualPin(interruptNum))
        host->detachInterrupt(interruptNum);
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
        irqLevel = false;
    } else if (!isVirtualPin(pin)) {
        host->digitalWrite(pin, value);
    }
}

uint32_t E5BridgeHal::digitalRead(uint32_t pin)
{
    if (pin == PIN_IRQ) {
        // RadioLib and the channel scan spin on this pin, so read any event the reader thread has not reached yet
        if (!irqLevel) {
            concurrency::LockGuard guard(&lock);
            size_t n;
            while (readFrame(&n)) {
            }
        }
        return irqLevel ? GpioLevelHigh : GpioLevelLow;
    }
    // The E5 waits for BUSY around every transfer, so RadioLib never has to
    if (isVirtualPin(pin))
        return GpioLevelLow;
    return host->digitalRead(pin);
}

void E5BridgeHal::spiTransfer(uint8_t *out, size_t len, uint8_t *in)
{
    concurrency::LockGuard guard(&lock);
    size_t respLen = 0;
    if (len <= E5_MAX_SPI && transact(E5_MSG_SPI_XFER, out, len, respBuf, sizeof(respBuf), &respLen) && respLen == 1 + len &&
        respBuf[0] == E5_STATUS_OK) {
        memcpy(in, &respBuf[1], len);
        // Still under the lock: an event the E5 sends after this response is not read yet, so it is not lost here
        if (len && out[0] == SX126X_CLEAR_IRQ_STATUS)
            irqLevel = false;
        return;
    }
    // RadioLib ignores this call's outcome, but SX126x::SPIparseStatus reads a 0xFF status as "chip not found"
    memset(in, 0xFF, len);
}

bool E5BridgeHal::readFrame(size_t *frameLen)
{
    int c;
    while ((c = link->read()) >= 0) {
        if (c != 0x00) {
            if (rxLen < sizeof(rxWire))
                rxWire[rxLen++] = (uint8_t)c;
            continue;
        }
        const size_t n = e5CobsDecode(rxWire, rxLen, rxFrame);
        rxLen = 0;
        if (!e5FrameValid(rxFrame, n))
            continue;
        if (rxFrame[1] == 0) {
            if (rxFrame[0] == E5_MSG_EVT_IRQ) {
                irqLevel = true;
                irqEventPending = true;
            }
            continue;
        }
        *frameLen = n;
        return true;
    }
    return false;
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

        uint32_t start = ::millis();
        while (Throttle::isWithinTimespanMs(start, RESPONSE_TIMEOUT_MS)) {
            size_t n;
            if (!readFrame(&n)) {
                host->yield();
                continue;
            }
            if (rxFrame[1] != seq)
                continue; // a stale response
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
