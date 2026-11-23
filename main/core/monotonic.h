#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Signed monotonic timestamps may be negative in tests; subtraction must not overflow.
 * A backwards clock never satisfies a deadline. Firmware uses esp_timer's 64-bit clock. */
static inline bool wd_elapsed_ms(int64_t now, int64_t then, uint32_t interval) {
    return now >= then && (uint64_t)now - (uint64_t)then >= interval;
}
