#include "ap_memory.h"
#include "core/delivery.h"
#include "core/monotonic.h"
#include "core/wardrive.h"
#include "core/wigle.h"
#include "core/wire.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gps_service.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "radio_capture.h"
#include "sdkconfig.h"
#include "status_led.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if CONFIG_FEB_FLIPPER_TX == 15 || CONFIG_FEB_FLIPPER_RX == 15 || CONFIG_FEB_GPS_RX == 15 ||       \
    CONFIG_FEB_GPS_TX == 15 || CONFIG_FEB_LED_GPIO == 15
#error "GPIO15 is reserved for PSRAM on ESP32-C5-WROOM-1U-N8R8"
#endif

#define VERSION "0.4.0"
#define INGEST_MAX_RECORDS 64
#define INGEST_BUDGET_US 2000
static wd_engine_t engine;

typedef struct {
    wifi_event_sta_scan_done_t done;
    int64_t completed_ms;
} scan_notice_t;
typedef struct {
    uint32_t radio_ms, ingest_ms, gap_ms, max_gap_ms, period_ms;
    uint64_t ingest_cpu_us, metadata_hits, metadata_misses, pressure_ms;
    size_t queue_high_water;
} scan_metrics_t;

typedef struct {
    uint32_t boot, sequence, scan_id, cycles, observations, acknowledged, rows;
    uint32_t no_gps, truncated, discarded;
    uint32_t interval_ms;
    int64_t next_scan_ms, scan_started_ms, scan_completed_ms, previous_start_ms;
    int64_t pressure_started_ms;
    bool gap_eligible;
    scan_metrics_t metrics;
    bool running, scanning, error, pending_csv, paused, enhanced, link_wait;
    uint32_t session_counter;
    int64_t session_started_ms;
    bool gps_ready, led_ready, wifi_ready, memory_ready, scan_timeout_reported, capture_ready;
    wifi_ap_record_t *records;
    uint16_t count, index, found;
    gps_fix_t scan_fix;
    delivery_t delivery;
    wire_reader_t reader;
    QueueHandle_t scan_events, uart_events;
    char pending_frame[WIRE_MAX];
} app_t;

static app_t app;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void stop_session(const char *reason);

/* Only the main task writes UART0: responses and records never interleave. */
static void send_payload(const char *payload) {
    char frame[WIRE_MAX];
    size_t length = wire_encode(payload, frame, sizeof(frame));
    if (length)
        uart_write_bytes(UART_NUM_0, frame, length);
}

static void send_message(const char *format, ...) {
    char payload[WIRE_MAX - 7];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(payload, sizeof(payload), format, args);
    va_end(args);
    if (length > 0 && (size_t)length < sizeof(payload))
        send_payload(payload);
}

static void reply(uint32_t id, bool ok, const char *code) {
    send_message("FBR1\t%" PRIu32 "\t%s\t%s", id, ok ? "OK" : "ERR", code);
}

static gps_fix_t current_fix(void) {
    gps_fix_t f = app.gps_ready ? gps_service_snapshot() : (gps_fix_t){0};
    f.valid = wd_fix_valid(f, &engine.config);
    return f;
}

static void send_status(uint32_t id) {
    gps_fix_t fix = current_fix();
    const char *phase =
        app.scanning ? "SCANNING" : ((app.count || engine.queued) ? "DRAINING" : "IDLE");
    send_message("FBR1\t%" PRIu32 "\tSTATUS\t%" PRIu32 "\t%u\t%s\t%" PRIu32 "\t%" PRIu32
                 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32
                 "\t%u\t%u\t%.2f\t%" PRIu32 "\t%.7f\t%.7f\t%.1f\t%s\t%u\t%u",
                 id, app.boot, app.running, phase, app.scan_id, app.cycles, app.observations,
                 app.acknowledged, app.rows, app.no_gps, app.truncated, app.discarded, fix.valid,
                 fix.satellites, fix.hdop, fix.age_ms, fix.latitude, fix.longitude, fix.altitude_m,
                 fix.utc[0] ? fix.utc : "-", app.gps_ready, app.wifi_ready);
}

static const char *auth_name(wifi_auth_mode_t auth) {
    switch (auth) {
    case WIFI_AUTH_OPEN:
        return "[ESS]";
    case WIFI_AUTH_WEP:
        return "[WEP][ESS]";
    case WIFI_AUTH_WPA_PSK:
        return "[WPA-PSK][ESS]";
    case WIFI_AUTH_WPA2_PSK:
        return "[WPA2-PSK][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "[WPA-PSK][WPA2-PSK][ESS]";
    case WIFI_AUTH_ENTERPRISE:
        return "[WPA2-EAP][ESS]";
    case WIFI_AUTH_WPA3_PSK:
        return "[WPA3-SAE][ESS]";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "[WPA2-PSK][WPA3-SAE][ESS]";
    case WIFI_AUTH_OWE:
        return "[OWE][ESS]";
    case WIFI_AUTH_WAPI_PSK:
        return "[WAPI-PSK][ESS]";
    case WIFI_AUTH_WPA3_ENT_192:
        return "[WPA3-EAP-SUITE-B-192][ESS]";
    case WIFI_AUTH_DPP:
        return "[DPP][ESS]";
    case WIFI_AUTH_WPA3_ENTERPRISE:
        return "[WPA3-EAP][ESS]";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
        return "[WPA2-EAP][WPA3-EAP][ESS]";
    case WIFI_AUTH_WPA_ENTERPRISE:
        return "[WPA-EAP][ESS]";
    default:
        return "[UNKNOWN][ESS]";
    }
}

