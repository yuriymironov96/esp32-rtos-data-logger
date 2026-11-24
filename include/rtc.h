#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t ds1307_init(void);
esp_err_t ds1307_read(uint64_t *timestamp);
esp_err_t ds1307_set(uint64_t timestamp);
bool rtc_parse_time(const char *text, uint64_t *timestamp);
void rtc_format_time(uint64_t timestamp, char *text, unsigned size);
