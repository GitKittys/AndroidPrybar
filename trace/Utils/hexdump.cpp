//
// Created by ASUS on 2025-11-30.
//

#include "hexdump.h"
#include <stdint.h>
#include <string.h>

static const char hexUpper[] = "0123456789ABCDEF";

size_t hexdump_to_buffer(
        uint64_t addr,
        const void *buf,
        size_t size,
        char *outbuf,
        size_t outcap)
{
    const size_t PER_LINE = 16;
    const uint8_t *data = (const uint8_t *)buf;

    size_t offset = addr;
    size_t pos = 0;

    if (outcap == 0)
        return 0;

    outbuf[0] = '\0';

    char line[80];

    while (offset < size)
    {
        size_t chunk = (size - offset > PER_LINE)
                       ? PER_LINE
                       : size - offset;

        size_t n = 0;

        // 8-digit hex address + ":  "
        uint32_t a = (uint32_t)offset;
        for (int k = 7; k >= 0; k--) { line[k] = hexUpper[a & 0xF]; a >>= 4; }
        n = 8;
        line[n++] = ':';
        line[n++] = ' ';
        line[n++] = ' ';

        for (size_t i = 0; i < chunk; i++) {
            uint8_t b = data[offset + i];
            line[n++] = hexUpper[b >> 4];
            line[n++] = hexUpper[b & 0xF];
            line[n++] = ' ';
        }

        for (size_t i = chunk; i < PER_LINE; i++) {
            line[n++] = ' ';
            line[n++] = ' ';
            line[n++] = ' ';
        }

        line[n++] = ' ';
        line[n++] = '|';

        for (size_t i = 0; i < chunk; i++) {
            uint8_t c = data[offset + i];
            line[n++] = (c >= 32 && c < 127) ? (char)c : '.';
        }

        line[n++] = '|';
        line[n++] = '\n';

        if (pos + n + 1 > outcap) {
            size_t can_write = outcap - pos - 1;
            memcpy(outbuf + pos, line, can_write);
            pos += can_write;
            outbuf[pos] = '\0';
            return pos;
        }

        memcpy(outbuf + pos, line, n);
        pos += n;
        outbuf[pos] = '\0';

        offset += chunk;
    }

    return pos;
}
