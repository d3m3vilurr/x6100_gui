/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "dialog_ft8.h"

#include "ft8/worker.h"
#include "lvgl/lvgl.h"
#include "dialog.h"
#include "styles.h"
#include "params/params.h"
#include "radio.h"
#include "audio.h"
#include "keyboard.h"
#include "events.h"
#include "buttons.h"
#include "main_screen.h"
#include "qth.h"
#include "msg.h"
#include "util.h"
#include "recorder.h"
#include "tx_info.h"
#include "textarea_window.h"

#include "widgets/lv_waterfall.h"
#include "widgets/lv_finder.h"

#include <ft8lib/message.h>
#include "ft8/worker.h"
#include "gfsk.h"
#include "adif.h"
#include "qso_log.h"
#include "scheduler.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <ctype.h>

#define FT8_160M_KR_ID  (100 - MEM_FT8_ID)
#define FT8_160M_ID     (101 - MEM_FT8_ID)
#define FT8_80M_JP_ID   (102 - MEM_FT8_ID)
#define FT8_80M_ID      (103 - MEM_FT8_ID)
#define FT8_60M_ID      (104 - MEM_FT8_ID)
#define FT8_40M_JP_ID   (105 - MEM_FT8_ID)
#define FT8_40M_ID      (106 - MEM_FT8_ID)
#define FT8_30M_ID      (107 - MEM_FT8_ID)
#define FT8_20M_ID      (108 - MEM_FT8_ID)
#define FT8_17M_ID      (109 - MEM_FT8_ID)
#define FT8_15M_ID      (110 - MEM_FT8_ID)
#define FT8_12M_ID      (111 - MEM_FT8_ID)
#define FT8_10M_ID      (112 - MEM_FT8_ID)
#define FT8_6M_ID       (113 - MEM_FT8_ID)
#define FT4_80M_JP_ID   (200 - MEM_FT4_ID)
#define FT4_80M_ID      (201 - MEM_FT4_ID)
#define FT4_40M_JP_ID   (202 - MEM_FT4_ID)
#define FT4_40M_ID      (203 - MEM_FT4_ID)
#define FT4_30M_ID      (204 - MEM_FT4_ID)
#define FT4_20M_ID      (205 - MEM_FT4_ID)
#define FT4_17M_ID      (206 - MEM_FT4_ID)
#define FT4_15M_ID      (207 - MEM_FT4_ID)
#define FT4_12M_ID      (208 - MEM_FT4_ID)
#define FT4_10M_ID      (209 - MEM_FT4_ID)
#define FT4_6M_ID       (210 - MEM_FT4_ID)

#define DECIM           6
#define SAMPLE_RATE     (AUDIO_CAPTURE_RATE / DECIM)

#define WIDTH           771

#define UNKNOWN_SNR     99

#define MAX_PWR         5.0f

#define FT8_WIDTH_HZ    50
#define FT4_WIDTH_HZ    83

#define MAX_TABLE_MSG   512
#define CLEAN_N_ROWS    64

#define WAIT_SYNC_TEXT "Wait sync"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

typedef enum {
    // "CQ CALL ..." or "CQ DX CALL ..." or "CQ EU CALL ..."
    MSG_TYPE_CQ,
    // "CALL1 CALL2 GRID"
    MSG_TYPE_GRID,
    // "CALL1 CALL2 +1"
    MSG_TYPE_REPORT,
    // "CALL1 CALL2 R+1"
    MSG_TYPE_R_REPORT,
    // "CALL1 CALL2 RR73"
    MSG_TYPE_RR73,
    // "CALL1 CALL2 73"
    MSG_TYPE_73,

    MSG_TYPE_OTHER,
} ftx_msg_type_t;

typedef enum {
    RX_PROCESS,
    TX_PROCESS,
} ft8_state_t;

typedef enum {
    CELL_RX_INFO = 0,
    CELL_RX_MSG,
    CELL_RX_CQ,
    CELL_RX_TO_ME,
    CELL_TX_MSG
} ft8_cell_type_t;


/**
 * Incoming message parse result.
 */
typedef struct {
    ftx_msg_type_t type;
    char           call_from[32];
    char           call_to[32];
    char           extra[32];
    int8_t         snr;
} msg_t;

/**
 * LVGL cell user data
 */
typedef struct {
    ft8_cell_type_t cell_type;
    int16_t         local_snr;
    int16_t         dist;
    bool            odd;
    msg_t           msg;
    char            text[128];

    qso_log_search_worked_t       worked_type;
} cell_data_t;

/**
 * Current QSO item
 */
typedef struct {
    char        remote_callsign[32];
    char        remote_qth[32];
    int16_t     rst_r;
    int16_t     rst_s;

    int16_t     last_snr;
    bool        rx_odd;
} ft8_qso_item_t;


typedef struct {
    uint8_t cur;
    uint8_t next;
    uint8_t prev;
    uint8_t another;
} band_relations_t;

band_relations_t ft8_relations[] = {
    {FT8_160M_KR_ID,    FT8_160M_ID,    FT8_6M_ID,      FT4_80M_JP_ID},
    {FT8_160M_ID,       FT8_80M_JP_ID,  FT8_160M_KR_ID, FT4_80M_JP_ID},
    {FT8_80M_JP_ID,     FT8_80M_ID,     FT8_160M_ID,    FT4_80M_JP_ID},
    {FT8_80M_ID,        FT8_60M_ID,     FT8_80M_JP_ID,  FT4_80M_ID},
    {FT8_60M_ID,        FT8_40M_JP_ID,  FT8_80M_ID,     FT4_40M_JP_ID},
    {FT8_40M_JP_ID,     FT8_40M_ID,     FT8_60M_ID,     FT4_40M_JP_ID},
    {FT8_40M_ID,        FT8_30M_ID,     FT8_40M_JP_ID,  FT4_40M_ID},
    {FT8_30M_ID,        FT8_20M_ID,     FT8_40M_ID,     FT4_30M_ID},
    {FT8_20M_ID,        FT8_17M_ID,     FT8_30M_ID,     FT4_20M_ID},
    {FT8_17M_ID,        FT8_15M_ID,     FT8_20M_ID,     FT4_17M_ID},
    {FT8_15M_ID,        FT8_12M_ID,     FT8_17M_ID,     FT4_15M_ID},
    {FT8_12M_ID,        FT8_10M_ID,     FT8_15M_ID,     FT4_12M_ID},
    {FT8_10M_ID,        FT8_6M_ID,      FT8_12M_ID,     FT4_10M_ID},
    {FT8_6M_ID,         FT8_160M_KR_ID,  FT8_10M_ID,    FT4_6M_ID},
};


