#include "ap_memory.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "sdkconfig.h"

static size_t reserved_bytes;
static bool allocated;

bool ap_memory_init(wd_engine_t *engine) {
    if (allocated || !esp_psram_is_initialized())
        return false;
    const size_t count = CONFIG_FEB_AP_CAPACITY;
    const size_t index_count = count * 2;
    const size_t queue_count = CONFIG_FEB_OBSERVATION_CAPACITY;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    wd_ap_t *aps = heap_caps_calloc(count, sizeof(*aps), caps);
    uint32_t *index = heap_caps_calloc(index_count, sizeof(*index), caps);
    wd_observation_t *queue = heap_caps_calloc(queue_count, sizeof(*queue), caps);
    if (!aps || !index || !queue ||
        !wd_engine_init(engine, aps, count, index, index_count, queue, queue_count)) {
        heap_caps_free(aps);
        heap_caps_free(index);
        heap_caps_free(queue);
        return false;
    }
    reserved_bytes =
        count * sizeof(*aps) + index_count * sizeof(*index) + queue_count * sizeof(*queue);
    allocated = true;
    return true;
}
size_t ap_memory_reserved(void) { return reserved_bytes; }
