#ifdef NDEBUG
#undef NDEBUG
#endif
#include "delivery.h"
#include "nmea.h"
#include "wigle.h"
#include "wire.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void sentence(nmea_parser_t *parser, const char *body, int64_t now) {
    unsigned crc = 0;
    for (const char *p = body; *p; ++p)
        crc ^= (unsigned char)*p;
    char line[256];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, crc);
    for (const char *p = line; *p; ++p)
        nmea_feed(parser, *p, now);
}

static gps_fix_t valid_fix(void) {
    nmea_parser_t parser = {0};
    sentence(&parser, "GNRMC,123519.00,A,4807.038,N,01131.000,E,0,0,211126,,,A", 1000);
    sentence(&parser, "GNGGA,123519.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", 1000);
    gps_fix_t fix = nmea_snapshot(&parser, 1000);
    assert(fix.valid);
    return fix;
}

static void test_gps(void) {
    gps_fix_t fix = valid_fix();
    assert(fabs(fix.latitude - 48.1173) < 0.0000001);
    assert(fabs(fix.longitude - 11.5166666667) < 0.0000001);
    assert(!strcmp(fix.utc, "2026-11-21 12:35:19"));
    assert(fix.satellites == 8 && fabs(fix.altitude_m - 545.4) < 0.01);
    nmea_parser_t p = {0};
    assert(!nmea_snapshot(&p, 0).valid);
    sentence(&p, "GPRMC,123519,A,4807.038,N,01131.000,E,0,0,211126,,,A", 1000);
    assert(!nmea_snapshot(&p, 1000).valid);
    sentence(&p, "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", 1000);
    assert(nmea_snapshot(&p, 4000).valid);
    assert(!nmea_snapshot(&p, 4001).valid);
    sentence(&p, "GPRMC,123520,V,,,,,,,211126,,,N", 1100);
    assert(!nmea_snapshot(&p, 1100).valid);
    sentence(&p, "GPRMC,123520,A,4807.038,N,01131.000,E,0,0,211126,,,A", 1200);
    sentence(&p, "GPGGA,123520,4807.038,N,01131.000,E,0,00,99,0,M,0,M,,", 1200);
    assert(!nmea_snapshot(&p, 1200).valid);
    sentence(&p, "GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", 1300);
    sentence(&p, "GPRMC,123520,A,4860.038,N,01131.000,E,0,0,211126,,,A", 1300);
    assert(!nmea_snapshot(&p, 1300).valid);
    sentence(&p, "GPRMC,123520,A,0000.000,N,00000.000,E,0,0,211126,,,A", 1400);
    sentence(&p, "GPGGA,123520,0000.000,N,00000.000,E,1,08,0.9,545.4,M,46.9,M,,", 1400);
    assert(nmea_snapshot(&p, 1400).valid); /* Zero coordinates are real coordinates. */
    sentence(&p, "GPRMC,123520,A,4807.038,S,01131.000,W,0,0,311126,,,A", 1500);
    assert(!nmea_snapshot(&p, 1500).valid); /* November has 30 days. */
    sentence(&p, "GPRMC,123520,A,4807.038,S,01131.000,W,0,0,211126,,,A", 1600);
    assert(nmea_snapshot(&p, 1600).latitude < 0 && nmea_snapshot(&p, 1600).longitude < 0);
    sentence(&p, "GPGGA,123530,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", 1600);
    assert(!nmea_snapshot(&p, 1600).valid); /* Do not combine unrelated epochs. */
    memset(&p, 0, sizeof(p));
    const char *bad = "$GPRMC,123520,A,4807.038,N,01131.000,E,0,0,211126,,,A*00\r\n";
    for (const char *c = bad; *c; ++c)
        nmea_feed(&p, *c, 1700);
    assert(!p.rmc_valid);
}

static void test_wire(void) {
    assert(wire_crc("123456789", 9) == 0x29b1);
    char frame[WIRE_MAX];
    size_t length = wire_encode("FBR1\t1\tSTART\t2000", frame, sizeof(frame));
    assert(length);
    wire_reader_t reader = {0};
    for (size_t i = 0; i < length; ++i)
        assert(wire_feed(&reader, frame[i]) == (i == length - 1));
    char *fields[WIRE_FIELDS];
    assert(wire_decode(reader.data, fields, WIRE_FIELDS) == 4);
    assert(!strcmp(fields[3], "2000"));
    frame[6] = '9';
    frame[length - 1] = '\0';
    assert(!wire_decode(frame, fields, WIRE_FIELDS));
    for (unsigned i = 0; i < WIRE_MAX * 2; ++i)
        assert(!wire_feed(&reader, 'X'));
    assert(!wire_feed(&reader, '\n'));
    length = wire_encode("FBR1\t2\tSTOP", frame, sizeof(frame));
    for (size_t i = 0; i < length; ++i)
        wire_feed(&reader, frame[i]);
    assert(wire_decode(reader.data, fields, WIRE_FIELDS) == 3);
    uint32_t value;
    assert(wire_u32("4294967295", &value) && value == UINT32_MAX);
    assert(!wire_u32("4294967296", &value));
    assert(!wire_u32("-1", &value) && !wire_u32("1x", &value) && !wire_u32("", &value));
    assert(!wire_encode("FBR1\t1\tSTOP", frame, 10));
}