band_relations_t ft4_relations[] = {
    {FT4_80M_JP_ID,     FT4_80M_ID,     FT4_6M_ID,      FT8_80M_JP_ID},
    {FT4_80M_ID,        FT4_40M_JP_ID,  FT4_80M_JP_ID,  FT8_80M_ID},
    {FT4_40M_JP_ID,     FT4_40M_ID,     FT4_80M_ID,     FT8_40M_JP_ID},
    {FT4_40M_ID,        FT4_30M_ID,     FT4_40M_JP_ID,  FT8_40M_ID},
    {FT4_30M_ID,        FT4_20M_ID,     FT4_40M_ID,     FT8_30M_ID},
    {FT4_20M_ID,        FT4_17M_ID,     FT4_30M_ID,     FT8_20M_ID},
    {FT4_17M_ID,        FT4_15M_ID,     FT4_20M_ID,     FT8_17M_ID},
    {FT4_15M_ID,        FT4_12M_ID,     FT4_17M_ID,     FT8_15M_ID},
    {FT4_12M_ID,        FT4_10M_ID,     FT4_15M_ID,     FT8_12M_ID},
    {FT4_10M_ID,        FT4_6M_ID,      FT4_12M_ID,     FT8_10M_ID},
    {FT4_6M_ID,         FT4_80M_JP_ID,  FT4_10M_ID,     FT8_6M_ID},
};


static ft8_state_t          state = RX_PROCESS;
static bool                 odd;
static bool                 tx_enabled=true;
static bool                 cq_enabled=false;
static bool                 tx_time_slot;

static char                 tx_msg[64] = "";
static ft8_qso_item_t       qso_item = {.rst_s=UNKNOWN_SNR, .rst_r=UNKNOWN_SNR};

static lv_obj_t             *table;

static lv_timer_t           *timer = NULL;
static lv_anim_t            fade;
static bool                 fade_run = false;
static bool                 disable_buttons = false;

static lv_obj_t             *finder;
static lv_obj_t             *waterfall;
static uint16_t             waterfall_nfft;
static spgramcf             waterfall_sg;
static float                *waterfall_psd;
static uint8_t              waterfall_fps_ms = (1000 / 5);
static uint64_t             waterfall_time;

static pthread_mutex_t      audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static cbuffercf            audio_buf;
static pthread_t            thread;

static firdecim_crcf        decim;
static float complex        *decim_buf;

static adif_log             ft8_log;

static float base_gain_offset;

static void construct_cb(lv_obj_t *parent);
static void key_cb(lv_event_t * e);
static void destruct_cb();
static void audio_cb(unsigned int n, float complex *samples);
static void rotary_cb(int32_t diff);
static void * decode_thread(void *arg);

static void show_cq_cb(lv_event_t * e);
static void show_all_cb(lv_event_t * e);

static void mode_ft4_cb(lv_event_t * e);
static void mode_ft8_cb(lv_event_t * e);

static void tx_cq_en_cb(lv_event_t * e);
static void tx_cq_dis_cb(lv_event_t * e);

static void tx_call_en_cb(lv_event_t * e);
static void tx_call_dis_cb(lv_event_t * e);

static void mode_auto_cb(lv_event_t * e);
static void cq_modifier_cb(lv_event_t * e);

static void cell_press_cb(lv_event_t * e);

static void keyboard_open();
static bool keyboard_cancel_cb();
static bool keyboard_ok_cb();
static void keyboard_close();

static void add_info(const char * fmt, ...);
static void add_tx_text(const char * text);
static void make_cq_msg(const char *callsign, const char *qth, const char *cq_mod, char *text);
static void generate_tx_msg(const msg_t * rx_msg);
static bool make_answer(const msg_t *msg, int8_t snr, bool rx_odd);
static bool get_time_slot(struct timespec now);
static bool str_equal(const char * a, const char * b);
static bool is_cq_modifier(const char * text);

// button label is current state, press action and name - next state
static button_item_t button_show_cq = { .label = "Show:\nAll", .press = show_cq_cb };
static button_item_t button_show_all = { .label = "Show:\nCQ", .press = show_all_cb };

static button_item_t button_mode_ft4 = { .label = "Mode:\nFT8", .press = mode_ft4_cb };
static button_item_t button_mode_ft8 = { .label = "Mode:\nFT4", .press = mode_ft8_cb };

static button_item_t button_tx_cq_en = { .label = "TX CQ:\nDisabled", .press = tx_cq_en_cb };
static button_item_t button_tx_cq_dis = { .label = "TX CQ:\nEnabled", .press = tx_cq_dis_cb };

static button_item_t button_tx_call_en = { .label = "TX Call:\nDisabled", .press = tx_call_en_cb, .hold = tx_cq_dis_cb };
static button_item_t button_tx_call_dis = { .label = "TX Call:\nEnabled", .press = tx_call_dis_cb, .hold = tx_cq_dis_cb };

static button_item_t button_auto_en = { .label = "Auto:\nDisabled", .press = mode_auto_cb };
static button_item_t button_auto_dis = { .label = "Auto:\nEnabled", .press = mode_auto_cb };

static button_item_t button_cq_mod = { .label = "CQ\nModifier", .press = cq_modifier_cb };

static dialog_t             dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .audio_cb = audio_cb,
    .rotary_cb = rotary_cb,
    .key_cb = key_cb
};

dialog_t                    *dialog_ft8 = &dialog;

static void save_qso() {
    if (
        (qso_item.rst_s == UNKNOWN_SNR) ||
        (qso_item.rst_r == UNKNOWN_SNR) ||
        (strlen(qso_item.remote_callsign) == 0)
    ) {
        LV_LOG_USER("Can't save QSO - not enough information");
        return;
    }
    time_t now = time(NULL);

    char * canonized_call = util_canonize_callsign(qso_item.remote_callsign, false);
    qso_log_record_t qso = qso_log_record_create(
        params.callsign.x,
        canonized_call,
        now, params.ft8_protocol == FTX_PROTOCOL_FT8 ? MODE_FT8 : MODE_FT4,
        qso_item.rst_s, qso_item.rst_r, params_band_cur_freq_get(), NULL, NULL,
        params.qth.x, qso_item.remote_qth
    );
    free(canonized_call);

    adif_add_qso(ft8_log, qso);

    // Save QSO to sqlite log
    qso_log_record_save(qso);

    msg_set_text_fmt("QSO saved");
}

