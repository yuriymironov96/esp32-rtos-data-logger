#include "bus.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static i2c_master_bus_handle_t bus;
static SemaphoreHandle_t mutex;

void bus_init(void)
{
    mutex = xSemaphoreCreateMutex();
    configASSERT(mutex);
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
}

esp_err_t bus_add(uint8_t address, i2c_master_dev_handle_t *device)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &config, device);
}

esp_err_t bus_read(i2c_master_dev_handle_t device, uint8_t reg, void *data, size_t size)
{
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit_receive(device, &reg, 1, data, size, 100);
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t bus_write(i2c_master_dev_handle_t device, const void *data, size_t size)
{
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit(device, data, size, 100);
    xSemaphoreGive(mutex);
    return err;
}
