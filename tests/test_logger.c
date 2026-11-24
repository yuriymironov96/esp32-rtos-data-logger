#include "bme280.h"
#include "bus.h"
#include "esp_partition.h"
#include "flash_store.h"
#include "rtc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[2 * LOG_SECTOR_SIZE], bme[256], rtc[256];
static const esp_partition_t partition = {.size = sizeof(flash)};
static unsigned writes;
static bool fail_write, fail_read, fail_erase, missing;

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_ANY);
    assert(!strcmp(label, "logdata"));
    return missing ? NULL : &partition;
}

esp_err_t esp_partition_read(const esp_partition_t *p, size_t offset, void *data, size_t size)
{
    assert(p == &partition && offset + size <= sizeof(flash));
    if (fail_read) {
        return ESP_FAIL;
    }
    memcpy(data, flash + offset, size);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *p, size_t offset, const void *data,
                              size_t size)
{
    assert(p == &partition && offset + size <= sizeof(flash));
    assert(offset % LOG_SECTOR_SIZE == 0 && size == LOG_SECTOR_SIZE);
    for (size_t i = 0; i < size; i++) {
        assert(flash[offset + i] == 0xFF);
    }
    writes++;
    memcpy(flash + offset, data, fail_write ? 3 : size);
    return fail_write ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t size)
{
    assert(p == &partition && offset + size <= sizeof(flash));
    if (fail_erase) {
        return ESP_FAIL;
    }
    memset(flash + offset, 0xFF, size);
    return ESP_OK;
}

esp_err_t bus_add(uint8_t address, i2c_master_dev_handle_t *device)
{
    *device = address == 0x68 ? rtc : bme;
    return ESP_OK;
}

esp_err_t bus_read(i2c_master_dev_handle_t device, uint8_t reg, void *data, size_t size)
{
    assert(reg + size <= 256);
    memcpy(data, (uint8_t *)device + reg, size);
    return ESP_OK;
}

esp_err_t bus_write(i2c_master_dev_handle_t device, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    assert(bytes[0] + size - 1 <= 256);
    memcpy((uint8_t *)device + bytes[0], bytes + 1, size - 1);
    return ESP_OK;
}

static void test_flash(void)
{
    const log_record_t record = {
        .timestamp = 1795478400, .temperature = -1234, .humidity = 4567, .pressure = 760};
    memset(flash, 0xFF, sizeof(flash));
    assert(flash_store_init() == ESP_OK);
    for (unsigned i = 0; i < LOG_RECORDS_PER_SECTOR - 1; i++) {
        assert(flash_store_append(&record) == ESP_OK);
    }
    assert(writes == 0 && flash_store_status().buffered == 255);
    assert(flash_store_append(&record) == ESP_OK);
    assert(writes == 1 && !memcmp(flash, &record, sizeof(record)));
    assert(flash_store_status().buffered == 0 && flash_store_status().sectors_used == 1);
    assert(flash_store_append(&record) == ESP_OK);
    flash_store_enable(false);
    assert(flash_store_flush() == ESP_OK); // Flush also works while logging is off.
    assert(writes == 2 && flash_store_status().full && !flash_store_status().enabled);
    for (unsigned i = LOG_SECTOR_SIZE + sizeof(record); i < sizeof(flash); i++) {
        assert(flash[i] == 0xFF);
    }
    flash_store_enable(true);
    assert(!flash_store_status().enabled && flash_store_append(&record) != ESP_OK);
    assert(flash_store_init() == ESP_OK && flash_store_status().full);
    assert(flash_store_erase() == ESP_OK && !flash_store_status().enabled);

    flash_store_enable(true);
    assert(flash_store_append(&record) == ESP_OK);
    assert(flash_store_init() == ESP_OK); // Reboot loses only buffered records.
    assert(!flash_store_status().buffered && flash_store_status().sectors_used == 0);
    flash[37] = 0;
    assert(flash_store_init() == ESP_OK && flash_store_status().sectors_used == 1);
    assert(flash_store_append(&record) == ESP_OK && flash_store_flush() == ESP_OK);
    assert(flash[37] == 0 && !memcmp(flash + LOG_SECTOR_SIZE, &record, sizeof(record)));

    assert(flash_store_erase() == ESP_OK);
    flash_store_enable(true);
    assert(flash_store_append(&record) == ESP_OK);
    fail_write = true;
    assert(flash_store_flush() == ESP_FAIL && flash_store_status().error &&
           !flash_store_status().enabled);
    fail_write = false;
    assert(flash_store_init() == ESP_OK && flash_store_status().sectors_used == 1);
    assert(flash_store_append(&record) == ESP_OK && flash_store_flush() == ESP_OK);
    assert(flash_store_status().full);
    assert(flash_store_erase() == ESP_OK);
    flash[LOG_SECTOR_SIZE + 4000] = 0; // Do not reuse holes before occupied sectors.
    assert(flash_store_init() == ESP_OK && flash_store_status().full);
    fail_read = true;
    assert(flash_store_init() == ESP_FAIL && flash_store_status().error);
    fail_read = false;
    fail_erase = true;
    assert(flash_store_erase() == ESP_FAIL && flash_store_status().error);
    fail_erase = false;
    missing = true;
    assert(flash_store_init() == ESP_ERR_NOT_FOUND && flash_store_status().error);
    flash_store_enable(true);
    assert(!flash_store_status().enabled);
    puts("PASS: buffering, flush, full partition, restart, torn writes, I/O errors");
}

