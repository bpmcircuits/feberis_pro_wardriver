#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIRE_MAX 1536
#define WIRE_FIELDS 48

typedef struct {
    char data[WIRE_MAX];
    size_t used;
    bool dropping;
} wire_reader_t;

uint16_t wire_crc(const char *data, size_t length);
/* Returns true for a complete, bounded line; overlong lines are discarded through LF. */
bool wire_feed(wire_reader_t *reader, char byte);
/* Mutates the input into zero-terminated fields; validates prefix, ASCII and CRC first. */
size_t wire_decode(char *line, char **fields, size_t capacity);
size_t wire_encode(const char *payload, char *out, size_t capacity);
bool wire_u32(const char *text, uint32_t *value);
void wire_hex(const uint8_t *bytes, size_t length, char *out);