static void scan_finished(void) {
    app.metrics.ingest_ms = (uint32_t)(now_ms() - app.scan_completed_ms);
    send_message("FBR1\t0\tSCAN_DONE\t%" PRIu32 "\t%" PRIu32 "\t%u\t%u\t%u", app.boot, app.scan_id,
                 app.found, app.count, app.found > app.count ? app.found - app.count : 0);
    app.count = app.index = 0;
    app.next_scan_ms = now_ms() + app.interval_ms;
}

static void send_next_record(void) {
    const wd_observation_t *o = wd_pending(&engine);
    if (!o)
        return;
    ap_observation_t ap = {.auth = auth_name(o->ap.auth),
                           .channel = o->ap.channel,
                           .rssi = o->ap.rssi,
                           .ssid_length = o->ap.ssid_len};
    memcpy(ap.bssid, o->ap.bssid, 6);
    memcpy(ap.ssid, o->ap.ssid, ap.ssid_length);
    char mac[18], ssid[65], csv[WIGLE_ROW_MAX], hex[WIGLE_ROW_MAX * 2 + 1];
    wigle_mac(ap.bssid, mac);
    wire_hex(ap.ssid, ap.ssid_length, ssid);
    size_t csv_len = wigle_row(&ap, &o->fix, csv, sizeof(csv));
    wire_hex((uint8_t *)csv, csv_len, hex);
    char payload[WIRE_MAX - 7];
    const gps_fix_t *f = &o->fix;
    int n;
    if (app.enhanced) {
        n = snprintf(payload, sizeof(payload),
                     "FBR1\t0\tOBSERVATION\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32
                     "\t%s\t%s\t%s\t%u\t%d\t%u\t%s\t%.7f\t%.7f\t%.1f\t%.2f\t%u\t%" PRIu32
                     "\t%s\t%u\t%u\t%u\t%" PRIu64 "\t%.2f\t%.1f\t%.2f\t%u",
                     app.boot, o->sequence, o->session, mac, ssid, ap.auth, ap.channel, ap.rssi,
                     f->valid, f->utc[0] ? f->utc : "-", f->latitude, f->longitude, f->altitude_m,
                     f->hdop, f->satellites, f->age_ms, csv_len ? hex : "-", o->ap.band,
                     o->ap.frequency_mhz, (unsigned)o->reason, o->detected_ms, f->speed_kmh,
                     f->course_deg, f->horizontal_accuracy_m, o->ap.phy);
    } else {
        n = snprintf(payload, sizeof(payload),
                     "FBR1\t0\tAP\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32
                     "\t%s\t%s\t%s\t%u\t%d\t%u\t%s\t%.7f\t%.7f\t%.1f\t%.2f\t%u\t%" PRIu32 "\t%s",
                     app.boot, o->sequence, o->session, mac, ssid, ap.auth, ap.channel, ap.rssi,
                     f->valid, f->utc[0] ? f->utc : "-", f->latitude, f->longitude, f->altitude_m,
                     f->hdop, f->satellites, f->age_ms, csv_len ? hex : "-");
    }
    if (n <= 0 || (size_t)n >= sizeof(payload) ||
        !wire_encode(payload, app.pending_frame, sizeof(app.pending_frame))) {
        send_message("FBR1\t0\tERROR\tFRAME_SIZE");
        app.paused = true;
        return;
    }
    app.sequence = o->sequence;
    app.pending_csv = csv_len != 0;
    delivery_begin(&app.delivery, o->sequence, now_ms());
    uart_write_bytes(UART_NUM_0, app.pending_frame, strlen(app.pending_frame));
}

static void stop_session(const char *reason) {
    app.running = false;
    app.paused = false;
    app.gap_eligible = false;
    if (app.scanning)
        esp_wifi_scan_stop();
    /* Preserve cancellation semantics for the old FAP; revision 2 drains reliably. */
    if (!app.enhanced) {
        app.discarded += (uint32_t)engine.queued + (app.count - app.index);
        engine.queued = 0;
        app.count = app.index = 0;
        app.delivery.pending = false;
    }
    /* Revision 2 accepted observations remain queued until durable ACK. */
    send_message("FBR1\t0\tSTOPPED\t%" PRIu32 "\t%s\t%" PRIu32, app.boot, reason, app.discarded);
    if (app.enhanced)
        send_message("FBR1\t0\tSESSION_STOPPED\t%" PRIu32 "\t%" PRIu32 "\t%u", app.boot,
                     engine.session, (unsigned)engine.queued);
}

