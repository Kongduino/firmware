#pragma once

// Wire format shared by E5BridgeHal and the e5-bridge firmware on the STM32WLE5; must stay valid C
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define E5_PROTO_VERSION 1
#define E5_MAX_PAYLOAD 300
#define E5_MAX_FRAME (2 + E5_MAX_PAYLOAD + 2)               // type, seq, payload, crc
#define E5_MAX_WIRE (E5_MAX_FRAME + E5_MAX_FRAME / 254 + 2) // COBS overhead, delimiter

#define E5_PIN_RADIO_RESET 0
#define E5_BOOT_MAGIC "BOOT"
#define E5_BOOT_MAGIC_LEN 4

enum {
    E5_MSG_HELLO = 0x01,
    E5_MSG_SPI_XFER = 0x02,
    E5_MSG_GPIO_WRITE = 0x03,
    E5_MSG_BOOT = 0x04,
    E5_MSG_RESPONSE = 0x80, // response type = request type | 0x80
    E5_MSG_ERROR = 0xFF,
};

enum {
    E5_STATUS_OK = 0,
    E5_STATUS_BUSY_TIMEOUT = 1,
    E5_STATUS_BAD_REQUEST = 2,
};

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF
static inline uint16_t e5Crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// COBS leaves no 0x00 in the output, so 0x00 can delimit frames; out needs len + len / 254 + 1 bytes
static inline size_t e5CobsEncode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t codeIdx = 0, o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == 0) {
            out[codeIdx] = code;
            codeIdx = o++;
            code = 1;
        } else {
            out[o++] = in[i];
            if (++code == 0xFF) {
                out[codeIdx] = code;
                codeIdx = o++;
                code = 1;
            }
        }
    }
    out[codeIdx] = code;
    return o;
}

// Returns the decoded length, or 0 if the input is malformed
static inline size_t e5CobsDecode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t i = 0, o = 0;
    while (i < len) {
        uint8_t code = in[i++];
        if (code == 0 || i + code - 1 > len)
            return 0;
        for (uint8_t j = 1; j < code; j++)
            out[o++] = in[i++];
        if (code != 0xFF && i < len)
            out[o++] = 0;
    }
    return o;
}

// Builds type, seq, payload and crc in frame, then encodes it into wire; returns the wire length, delimiter included
static inline size_t e5BuildWire(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len, uint8_t *frame, uint8_t *wire)
{
    frame[0] = type;
    frame[1] = seq;
    if (len)
        memcpy(&frame[2], payload, len);
    uint16_t crc = e5Crc16(frame, 2 + len);
    frame[2 + len] = (uint8_t)(crc & 0xFF);
    frame[3 + len] = (uint8_t)(crc >> 8);

    size_t wireLen = e5CobsEncode(frame, 4 + len, wire);
    wire[wireLen++] = 0x00;
    return wireLen;
}

// True if a decoded frame is long enough and its CRC matches
static inline int e5FrameValid(const uint8_t *frame, size_t n)
{
    return n >= 4 && e5Crc16(frame, n - 2) == (uint16_t)(frame[n - 2] | frame[n - 1] << 8);
}
