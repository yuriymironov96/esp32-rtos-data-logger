#pragma once
#include "driver/i2c_master.h"

void bus_init(void);
esp_err_t bus_add(uint8_t address, i2c_master_dev_handle_t *device);
esp_err_t bus_read(i2c_master_dev_handle_t device, uint8_t reg, void *data, size_t size);
esp_err_t bus_write(i2c_master_dev_handle_t device, const void *data, size_t size);
