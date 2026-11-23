#pragma once
#include "nmea.h"

#define WIGLE_ROW_MAX 512
#define WIGLE_METADATA                                                                             \
    "WigleWifi-1.4,appRelease=0.3.1,model=Feberis "                                                \
    "Pro,release=0.3.1,device=ESP32-C5,display=Flipper Zero,board=Feberis Pro,brand=Sapsan\n"
#define WIGLE_COLUMNS                                                                              \
    "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,"    \
    "AccuracyMeters,Type\n"

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[32];
    size_t ssid_length;
    const char *auth;
    unsigned channel;
    int rssi;
} ap_observation_t;

/* No row is produced without a fresh GPS fix. Returns length including LF, or zero. */
size_t wigle_row(const ap_observation_t *ap, const gps_fix_t *fix, char *out, size_t capacity);
void wigle_mac(const uint8_t *bssid, char *out);
