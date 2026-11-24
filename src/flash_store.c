#include "flash_store.h"
#include "esp_partition.h"
#include <string.h>

static const esp_partition_t *partition;
static uint8_t buffer[LOG_SECTOR_SIZE];
static log_status_t status;

esp_err_t flash_store_init(void)
{
    status = (log_status_t){0};
    partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "logdata");
    if (!partition || partition->size % LOG_SECTOR_SIZE) {
        status.error = true;
        return ESP_ERR_NOT_FOUND;
    }
    status.sectors_total = partition->size / LOG_SECTOR_SIZE;
    // Skip every occupied sector, including a possibly interrupted write.
    for (unsigned sector = 0; sector < status.sectors_total; sector++) {
        esp_err_t err =
            esp_partition_read(partition, sector * LOG_SECTOR_SIZE, buffer, sizeof(buffer));
        if (err != ESP_OK) {
            status.error = true;
            return err;
        }
        for (unsigned i = 0; i < sizeof(buffer); i++) {
            if (buffer[i] != 0xFF) {
                status.sectors_used = sector + 1;
                break;
            }
        }
    }
    memset(buffer, 0xFF, sizeof(buffer));
    status.full = status.sectors_used == status.sectors_total;
    status.enabled = !status.full;
    return ESP_OK;
}

esp_err_t flash_store_flush(void)
{
    if (status.error) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!status.buffered) {
        return ESP_OK;
    }
    if (status.full) {
        return ESP_ERR_NO_MEM;
    }
    // Padding remains erased (0xFF); even an explicit flush writes one whole sector.
    esp_err_t err = esp_partition_write(partition, status.sectors_used * LOG_SECTOR_SIZE, buffer,
                                        sizeof(buffer));
    if (err != ESP_OK) {
        status.error = true;
        status.enabled = false;
        return err;
    }
    status.sectors_used++;
    status.buffered = 0;
    memset(buffer, 0xFF, sizeof(buffer));
    status.full = status.sectors_used == status.sectors_total;
    if (status.full) {
        status.enabled = false;
    }
    return ESP_OK;
}

esp_err_t flash_store_append(const log_record_t *record)
{
    if (!status.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(buffer + status.buffered * sizeof(*record), record, sizeof(*record));
    status.buffered++;
    return status.buffered == LOG_RECORDS_PER_SECTOR ? flash_store_flush() : ESP_OK;
}

esp_err_t flash_store_erase(void)
{
    if (!partition) {
        return ESP_ERR_INVALID_STATE;
    }
    // Erase one sector per call to the driver, allowing the scheduler to run.
    status.enabled = false;
    for (unsigned offset = 0; offset < partition->size; offset += LOG_SECTOR_SIZE) {
        esp_err_t err = esp_partition_erase_range(partition, offset, LOG_SECTOR_SIZE);
        if (err != ESP_OK) {
            status.error = true;
            return err;
        }
    }
    status = (log_status_t){.sectors_total = partition->size / LOG_SECTOR_SIZE};
    memset(buffer, 0xFF, sizeof(buffer));
    return ESP_OK;
}

void flash_store_enable(bool enabled)
{
    status.enabled = enabled && !status.full && !status.error;
}

log_status_t flash_store_status(void)
{
    return status;
}
