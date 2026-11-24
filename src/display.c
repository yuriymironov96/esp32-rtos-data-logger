#include "display.h"
#include "bus.h"
#include "config.h"
#include "esp_timer.h"
#include "font5x7.h"
#include "rtc.h"
#include <stdio.h>
#include <string.h>

static i2c_master_dev_handle_t device;
static uint8_t frame[8][129];

static void line(unsigned row, const char *text)
{
    for (unsigned column = 0; *text && column < 21; column++, text++) {
        char ch = *text;
        const char *glyph = strchr(alphabet, ch);
        if (!glyph) {
            glyph = strchr(alphabet, '?');
        }
        memcpy(&frame[row][1 + column * 6], font[glyph - alphabet], 5);
    }
}

esp_err_t display_init(void)
{
    esp_err_t err;
    if (!device && (err = bus_add(OLED_ADDRESS, &device)) != ESP_OK) {
        return err;
    }
    const uint8_t commands[] = {
        0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x02,
        0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
    };
    return bus_write(device, commands, sizeof(commands));
}

esp_err_t display_update(const display_state_t *state)
{
    memset(frame, 0, sizeof(frame));
    char text[64];
    int64_t now = esp_timer_get_time();
    bool remote = (now / 5000000) % 2;
    if (remote) {
        const char *base = MQTT_SUB_BASE;
        const char *slash = strchr(base, '/');
        snprintf(text, sizeof(text), "%.*s", slash ? (int)(slash - base) : (int)strlen(base), base);
        line(0, text);
        line(1, slash ? slash + 1 : "REMOTE");
        if (slash && strlen(slash + 1) > 21) {
            line(2, slash + 22);
        }
        const char *units[] = {"C", "%", "MMHG"};
        const char *labels[] = {"T", "H", "P"};
        bool fresh = true;
        for (unsigned i = 0; i < 3; i++) {
            bool valid = state->remote_seen[i] &&
                         now - state->remote_seen[i] < REMOTE_STALE_SECONDS * 1000000LL;
            if (valid) {
                snprintf(text, sizeof(text), "%s %.2F %s", labels[i], (double)state->remote[i],
                         units[i]);
            } else {
                snprintf(text, sizeof(text), "%s -- %s", labels[i], units[i]);
            }
            line(i + 3, text);
            fresh &= valid;
        }
        line(6, fresh ? "REMOTE LIVE" : "REMOTE WAIT / STALE");
    } else {
        line(0, "LOCAL BME280 / DS1307");
        if (state->local.rtc_ok) {
            rtc_format_time(state->local.record.timestamp, text, sizeof(text));
            line(1, text);
        } else {
            line(1, "SET RTC: TIME COMMAND");
        }
        if (state->local.sensor_ok) {
            snprintf(text, sizeof(text), "T %.2F C", state->local.record.temperature / 100.0);
            line(3, text);
            snprintf(text, sizeof(text), "H %.2F %%", state->local.record.humidity / 100.0);
            line(4, text);
            snprintf(text, sizeof(text), "P %u MMHG", state->local.record.pressure);
            line(5, text);
        } else {
            line(3, "BME280 READ ERROR");
        }
        snprintf(text, sizeof(text), "WIFI %s MQTT %s", state->wifi ? "ON" : "OFF",
                 state->mqtt ? "ON" : "OFF");
        line(6, text);
    }
    if (state->log.error) {
        line(7, "FLASH ERROR LOG OFF");
    } else if (state->log.full) {
        line(7, "FLASH FULL LOG OFF");
    } else {
        snprintf(text, sizeof(text), "LOG %s RAM %u/256", state->log.enabled ? "ON" : "OFF",
                 state->log.buffered);
        line(7, text);
    }
    for (unsigned page = 0; page < 8; page++) {
        uint8_t position[] = {0x00, 0xB0 | page, 0x00, 0x10};
        esp_err_t err = bus_write(device, position, sizeof(position));
        if (err != ESP_OK) {
            return err;
        }
        frame[page][0] = 0x40;
        if ((err = bus_write(device, frame[page], sizeof(frame[page]))) != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
