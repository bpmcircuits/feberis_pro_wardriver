#include "nmea.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool number(const char *text, double *out) {
    if (!*text)
        return false;
    const char *p = text;
    if (*p == '-')
        ++p;
    bool digit = false, point = false;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9')
            digit = true;
        else if (*p == '.' && !point)
            point = true;
        else
            return false;
    }
    if (!digit)
        return false;
    char *end;
    *out = strtod(text, &end);
    return !*end && isfinite(*out);
}

static bool digits(const char *text, size_t length) {
    if (strlen(text) < length)
        return false;
    for (size_t i = 0; i < length; ++i)
        if (text[i] < '0' || text[i] > '9')
            return false;
    return true;
}

static int pair(const char *text) { return (text[0] - '0') * 10 + text[1] - '0'; }

static int time_of_day(const char *text) {
    double time;
    if (!digits(text, 6) || !number(text, &time))
        return -1;
    if (text[6] && (text[6] != '.' || !text[7] || !digits(text + 7, strlen(text + 7))))
        return -1;
    int h = pair(text), m = pair(text + 2), s = pair(text + 4);
    if (h > 23 || m > 59 || s > 59)
        return -1;
    return h * 3600 + m * 60 + s;
}

static bool coordinate(const char *text, const char *direction, bool latitude, double *out) {
    double raw;
    size_t width = latitude ? 4 : 5;
    if (!digits(text, width) || (text[width] && text[width] != '.') || !number(text, &raw) ||
        raw < 0 || strlen(direction) != 1)
        return false;
    if (latitude ? (*direction != 'N' && *direction != 'S')
                 : (*direction != 'E' && *direction != 'W'))
        return false;
    double degrees = floor(raw / 100), minutes = raw - degrees * 100;
    *out = degrees + minutes / 60;
    if (minutes >= 60 || *out > (latitude ? 90 : 180))
        return false;
    if (*direction == 'S' || *direction == 'W')
        *out = -*out;
    return true;
}

static bool timestamp(const char *date, const char *time, char *out) {
    if (strlen(date) != 6 || !digits(date, 6) || time_of_day(time) < 0)
        return false;
    int d = pair(date), m = pair(date + 2), y = 2000 + pair(date + 4);
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1 || d > days[m - 1] + (m == 2 && y % 4 == 0))
        return false;
    memcpy(out, "2000-00-00 00:00:00", 20);
    memcpy(out + 2, date + 4, 2);
    memcpy(out + 5, date + 2, 2);
    memcpy(out + 8, date, 2);
    memcpy(out + 11, time, 2);
    memcpy(out + 14, time + 2, 2);
    memcpy(out + 17, time + 4, 2);
    return true;
}

static void parse_rmc(nmea_parser_t *p, char **f, size_t count, int64_t now) {
    p->rmc_valid = false;
    if (count < 10 || strcmp(f[2], "A") != 0 || !coordinate(f[3], f[4], true, &p->fix.latitude) ||
        !coordinate(f[5], f[6], false, &p->fix.longitude) || !timestamp(f[9], f[1], p->fix.utc))
        return;
    double speed = NAN, course = NAN;
    if ((f[7][0] && (!number(f[7], &speed) || speed < 0 || speed > 2000)) ||
        (f[8][0] && (!number(f[8], &course) || course < 0 || course >= 360)))
        return;
    p->fix.speed_kmh = speed * 1.852;
    p->fix.course_deg = course;
    p->fix.horizontal_accuracy_m = NAN;
    p->rmc_second = time_of_day(f[1]);
    p->rmc_ms = now;
    p->rmc_valid = true;
}

static void parse_gga(nmea_parser_t *p, char **f, size_t count, int64_t now) {
    p->gga_valid = false;
    double quality, satellites, altitude = NAN, hdop, lat, lon;
    if (count < 11 || !number(f[6], &quality) || quality < 1 || quality > 5 ||
        floor(quality) != quality || !number(f[7], &satellites) || satellites < 0 ||
        satellites > 99 || floor(satellites) != satellites || !number(f[8], &hdop) || hdop <= 0 ||
        hdop > 99 || !coordinate(f[2], f[3], true, &lat) || !coordinate(f[4], f[5], false, &lon) ||
        (f[9][0] && (!number(f[9], &altitude) || strcmp(f[10], "M") != 0)))
        return;
    p->gga_second = time_of_day(f[1]);
    if (p->gga_second < 0)
        return;
    p->gga_latitude = lat;
    p->gga_longitude = lon;
    p->fix.altitude_m = altitude;
    p->fix.hdop = hdop;
    p->fix.satellites = (unsigned)satellites;
    p->gga_ms = now;
    p->gga_valid = true;
}

