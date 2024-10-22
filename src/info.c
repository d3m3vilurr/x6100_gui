/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "info.h"

#include "styles.h"
#include "params/params.h"
#include "pubsub_ids.h"
#include "wifi.h"

typedef enum {
    INFO_VFO = 0,
    INFO_MODE,
    INFO_AGC,
    INFO_PRE_ATT,
    INFO_ATU,
    INFO_WIFI
} info_items_t;

static lv_obj_t     *obj;
static lv_obj_t     *items[6];

static bool         mode_lock = false;

static void wifi_state_change_cb(void *s, lv_msg_t *m);

lv_obj_t * info_init(lv_obj_t * parent) {
    obj = lv_obj_create(parent);

    lv_obj_add_style(obj, &info_style, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_left(obj, 0, 0);

    uint8_t index = 0;

    for (uint8_t y = 0; y < 2; y++)
        for (uint8_t x = 0; x < 3; x++) {
            lv_obj_t *item = lv_label_create(obj);

            lv_obj_add_style(item, &info_item_style, 0);
            lv_obj_set_pos(item, x * 58 + 15, y * 22 + 5);
            lv_obj_set_style_text_align(item, LV_TEXT_ALIGN_CENTER, 0);

            lv_obj_set_style_text_color(item, lv_color_white(), 0);
            lv_obj_set_size(item, 56, 26);

            items[index] = item;
            index++;
        }

    lv_label_set_text(items[INFO_PRE_ATT], "PRE/ATT");
    lv_label_set_text(items[INFO_WIFI], LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(items[INFO_WIFI], lv_color_hex(0x909090), 0);

    info_params_set();

    lv_msg_subscribe(MSG_WIFI_STATE_CHANGED, wifi_state_change_cb, NULL);

    return obj;
}

void info_atu_update() {
    lv_label_set_text_fmt(items[INFO_ATU], "ATU%i", params.ant);

    if (!params.atu) {
        lv_obj_set_style_text_color(items[INFO_ATU], lv_color_white(), 0);
        lv_obj_set_style_bg_color(items[INFO_ATU], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(items[INFO_ATU], LV_OPA_0, 0);
    } else {
        if (params_band_cur_shift_get()) {
            lv_obj_set_style_text_color(items[INFO_ATU], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_bg_opa(items[INFO_ATU], LV_OPA_20, 0);
        } else {
            lv_obj_set_style_text_color(items[INFO_ATU], params.atu_loaded ? lv_color_black() : lv_color_hex(0xFF0000), 0);
            lv_obj_set_style_bg_opa(items[INFO_ATU], LV_OPA_50, 0);
        }
        lv_obj_set_style_bg_color(items[INFO_ATU], lv_color_white(), 0);
    }
}

const char* info_params_mode() {
    x6100_mode_t    mode = radio_current_mode();
    char            *str;

    switch (mode) {
        case x6100_mode_lsb:
            str = "LSB";
            break;

        case x6100_mode_lsb_dig:
            str = "LSB-D";
            break;

        case x6100_mode_usb:
            str = "USB";
            break;

        case x6100_mode_usb_dig:
            str = "USB-D";
            break;

        case x6100_mode_cw:
            str = "CW";
            break;

        case x6100_mode_cwr:
            str = "CW-R";
            break;

        case x6100_mode_am:
            str = "AM";
            break;

        case x6100_mode_nfm:
            str = "NFM";
            break;

        default:
            str = "?";
            break;
    }

    return str;
}

const char* info_params_agc() {
    x6100_agc_t     agc = params_band_cur_agc_get();
    char            *str;

    switch (agc) {
        case x6100_agc_off:
            str = "OFF";
            break;

        case x6100_agc_slow:
            str = "SLOW";
            break;

        case x6100_agc_fast:
            str = "FAST";
            break;

        case x6100_agc_auto:
            str = "AUTO";
            break;

        default:
            str = "?";
            break;

    }

    return str;
}

const char* info_params_vfo() {
    x6100_vfo_t cur_vfo = params_band_vfo_get();
    char            *str;

    if (params_band_split_get()) {
        str = cur_vfo == X6100_VFO_A ? "SPL-A" : "SPL-B";
    } else {
        str = cur_vfo == X6100_VFO_A ? "VFO-A" : "VFO-B";
    }

    return str;
}

bool info_params_att() {
    x6100_att_t     att = params_band_cur_att_get();

    return att == x6100_att_on;
}

bool info_params_pre() {
    x6100_pre_t     pre = params_band_cur_pre_get();

    return pre == x6100_pre_on;
}

void info_params_set() {
    lv_label_set_text(items[INFO_VFO], info_params_vfo());
    lv_label_set_text(items[INFO_MODE], info_params_mode());
    lv_label_set_text(items[INFO_AGC], info_params_agc());

    if (mode_lock) {
        lv_obj_set_style_text_color(items[INFO_MODE], lv_color_hex(0xAAAAAA), 0);
    } else {
        lv_obj_set_style_text_color(items[INFO_MODE], lv_color_white(), 0);
    }

    if (info_params_att()) {
        lv_obj_set_style_text_color(items[INFO_PRE_ATT], lv_color_black(), 0);
        lv_obj_set_style_bg_color(items[INFO_PRE_ATT], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(items[INFO_PRE_ATT], LV_OPA_50, 0);
        lv_label_set_text(items[INFO_PRE_ATT], "ATT");
    } else if (info_params_pre()) {
        lv_obj_set_style_text_color(items[INFO_PRE_ATT], lv_color_black(), 0);
        lv_obj_set_style_bg_color(items[INFO_PRE_ATT], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(items[INFO_PRE_ATT], LV_OPA_50, 0);
        lv_label_set_text(items[INFO_PRE_ATT], "PRE");
    } else {
        lv_obj_set_style_text_color(items[INFO_PRE_ATT], lv_color_white(), 0);
        lv_obj_set_style_bg_color(items[INFO_PRE_ATT], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(items[INFO_PRE_ATT], LV_OPA_0, 0);
        lv_label_set_text(items[INFO_PRE_ATT], "P/A");
    }

    x6100_mode_t mode = radio_current_mode();
    if ((mode == x6100_mode_lsb_dig) || (mode == x6100_mode_usb_dig)) {
        lv_obj_set_style_text_color(items[INFO_MODE], lv_color_hex(COLOR_LIGHT_RED), 0);
    }


    info_atu_update();
}

void info_lock_mode(bool lock) {
    mode_lock = lock;
    info_params_set();
}

static void wifi_state_change_cb(void *s, lv_msg_t *m) {
    lv_color_t color;
    switch (wifi_get_status()) {
    case WIFI_CONNECTED:
        color = lv_color_white();
        break;
    case WIFI_OFF:
        color = lv_color_black();
        break;
    default:
        color = lv_color_hex(0x909090);
        break;
    }
    lv_obj_set_style_text_color(items[INFO_WIFI], color, 0);
}