static void send_config(uint32_t id) {
    const wd_config_t *c = &engine.config;
    send_message("FBR1\t%" PRIu32 "\tCONFIG\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32
                 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32
                 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32,
                 id, c->version, c->mode, c->band, c->active, c->dwell_ms, c->min_distance_m,
                 c->rssi_delta_db, c->max_gap_ms, c->min_gap_ms, c->gps_max_age_ms,
                 c->gps_max_hdop_x10, c->full_gap_ms, c->baud);
}
static void send_stats(uint32_t id, bool lifetime) {
    const wd_stats_t *t = lifetime ? &engine.lifetime : &engine.stats;
    send_message("FBR1\t%" PRIu32 "\tSTATS\t%u\t%" PRIu32 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                 "\t%" PRIu64 "\t%" PRIu64 "\t%u\t%" PRIu32 "\t%u\t%u",
                 id, lifetime, engine.session, t->total_detections, t->unique_aps,
                 t->observations_saved, t->observations_filtered, t->gps_valid_observations,
                 t->gps_invalid_observations, t->wifi_2g_aps, t->wifi_5g_aps, t->open_aps,
                 t->secured_aps, t->scan_cycles, t->scan_errors, t->uart_retransmissions,
                 t->uart_dropped_frames, t->storage_errors, t->queue_overflows, t->table_overflows,
                 (unsigned)engine.queued, engine.committed, (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
}
static void send_scan_metrics(uint32_t id) {
    radio_capture_stats_t c = radio_capture_stats();
    send_message("FBR1\t%" PRIu32 "\tSCAN_METRICS\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32
                 "\t%u\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu64
                 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                 "\t%u\t%" PRIu64 "\t%u\t%u\t%u\t%u",
                 id, app.boot, engine.session, app.interval_ms, engine.config.dwell_ms,
                 app.capture_ready, app.metrics.radio_ms, app.metrics.ingest_ms, app.metrics.gap_ms,
                 app.metrics.max_gap_ms, app.metrics.period_ms, app.metrics.ingest_cpu_us,
                 c.received, c.dropped, c.malformed, radio_capture_overflows(),
                 app.metrics.metadata_hits, app.metrics.metadata_misses,
                 (unsigned)app.metrics.queue_high_water, app.metrics.pressure_ms,
                 (unsigned)(CONFIG_FEB_MAX_APS * sizeof(wifi_ap_record_t)),
                 (unsigned)radio_capture_storage_bytes(),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)CONFIG_FEB_MAX_APS);
}
static bool save_config(const wd_config_t *c) {
    nvs_handle_t h;
    if (nvs_open("wardrive", NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t err = nvs_set_blob(h, "config", c, sizeof(*c));
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static void handle_command(char *line) {
    char *f[WIRE_FIELDS];
    size_t count = wire_decode(line, f, WIRE_FIELDS);
    uint32_t id;
    if (count < 3 || !wire_u32(f[1], &id) || id == 0) {
        ++engine.stats.uart_dropped_frames;
        ++engine.lifetime.uart_dropped_frames;
        return;
    }
    const char *command = f[2];
    if (!strcmp(command, "HELLO") && (count == 3 || (count == 4 && !strcmp(f[3], "2")))) {
        app.enhanced = count == 4;
        app.link_wait = false;
        app.delivery.pending = false;
        send_message("FBR1\t%" PRIu32 "\tOK\tHELLO\t%s\t%" PRIu32 "\t%d\t%s\t%d\t2000\t3", id,
                     VERSION, app.boot, (int)engine.config.baud, CONFIG_FEB_COUNTRY,
                     CONFIG_FEB_MAX_APS);
    } else if (!strcmp(command, "PING") && count == 3) {
        reply(id, true, "PONG");
    } else if (!strcmp(command, "STATUS") && count == 3) {
        send_status(id);
    } else if (!strcmp(command, "GET_INFO") && count == 3) {
        send_message("FBR1\t%" PRIu32 "\tINFO\t%s\t%" PRIu32 "\t%" PRIu32 "\t%s\t%u\t%u\t2", id,
                     VERSION, app.boot, engine.config.baud, CONFIG_FEB_COUNTRY,
                     (unsigned)engine.ap_capacity, (unsigned)engine.queue_capacity);
    } else if (!strcmp(command, "GET_SCAN_METRICS") && count == 3) {
        send_scan_metrics(id);
    } else if (!strcmp(command, "GET_MEMORY") && count == 3) {
        const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        send_message(
            "FBR1\t%" PRIu32 "\tMEMORY\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u",
            id, app.memory_ready, esp_psram_is_initialized(),
            (unsigned)heap_caps_get_total_size(external),
            (unsigned)heap_caps_get_free_size(external),
            (unsigned)heap_caps_get_minimum_free_size(external),
            (unsigned)heap_caps_get_largest_free_block(external),
            (unsigned)heap_caps_get_free_size(internal),
            (unsigned)heap_caps_get_minimum_free_size(internal),
            (unsigned)heap_caps_get_largest_free_block(internal), (unsigned)ap_memory_reserved(),
            (unsigned)sizeof(wd_ap_t), (unsigned)engine.ap_capacity, (unsigned)engine.ap_count,
            (unsigned)engine.queue_capacity, (unsigned)engine.queued);
    } else if (!strcmp(command, "GET_STATUS") && count == 3) {
        send_message("FBR1\t%" PRIu32 "\tSTATE\t%" PRIu32 "\t%" PRIu32
                     "\t%u\t%u\t%u\t%u\t%u\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu64,
                     id, app.boot, engine.session, app.running, app.paused, app.scanning,
                     app.link_wait, (unsigned)engine.queued, engine.next_sequence, engine.committed,
                     engine.session ? (uint64_t)(now_ms() - app.session_started_ms) : 0);
    } else if (!strcmp(command, "GET_GPS") && count == 3) {
        gps_fix_t g = current_fix();
        send_message("FBR1\t%" PRIu32 "\tGPS\t%u\t%s\t%.7f\t%.7f\t%.1f\t%.2f\t%u\t%" PRIu32
                     "\t%.2f\t%.1f\t%.2f",
                     id, g.valid, g.utc[0] ? g.utc : "-", g.latitude, g.longitude, g.altitude_m,
                     g.hdop, g.satellites, g.age_ms, g.speed_kmh, g.course_deg,
                     g.horizontal_accuracy_m);
    } else if (!strcmp(command, "GET_AP") && count == 4) {
        uint32_t slot;
        if (!wire_u32(f[3], &slot) || slot >= engine.ap_capacity)
            reply(id, false, "BAD_ARGUMENT");
        else if (!engine.aps[slot].used)
            reply(id, true, "EMPTY");
        else {
            const wd_ap_t *a = &engine.aps[slot];
            char mac[18], ssid[65];
            wigle_mac(a->latest.bssid, mac);
            wire_hex(a->latest.ssid, a->latest.ssid_len, ssid);
            send_message(
                "FBR1\t%" PRIu32 "\t%s\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32
                "\t%s\t%s\t%u\t%u\t%s\t%d\t%d\t%.3f\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                "\t%" PRIu64 "\t%" PRIu32 "\t%u\t%.7f\t%.7f\t%u\t%.7f\t%.7f\t%s\t%s\t%u\t%u",
                id, a->detections == 1 ? "AP_DISCOVERED" : "AP_UPDATED", app.boot, engine.session,
                slot, mac, ssid, a->latest.band, a->latest.channel, auth_name(a->latest.auth),
                a->latest.rssi, a->strongest_rssi, a->mean_rssi, a->detections, a->observations,
                a->first_seen_ms, a->last_seen_ms, a->changes, a->last_location.valid,
                a->last_location.latitude, a->last_location.longitude, a->best_location.valid,
                a->best_location.latitude, a->best_location.longitude,
                a->first_utc[0] ? a->first_utc : "-", a->last_utc[0] ? a->last_utc : "-",
                a->latest.frequency_mhz, a->latest.phy);
        }
    } else if (!strcmp(command, "GET_CONFIG") && count == 3) {
        send_config(id);
    } else if (!strcmp(command, "GET_STATS") &&
               (count == 3 || (count == 4 && !strcmp(f[3], "TOTAL")))) {
        send_stats(id, count == 4);
    } else if (!strcmp(command, "SET_CONFIG")) {
        wd_config_t c = {0};
        uint32_t v[13];
        bool valid = count == 16;
        for (unsigned i = 0; valid && i < 13; i++)
            valid = wire_u32(f[3 + i], &v[i]);
        if (valid)
            c = (wd_config_t){v[0], v[1], v[2], v[3],  v[4],  v[5], v[6],
                              v[7], v[8], v[9], v[10], v[11], v[12]};
        if (!valid || !wd_config_valid(&c))
            reply(id, false, "BAD_ARGUMENT");
        else if (app.scanning || app.count ||
                 (c.baud != engine.config.baud && (app.running || engine.queued)))
            reply(id, false, "BUSY_OR_BAUD");
        else if (!app.wifi_ready ||
                 esp_wifi_set_band_mode(c.band == 1   ? WIFI_BAND_MODE_2G_ONLY
                                        : c.band == 2 ? WIFI_BAND_MODE_5G_ONLY
                                                      : WIFI_BAND_MODE_AUTO) != ESP_OK)
            reply(id, false, "BAND");
        else if (!save_config(&c)) {
            esp_wifi_set_band_mode(engine.config.band == 1   ? WIFI_BAND_MODE_2G_ONLY
                                   : engine.config.band == 2 ? WIFI_BAND_MODE_5G_ONLY
                                                             : WIFI_BAND_MODE_AUTO);
            reply(id, false, "NVS");
        } else {
            bool baud_changed = c.baud != engine.config.baud;
            engine.config = c;
            reply(id, true, "SET_CONFIG");
            if (baud_changed) {
                uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000));
                uart_set_baudrate(UART_NUM_0, c.baud);
            }
        }
    } else if (!strcmp(command, "RESUME_LINK") && count == 6) {
        uint32_t boot, session, seq;
        if (!wire_u32(f[3], &boot) || !wire_u32(f[4], &session) || !wire_u32(f[5], &seq))
            reply(id, false, "BAD_ARGUMENT");
        else if (boot != app.boot || session != engine.session)
            reply(id, false, "SESSION_MISMATCH");
        else {
            bool newly_committed = seq != engine.committed;
            if (!wd_resume_ack(&engine, session, seq, app.sequence)) {
                reply(id, false, "SEQUENCE_MISMATCH");
            } else {
                if (newly_committed) {
                    ++app.acknowledged;
                    if (app.pending_csv)
                        ++app.rows;
                }
                app.delivery.pending = false;
                app.link_wait = false;
                reply(id, true, "RESUME_LINK");
            }
        }
    } else if (!strcmp(command, "NACK") && count == 4 && !strcmp(f[3], "STORAGE")) {
        ++engine.stats.storage_errors;
        ++engine.lifetime.storage_errors;
        app.paused = true;
        app.link_wait = true;
        reply(id, true, "NACK");
    } else if (!strcmp(command, "PAUSE_SCAN") && count == 3) {
        app.paused = true;
        app.gap_eligible = false;
        reply(id, true, "PAUSE_SCAN");
    } else if (!strcmp(command, "RESUME_SCAN") && count == 3) {
        app.paused = false;
        app.gap_eligible = false;
        reply(id, true, "RESUME_SCAN");
    } else if (!strcmp(command, "RESET_STATS") && count == 3) {
        if (app.running || app.scanning || app.count || engine.queued)
            reply(id, false, "BUSY");
        else if (!wd_session_begin(&engine, engine.session))
            reply(id, false, "AP_MEMORY_UNAVAILABLE");
        else
            reply(id, true, "RESET_STATS");
    } else if (!strcmp(command, "START") || !strcmp(command, "START_SCAN")) {
        uint32_t interval = 0;
        if ((count != 3 && count != 4) || (count == 4 && !wire_u32(f[3], &interval)) ||
            interval > 60000) {
            reply(id, false, "BAD_ARGUMENT");
        } else if (!app.memory_ready) {
            reply(id, false, "AP_MEMORY_UNAVAILABLE");
        } else if (!app.wifi_ready) {
            reply(id, false, "WIFI_UNAVAILABLE");
        } else if (!app.running && (app.scanning || app.count || engine.queued)) {
            reply(id, false, "BUSY");
        } else {
            app.interval_ms = interval;
            if (!app.running) {
                if (app.session_counter == UINT32_MAX) {
                    reply(id, false, "SESSION_EXHAUSTED");
                    return;
                }
                if (!wd_session_begin(&engine, app.session_counter + 1)) {
                    reply(id, false, "AP_MEMORY_UNAVAILABLE");
                    return;
                }
                ++app.session_counter;
                app.metrics = (scan_metrics_t){0};
                app.previous_start_ms = 0;
                app.scan_completed_ms = 0;
                app.gap_eligible = false;
                app.pressure_started_ms = 0;
                radio_capture_reset_stats();
                app.next_scan_ms = now_ms();
                app.paused = false;
                app.session_started_ms = now_ms();
                if (app.enhanced)
                    send_message("FBR1\t0\tSESSION_STARTED\t%" PRIu32 "\t%" PRIu32, app.boot,
                                 engine.session);
            }
            app.running = true;
            app.error = !app.led_ready || !app.gps_ready || !app.memory_ready || !app.capture_ready;
            reply(id, true, "START");
        }
    } else if ((!strcmp(command, "STOP") || !strcmp(command, "STOP_SCAN")) && count == 3) {
        reply(id, true, "STOP");
        stop_session("USER");
    } else if (!strcmp(command, "ACK")) {
        uint32_t boot, sequence;
        uint32_t session = engine.session;
        if ((count != 5 && count != 6) || !wire_u32(f[3], &boot) ||
            !wire_u32(f[count - 1], &sequence) || (count == 6 && !wire_u32(f[4], &session))) {
            reply(id, false, "BAD_ARGUMENT");
        } else if (boot != app.boot) {
            reply(id, false, "BOOT_MISMATCH");
        } else {
            /* ACK is one-way. Duplicate/late sequence values have no effect. */
            if (sequence == app.sequence && wd_ack(&engine, session, sequence)) {
                delivery_ack(&app.delivery, sequence);
                ++app.acknowledged;
                if (app.pending_csv)
                    ++app.rows;
            }
        }
    } else if (!strcmp(command, "BAND")) {
        wifi_band_mode_t mode = WIFI_BAND_MODE_AUTO;
        if (count != 4 || (strcmp(f[3], "AUTO") && strcmp(f[3], "2G") && strcmp(f[3], "5G"))) {
            reply(id, false, "BAD_ARGUMENT");
        } else if (!app.wifi_ready) {
            reply(id, false, "WIFI_UNAVAILABLE");
        } else if (app.running || app.scanning || app.count) {
            reply(id, false, "BUSY");
        } else {
            if (!strcmp(f[3], "2G"))
                mode = WIFI_BAND_MODE_2G_ONLY;
            if (!strcmp(f[3], "5G"))
                mode = WIFI_BAND_MODE_5G_ONLY;
            if (esp_wifi_set_band_mode(mode) != ESP_OK)
                reply(id, false, "BAND");
            else {
                wd_config_t c = engine.config;
                c.band = mode == WIFI_BAND_MODE_2G_ONLY   ? 1
                         : mode == WIFI_BAND_MODE_5G_ONLY ? 2
                                                          : 0;
                if (save_config(&c)) {
                    engine.config = c;
                    reply(id, true, "BAND");
                } else {
                    esp_wifi_set_band_mode(engine.config.band == 1   ? WIFI_BAND_MODE_2G_ONLY
                                           : engine.config.band == 2 ? WIFI_BAND_MODE_5G_ONLY
                                                                     : WIFI_BAND_MODE_AUTO);
                    reply(id, false, "NVS");
                }
            }
        }
    } else {
        reply(id, false, "UNKNOWN_COMMAND");
    }
}

static void wifi_scan_event(void *argument, esp_event_base_t base, int32_t id, void *data) {
    (void)argument;
    (void)base;
    (void)id;
    /* One scan in flight; overwrite prevents a full notification queue from losing completion. */
    scan_notice_t notice = {.done = *(wifi_event_sta_scan_done_t *)data, .completed_ms = now_ms()};
    radio_capture_end();
    xQueueOverwrite(app.scan_events, &notice);
}

static void process_scan_event(void) {
    scan_notice_t notice;
    if (xQueueReceive(app.scan_events, &notice, 0) != pdTRUE)
        return;
    app.scanning = false;
    app.scan_completed_ms = notice.completed_ms;
    radio_capture_drain(&engine.config);
    if (!app.running || notice.done.status != 0) {
        esp_wifi_clear_ap_list();
        if (app.running) {
            ++engine.stats.scan_errors;
            ++engine.lifetime.scan_errors;
            app.error = true;
            send_message("FBR1\t0\tERROR\tSCAN_FAILED");
            app.next_scan_ms = now_ms() + 2000;
        }
        return;
    }
    esp_err_t err = esp_wifi_scan_get_ap_num(&app.found);
    app.count = CONFIG_FEB_MAX_APS;
    if (err == ESP_OK)
        err = esp_wifi_scan_get_ap_records(&app.count, app.records);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        app.count = 0;
        app.error = true;
        ++engine.stats.scan_errors;
        ++engine.lifetime.scan_errors;
        send_message("FBR1\t0\tERROR\tSCAN_READ");
        app.next_scan_ms = now_ms() + 2000;
        return;
    }
    /* A STOP/failed scan has no ingestion batch. Retain the previous batch's
     * diagnostics until a successful result read starts the next one. */
    app.metrics.radio_ms = (uint32_t)(notice.completed_ms - app.scan_started_ms);
    app.metrics.ingest_cpu_us = 0;
    ++app.cycles;
    ++engine.stats.scan_cycles;
    ++engine.lifetime.scan_cycles;
    app.index = 0;
    if (app.found > app.count)
        app.truncated += app.found - app.count;
    app.scan_fix = app.gps_ready ? gps_service_snapshot_at(notice.completed_ms) : (gps_fix_t){0};
    if (!app.count)
        scan_finished();
}

static void advance_scanner(void) {
    /* PAUSE/STOP must not disable the in-flight scan watchdog. Report only once
     * until the driver signals completion; never start an overlapping scan. */
    if (app.scanning && !app.scan_timeout_reported &&
        wd_elapsed_ms(now_ms(), app.scan_started_ms, 120000)) {
        app.scan_timeout_reported = true;
        ++engine.stats.scan_errors;
        ++engine.lifetime.scan_errors;
        app.error = true;
        send_message("FBR1\t0\tERROR\tSCAN_TIMEOUT");
        stop_session("SCAN_TIMEOUT");
    }
    /* Backpressure retains both the accepted queue and unread scan results in bounded RAM. */
    int64_t slice_started_us = esp_timer_get_time();
    unsigned processed = 0;
    while (app.count && app.index < app.count && engine.queued < engine.queue_capacity &&
           processed < INGEST_MAX_RECORDS &&
           esp_timer_get_time() - slice_started_us < INGEST_BUDGET_US) {
        ++processed;
        wifi_ap_record_t *r = &app.records[app.index];
        wd_detection_t d = {.ssid_len = strnlen((char *)r->ssid, 32),
                            .band = r->primary > 14 ? 5 : 2,
                            .channel = r->primary,
                            .auth = r->authmode,
                            .rssi = r->rssi,
                            .frequency_mhz = r->primary == 14  ? 2484
                                             : r->primary < 14 ? 2407 + 5 * r->primary
                                                               : 5000 + 5 * r->primary,
                            .phy = (r->phy_11b ? 1 : 0) | (r->phy_11g ? 2 : 0) |
                                   (r->phy_11n ? 4 : 0) | (r->phy_11a ? 8 : 0) |
                                   (r->phy_11ac ? 16 : 0) | (r->phy_11ax ? 32 : 0)};
        memcpy(d.bssid, r->bssid, 6);
        memcpy(d.ssid, r->ssid, d.ssid_len);
        const wd_radio_entry_t *sample =
            app.capture_ready ? radio_capture_find(d.bssid, d.channel, app.scan_completed_ms)
                              : NULL;
        gps_fix_t fix = app.scan_fix;
        int64_t detected_ms = app.scan_completed_ms;
        uint32_t tags = 0;
        if (sample) {
            ++app.metrics.metadata_hits;
            fix = sample->fix;
            detected_ms = sample->beacon.received_ms;
            d.rssi = sample->beacon.rssi;
            if (sample->beacon.ssid_len || !d.ssid_len) {
                d.ssid_len = sample->beacon.ssid_len;
                memcpy(d.ssid, sample->beacon.ssid, d.ssid_len);
            }
        } else {
            ++app.metrics.metadata_misses;
            tags = WD_RX_UNVERIFIED;
            /* An SDK aggregate has no per-frame timestamp. Preserve detection, never invent a
             * location. */
            fix.valid = false;
        }
        wd_result_t result = wd_detect_tagged(&engine, &d, fix, (uint64_t)detected_ms, tags);
        if (result == WD_TABLE_FULL || result == WD_SEQUENCE_FULL) {
            app.discarded += app.count - app.index;
            app.count = app.index = 0;
            send_message("FBR1\t0\tERROR\t%s",
                         result == WD_TABLE_FULL ? "AP_TABLE_FULL" : "SEQUENCE_EXHAUSTED");
            stop_session("CAPACITY");
        } else {
            if (result == WD_ACCEPTED) {
                ++app.observations;
                if (!wd_fix_valid(fix, &engine.config))
                    ++app.no_gps;
            }
            if (++app.index == app.count && app.enhanced)
                scan_finished();
        }
    }
    if (processed)
        app.metrics.ingest_cpu_us += (uint64_t)(esp_timer_get_time() - slice_started_us);
    if (engine.queued > app.metrics.queue_high_water)
        app.metrics.queue_high_water = engine.queued;
    if (engine.queued == engine.queue_capacity && app.running) {
        if (!app.pressure_started_ms)
            app.pressure_started_ms = now_ms();
    } else if (app.pressure_started_ms) {
        app.metrics.pressure_ms += (uint64_t)(now_ms() - app.pressure_started_ms);
        app.pressure_started_ms = 0;
    }
    if (!app.enhanced && app.count && app.index == app.count && !engine.queued)
        scan_finished();
    if (app.running && !app.paused && !app.count && !app.scanning &&
        engine.queued < engine.queue_capacity && now_ms() >= app.next_scan_ms) {
        wifi_scan_config_t config = {.show_hidden = true,
                                     .scan_type = engine.config.active ? WIFI_SCAN_TYPE_ACTIVE
                                                                       : WIFI_SCAN_TYPE_PASSIVE};
        if (engine.config.active) {
            config.scan_time.active.min = engine.config.dwell_ms;
            config.scan_time.active.max = engine.config.dwell_ms;
        } else
            config.scan_time.passive = engine.config.dwell_ms;
        int64_t started = now_ms();
        if (app.capture_ready)
            radio_capture_begin(started);
        esp_err_t err = esp_wifi_scan_start(&config, false);
        if (err != ESP_OK) {
            radio_capture_end();
            ++engine.stats.scan_errors;
            ++engine.lifetime.scan_errors;
            app.error = true;
            app.next_scan_ms = now_ms() + 2000;
            send_message("FBR1\t0\tERROR\tSCAN_START\t%d", err);
        } else {
            app.scanning = true;
            app.scan_timeout_reported = false;
            app.scan_started_ms = started;
            if (app.gap_eligible && app.scan_completed_ms) {
                app.metrics.gap_ms = (uint32_t)(started - app.scan_completed_ms);
                if (app.metrics.gap_ms > app.metrics.max_gap_ms)
                    app.metrics.max_gap_ms = app.metrics.gap_ms;
                app.metrics.period_ms = (uint32_t)(started - app.previous_start_ms);
            }
            app.previous_start_ms = started;
            app.gap_eligible = true;
            ++app.scan_id;
            send_message("FBR1\t0\tSCAN_BEGIN\t%" PRIu32 "\t%" PRIu32, app.boot, app.scan_id);
        }
    }
    if (app.delivery.pending && !app.link_wait) {
        delivery_action_t action = delivery_poll(&app.delivery, now_ms());
        if (action == DELIVERY_RETRY) {
            ++engine.stats.uart_retransmissions;
            ++engine.lifetime.uart_retransmissions;
            uart_write_bytes(UART_NUM_0, app.pending_frame, strlen(app.pending_frame));
        }
        if (action == DELIVERY_TIMEOUT) {
            app.link_wait = true;
            send_message("FBR1\t0\tERROR\tACK_TIMEOUT");
        }
    }
    if (!app.delivery.pending && engine.queued && !app.link_wait)
        send_next_record();
}

static esp_err_t initialize_wifi(void) {
    /* Never erase NVS automatically: it may contain board calibration or user configuration. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
        return err;
    nvs_handle_t h;
    if (nvs_open("wardrive", NVS_READONLY, &h) == ESP_OK) {
        wd_config_t c;
        size_t size = sizeof(c);
        if (nvs_get_blob(h, "config", &c, &size) == ESP_OK && size == sizeof(c) &&
            wd_config_valid(&c))
            engine.config = c;
        nvs_close(h);
    }
    err = esp_netif_init();
    if (err != ESP_OK)
        return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK)
        return err;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK)
        return err;
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_scan_event, NULL);
    if (err != ESP_OK)
        return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK)
        return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
        return err;
    err = esp_wifi_start();
    if (err != ESP_OK)
        return err;
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK)
        return err;
    err = esp_wifi_set_country_code(CONFIG_FEB_COUNTRY, true);
    if (err != ESP_OK)
        return err;
    return esp_wifi_set_band_mode(engine.config.band == 1   ? WIFI_BAND_MODE_2G_ONLY
                                  : engine.config.band == 2 ? WIFI_BAND_MODE_5G_ONLY
                                                            : WIFI_BAND_MODE_AUTO);
}

static esp_err_t initialize_link(void) {
    uart_config_t config = {
        .baud_rate = CONFIG_FEB_FLIPPER_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(UART_NUM_0, 4096, 4096, 20, &app.uart_events, 0);
    if (err != ESP_OK)
        return err;
    err = uart_param_config(UART_NUM_0, &config);
    if (err != ESP_OK)
        return err;
    return uart_set_pin(UART_NUM_0, CONFIG_FEB_FLIPPER_TX, CONFIG_FEB_FLIPPER_RX,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static bool pins_unique(void) {
    const int pins[] = {CONFIG_FEB_FLIPPER_TX, CONFIG_FEB_FLIPPER_RX, CONFIG_FEB_GPS_TX,
                        CONFIG_FEB_GPS_RX, CONFIG_FEB_LED_GPIO};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i)
        for (unsigned j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j)
            if (pins[i] == pins[j])
                return false;
    return true;
}

void app_main(void) {
    engine.config = wd_defaults();
    engine.config.baud = CONFIG_FEB_FLIPPER_BAUD;
    if (!pins_unique())
        return;
    if (initialize_link() != ESP_OK)
        return;
    app.memory_ready = ap_memory_init(&engine);
    app.records = heap_caps_calloc(CONFIG_FEB_MAX_APS, sizeof(*app.records),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    app.memory_ready = app.memory_ready && app.records;
    engine.config.baud = CONFIG_FEB_FLIPPER_BAUD;
    app.scan_events = xQueueCreate(1, sizeof(scan_notice_t));
    if (!app.scan_events) {
        send_message("FBR1\t0\tERROR\tNO_MEMORY");
        return;
    }
    esp_fill_random(&app.boot, sizeof(app.boot));
    app.interval_ms = 0;
    app.led_ready = status_led_init() == ESP_OK;
    app.gps_ready = gps_service_start() == ESP_OK;
    esp_err_t wifi_error = initialize_wifi();
    app.wifi_ready = wifi_error == ESP_OK;
    app.capture_ready = app.wifi_ready && radio_capture_init() == ESP_OK;
    uart_set_baudrate(UART_NUM_0, engine.config.baud);
    app.error = !app.led_ready || !app.gps_ready || !app.wifi_ready || !app.memory_ready ||
                !app.capture_ready;
    send_message("FBR1\t0\tREADY\t%s\t%" PRIu32, VERSION, app.boot);
    if (!app.capture_ready)
        send_message("FBR1\t0\tERROR\tRADIO_CAPTURE_INIT");
    if (!app.memory_ready)
        send_message("FBR1\t0\tERROR\tAP_MEMORY_UNAVAILABLE");
    if (!app.gps_ready)
        send_message("FBR1\t0\tERROR\tGPS_INIT");
    if (!app.led_ready)
        send_message("FBR1\t0\tERROR\tLED_INIT");
    if (!app.wifi_ready)
        send_message("FBR1\t0\tERROR\tWIFI_INIT\t%d", wifi_error);
    int64_t last_status = 0;
    uint8_t bytes[256];
    for (;;) {
        uart_event_t event;
        while (xQueueReceive(app.uart_events, &event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
                ++engine.stats.uart_dropped_frames;
                ++engine.lifetime.uart_dropped_frames;
                uart_flush_input(UART_NUM_0);
                app.reader.used = 0;
                app.reader.dropping = true;
                send_message("FBR1\t0\tERROR\tUART_RX");
            }
        }
        int length = uart_read_bytes(UART_NUM_0, bytes, sizeof(bytes), 0);
        for (int i = 0; i < length; ++i)
            if (wire_feed(&app.reader, (char)bytes[i]))
                handle_command(app.reader.data);
        process_scan_event();
        if (app.capture_ready)
            radio_capture_drain(&engine.config);
        advance_scanner();
        if (now_ms() - last_status >= 1000) {
            send_status(0);
            if (app.enhanced)
                send_stats(0, false);
            if (app.led_ready &&
                status_led_update(app.running, current_fix().valid, app.error) != ESP_OK) {
                app.led_ready = false;
                app.error = true;
                send_message("FBR1\t0\tERROR\tLED_WRITE");
            }
            last_status = now_ms();
        }
        /* Fine scheduling without a busy loop; GPS/Wi-Fi tasks have higher priority. */
        vTaskDelay(1);
    }
}
