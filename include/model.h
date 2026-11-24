#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t timestamp;  // Unix seconds, UTC.
    int32_t temperature; // Celsius x 100.
    uint16_t humidity;   // Relative humidity x 100.
    uint16_t pressure;   // mmHg.
} log_record_t;

_Static_assert(sizeof(log_record_t) == 16, "Flash record must be 16 bytes");

typedef struct {
    log_record_t record;
    bool sensor_ok;
    bool rtc_ok;
} sample_t;

typedef enum {
    CMD_STATUS,
    CMD_LOG_ON,
    CMD_LOG_OFF,
    CMD_FLUSH,
    CMD_ERASE,
    CMD_TIME
} command_t;
typedef enum {
    EVENT_SAMPLE,
    EVENT_COMMAND,
    EVENT_REMOTE
} event_type_t;
typedef struct {
    event_type_t type;
    union {
        sample_t sample;
        struct {
            command_t type;
            uint64_t timestamp;
        } command;
        struct {
            unsigned field;
            float value;
        } remote;
    };
} app_event_t;

typedef struct {
    bool enabled;
    bool full;
    bool error;
    uint32_t sectors_used;
    uint32_t sectors_total;
    unsigned buffered;
} log_status_t;

typedef struct {
    sample_t local;
    float remote[3];
    int64_t remote_seen[3];
    log_status_t log;
    bool wifi;
    bool mqtt;
} display_state_t;