static void clear_qso() {
    qso_item.remote_callsign[0] = 0;
    qso_item.remote_qth[0] = 0;
    qso_item.rst_r = UNKNOWN_SNR;
    qso_item.rst_s = UNKNOWN_SNR;
}

static void start_qso(msg_t * msg) {
    if (!str_equal(qso_item.remote_callsign, msg->call_from)) {
        clear_qso();
        strncpy(qso_item.remote_callsign, msg->call_from, sizeof(qso_item.remote_callsign) - 1);
    }
    if ((msg->type == MSG_TYPE_CQ) || (msg->type == MSG_TYPE_GRID)) {
        strncpy(qso_item.remote_qth, msg->extra, sizeof(qso_item.remote_qth) - 1);
    }
    if ((msg->type == MSG_TYPE_REPORT) || (msg->type == MSG_TYPE_R_REPORT)) {
        qso_item.rst_r = msg->snr;
    }
    add_info("Start QSO with %s", qso_item.remote_callsign);
    buttons_load(2, &button_tx_call_dis);
}

static bool active_qso() {
    return qso_item.remote_callsign[0] != 0;
}

static void worker_init() {

    /* ftx worker */

    ftx_worker_init(SAMPLE_RATE, params.ft8_protocol);
    int block_size = ftx_worker_get_block_size();

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    odd = get_time_slot(now);
    decim_buf = (float complex *) malloc(block_size * sizeof(float complex));

    /* Waterfall */
    // TODO: check nfft for waterfall
    waterfall_nfft = block_size * 2;

    waterfall_sg = spgramcf_create(waterfall_nfft, LIQUID_WINDOW_HANN, waterfall_nfft, waterfall_nfft / 4);
    waterfall_psd = (float *) malloc(waterfall_nfft * sizeof(float));
    waterfall_time = get_time();

    /* Worker */
    pthread_create(&thread, NULL, decode_thread, NULL);
}

static void worker_done() {
    state = RX_PROCESS;

    pthread_cancel(thread);
    pthread_join(thread, NULL);
    radio_set_modem(false);
    pthread_mutex_unlock(&audio_mutex);
    ftx_worker_free();
    free(decim_buf);

    spgramcf_destroy(waterfall_sg);
    free(waterfall_psd);

    clear_qso();
    tx_msg[0] = 0;
}

static const char * find_qth(const char *str) {
    char *ptr = rindex(str, ' ');

    if (ptr) {
        ptr++;

        if (strcmp(ptr, "RR73") != 0 && grid_check(ptr)) {
            return ptr;
        }
    }

    return NULL;
}

void static waterfall_process(float complex *frame, const size_t size) {
    uint64_t now = get_time();

    spgramcf_write(waterfall_sg, frame, size);

    if (now - waterfall_time > waterfall_fps_ms) {
        uint32_t low_bin = waterfall_nfft / 2 + waterfall_nfft * params_current_mode_filter_low_get() / SAMPLE_RATE;
        uint32_t high_bin = waterfall_nfft / 2 + waterfall_nfft * params_current_mode_filter_high_get() / SAMPLE_RATE;

        spgramcf_get_psd(waterfall_sg, waterfall_psd);
        // Normalize FFT
        liquid_vectorf_addscalar(
            &waterfall_psd[low_bin],
            high_bin - low_bin,
            -10.f * log10f(sqrtf(waterfall_nfft)) - 16.0f,
            &waterfall_psd[low_bin]);

        lv_waterfall_add_data(waterfall, &waterfall_psd[low_bin], high_bin - low_bin);

        waterfall_time = now;
        spgramcf_reset(waterfall_sg);
    }
}

static void truncate_table() {
    lv_coord_t     removed_rows_height = 0;
    uint16_t       table_rows = lv_table_get_row_cnt(table);
    if (table_rows > MAX_TABLE_MSG) {
        LV_LOG_USER("Start");

        lv_table_t *table_obj = (lv_table_t*) table;

        for (size_t i = 0; i < CLEAN_N_ROWS; i++) {
            removed_rows_height += table_obj->row_h[i];
        }

        if (table_obj->row_act > CLEAN_N_ROWS) {
            table_obj->row_act -= CLEAN_N_ROWS;
        } else {
            table_obj->row_act = 0;
        }

        for (uint16_t i = CLEAN_N_ROWS; i < table_rows; i++) {
            cell_data_t *cell_data_copy = malloc(sizeof(cell_data_t));
            lv_table_set_cell_value(table, i-CLEAN_N_ROWS, 0, lv_table_get_cell_value(table, i, 0));
            *cell_data_copy = *(cell_data_t *) lv_table_get_cell_user_data(table, i, 0);
            lv_table_set_cell_user_data(table, i-CLEAN_N_ROWS, 0, cell_data_copy);
        }
        table_rows -= CLEAN_N_ROWS;

        lv_table_set_row_cnt(table, table_rows);
        lv_obj_scroll_by_bounded(table, 0, removed_rows_height, LV_ANIM_OFF);
    }
}

static void add_msg_cb(lv_event_t * e) {
    truncate_table();
    cell_data_t *cell_data = (cell_data_t *) lv_event_get_param(e);
    uint16_t    row = 0;
    uint16_t    col = 0;
    bool        scroll;

    lv_table_get_selected_cell(table, &row, &col);
    uint16_t table_rows = lv_table_get_row_cnt(table);
    if ((table_rows == 1) && strcmp(lv_table_get_cell_value(table, 0, 0), WAIT_SYNC_TEXT) == 0) {
        table_rows--;
    }
    scroll = table_rows == (row + 1);

    // Copy data, because original event data will be deleted
    cell_data_t *cell_data_copy = malloc(sizeof(cell_data_t));
    *cell_data_copy = *cell_data;

    lv_table_set_cell_value(table, table_rows, 0, cell_data_copy->text);
    lv_table_set_cell_user_data(table, table_rows, 0, cell_data_copy);

    if (scroll) {
        static int32_t c = LV_KEY_DOWN;

        lv_event_send(table, LV_EVENT_KEY, &c);
    }
}

