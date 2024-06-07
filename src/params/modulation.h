/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#pragma once

#include <stdint.h>

uint32_t params_current_mode_filter_low_get();
uint32_t params_current_mode_filter_low_set(uint32_t val);
uint32_t params_current_mode_filter_high_get();
uint32_t params_current_mode_filter_high_set(uint32_t val);

int16_t params_current_mode_spectrum_factor_get();
int16_t params_current_mode_spectrum_factor_add(int16_t diff);

void params_current_mode_filter_get(int32_t *low, int32_t *high);

uint16_t params_current_mode_freq_step_change(bool up);

uint32_t params_current_mode_filter_bw();


uint16_t params_current_mode_freq_step_get();


void params_modulation_setup();
void params_mode_save();
