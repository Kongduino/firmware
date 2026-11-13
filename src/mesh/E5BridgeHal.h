#pragma once

#include "E5BridgeFrame.h"
#include "concurrency/Lock.h"
#include <Arduino.h>
#include <RadioLib.h>
#include <atomic>

/** Byte pipe to the E5 */
class E5Link
{
  public:
    virtual ~E5Link() = default;
    virtual void write(const uint8_t *buf, size_t len) = 0;
    /** Next received byte, or -1 if none is waiting */
    virtual int read() = 0;
};

class E5StreamLink : public E5Link
{
  public:
    explicit E5StreamLink(Stream &port) : port(port) {}
    void write(const uint8_t *buf, size_t len) override { port.write(buf, len); }
    // Checked first because some Streams (Portduino's LinuxSerial) return 0 from an empty read()
    int read() override { return port.available() > 0 ? port.read() : -1; }

  private:
    Stream &port;
};

/** RadioLibHal for the SX126x-class radio inside an STM32WLE5 running the e5-bridge firmware, reached over a UART.
 *  SPI and the radio's NSS/BUSY/RESET go over the link; timing and any wired IRQ line stay on the host HAL. */
class E5BridgeHal : public RadioLibHal
{
  public:
    // Virtual pins to give the radio's Module; any other pin goes to the host HAL
    static constexpr uint32_t PIN_NSS = 0x80000000;
    static constexpr uint32_t PIN_BUSY = 0x80000001;
    static constexpr uint32_t PIN_RESET = 0x80000002;
    // The radio IRQ carried as event frames, for hosts with no GPIO wired to the E5; needs irqOverLink
    static constexpr uint32_t PIN_IRQ = 0x80000003;

    E5BridgeHal(RadioLibHal *host, E5Link *link, bool irqOverLink = false);

    /** True once the E5 has answered HELLO with our protocol version */
    bool isConnected() const { return connected; }

    /** Restart the E5 into its ROM bootloader, which then speaks AN3155 on the same UART: 8E1, autobaud on 0x7F */
    bool enterBootloader();

    /** With irqOverLink, call often from a thread of its own: reads pending IRQ events and runs the attached ISR */
    void pollLink();

    void init() override;
    void term() override {}

    void pinMode(uint32_t pin, uint32_t mode) override
    {
        if (!isVirtualPin(pin))
            host->pinMode(pin, mode);
    }
    void digitalWrite(uint32_t pin, uint32_t value) override;
    uint32_t digitalRead(uint32_t pin) override;
    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override;
    void detachInterrupt(uint32_t interruptNum) override;
    uint32_t pinToInterrupt(uint32_t pin) override { return isVirtualPin(pin) ? pin : host->pinToInterrupt(pin); }

    void delay(RadioLibTime_t ms) override { host->delay(ms); }
    void delayMicroseconds(RadioLibTime_t us) override { host->delayMicroseconds(us); }
    RadioLibTime_t millis() override { return host->millis(); }
    RadioLibTime_t micros() override { return host->micros(); }
    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override { return host->pulseIn(pin, state, timeout); }
    void yield() override { host->yield(); }

    // The SPI bus is on the E5, and the caller opens the UART
    void spiBegin() override {}
    void spiBeginTransaction() override {}
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override;
    void spiEndTransaction() override {}
    void spiEnd() override {}

  private:
    static bool isVirtualPin(uint32_t pin) { return pin >= PIN_NSS && pin <= PIN_IRQ; }

    /** One request/response exchange, with retries; the caller holds lock */
    bool transact(uint8_t type, const uint8_t *req, size_t reqLen, uint8_t *resp, size_t respMax, size_t *respLen);

    /** Decode waiting bytes until a response frame is in rxFrame (true) or none is left; handles IRQ events on the way */
    bool readFrame(size_t *frameLen);

    RadioLibHal *host;
    E5Link *link;
    const bool irqOverLink;
    concurrency::Lock lock;
    bool connected = false;
    uint8_t seq = 0;

    // DIO1's level as it would read on a real SX1262: set by an IRQ event, cleared by ClearIrqStatus
    std::atomic<bool> irqLevel{false};
    std::atomic<bool> irqEventPending{false};
    std::atomic<void (*)(void)> irqCallback{nullptr};

    // Kept off the stack, which is small on nRF52 threads
    uint8_t txFrame[E5_MAX_FRAME];
    uint8_t txWire[E5_MAX_WIRE];
    uint8_t rxWire[E5_MAX_WIRE];
    size_t rxLen = 0;
    uint8_t rxFrame[E5_MAX_WIRE];
    uint8_t respBuf[E5_MAX_PAYLOAD];
};