static void table_draw_part_begin_cb(lv_event_t * e) {
    lv_obj_t                *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t  *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part == LV_PART_ITEMS) {
        uint32_t    row = dsc->id / lv_table_get_col_cnt(obj);
        uint32_t    col = dsc->id - row * lv_table_get_col_cnt(obj);
        cell_data_t *cell_data = lv_table_get_cell_user_data(obj, row, col);

        dsc->rect_dsc->bg_opa = LV_OPA_50;

        if (cell_data == NULL) {
            dsc->label_dsc->align = LV_TEXT_ALIGN_CENTER;
            dsc->rect_dsc->bg_color = lv_color_hex(0x303030);
        } else {
            switch (cell_data->cell_type) {
                case CELL_RX_INFO:
                    dsc->label_dsc->align = LV_TEXT_ALIGN_CENTER;
                    dsc->rect_dsc->bg_color = lv_color_hex(0x303030);
                    break;

                case CELL_RX_CQ:
                    switch (cell_data->worked_type) {
                        case SEARCH_WORKED_NO:
                            // green
                            dsc->rect_dsc->bg_color = lv_color_hex(0x00DD00);
                            break;
                        case SEARCH_WORKED_YES:
                            // dark green
                            dsc->label_dsc->opa = LV_OPA_90;
                            dsc->rect_dsc->bg_color = lv_color_hex(0x2e5a00);
                            break;
                        case SEARCH_WORKED_SAME_MODE:
                            // darker green
                            dsc->label_dsc->decor = LV_TEXT_DECOR_STRIKETHROUGH;
                            dsc->label_dsc->opa = LV_OPA_80;
                            dsc->rect_dsc->bg_color = lv_color_hex(0x224400);
                            break;
                    }
                    break;

                case CELL_RX_TO_ME:
                    dsc->rect_dsc->bg_color = lv_color_hex(0xFF0000);
                    break;

                case CELL_TX_MSG:
                    dsc->rect_dsc->bg_color = lv_color_hex(0x0000FF);
                    break;
                default:
                    dsc->rect_dsc->bg_color = lv_color_black();
                    break;
            }
        }

        uint16_t    selected_row, selected_col;
        lv_table_get_selected_cell(obj, &selected_row, &selected_col);

        if (selected_row == row) {
            dsc->rect_dsc->bg_color = lv_color_lighten(dsc->rect_dsc->bg_color, 20);
        }
    }
}

static void table_draw_part_end_cb(lv_event_t * e) {
    lv_obj_t                *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t  *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part == LV_PART_ITEMS) {
        uint32_t    row = dsc->id / lv_table_get_col_cnt(obj);
        uint32_t    col = dsc->id - row * lv_table_get_col_cnt(obj);
        cell_data_t *cell_data = lv_table_get_cell_user_data(obj, row, col);

        if (cell_data == NULL) {
            return;
        }

        if ((cell_data->cell_type == CELL_RX_MSG) ||
            (cell_data->cell_type == CELL_RX_CQ) ||
            (cell_data->cell_type == CELL_RX_TO_ME)
        ) {
            char                buf[64];
            const lv_coord_t    cell_top = lv_obj_get_style_pad_top(obj, LV_PART_ITEMS);
            const lv_coord_t    cell_bottom = lv_obj_get_style_pad_bottom(obj, LV_PART_ITEMS);
            lv_area_t           area;

            dsc->label_dsc->align = LV_TEXT_ALIGN_RIGHT;

            area.y1 = dsc->draw_area->y1 + cell_top;
            area.y2 = dsc->draw_area->y2 - cell_bottom;

            area.x2 = dsc->draw_area->x2 - 15;
            area.x1 = area.x2 - 120;

            snprintf(buf, sizeof(buf), "%i dB", cell_data->local_snr);
            lv_draw_label(dsc->draw_ctx, dsc->label_dsc, &area, buf, NULL);

            if (cell_data->dist > 0) {
                area.x2 = area.x1 - 10;
                area.x1 = area.x2 - 200;

                snprintf(buf, sizeof(buf), "%i km", cell_data->dist);
                lv_draw_label(dsc->draw_ctx, dsc->label_dsc, &area, buf, NULL);
            }
        }
    }
}