static void test_rtc(void)
{
    uint64_t timestamp, readback;
    assert(ds1307_init() == ESP_OK);
    assert(rtc_parse_time("2024-02-29T23:59:59", &timestamp));
    assert(timestamp == 1709251199);
    assert(ds1307_set(timestamp) == ESP_OK && !(rtc[0] & 0x80));
    assert(ds1307_read(&readback) == ESP_OK && timestamp == readback);
    char text[32];
    rtc_format_time(timestamp, text, sizeof(text));
    assert(!strcmp(text, "2024-02-29 23:59:59"));
    rtc[2] = 0x72; // 12 PM in 12-hour mode.
    assert(ds1307_read(&readback) == ESP_OK && readback == timestamp - 11 * 3600);
    rtc[2] = 0x52; // 12 AM.
    assert(ds1307_read(&readback) == ESP_OK && readback == timestamp - 23 * 3600);
    rtc[0] |= 0x80;
    assert(ds1307_read(&readback) == ESP_ERR_INVALID_STATE);
    rtc[0] = 0x6A;
    assert(ds1307_read(&readback) == ESP_ERR_INVALID_RESPONSE);
    const char *invalid[] = {"2023-02-29T00:00:00",    "2100-01-01T00:00:00", "2024-13-01T00:00:00",
                             "2024-01-00T00:00:00",    "2024-01-01T24:00:00", "2024-01-01T00:00:60",
                             "2024-01-01T00:00:00junk"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        assert(!rtc_parse_time(invalid[i], &timestamp));
    }
    puts("PASS: DS1307 UTC round trip, BCD, oscillator flag, 12-hour mode, invalid dates");
}

static void put16(unsigned reg, int value)
{
    bme[reg] = value & 255;
    bme[reg + 1] = ((unsigned)value >> 8) & 255;
}

static void test_bme(void)
{
    bme[0xD0] = 0x60;
    put16(0x88, 27504);
    put16(0x8A, 26435);
    put16(0x8C, -1000);
    put16(0x8E, 36477);
    put16(0x90, -10685);
    put16(0x92, 3024);
    put16(0x94, 2855);
    put16(0x96, 140);
    put16(0x98, -7);
    put16(0x9A, 15500);
    put16(0x9C, -14600);
    put16(0x9E, 6000);
    bme[0xA1] = 75;
    put16(0xE1, 362);
    bme[0xE3] = 0;
    bme[0xE4] = 20;
    bme[0xE5] = 0x25;
    bme[0xE6] = 3;
    bme[0xE7] = 30;
    unsigned ap = 415148, at = 519888;
    bme[0xF7] = ap >> 12;
    bme[0xF8] = ap >> 4;
    bme[0xF9] = ap << 4;
    bme[0xFA] = at >> 12;
    bme[0xFB] = at >> 4;
    bme[0xFC] = at << 4;
    bme[0xFD] = 35000 >> 8;
    bme[0xFE] = 35000 & 255;
    log_record_t record = {0};
    assert(bme280_init() == ESP_OK && bme280_read(&record) == ESP_OK);
    assert(record.temperature == 2508 && record.pressure == 755);
    assert(record.humidity > 0 && record.humidity <= 10000);
    bme[0xF3] = 8;
    assert(bme280_read(&record) == ESP_ERR_TIMEOUT);
    bme[0xD0] = 0x58;
    assert(bme280_init() == ESP_ERR_NOT_FOUND);
    puts("PASS: BME280 Bosch compensation vector, busy flag, BMP280 rejection");
}

int main(void)
{
    test_flash();
    test_rtc();
    test_bme();
    return 0;
}
