"""Run storage and sensor tests on a host with Python 3 and a C compiler."""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HEADERS = {
    "esp_err.h": """
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_INVALID_ARG 5
#define ESP_ERR_INVALID_RESPONSE 6
#define ESP_ERR_TIMEOUT 7
""",
    "esp_partition.h": """
#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef struct { size_t size; } esp_partition_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
esp_err_t esp_partition_write(const esp_partition_t *, size_t, const void *, size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
""",
    "driver/i2c_master.h": """
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *i2c_master_dev_handle_t;
""",
    "freertos/FreeRTOS.h": "#pragma once\n#define pdMS_TO_TICKS(ms) (ms)\n",
    "freertos/task.h": "#pragma once\nstatic inline void vTaskDelay(unsigned ticks) { (void)ticks; }\n",
}

with tempfile.TemporaryDirectory(prefix="rtos-logger-tests-") as directory:
    work = Path(directory)
    for name, content in HEADERS.items():
        header = work / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(content)
    binary = work / "test_logger"
    subprocess.run(
        [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
         "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         "-I", str(work), "-I", str(ROOT / "include"),
         str(ROOT / "tests/test_logger.c"), str(ROOT / "src/flash_store.c"),
         str(ROOT / "src/rtc.c"), str(ROOT / "src/bme280.c"), "-lm", "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