static void key_cb(lv_event_t * e) {
    uint32_t key = *((uint32_t *) lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct(&dialog);
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

static void destruct_cb() {
    // TODO: check free mem
    keyboard_close();
    worker_done();

    firdecim_crcf_destroy(decim);
    free(audio_buf);

    mem_load(MEM_BACKUP_ID);

    main_screen_lock_mode(false);
    main_screen_lock_freq(false);
    main_screen_lock_band(false);

    radio_set_pwr(params.pwr);
    adif_log_close(ft8_log);
}

static void load_band() {
    uint16_t mem_id = 0;

    switch (params.ft8_protocol) {
        case FTX_PROTOCOL_FT8:
            mem_id = MEM_FT8_ID;
            lv_finder_set_width(finder, FT8_WIDTH_HZ);
            break;

        case FTX_PROTOCOL_FT4:
            mem_id = MEM_FT4_ID;
            lv_finder_set_width(finder, FT4_WIDTH_HZ);
            break;
    }
    mem_load(mem_id + params.ft8_band);
}

/// @brief Clean waterfall and table
static void clean_screen() {
    lv_table_set_row_cnt(table, 0);
    lv_table_set_row_cnt(table, 1);
    lv_table_set_cell_value(table, 0, 0, WAIT_SYNC_TEXT);

    lv_waterfall_clear_data(waterfall);

    int32_t *c = malloc(sizeof(int32_t));
    *c = LV_KEY_UP;

    lv_event_send(table, LV_EVENT_KEY, c);
}

static band_relations_t * get_band_relation() {
    int band = params.ft8_band;

    band_relations_t *rel;
    size_t arr_size;

    switch (params.ft8_protocol) {
        case FTX_PROTOCOL_FT8:
            rel = ft8_relations;
            arr_size = ARRAY_SIZE(ft8_relations);
            break;
        case FTX_PROTOCOL_FT4:
            rel = ft4_relations;
            arr_size = ARRAY_SIZE(ft4_relations);
            break;
    }
    for (size_t i = 0; i < arr_size; i++){
        if (rel[i].cur == band) {
            rel += i;
            break;
        }
    }
    return rel;
}

static void band_cb(lv_event_t * e) {
    int band = params.ft8_band;
    int max_band = 0;

    band_relations_t *rel = get_band_relation();

    if (lv_event_get_code(e) == EVENT_BAND_UP) {
        band = rel->next;
    } else {
        band = rel->prev;
    }

    params_lock();
    params.ft8_band = band;
    params_unlock(&params.dirty.ft8_band);
    load_band();

    worker_done();
    worker_init();
    clean_screen();
}

static void msg_timer(lv_timer_t *t) {
    lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(table, 0), LV_OPA_COVER);
    lv_anim_start(&fade);
    timer = NULL;
}

static void fade_anim(void * obj, int32_t v) {
    lv_obj_set_style_opa_layered(obj, v, 0);
}

static void fade_ready(lv_anim_t * a) {
    fade_run = false;
}

static void rotary_cb(int32_t diff) {
    uint32_t f = params.ft8_tx_freq.x + diff;
    uint32_t f_low, f_high;
    params_current_mode_filter_get(&f_low, &f_high);

    if (f > f_high) {
        f = f_high;
    }

    if (f < f_low) {
        f = f_low;
    }

    params_uint16_set(&params.ft8_tx_freq, f);

    lv_finder_set_value(finder, f);
    lv_obj_invalidate(finder);

    if (!fade_run) {
        fade_run = true;
        lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(table, 0), LV_OPA_TRANSP);
        lv_anim_start(&fade);
    }

    if (timer) {
        lv_timer_reset(timer);
    } else {
        timer = lv_timer_create(msg_timer, 1000, NULL);
        lv_timer_set_repeat_count(timer, 1);
    }
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    decim = firdecim_crcf_create_kaiser(DECIM, 16, 40.0f);
    audio_buf = cbuffercf_create(AUDIO_CAPTURE_RATE * 3);

    /* Waterfall */

    waterfall = lv_waterfall_create(dialog.obj);

    lv_obj_add_style(waterfall, &waterfall_style, 0);
    lv_obj_clear_flag(waterfall, LV_OBJ_FLAG_SCROLLABLE);

    lv_color_t palette[256];

    styles_waterfall_palette(palette, 256);
    lv_waterfall_set_palette(waterfall, palette, 256);
    lv_waterfall_set_size(waterfall, WIDTH, 325);
    lv_waterfall_set_min(waterfall, -70);

    lv_obj_set_pos(waterfall, 13, 13);

    /* Freq finder */

    finder = lv_finder_create(waterfall);

    lv_finder_set_width(finder, 50);
    lv_finder_set_value(finder, params.ft8_tx_freq.x);

    lv_obj_set_size(finder, WIDTH, 325);
    lv_obj_set_pos(finder, 0, 0);

    lv_obj_set_style_radius(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(finder, LV_OPA_0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(finder, bg_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    lv_obj_set_style_border_width(finder, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(finder, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    /* Table */

    table = lv_table_create(dialog.obj);

    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_add_event_cb(table, add_msg_cb, EVENT_FT8_MSG, NULL);
    lv_obj_add_event_cb(table, cell_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(table, table_draw_part_begin_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(table, table_draw_part_end_cb, LV_EVENT_DRAW_PART_END, NULL);

    lv_obj_set_size(table, WIDTH, 325 - 55);
    lv_obj_set_pos(table, 13, 13 + 55);

    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, WIDTH - 2);

    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);

    lv_obj_set_style_bg_opa(table, 192, LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(table, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(table, 128, LV_PART_MAIN);

    lv_obj_set_style_text_color(table, lv_color_hex(0xC0C0C0), LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, 0, LV_PART_ITEMS);

    lv_table_set_cell_value(table, 0, 0, "Wait sync");

    /* Fade */

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, table);
    lv_anim_set_time(&fade, 250);
    lv_anim_set_exec_cb(&fade, fade_anim);
    lv_anim_set_ready_cb(&fade, fade_ready);

    /* * */

    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    if (params.ft8_show_all) {
        buttons_load(0, &button_show_cq);
    } else {
        buttons_load(0, &button_show_all);
    }

    switch (params.ft8_protocol) {
        case FTX_PROTOCOL_FT8:
            buttons_load(1, &button_mode_ft4);
            break;

        case FTX_PROTOCOL_FT4:
            buttons_load(1, &button_mode_ft8);
            break;
    }

    buttons_load(2, &button_tx_cq_en);
    buttons_load(3, params.ft8_auto.x ? &button_auto_dis : &button_auto_en);
    buttons_load(4, &button_cq_mod);

    mem_save(MEM_BACKUP_ID);
    load_band();

    uint32_t f_low, f_high;
    params_current_mode_filter_get(&f_low, &f_high);

    lv_finder_set_range(finder, f_low, f_high);

    main_screen_lock_mode(true);
    main_screen_lock_freq(true);
    main_screen_lock_band(true);

    worker_init();

    /* Logger */
    ft8_log = adif_log_init("/mnt/ft_log.adi");

    if (params.pwr > MAX_PWR) {
        radio_set_pwr(MAX_PWR);
        msg_set_text_fmt("Power was limited to %0.0fW", MAX_PWR);
    }

    // setup gain offset
    float target_pwr = LV_MIN(params.pwr, MAX_PWR);
    base_gain_offset = -16.4f + log10f(target_pwr) * 10.0f;
}

static void show_cq_cb(lv_event_t * e) {
    if (disable_buttons) return;
    params_lock();
    params.ft8_show_all = false;
    params_unlock(&params.dirty.ft8_show_all);

    buttons_load(0, &button_show_all);
}

static void show_all_cb(lv_event_t * e) {
    if (disable_buttons) return;
    params_lock();
    params.ft8_show_all = true;
    params_unlock(&params.dirty.ft8_show_all);

    buttons_load(0, &button_show_cq);
}

static void mode_ft4_cb(lv_event_t * e) {
    if (disable_buttons) return;

    band_relations_t *rel = get_band_relation();

    params_lock();
    params.ft8_protocol = FTX_PROTOCOL_FT4;
    params.ft8_band = rel->another;
    params.dirty.ft8_band = true;
    params_unlock(&params.dirty.ft8_protocol);

    buttons_load(1, &button_mode_ft8);

    worker_done();
    worker_init();
    clean_screen();
    load_band();
}

static void mode_ft8_cb(lv_event_t * e) {
    if (disable_buttons) return;
    band_relations_t *rel = get_band_relation();
    params_lock();
    params.ft8_protocol = FTX_PROTOCOL_FT8;
    params.ft8_band = rel->another;
    params.dirty.ft8_band = true;
    params_unlock(&params.dirty.ft8_protocol);

    buttons_load(1, &button_mode_ft4);

    worker_done();
    worker_init();
    clean_screen();
    load_band();
}

static void mode_auto_cb(lv_event_t * e) {
    if (disable_buttons) return;
    params_bool_set(&params.ft8_auto, !params.ft8_auto.x);

    buttons_load(3, params.ft8_auto.x ? &button_auto_dis : &button_auto_en);
}

static void tx_cq_en_cb(lv_event_t * e) {
    if (disable_buttons) return;
    if (strlen(params.callsign.x) == 0) {
        msg_set_text_fmt("Call sign required");

        return;
    }

    buttons_load(2, &button_tx_cq_dis);
    clear_qso();


    char qth[5] = "";
    if (strlen(params.qth.x) >= 4) {
        strncpy(qth, params.qth.x, sizeof(qth) - 1);
    }
    make_cq_msg(params.callsign.x, qth, params.ft8_cq_modifier.x, tx_msg);
    tx_time_slot = !odd;
    if (tx_msg[2] == '_') {
        msg_set_text_fmt("Next TX: CQ %s", tx_msg + 3);
    } else {
        msg_set_text_fmt("Next TX: %s", tx_msg);
    }
    cq_enabled = true;
    tx_enabled = true;
}

static void tx_cq_dis_cb(lv_event_t * e) {
    if (disable_buttons) return;
    buttons_load(2, &button_tx_cq_en);

    if (state == TX_PROCESS) {
        state = RX_PROCESS;
    }
    cq_enabled = false;
    tx_msg[0] = '\0';
}

static void tx_call_off() {
    buttons_load(2, &button_tx_call_en);
    state = RX_PROCESS;
    tx_enabled = false;
}

static void tx_call_en_cb(lv_event_t * e) {
    if (disable_buttons) return;
    if (strlen(params.callsign.x) == 0) {
        msg_set_text_fmt("Call sign required");

        return;
    }
    buttons_load(2, &button_tx_call_dis);
    tx_enabled = true;
}

static void cell_press_cb(lv_event_t * e) {
    if (state == TX_PROCESS) {
        tx_call_off();
    } else {
        uint16_t     row;
        uint16_t     col;

        lv_table_get_selected_cell(table, &row, &col);

        cell_data_t  *cell_data = lv_table_get_cell_user_data(table, row, col);

        if ((cell_data == NULL) ||
            (cell_data->cell_type == CELL_TX_MSG) ||
            (cell_data->cell_type == CELL_RX_INFO)
        ) {
            msg_set_text_fmt("What should I do about it?");
        } else {
            if (make_answer(&cell_data->msg, cell_data->local_snr, cell_data->odd)) {
                start_qso(&cell_data->msg);
                tx_enabled = true;
            } else {
                msg_set_text_fmt("Invalid message");
                tx_call_off();
            }
        }
    }
}

static void tx_call_dis_cb(lv_event_t * e) {
    if (disable_buttons) return;
    buttons_load(2, &button_tx_call_en);

    if (state == TX_PROCESS) {
        state = RX_PROCESS;
    }
    tx_enabled = false;
}

static void keyboard_open() {
    lv_group_remove_obj(table);
    textarea_window_open(keyboard_ok_cb, keyboard_cancel_cb);
    lv_obj_t *text = textarea_window_text();

    lv_textarea_set_accepted_chars(text,
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    );

    if (strlen(params.ft8_cq_modifier.x) > 0) {
        textarea_window_set(params.ft8_cq_modifier.x);
    } else {
        lv_obj_t *text = textarea_window_text();
        lv_textarea_set_placeholder_text(text, " CQ modifier");
    }
    disable_buttons = true;
}

static void keyboard_close() {
    textarea_window_close();
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);
    disable_buttons = false;
}

static bool keyboard_cancel_cb() {
    keyboard_close();
    return true;
}

static bool keyboard_ok_cb() {
    char *cq_mod = (char *)textarea_window_get();
    if ((strlen(cq_mod) > 0) && !is_cq_modifier(cq_mod)) {
        msg_set_text_fmt("Unsupported CQ modifier");
        return false;
    }
    params_str_set(&params.ft8_cq_modifier, cq_mod);
    keyboard_close();
    return true;
}

static void audio_cb(unsigned int n, float complex *samples) {
    if (state == RX_PROCESS) {
        pthread_mutex_lock(&audio_mutex);
        cbuffercf_write(audio_buf, samples, n);
        pthread_mutex_unlock(&audio_mutex);
    }
}


static void cq_modifier_cb(lv_event_t * e) {
    if (disable_buttons) return;
    keyboard_open();
}

static bool get_time_slot(struct timespec now) {
    bool cur_odd;
    float sec = (now.tv_sec % 60) + now.tv_nsec / 1000000000.0f;

    switch (params.ft8_protocol) {
    case FTX_PROTOCOL_FT4:
        cur_odd = (int)(sec / FT4_SLOT_TIME) % 2;
        break;

    case FTX_PROTOCOL_FT8:
        cur_odd = (int)(sec / FT8_SLOT_TIME) % 2;
        break;
    }
    return cur_odd;
}

/**
 * Compare 2 strings ignoring case
 */
static bool str_equal(const char * a, const char * b) {
    size_t a_len = strlen(a);
    if (a_len != strlen(b)) {
        return false;
    }
    if (a_len == 0) {
        return false;
    }
    return strncasecmp(a, b, a_len) == 0;
}

/**
 * Check that text is CQ modifier (3 digits or 1 to 4 letters)
 */
static bool is_cq_modifier(const char *text) {
    size_t len = strlen(text);
    char   c;

    // Check for 3 digits
    bool correct = true;
    if (len == 3) {
        // Check for 3 digits
        for (size_t i = 0; i < len; i++) {
            if ((text[i] < '0') || (text[i] > '9')) {
                correct = false;
                break;
            }
        }
        if (correct) {
            return true;
        }
    }
    // Check for 1 to 4 letters
    correct = true;
    if ((len >= 1) && (len <= 4)) {
        for (size_t i = 0; i < len; i++) {
            c = toupper(text[i]);
            if ((c < 'A') || (c > 'Z')) {
                correct = false;
                break;
            }
        }
        if (correct) {
            return true;
        }
    }
    return false;
}

/**
 * Create CQ TX message
 */
static void make_cq_msg(const char *callsign, const char *qth, const char *cq_mod, char *text) {
    if (strlen(cq_mod)) {
        snprintf(text, FTX_MAX_MESSAGE_LENGTH, "CQ_%s %s %s", cq_mod, callsign, qth);
    } else {
        snprintf(text, FTX_MAX_MESSAGE_LENGTH, "CQ %s %s", callsign, qth);
    }
}

/**
 * Create answer for incoming message.
 */
static bool make_answer(const msg_t * msg, int8_t snr, bool rx_odd) {
    char qth[5] = "";

    if (strlen(params.qth.x) >= 4) {
        strncpy(qth, params.qth.x, sizeof(qth) - 1);
    }
    switch(msg->type) {
        case MSG_TYPE_CQ:
            snprintf(tx_msg, sizeof(tx_msg) - 1, "%s %s %s", msg->call_from, params.callsign.x, qth);
            break;
        case MSG_TYPE_GRID:
            snprintf(tx_msg, sizeof(tx_msg) - 1, "%s %s %+02i", msg->call_from, params.callsign.x, snr);
            qso_item.rst_s = snr;
            break;
        case MSG_TYPE_REPORT:
            snprintf(tx_msg, sizeof(tx_msg) - 1, "%s %s R%+02i", msg->call_from, params.callsign.x, snr);
            qso_item.rst_s = snr;
            break;
        case MSG_TYPE_R_REPORT:
            snprintf(tx_msg, sizeof(tx_msg) - 1, "%s %s RR73", msg->call_from, params.callsign.x);
            break;
        case MSG_TYPE_RR73:
            snprintf(tx_msg, sizeof(tx_msg) - 1, "%s %s 73", msg->call_from, params.callsign.x);
            break;
        default:
            return false;
    }
    tx_time_slot = !rx_odd;
    msg_set_text_fmt("Next TX: %s", tx_msg);
    return true;
}

/**
 * Get output level correction, based on output power and ALC
 */
static float get_correction() {
    static uint8_t msg_id = 0;
    float correction = 0.0f;
    float pwr, alc;

    if (tx_info_refresh(&msg_id, &alc, &pwr, NULL)) {
        float target_pwr = LV_MIN(params.pwr, MAX_PWR);
        if (alc > 0.5f) {
            correction = log10f(log10f(10.0f - alc)) * 20.0f;
        } else if (target_pwr - pwr > 0.5f) {
            // TODO: check battery level
            correction = log10f(target_pwr / pwr) * 10.0f;
        }
    }
    return correction;
}

static void tx_worker() {
    const uint16_t signal_freq = 1325;
    int16_t       *samples;
    uint32_t       n_samples;

    if (!ftx_worker_generate_tx_samples(tx_msg, signal_freq, &samples, &n_samples)) {
        state = RX_PROCESS;
        return;
    }
    float gain_offset = base_gain_offset + params.ft8_output_gain_offset.x;
    float play_gain_offset = audio_set_play_vol(gain_offset + 4.0f);
    gain_offset -= play_gain_offset;

    // Change freq before tx
    uint64_t radio_freq = params_band_cur_freq_get();
    radio_set_freq(radio_freq + params.ft8_tx_freq.x - signal_freq);
    radio_set_modem(true);

    float    prev_gain_offset = gain_offset;
    size_t   counter = 0;
    int16_t *ptr = samples;
    size_t   part;

    while (true) {
        if (counter > 30) {
            gain_offset += get_correction() * 0.4f;

            if (gain_offset > 0.0)
                gain_offset = 0.0f;
            else if (gain_offset < -40.0f)
                gain_offset = -40.0f;
        }
        if (n_samples <= 0 || state != TX_PROCESS) {
            state = RX_PROCESS;
            break;
        }
        part = LV_MIN(1024 * 2, n_samples);
        if (gain_offset == prev_gain_offset) {
            if (gain_offset != 0.0f) {
                audio_gain_db(ptr, part, gain_offset, ptr);
            }
        } else {
            // Smooth change gain
            audio_gain_db_transition(ptr, part, prev_gain_offset, gain_offset, ptr);
            prev_gain_offset = gain_offset;
        }
        audio_play(ptr, part);

        n_samples -= part;
        ptr += part;
        counter++;
    }

    params_float_set(&params.ft8_output_gain_offset, gain_offset - base_gain_offset + play_gain_offset);
    audio_play_wait();
    radio_set_modem(false);
    // Restore freq
    radio_set_freq(radio_freq);
    free(samples);
    audio_set_play_vol(params.play_gain_db_f.x);
}

/**
 * Parse incoming text to msg
 */
static msg_t parse_rx_msg(const char * str) {
    char            s[33];
    char            *call_to = NULL;
    char            *call_de = NULL;
    char            *extra = NULL;

    strncpy(s, str, sizeof(s) - 1);

    msg_t msg;
    msg.type = MSG_TYPE_OTHER;
    msg.call_from[0] = 0;
    msg.call_to[0] = 0;
    msg.extra[0] = 0;

    /* Split */

    call_to = strtok(s, " ");

    if (call_to) {
        call_de = strtok(NULL, " ");

        if (call_de) {
            extra = strtok(NULL, " ");
        }
    }

    if (!call_to || !call_de) {
        return msg;
    }

    /* Analysis */

    if (strcmp(call_to, "CQ") == 0) {
        if (is_cq_modifier(call_de)) {
            call_de = extra;
            extra = strtok(NULL, " ");
        }
        msg.type = MSG_TYPE_CQ;
        strncpy(msg.call_from, call_de, sizeof(msg.call_from) - 1);
        strncpy(msg.extra, extra ? extra : "", sizeof(msg.extra) - 1);
    } else {
        strncpy(msg.call_from, call_de, sizeof(msg.call_from) - 1);
        strncpy(msg.call_to, call_to, sizeof(msg.call_to) - 1);
        if (extra) {
            strncpy(msg.extra, extra, sizeof(msg.extra) - 1);
        }
        if ((strcmp(msg.extra, "RR73") == 0) || (strcmp(msg.extra, "RRR") == 0)) {
            msg.type = MSG_TYPE_RR73;
        } else if (strcmp(msg.extra, "73") == 0) {
            msg.type = MSG_TYPE_73;
        } else if (grid_check(msg.extra)) {
            msg.type = MSG_TYPE_GRID;
        } else if (msg.extra[0] == 'R' && (msg.extra[1] == '-' || msg.extra[1] == '+')) {
            msg.snr = atoi(msg.extra + 1);
            msg.type = MSG_TYPE_R_REPORT;
        } else if (msg.extra[0] == '-' || msg.extra[0] == '+') {
            msg.snr = atoi(msg.extra);
            msg.type = MSG_TYPE_REPORT;
        }
    }
    return msg;
}

/**
 * Add INFO message to the table
 */
static void add_info(const char * fmt, ...) {
    va_list     args;
    cell_data_t  *cell_data = malloc(sizeof(cell_data_t));
    cell_data->cell_type = CELL_RX_INFO;

    va_start(args, fmt);
    vsnprintf(cell_data->text, sizeof(cell_data->text), fmt, args);
    va_end(args);

    event_send(table, EVENT_FT8_MSG, cell_data);
}

/**
 * Add TX message to the table
 */
static void add_tx_text(const char * text) {
    cell_data_t  *cell_data = malloc(sizeof(cell_data_t));
    cell_data->cell_type = CELL_TX_MSG;

    strncpy(cell_data->text, text, sizeof(cell_data->text) - 1);
    if (strncmp(cell_data->text, "CQ_", 3) == 0) {
        cell_data->text[2] = ' ';
    }

    event_send(table, EVENT_FT8_MSG, cell_data);
}

/**
 * Parse and add RX messages to the table
 */
static void add_rx_text(int16_t snr, const char * text, bool odd) {
    static size_t n_found_items;
    ft8_cell_type_t cell_type;

    msg_t msg = parse_rx_msg(text);

    if (str_equal(msg.call_to, params.callsign.x)) {
        cell_type = CELL_RX_TO_ME;
        if (!active_qso() && ((msg.type == MSG_TYPE_GRID) || (msg.type == MSG_TYPE_REPORT))) {
            // Use first decoded answer
            strncpy(qso_item.remote_callsign, msg.call_from, sizeof(qso_item.remote_callsign) - 1);
        }
        if (active_qso() && (msg.type != MSG_TYPE_73) && str_equal(msg.call_from, qso_item.remote_callsign)) {
            qso_item.last_snr = snr;
            qso_item.rx_odd = odd;
            if (msg.type == MSG_TYPE_GRID) {
                strncpy(qso_item.remote_qth, msg.extra, sizeof(qso_item.remote_qth) - 1);
            }
            if ((msg.type == MSG_TYPE_REPORT) || (msg.type == MSG_TYPE_R_REPORT)) {
                qso_item.rst_r = msg.snr;
            }
            if (params.ft8_auto.x) {
                generate_tx_msg(&msg);
            }
        }
    } else if (msg.type == MSG_TYPE_CQ) {
        cell_type = CELL_RX_CQ;
    } else if (!params.ft8_show_all) {
        return;
    } else {
        cell_type = CELL_RX_MSG;
    }
    cell_data_t  *cell_data = malloc(sizeof(cell_data_t));
    if (msg.type == MSG_TYPE_CQ) {
        cell_data->worked_type = qso_log_search_worked(
            msg.call_from,
            params.ft8_protocol == FTX_PROTOCOL_FT8 ? MODE_FT8 : MODE_FT4,
            qso_log_freq_to_band(params_band_cur_freq_get())
        );
    }

    cell_data->cell_type = cell_type;
    strncpy(cell_data->text, text, sizeof(cell_data->text) - 1);
    cell_data->msg = msg;
    cell_data->local_snr = snr;
    cell_data->odd = odd;
    if (params.qth.x[0] != 0) {
        const char *qth = find_qth(text);

        cell_data->dist = qth ? grid_dist(qth) : 0;
    } else {
        cell_data->dist = 0;
    }
    event_send(table, EVENT_FT8_MSG, cell_data);
}

static void received_message_cb(const char *text, int snr, float freq_hz, float time_sec, void *user_data) {
    bool *odd = (bool *)user_data;
    add_rx_text(snr, text, *odd);
}

static void rx_worker(bool new_slot, bool odd) {
    unsigned int   n;
    float complex *buf;
    const int block_size = ftx_worker_get_block_size();
    const size_t   size = block_size * DECIM;

    pthread_mutex_lock(&audio_mutex);

    while (cbuffercf_size(audio_buf) > size) {
        cbuffercf_read(audio_buf, size, &buf, &n);

        firdecim_crcf_execute_block(decim, buf, block_size, decim_buf);
        cbuffercf_release(audio_buf, size);

        waterfall_process(decim_buf, block_size);

        ftx_worker_put_rx_samples(decim_buf, block_size);

        if (ftx_worker_is_full()) {
            ftx_worker_decode(received_message_cb, true, (void *)&odd);
            ftx_worker_reset();
        } else {
            ftx_worker_decode(received_message_cb, false, (void *)&odd);
        }
    }
    pthread_mutex_unlock(&audio_mutex);

    if (new_slot) {
        ftx_worker_decode(received_message_cb, true, (void *)&odd);
        ftx_worker_reset();
    }
}

static void update_call_btn(void * arg) {
    if (tx_enabled) {
        buttons_load(2, &button_tx_call_dis);
    } else {
        buttons_load(2, &button_tx_call_en);
    }
}


static void generate_tx_msg(const msg_t * rx_msg) {
    if (!make_answer(rx_msg, qso_item.last_snr, qso_item.rx_odd)) {
        tx_msg[0] = 0;
    } else {
        cq_enabled = false;
        scheduler_put(update_call_btn, NULL, 0);
    }
    switch (rx_msg->type) {
        case MSG_TYPE_RR73:
        case MSG_TYPE_R_REPORT:
            save_qso();
            clear_qso();
            break;
        default:
            break;
    }
}

static void * decode_thread(void *arg) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    struct timespec now;
    bool            new_odd;
    struct tm       *ts;
    bool            new_slot=false;
    bool            have_tx_msg=false;

    while (true) {
        clock_gettime(CLOCK_REALTIME, &now);
        new_odd = get_time_slot(now);
        new_slot = new_odd != odd;
        rx_worker(new_slot, odd);

        if (new_slot) {
            have_tx_msg = strlen(tx_msg) != 0;

            if (have_tx_msg && (tx_time_slot == new_odd) && tx_enabled) {
                state = TX_PROCESS;
                add_tx_text(tx_msg);
                tx_worker();
                if (!active_qso() && !cq_enabled) {
                    tx_msg[0] = 0;
                }
            } else {
                state = RX_PROCESS;
                if ((!have_tx_msg || !tx_enabled)) {
                    ts = localtime(&now.tv_sec);
                    add_info("RX %s %02i:%02i:%02i", params_band_label_get(),
                        ts->tm_hour, ts->tm_min, ts->tm_sec);
                }
            }
        } else {
            usleep(30000);
        }
        odd = new_odd;
    }

    return NULL;
}
