#pragma once
#include "esp_err.h"
#include "model.h"

#define LOG_SECTOR_SIZE 4096
#define LOG_RECORDS_PER_SECTOR (LOG_SECTOR_SIZE / sizeof(log_record_t))

// Only Flash Writer Task may access the store after initialization.
esp_err_t flash_store_init(void);
esp_err_t flash_store_append(const log_record_t *record);
esp_err_t flash_store_flush(void);
esp_err_t flash_store_erase(void);
void flash_store_enable(bool enabled);
log_status_t flash_store_status(void);
