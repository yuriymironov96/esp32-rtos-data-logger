#include "bme280.h"
#include "bus.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static i2c_master_dev_handle_t device;
static struct {
    uint16_t t1, p1;
    int16_t t2, t3, p2, p3, p4, p5, p6, p7, p8, p9;
    uint8_t h1, h3;
    int16_t h2, h4, h5;
    int8_t h6;
} cal;

static uint16_t u16(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}
static int16_t s16(const uint8_t *p)
{
    return (int16_t)u16(p);
}
static int16_t signed12(unsigned n)
{
    return n & 0x800 ? (int)n - 4096 : (int)n;
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return bus_write(device, data, sizeof(data));
}

esp_err_t bme280_init(void)
{
    esp_err_t err;
    if (!device && (err = bus_add(BME280_ADDRESS, &device)) != ESP_OK) {
        return err;
    }
    uint8_t id;
    if ((err = bus_read(device, 0xD0, &id, 1)) != ESP_OK) {
        return err;
    }
    if (id != 0x60) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((err = write_reg(0xE0, 0xB6)) != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t a[26], h[7];
    if ((err = bus_read(device, 0x88, a, sizeof(a))) != ESP_OK) {
        return err;
    }
    if ((err = bus_read(device, 0xE1, h, sizeof(h))) != ESP_OK) {
        return err;
    }
    cal.t1 = u16(a);
    cal.t2 = s16(a + 2);
    cal.t3 = s16(a + 4);
    cal.p1 = u16(a + 6);
    cal.p2 = s16(a + 8);
    cal.p3 = s16(a + 10);
    cal.p4 = s16(a + 12);
    cal.p5 = s16(a + 14);
    cal.p6 = s16(a + 16);
    cal.p7 = s16(a + 18);
    cal.p8 = s16(a + 20);
    cal.p9 = s16(a + 22);
    cal.h1 = a[25];
    cal.h2 = s16(h);
    cal.h3 = h[2];
    cal.h4 = signed12((h[3] << 4) | (h[4] & 0x0F));
    cal.h5 = signed12((h[5] << 4) | (h[4] >> 4));
    cal.h6 = (int8_t)h[6];
    if (!cal.t1 || !cal.p1 || cal.p1 == UINT16_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return write_reg(0xF2, 1); // Humidity oversampling x1, latched by ctrl_meas.
}

esp_err_t bme280_read(log_record_t *record)
{
    esp_err_t err = write_reg(0xF4, 0x25); // Forced mode, temperature/pressure x1.
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t raw[8], status;
    if ((err = bus_read(device, 0xF3, &status, 1)) != ESP_OK) {
        return err;
    }
    if (status & 0x08) {
        return ESP_ERR_TIMEOUT;
    }
    if ((err = bus_read(device, 0xF7, raw, sizeof(raw))) != ESP_OK) {
        return err;
    }
    int32_t ap = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);
    int32_t at = (raw[3] << 12) | (raw[4] << 4) | (raw[5] >> 4);
    int32_t ah = (raw[6] << 8) | raw[7];
    if (ap == 0x80000 || at == 0x80000 || ah == 0x8000) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Floating-point compensation from Bosch BME280 datasheet, section 8.1.
    double v1 = (at / 16384.0 - cal.t1 / 1024.0) * cal.t2;
    double v2 = (at / 131072.0 - cal.t1 / 8192.0);
    v2 = v2 * v2 * cal.t3;
    double fine = v1 + v2;
    double temperature = fine / 5120.0;

    v1 = fine / 2.0 - 64000.0;
    v2 = v1 * v1 * cal.p6 / 32768.0;
    v2 += v1 * cal.p5 * 2.0;
    v2 = v2 / 4.0 + cal.p4 * 65536.0;
    v1 = (cal.p3 * v1 * v1 / 524288.0 + cal.p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * cal.p1;
    if (v1 == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    double pressure = (1048576.0 - ap - v2 / 4096.0) * 6250.0 / v1;
    v1 = cal.p9 * pressure * pressure / 2147483648.0;
    v2 = pressure * cal.p8 / 32768.0;
    pressure += (v1 + v2 + cal.p7) / 16.0;

    double humidity = fine - 76800.0;
    humidity = (ah - (cal.h4 * 64.0 + cal.h5 / 16384.0 * humidity)) *
               (cal.h2 / 65536.0 *
                (1.0 + cal.h6 / 67108864.0 * humidity * (1.0 + cal.h3 / 67108864.0 * humidity)));
    humidity *= 1.0 - cal.h1 * humidity / 524288.0;
    humidity = fmax(0.0, fmin(100.0, humidity));
    if (!isfinite(temperature) || !isfinite(pressure) || !isfinite(humidity) || temperature < -40 ||
        temperature > 85 || pressure < 30000 || pressure > 110000) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    record->temperature = (int32_t)lround(temperature * 100);
    record->humidity = (uint16_t)lround(humidity * 100);
    record->pressure = (uint16_t)lround(pressure / 133.322387415);
    return ESP_OK;
}
