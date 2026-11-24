#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "model.h"

void network_init(QueueHandle_t events);
void network_tick(void);
void network_publish(const sample_t *sample);
bool network_wifi_connected(void);
bool network_mqtt_connected(void);