static void test_delivery(void) {
    delivery_t state = {0};
    delivery_begin(&state, 7, 1000);
    assert(!delivery_ack(&state, 6));
    assert(delivery_poll(&state, 2999) == DELIVERY_WAIT);
    assert(delivery_poll(&state, 3000) == DELIVERY_RETRY);
    assert(delivery_poll(&state, 5000) == DELIVERY_RETRY);
    assert(delivery_poll(&state, 7000) == DELIVERY_RETRY);
    assert(delivery_poll(&state, 9000) == DELIVERY_TIMEOUT);
    assert(!state.pending);
    delivery_begin(&state, 8, 10000);
    assert(delivery_ack(&state, 8));
    assert(!delivery_ack(&state, 8));
    assert(delivery_poll(&state, 999999) == DELIVERY_WAIT);
}

static ap_observation_t sample_ap(void) {
    ap_observation_t ap = {.bssid = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc},
                           .ssid = {'a', ',', '"', 'b', '\n', 0xff},
                           .ssid_length = 6,
                           .auth = "[WPA2-PSK][ESS]",
                           .channel = 36,
                           .rssi = -61};
    return ap;
}

static void test_csv(void) {
    char row[WIGLE_ROW_MAX];
    ap_observation_t ap = sample_ap();
    gps_fix_t fix = valid_fix();
    assert(wigle_row(&ap, &fix, row, sizeof(row)));
    assert(strstr(row, "\"a,\"\"b\\x0A\\xFF\"") != NULL);
    assert(!wigle_row(&ap, &fix, row, 8));
    fix.valid = false;
    assert(!wigle_row(&ap, &fix, row, sizeof(row)));
    fix = valid_fix();
    fix.latitude = NAN;
    assert(!wigle_row(&ap, &fix, row, sizeof(row)));
    fix = valid_fix();
    fix.longitude = 181;
    assert(!wigle_row(&ap, &fix, row, sizeof(row)));
    fix = valid_fix();
    fix.horizontal_accuracy_m = 3.2;
    assert(wigle_row(&ap, &fix, row, sizeof(row)) && strstr(row, ",3.2,WIFI"));
    ap.ssid_length = 0;
    assert(wigle_row(&ap, &fix, row, sizeof(row)) && strstr(row, ",\"\","));
    ap.auth = "[UNKNOWN][ESS]";
    assert(!wigle_row(&ap, &fix, row, sizeof(row)));
    ap.auth = "[WPA2-PSK][ESS]";
    memset(ap.ssid, 0xff, 32);
    ap.ssid_length = 32;
    assert(wigle_row(&ap, &fix, row, sizeof(row)));
}

static void test_noise(void) {
    nmea_parser_t parser = {0};
    wire_reader_t reader = {0};
    uint32_t rng = 42;
    for (unsigned i = 0; i < 100000; ++i) {
        rng = rng * 1664525U + 1013904223U;
        char c = (char)(rng >> 24);
        nmea_feed(&parser, c, i);
        if (wire_feed(&reader, c)) {
            char *fields[WIRE_FIELDS];
            wire_decode(reader.data, fields, WIRE_FIELDS);
        }
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--frame")) {
        char frame[WIRE_MAX];
        assert(wire_encode("FBR1\t1\tSTART\t2000", frame, sizeof(frame)));
        fputs(frame, stdout);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--csv")) {
        char row[WIGLE_ROW_MAX];
        ap_observation_t ap = sample_ap();
        gps_fix_t fix = valid_fix();
        assert(wigle_row(&ap, &fix, row, sizeof(row)));
        fputs(WIGLE_METADATA WIGLE_COLUMNS, stdout);
        fputs(row, stdout);
        return 0;
    }
    test_gps();
    test_wire();
    test_delivery();
    test_csv();
    test_noise();
    puts("GPS, framing, retransmission, CSV and noise tests passed");
    return 0;
}
