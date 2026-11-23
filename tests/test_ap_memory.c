#ifdef NDEBUG
#undef NDEBUG
#endif
#include "ap_memory.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static bool present;
static unsigned allocations, released, fail_at;
static wd_engine_t e;
bool esp_psram_is_initialized(void) { return present; }
void *heap_caps_calloc(size_t n, size_t bytes, uint32_t caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++allocations;
    if (allocations == fail_at)
        return NULL;
    return calloc(n, bytes);
}
void heap_caps_free(void *ptr) {
    if (ptr)
        ++released;
    free(ptr);
}
int main(void) {
    assert(!ap_memory_init(&e) && allocations == 0 && ap_memory_reserved() == 0);
    present = true;
    for (unsigned point = 1; point <= 3; point++) {
        allocations = released = 0;
        fail_at = point;
        assert(!ap_memory_init(&e));
        assert(allocations == 3 && released == 2 && !e.aps && !e.queue);
        assert(ap_memory_reserved() == 0);
    }
    allocations = released = fail_at = 0;
    assert(ap_memory_init(&e));
    assert(allocations == 3 && released == 0);
    assert(e.ap_capacity == CONFIG_FEB_AP_CAPACITY && e.index_capacity == 2 * e.ap_capacity);
    assert(e.queue_capacity == CONFIG_FEB_OBSERVATION_CAPACITY);
    assert(ap_memory_reserved() == e.ap_capacity * sizeof(wd_ap_t) +
                                       e.index_capacity * sizeof(uint32_t) +
                                       e.queue_capacity * sizeof(wd_observation_t));
    assert(!ap_memory_init(&e) && allocations == 3); /* no repeated or per-session allocation */
    assert(wd_session_begin(&e, 1));
    wd_detection_t d = {.rssi = -60, .band = 2, .channel = 1};
    assert(wd_detect(&e, &d, (gps_fix_t){0}, 0) == WD_ACCEPTED);
    assert(allocations == 3);
    puts("Missing PSRAM, allocation failures, cleanup, explicit SPIRAM caps and one-time "
         "allocation passed");
    /* Firmware intentionally owns the successful allocation for its entire uptime. */
}
