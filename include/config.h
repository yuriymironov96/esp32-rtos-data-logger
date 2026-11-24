#pragma once

// Copy config.local.example.h to config.local.h for private settings.
#if __has_include("config.local.h")
#include "config.local.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "broker.hivemq.com"
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif
#ifndef MQTT_PUB_BASE
#define MQTT_PUB_BASE "EMB1/ymironov/bme280"
#endif
#ifndef MQTT_SUB_BASE
#define MQTT_SUB_BASE "EMB1/teacher/bme280"
#endif

#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9
#define BME280_ADDRESS 0x76
#define OLED_ADDRESS 0x3C

#define SAMPLE_PERIOD_MS 1000
#ifndef LOG_INTERVAL_MINUTES
#define LOG_INTERVAL_MINUTES 1
#endif
#define REMOTE_STALE_SECONDS 10
#define UART_ACTIVE_SECONDS 30

_Static_assert(LOG_INTERVAL_MINUTES > 0, "Logging interval must be positive");
