#include "radio_capture.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "gps_service.h"
#include "sdkconfig.h"
#include <stdatomic.h>

#define RX_QUEUE_LENGTH 256
static wd_radio_cache_t cache;
static QueueHandle_t queue;
static atomic_uint active_generation, received, dropped, malformed;
static radio_capture_stats_t stats;
static size_t storage_bytes;

static void receive_frame(void *buffer, wifi_promiscuous_pkt_type_t type) {
    unsigned generation = atomic_load_explicit(&active_generation, memory_order_relaxed);
    if (!generation || type != WIFI_PKT_MGMT)
        return;
    const wifi_promiscuous_pkt_t *packet = buffer;
    if (packet->rx_ctrl.rx_state || packet->rx_ctrl.sig_len < 40)
        return;
    if (packet->payload[0] != 0x80 && packet->payload[0] != 0x50)
        return;
    wd_beacon_t beacon;
    if (!wd_beacon_parse(packet->payload, packet->rx_ctrl.sig_len - 4, packet->rx_ctrl.channel,
                         packet->rx_ctrl.rssi, esp_timer_get_time() / 1000, &beacon)) {
        atomic_fetch_add_explicit(&malformed, 1, memory_order_relaxed);
        return;
    }
    beacon.generation = generation;
    atomic_fetch_add_explicit(&received, 1, memory_order_relaxed);
    if (xQueueSend(queue, &beacon, 0) != pdTRUE)
        atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
}
esp_err_t radio_capture_init(void) {
    const size_t slots = CONFIG_FEB_MAX_APS * 4;
    wd_radio_entry_t *entries =
        heap_caps_calloc(slots, sizeof(*entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries)
        return ESP_ERR_NO_MEM;
    if (!wd_radio_cache_init(&cache, entries, slots)) {
        heap_caps_free(entries);
        return ESP_ERR_INVALID_ARG;
    }
    storage_bytes = slots * sizeof(*entries);
    queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(wd_beacon_t));
    if (!queue) {
        heap_caps_free(entries);
        cache = (wd_radio_cache_t){0};
        storage_bytes = 0;
        return ESP_ERR_NO_MEM;
    }
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_err_t err = esp_wifi_set_promiscuous_filter(&filter);
    if (err == ESP_OK)
        err = esp_wifi_set_promiscuous_rx_cb(receive_frame);
    if (err == ESP_OK)
        err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        radio_capture_end();
        esp_wifi_set_promiscuous_rx_cb(NULL);
        vQueueDelete(queue);
        queue = NULL;
        heap_caps_free(entries);
        cache = (wd_radio_cache_t){0};
        storage_bytes = 0;
    }
    return err;
}
void radio_capture_begin(int64_t started_ms) {
    if (!queue)
        return;
    xQueueReset(queue);
    unsigned generation = wd_radio_cache_begin(&cache, started_ms);
    atomic_store_explicit(&active_generation, generation, memory_order_relaxed);
}
void radio_capture_end(void) { atomic_store_explicit(&active_generation, 0, memory_order_relaxed); }
void radio_capture_drain(const wd_config_t *config) {
    stats.received += atomic_exchange_explicit(&received, 0, memory_order_relaxed);
    stats.dropped += atomic_exchange_explicit(&dropped, 0, memory_order_relaxed);
    stats.malformed += atomic_exchange_explicit(&malformed, 0, memory_order_relaxed);
    if (!queue)
        return;
    size_t waiting = uxQueueMessagesWaiting(queue);
    if (waiting > stats.queue_high_water)
        stats.queue_high_water = waiting;
    wd_beacon_t b;
    /* Bounded callback work and a bounded main-task drain; no parsing in an ISR. */
    for (unsigned n = 0; n < RX_QUEUE_LENGTH && xQueueReceive(queue, &b, 0) == pdTRUE; n++) {
        gps_fix_t fix = gps_service_snapshot_at(b.received_ms);
        fix.valid = wd_fix_valid(fix, config);
        wd_radio_cache_store(&cache, &b, fix);
    }
}
void radio_capture_reset_stats(void) {
    stats = (radio_capture_stats_t){0};
    cache.overflows = 0;
    atomic_store(&received, 0);
    atomic_store(&dropped, 0);
    atomic_store(&malformed, 0);
}
radio_capture_stats_t radio_capture_stats(void) { return stats; }
const wd_radio_entry_t *radio_capture_find(const uint8_t bssid[6], uint8_t channel,
                                           int64_t completed_ms) {
    return cache.entries ? wd_radio_cache_find(&cache, bssid, channel, completed_ms) : NULL;
}
uint64_t radio_capture_overflows(void) { return cache.overflows; }

size_t radio_capture_storage_bytes(void) { return storage_bytes; }
