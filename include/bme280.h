#pragma once
#include "esp_err.h"
#include "model.h"

esp_err_t bme280_init(void);
esp_err_t bme280_read(log_record_t *record);
