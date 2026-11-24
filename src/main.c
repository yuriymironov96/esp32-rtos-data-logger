#include "bme280.h"
#include "bus.h"
#include "config.h"
#include "display.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "flash_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "model.h"
#include "network.h"
#include "nvs_flash.h"
#include "rtc.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool is_record;
    command_t command;
    log_record_t record;
} flash_message_t;

static QueueHandle_t events, display_queue, flash_queue, flash_status_queue, uart_events;
static TaskHandle_t sensor_handle;

static void sensor_task(void *arg)
{
    bool ready = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!ready) {
            ready = bme280_init() == ESP_OK;
        }
        app_event_t event = {.type = EVENT_SAMPLE};
        event.sample.sensor_ok = ready && bme280_read(&event.sample.record) == ESP_OK;
        if (!event.sample.sensor_ok) {
            ready = false;
            ESP_LOGW("sensor", "BME280 unavailable; check address and wiring");
        }
        event.sample.rtc_ok = ds1307_read(&event.sample.record.timestamp) == ESP_OK;
        if (xQueueSend(events, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW("sensor", "Sample queue full");
        }
    }
}

static void print_flash_status(log_status_t status)
{
    printf("Logging %s | sectors %" PRIu32 "/%" PRIu32 " | RAM %u/%u | full=%d error=%d\n",
           status.enabled ? "ON" : "OFF", status.sectors_used, status.sectors_total,
           status.buffered, (unsigned)LOG_RECORDS_PER_SECTOR, status.full, status.error);
}

static void print_status(const display_state_t *state)
{
    char time[32] = "RTC NOT SET";
    if (state->local.rtc_ok) {
        rtc_format_time(state->local.record.timestamp, time, sizeof(time));
    }
    printf("%s UTC | BME280 %s | Wi-Fi %s | MQTT %s\n", time,
           state->local.sensor_ok ? "OK" : "ERROR", network_wifi_connected() ? "ON" : "OFF",
           network_mqtt_connected() ? "ON" : "OFF");
    printf("T %.2f C | H %.2f %% | P %u mmHg | interval %u min\n",
           state->local.record.temperature / 100.0, state->local.record.humidity / 100.0,
           state->local.record.pressure, (unsigned)LOG_INTERVAL_MINUTES);
    esp_pm_dump_locks(stdout);
}

static void flash_writer_task(void *arg)
{
    log_status_t previous = flash_store_status();
    xQueueOverwrite(flash_status_queue, &previous);
    if (previous.full) {
        ESP_LOGW("flash", "FLASH FULL: logging stopped");
    }
    flash_message_t message;
    for (;;) {
        xQueueReceive(flash_queue, &message, portMAX_DELAY);
        esp_err_t err = ESP_OK;
        if (message.is_record) {
            // A preceding log-off command may have disabled logging already.
            if (flash_store_status().enabled) {
                err = flash_store_append(&message.record);
            }
        } else {
            switch (message.command) {
            case CMD_LOG_ON:
                flash_store_enable(true);
                break;
            case CMD_LOG_OFF:
                flash_store_enable(false);
                break;
            case CMD_FLUSH:
                err = flash_store_flush();
                break;
            case CMD_ERASE:
                err = flash_store_erase();
                break;
            default:
                break;
            }
        }
        log_status_t status = flash_store_status();
        if (err != ESP_OK) {
            ESP_LOGE("flash", "%s", esp_err_to_name(err));
        }
        if (status.full && !previous.full) {
            ESP_LOGW("flash", "FLASH FULL: logging stopped");
        }
        if (status.sectors_used > previous.sectors_used) {
            ESP_LOGI("flash", "Sector written: %" PRIu32 "/%" PRIu32, status.sectors_used,
                     status.sectors_total);
        }
        if (!message.is_record) {
            print_flash_status(status);
        }
        xQueueOverwrite(flash_status_queue, &status);
        previous = status;
    }
}

