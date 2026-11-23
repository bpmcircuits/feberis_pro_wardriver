#include "gps_service.h"
#include "core/gps_history.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>

static nmea_parser_t parser;
static wd_gps_history_t history;
static SemaphoreHandle_t mutex;
static QueueHandle_t events;

static void gps_task(void *argument) {
    (void)argument;
    uart_event_t event;
    uint8_t bytes[256];
    for (;;) {
        if (xQueueReceive(events, &event, portMAX_DELAY) != pdTRUE)
            continue;
        if (event.type == UART_DATA) {
            size_t remaining = event.size;
            while (remaining) {
                int length = uart_read_bytes(UART_NUM_1, bytes,
                                             remaining > sizeof(bytes) ? sizeof(bytes) : remaining,
                                             pdMS_TO_TICKS(50));
                if (length <= 0)
                    break;
                xSemaphoreTake(mutex, portMAX_DELAY);
                int64_t now = esp_timer_get_time() / 1000;
                for (int i = 0; i < length; ++i)
                    wd_gps_history_feed(&history, &parser, (char)bytes[i], now);
                xSemaphoreGive(mutex);
                remaining -= (size_t)length;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                   event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
            uart_flush_input(UART_NUM_1);
            xQueueReset(events);
            xSemaphoreTake(mutex, portMAX_DELAY);
            memset(&parser, 0, sizeof(parser));
            wd_gps_history_push(&history, (gps_fix_t){.age_ms = UINT32_MAX},
                                esp_timer_get_time() / 1000);
            xSemaphoreGive(mutex);
        }
    }
}

esp_err_t gps_service_start(void) {
    mutex = xSemaphoreCreateMutex();
    if (!mutex)
        return ESP_ERR_NO_MEM;
    uart_config_t config = {
        .baud_rate = CONFIG_FEB_GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(UART_NUM_1, 4096, 0, 20, &events, 0);
    if (err != ESP_OK)
        return err;
    err = uart_param_config(UART_NUM_1, &config);
    if (err != ESP_OK)
        return err;
    err = uart_set_pin(UART_NUM_1, CONFIG_FEB_GPS_TX, CONFIG_FEB_GPS_RX, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
        return err;
    return xTaskCreate(gps_task, "gps", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

gps_fix_t gps_service_snapshot_at(int64_t at_ms) {
    if (!mutex)
        return wd_gps_history_at(&history, at_ms);
    xSemaphoreTake(mutex, portMAX_DELAY);
    gps_fix_t fix = wd_gps_history_at(&history, at_ms);
    xSemaphoreGive(mutex);
    return fix;
}
gps_fix_t gps_service_snapshot(void) {
    return gps_service_snapshot_at(esp_timer_get_time() / 1000);
}