static void parse_line(nmea_parser_t *p, int64_t now) {
    char *line = p->line;
    char *star = strrchr(line, '*');
    if (*line != '$' || !star || strlen(star + 1) != 2 || !isxdigit((unsigned char)star[1]) ||
        !isxdigit((unsigned char)star[2]))
        return;
    unsigned checksum = 0;
    for (char *c = line + 1; c < star; ++c)
        checksum ^= (uint8_t)*c;
    char *end;
    unsigned long supplied = strtoul(star + 1, &end, 16);
    if (*end || end != star + 3 || supplied != checksum)
        return;
    *star = '\0';
    char *fields[24] = {line + 1};
    size_t count = 1;
    for (char *c = line + 1; c < star; ++c) {
        if (*c == ',') {
            if (count == 24)
                return;
            *c = '\0';
            fields[count++] = c + 1;
        }
    }
    if (strlen(fields[0]) != 5 || fields[0][0] != 'G')
        return;
    if (strcmp(fields[0] + 2, "RMC") == 0)
        parse_rmc(p, fields, count, now);
    if (strcmp(fields[0] + 2, "GGA") == 0)
        parse_gga(p, fields, count, now);
}

void nmea_feed(nmea_parser_t *p, char byte, int64_t now_ms) {
    if (byte == '$') {
        p->used = 0;
        p->dropping = false;
    }
    if (byte == '\r')
        return;
    if (byte == '\n') {
        if (!p->dropping && p->used) {
            p->line[p->used] = '\0';
            parse_line(p, now_ms);
        }
        p->used = 0;
        p->dropping = false;
    } else if (!p->dropping) {
        if (byte == '\0' || p->used == sizeof(p->line) - 1)
            p->dropping = true;
        else
            p->line[p->used++] = byte;
    }
}

gps_fix_t nmea_snapshot_max_age(const nmea_parser_t *p, int64_t now_ms, uint32_t max_age_ms) {
    gps_fix_t fix = p->fix;
    uint64_t age = now_ms >= p->rmc_ms ? (uint64_t)now_ms - (uint64_t)p->rmc_ms : UINT64_MAX;
    uint64_t gga_age = now_ms >= p->gga_ms ? (uint64_t)now_ms - (uint64_t)p->gga_ms : UINT64_MAX;
    int delta = abs(p->rmc_second - p->gga_second);
    if (delta > 43200)
        delta = 86400 - delta;
    const double radians = 0.017453292519943295;
    double x = sin((fix.latitude - p->gga_latitude) * radians / 2);
    double y = sin((fix.longitude - p->gga_longitude) * radians / 2);
    double h = x * x + cos(fix.latitude * radians) * cos(p->gga_latitude * radians) * y * y;
    double separation = 12742000.0 * asin(sqrt(fmin(1.0, fmax(0.0, h))));
    double speed = isfinite(fix.speed_kmh) ? fix.speed_kmh / 3.6 : 0;
    /* Allow reception skew/subsecond motion, reject mixing geographically unrelated fixes. */
    bool coherent = separation <= 100.0 + speed * (delta + 1);
    fix.valid = coherent && p->rmc_valid && p->gga_valid && age <= max_age_ms &&
                gga_age <= max_age_ms && delta <= 1;
    uint64_t oldest = age > gga_age ? age : gga_age;
    fix.age_ms = oldest > UINT32_MAX ? UINT32_MAX : (uint32_t)oldest;
    if (!p->rmc_valid || age > max_age_ms)
        fix.utc[0] = 0;
    return fix;
}

gps_fix_t nmea_snapshot(const nmea_parser_t *p, int64_t now_ms) {
    return nmea_snapshot_max_age(p, now_ms, GPS_MAX_AGE_MS);
}