static void display_task(void *arg)
{
    bool ready = false;
    display_state_t state;
    for (;;) {
        xQueueReceive(display_queue, &state, portMAX_DELAY);
        if (!ready) {
            ready = display_init() == ESP_OK;
        }
        if (ready) {
            ready = display_update(&state) == ESP_OK;
        }
        if (!ready) {
            ESP_LOGW("display", "SSD1306 unavailable; check address and wiring");
        }
    }
}

static void handle_line(const char *line)
{
    if (!*line) {
        return;
    }
    app_event_t event = {.type = EVENT_COMMAND};
    if (!strcmp(line, "status")) {
        event.command.type = CMD_STATUS;
    } else if (!strcmp(line, "log on")) {
        event.command.type = CMD_LOG_ON;
    } else if (!strcmp(line, "log off")) {
        event.command.type = CMD_LOG_OFF;
    } else if (!strcmp(line, "flush")) {
        event.command.type = CMD_FLUSH;
    } else if (!strcmp(line, "erase")) {
        event.command.type = CMD_ERASE;
    } else if (!strncmp(line, "time ", 5) && rtc_parse_time(line + 5, &event.command.timestamp)) {
        event.command.type = CMD_TIME;
    } else {
        printf("Commands: status | log on | log off | flush | erase | time YYYY-MM-DDTHH:MM:SS "
               "(UTC)\n");
        return;
    }
    if (xQueueSend(events, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        printf("Busy; retry command\n");
    }
}

static void uart_task(void *arg)
{
    esp_pm_lock_handle_t console_lock;
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "console", &console_lock));
    ESP_ERROR_CHECK(esp_pm_lock_acquire(console_lock));
    bool active = true, overflow = false;
    unsigned used = 0;
    char line[96];
    printf("\nRTOS MQTT Logger | EMB1 6.8\nPress Enter to wake the console; type status for system "
           "state.\n");
    for (;;) {
        uart_event_t event;
        if (xQueueReceive(uart_events, &event,
                          active ? pdMS_TO_TICKS(UART_ACTIVE_SECONDS * 1000) : portMAX_DELAY) !=
            pdTRUE) {
            printf("Console idle; press Enter before the next command.\n");
            uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
            ESP_ERROR_CHECK(esp_pm_lock_release(console_lock));
            active = false;
            continue;
        }
        if (!active) {
            ESP_ERROR_CHECK(esp_pm_lock_acquire(console_lock));
            active = true;
        }
        if (event.type == UART_WAKEUP) {
            used = 0;
            overflow = false;
            printf("\nConsole awake\n");
        } else if (event.type == UART_DATA) {
            for (size_t i = 0; i < event.size; i++) {
                uint8_t ch;
                if (uart_read_bytes(UART_NUM_0, &ch, 1, 0) != 1) {
                    break;
                }
                if (ch == '\r' || ch == '\n') {
                    line[used] = '\0';
                    if (overflow) {
                        printf("Command too long\n");
                    } else {
                        handle_line(line);
                    }
                    used = 0;
                    overflow = false;
                } else if (ch == 8 || ch == 127) {
                    if (used) {
                        used--;
                    }
                } else if (ch >= 32 && ch <= 126) {
                    if (used < sizeof(line) - 1) {
                        line[used++] = ch;
                    } else {
                        overflow = true;
                    }
                }
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            uart_flush_input(UART_NUM_0);
            xQueueReset(uart_events);
            used = 0;
            overflow = false;
            printf("UART overflow; retry command\n");
        }
    }
}

