#pragma once
#include "esp_err.h"
#include "model.h"

esp_err_t display_init(void);
esp_err_t display_update(const display_state_t *state);
