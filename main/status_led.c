#include "status_led.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "sdkconfig.h"
#include <stdint.h>

static rmt_channel_handle_t channel;
static rmt_encoder_handle_t encoder;
static uint32_t previous = UINT32_MAX;

esp_err_t status_led_init(void) {
    rmt_tx_channel_config_t config = {
        .gpio_num = CONFIG_FEB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&config, &channel);
    if (err != ESP_OK)
        return err;
    rmt_copy_encoder_config_t copy = {};
    err = rmt_new_copy_encoder(&copy, &encoder);
    if (err != ESP_OK)
        return err;
    return rmt_enable(channel);
}

esp_err_t status_led_update(bool running, bool gps_valid, bool error) {
    /* Low brightness: red error, amber searching, green recording, blue idle. */
    uint8_t r = error ? 16 : (running && !gps_valid ? 12 : 0);
    uint8_t g = error ? 0 : (running ? (gps_valid ? 16 : 6) : 0);
    uint8_t b = !error && !running ? 8 : 0;
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    if (grb == previous)
        return ESP_OK;
    /* Static storage remains valid even if the hardware completion wait fails. */
    static rmt_symbol_word_t symbols[25];
    for (unsigned i = 0; i < 24; ++i) {
        bool one = (grb & (1UL << (23 - i))) != 0;
        symbols[i].level0 = 1;
        symbols[i].duration0 = one ? 9 : 3;
        symbols[i].level1 = 0;
        symbols[i].duration1 = one ? 3 : 9;
    }
    symbols[24].duration0 = 500;
    symbols[24].duration1 = 500;
    rmt_transmit_config_t tx = {.loop_count = 0};
    esp_err_t err = rmt_transmit(channel, encoder, symbols, sizeof(symbols), &tx);
    if (err != ESP_OK)
        return err;
    err = rmt_tx_wait_all_done(channel, 100);
    if (err == ESP_OK)
        previous = grb;
    return err;
}