static void manager_task(void *arg)
{
    display_state_t state = {0};
    int64_t next_sample = esp_timer_get_time(), next_log = 0;
    for (;;) {
        xQueueReceive(flash_status_queue, &state.log, 0);
        int64_t now = esp_timer_get_time();
        if (now >= next_sample) {
            xTaskNotifyGive(sensor_handle);
            next_sample += SAMPLE_PERIOD_MS * 1000LL;
            if (next_sample <= now) {
                next_sample = now + SAMPLE_PERIOD_MS * 1000LL;
            }
        }
        app_event_t event;
        int64_t remaining = next_sample - esp_timer_get_time();
        const int64_t tick_us = portTICK_PERIOD_MS * 1000LL;
        TickType_t wait = remaining > 0 ? (remaining + tick_us - 1) / tick_us : 0;
        if (xQueueReceive(events, &event, wait) != pdTRUE) {
            continue;
        }
        switch (event.type) {
        case EVENT_SAMPLE:
            state.local = event.sample;
            network_publish(&state.local);
            network_tick();
            now = esp_timer_get_time();
            if (state.log.enabled && state.local.sensor_ok && state.local.rtc_ok &&
                now >= next_log) {
                flash_message_t message = {.is_record = true, .record = state.local.record};
                if (xQueueSend(flash_queue, &message, 0) == pdTRUE) {
                    next_log = now + LOG_INTERVAL_MINUTES * 60000000LL;
                } else {
                    ESP_LOGW("manager", "Flash queue full; logging will retry");
                }
            }
            break;
        case EVENT_REMOTE: {
            unsigned field = event.remote.field;
            state.remote[field] = event.remote.value;
            state.remote_seen[field] = esp_timer_get_time();
            const char *names[] = {"temp", "hum", "pres"};
            printf("MQTT %s/%s = %.2f\n", MQTT_SUB_BASE, names[field], (double)event.remote.value);
            break;
        }
        case EVENT_COMMAND:
            if (event.command.type == CMD_TIME) {
                esp_err_t err = ds1307_set(event.command.timestamp);
                printf("DS1307 time: %s\n", esp_err_to_name(err));
            } else {
                flash_message_t message = {.command = event.command.type};
                if (xQueueSend(flash_queue, &message, 0) != pdTRUE) {
                    printf("Flash busy; retry command\n");
                }
                if (event.command.type == CMD_LOG_ON) {
                    next_log = 0;
                }
                if (event.command.type == CMD_STATUS) {
                    print_status(&state);
                }
            }
            break;
        }
        xQueueReceive(flash_status_queue, &state.log, 0);
        state.wifi = network_wifi_connected();
        state.mqtt = network_mqtt_connected();
        if (event.type == EVENT_SAMPLE) {
            xQueueOverwrite(display_queue, &state);
        }
    }
}

static void init_uart(void)
{
    const uart_config_t uart = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_XTAL,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 512, 0, 16, &uart_events, 0));
    uart_vfs_dev_use_driver(UART_NUM_0);
    ESP_ERROR_CHECK(uart_set_wakeup_threshold(UART_NUM_0, 3));
    ESP_ERROR_CHECK(esp_sleep_enable_uart_wakeup(UART_NUM_0));
}

static void start_task(TaskFunction_t task, const char *name, uint32_t stack, UBaseType_t priority,
                       TaskHandle_t *handle)
{
    BaseType_t result = xTaskCreate(task, name, stack, NULL, priority, handle);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    bus_init();
    ESP_ERROR_CHECK(ds1307_init());
    err = flash_store_init();
    if (err != ESP_OK) {
        ESP_LOGE("flash", "Initialization: %s", esp_err_to_name(err));
    }
    events = xQueueCreate(16, sizeof(app_event_t));
    display_queue = xQueueCreate(1, sizeof(display_state_t));
    flash_queue = xQueueCreate(8, sizeof(flash_message_t));
    flash_status_queue = xQueueCreate(1, sizeof(log_status_t));
    configASSERT(events && display_queue && flash_queue && flash_status_queue);

    init_uart();
    network_init(events);

    const esp_pm_config_t power = {
        .max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true};
    ESP_ERROR_CHECK(esp_pm_configure(&power));
    start_task(sensor_task, "Sensor Task", 4096, 4, &sensor_handle);
    start_task(flash_writer_task, "Flash Writer Task", 4096, 3, NULL);
    start_task(display_task, "Display Task", 3072, 2, NULL);
    start_task(uart_task, "UART Task", 4096, 3, NULL);
    start_task(manager_task, "Manager Task", 6144, 4, NULL);
}
