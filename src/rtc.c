#include "rtc.h"
#include "bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static i2c_master_dev_handle_t device;

esp_err_t ds1307_init(void)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    return bus_add(0x68, &device);
}

static bool from_calendar(struct tm *date, uint64_t *timestamp)
{
    if (date->tm_year < 100 || date->tm_year > 199) {
        return false;
    }
    struct tm original = *date;
    time_t value = mktime(date);
    if (value < 0 || date->tm_mon != original.tm_mon || date->tm_mday != original.tm_mday ||
        date->tm_hour != original.tm_hour || date->tm_min != original.tm_min ||
        date->tm_sec != original.tm_sec) {
        return false;
    }
    *timestamp = (uint64_t)value;
    return true;
}

static int bcd(uint8_t value)
{
    if ((value & 15) > 9 || (value >> 4) > 9) {
        return -1000;
    }
    return (value >> 4) * 10 + (value & 15);
}

esp_err_t ds1307_read(uint64_t *timestamp)
{
    uint8_t data[7];
    esp_err_t err;
    if ((err = bus_read(device, 0, data, sizeof(data))) != ESP_OK) {
        return err;
    }
    if (data[0] & 0x80) {
        return ESP_ERR_INVALID_STATE;
    }
    int hour = bcd(data[2] & 0x3F);
    if (data[2] & 0x40) {
        int h12 = bcd(data[2] & 0x1F);
        if (h12 < 1 || h12 > 12) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        hour = h12 % 12 + ((data[2] & 0x20) ? 12 : 0);
    }
    struct tm date = {
        .tm_sec = bcd(data[0] & 0x7F),
        .tm_min = bcd(data[1]),
        .tm_hour = hour,
        .tm_mday = bcd(data[4]),
        .tm_mon = bcd(data[5]) - 1,
        .tm_year = bcd(data[6]) + 100,
    };
    return from_calendar(&date, timestamp) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t ds1307_set(uint64_t timestamp)
{
    time_t value = (time_t)timestamp;
    struct tm date;
    gmtime_r(&value, &date);
    if (date.tm_year < 100 || date.tm_year > 199) {
        return ESP_ERR_INVALID_ARG;
    }
    int fields[] = {date.tm_sec,  date.tm_min,     date.tm_hour,      date.tm_wday + 1,
                    date.tm_mday, date.tm_mon + 1, date.tm_year - 100};
    uint8_t data[8] = {0};
    for (unsigned i = 0; i < 7; i++) {
        data[i + 1] = ((fields[i] / 10) << 4) | (fields[i] % 10);
    }
    return bus_write(device, data, sizeof(data));
}

bool rtc_parse_time(const char *text, uint64_t *timestamp)
{
    int year, month, day, hour, minute, second, end = 0;
    if (sscanf(text, "%d-%d-%dT%d:%d:%d%n", &year, &month, &day, &hour, &minute, &second, &end) !=
            6 ||
        text[end] != '\0' || strlen(text) != 19) {
        return false;
    }
    struct tm date = {.tm_year = year - 1900,
                      .tm_mon = month - 1,
                      .tm_mday = day,
                      .tm_hour = hour,
                      .tm_min = minute,
                      .tm_sec = second};
    return from_calendar(&date, timestamp);
}

void rtc_format_time(uint64_t timestamp, char *text, unsigned size)
{
    time_t value = (time_t)timestamp;
    struct tm date;
    gmtime_r(&value, &date);
    strftime(text, size, "%Y-%m-%d %H:%M:%S", &date);
}
