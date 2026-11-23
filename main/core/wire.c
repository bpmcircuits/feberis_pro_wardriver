#include "wire.h"
#include <stdio.h>
#include <string.h>

uint16_t wire_crc(const char *data, size_t length) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)(uint8_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

bool wire_feed(wire_reader_t *r, char byte) {
    if (byte == '\n') {
        bool ready = !r->dropping && r->used > 0;
        r->data[r->used] = '\0';
        r->used = 0;
        r->dropping = false;
        return ready;
    }
    if (r->dropping)
        return false;
    if (byte == '\0' || byte == '\r' || r->used == sizeof(r->data) - 2) {
        r->dropping = true;
        return false;
    }
    r->data[r->used++] = byte;
    return false;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

size_t wire_decode(char *line, char **fields, size_t capacity) {
    char *star = strrchr(line, '*');
    if (!star || strlen(star + 1) != 4 || strncmp(line, "FBR1\t", 5) != 0)
        return 0;
    unsigned expected = 0;
    for (unsigned i = 1; i <= 4; ++i) {
        int digit = hex_digit(star[i]);
        if (digit < 0)
            return 0;
        expected = (expected << 4) | (unsigned)digit;
    }
    for (char *p = line; p < star; ++p)
        if (*p != '\t' && ((uint8_t)*p < 32 || (uint8_t)*p > 126 || *p == '*'))
            return 0;
    if (expected != wire_crc(line, (size_t)(star - line)) || capacity == 0)
        return 0;
    *star = '\0';
    size_t count = 1;
    fields[0] = line;
    for (char *p = line; p < star; ++p) {
        if (*p == '\t') {
            if (count == capacity)
                return 0;
            *p = '\0';
            fields[count++] = p + 1;
        }
    }
    return count;
}

size_t wire_encode(const char *payload, char *out, size_t capacity) {
    size_t length = strlen(payload);
    if (length + 7 > capacity)
        return 0;
    int written = snprintf(out, capacity, "%s*%04X\n", payload, wire_crc(payload, length));
    return written > 0 && (size_t)written < capacity ? (size_t)written : 0;
}

bool wire_u32(const char *text, uint32_t *value) {
    if (!text || !*text)
        return false;
    uint32_t result = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || result > (UINT32_MAX - (unsigned)(*p - '0')) / 10)
            return false;
        result = result * 10 + (unsigned)(*p - '0');
    }
    *value = result;
    return true;
}

void wire_hex(const uint8_t *bytes, size_t length, char *out) {
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * length] = '\0';
}
