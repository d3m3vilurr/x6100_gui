/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

#include "lvgl/lvgl.h"
#include "dialog.h"
#include "dialog_ft8.h"
#include "styles.h"
#include "params.h"
#include "radio.h"
#include "audio.h"
#include "keyboard.h"
#include "events.h"
#include "radio.h"
#include "buttons.h"
#include "main_screen.h"
#include "qth.h"

#include "ft8/unpack.h"
#include "ft8/ldpc.h"
#include "ft8/decode.h"
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/crc.h"

#define DECIM           4
#define SAMPLE_RATE     (AUDIO_CAPTURE_RATE / DECIM)

#define MIN_SCORE       10
#define MAX_CANDIDATES  120
#define LDPC_ITER       20
#define MAX_DECODED     50
#define FREQ_OSR        2
#define TIME_OSR        4

#define FT8_BANDS       10
#define FT4_BANDS       8

typedef enum {
    RX_IDLE = 0,
    RX_PROCESS,
} rx_state_t;

typedef enum {
    MSG_RX_INFO = 0,
    MSG_RX_MSG,
} ft8_msg_type_t;

typedef struct {
    int16_t         snr;
    int16_t         dist;
} ft8_cell_t;

typedef struct {
    ft8_msg_type_t  type;
    char            *msg;
    ft8_cell_t      *cell;
} ft8_msg_t;

static ft8_state_t          state = FT8_OFF;
static rx_state_t           rx_state = RX_IDLE;

static lv_obj_t             *table;
static int16_t              table_rows;

static pthread_cond_t       audio_cond;
static pthread_mutex_t      audio_mutex;
static cbuffercf            audio_buf;
static pthread_t            thread;

static firdecim_crcf        decim;
static float complex        *decim_buf;
static complex float        *rx_window = NULL;
static complex float        *time_buf;
static complex float        *freq_buf;
static windowcf             frame_window;
static fftplan              fft;

static float                symbol_period;
static uint32_t             block_size;
static uint32_t             subblock_size;
static uint16_t             nfft;
static float                fft_norm;
static waterfall_t          wf;

static candidate_t          candidate_list[MAX_CANDIDATES];
static message_t            decoded[MAX_DECODED];
static message_t*           decoded_hashtable[MAX_DECODED];

static struct tm            timestamp;

static void construct_cb(lv_obj_t *parent);
static void key_cb(lv_event_t * e);
static void destruct_cb();
static void * decode_thread(void *arg);

static void show_all_cb(lv_event_t * e);
static void show_cq_cb(lv_event_t * e);
static void mode_ft8_cb(lv_event_t * e);
static void mode_ft4_cb(lv_event_t * e);

static button_item_t button_show_all = { .label = "Show\nAll", .press = show_all_cb };
static button_item_t button_show_cq = { .label = "Show\nCQ", .press = show_cq_cb };
static button_item_t button_mode_ft8 = { .label = "Mode\nFT8", .press = mode_ft8_cb };
static button_item_t button_mode_ft4 = { .label = "Mode\nFT4", .press = mode_ft4_cb };

static dialog_t             dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .key_cb = key_cb
};

dialog_t                    *dialog_ft8 = &dialog;

static void reset() {
    wf.num_blocks = 0;
    rx_state = RX_IDLE;
}

static void init() {
    /* FT8 decoder */

    float   slot_time;
    
    switch (params.ft8_protocol) {
        case PROTO_FT4:
            slot_time = FT4_SLOT_TIME;
            symbol_period = FT4_SYMBOL_PERIOD;
            break;
            
        case PROTO_FT8:
            slot_time = FT8_SLOT_TIME;
            symbol_period = FT8_SYMBOL_PERIOD;
            break;
    }
    
    block_size = SAMPLE_RATE * symbol_period;
    subblock_size = block_size / TIME_OSR;
    nfft = block_size * FREQ_OSR;
    fft_norm = 2.0f / nfft;
    
    const uint32_t max_blocks = slot_time / symbol_period;
    const uint32_t num_bins = SAMPLE_RATE * symbol_period / 2;

    size_t mag_size = max_blocks * TIME_OSR * FREQ_OSR * num_bins * sizeof(uint8_t);
    
    wf.max_blocks = max_blocks;
    wf.num_bins = num_bins;
    wf.time_osr = TIME_OSR;
    wf.freq_osr = FREQ_OSR;
    wf.block_stride = TIME_OSR * FREQ_OSR * num_bins;
    wf.mag = (uint8_t *) malloc(mag_size);
    wf.protocol = params.ft8_protocol;

    /* DSP */
    
    decim_buf = (float complex *) malloc(block_size * sizeof(float complex));
    time_buf = (float complex*) malloc(nfft * sizeof(float complex));
    freq_buf = (float complex*) malloc(nfft * sizeof(float complex));
    fft = fft_create_plan(nfft, time_buf, freq_buf, LIQUID_FFT_FORWARD, 0);
    frame_window = windowcf_create(nfft);

    rx_window = malloc(nfft * sizeof(complex float));

    for (uint16_t i = 0; i < nfft; i++)
        rx_window[i] = liquid_hann(i, nfft);

    float gain = 0.0f;

    for (uint16_t i = 0; i < nfft; i++)
        gain += rx_window[i] * rx_window[i];
        
    gain = 1.0f / sqrtf(gain);

    for (uint16_t i = 0; i < nfft; i++)
        rx_window[i] *= gain;

    reset();
        
    /* Worker */
        
    pthread_mutex_init(&audio_mutex, NULL);
    pthread_cond_init(&audio_cond, NULL);
    pthread_create(&thread, NULL, decode_thread, NULL);

    state = FT8_RX;
}

static void done() {
    state = FT8_OFF;

    pthread_cancel(thread);
    pthread_join(thread, NULL);

    free(wf.mag);
    windowcf_destroy(frame_window);

    free(decim_buf);
    free(time_buf);
    free(freq_buf);
    fft_destroy_plan(fft);

    free(rx_window);
}

static void send_info(const char * fmt, ...) {
    va_list     args;
    char        buf[128];
    ft8_msg_t   *msg = malloc(sizeof(ft8_msg_t));

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    msg->type = MSG_RX_INFO;
    msg->msg = strdup(buf);
    msg->cell = NULL;

    event_send(table, EVENT_FT8_MSG, msg);
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

static void send_text(int16_t snr, const char * text) {
    ft8_msg_t   *msg = malloc(sizeof(ft8_msg_t));

    msg->type = MSG_RX_MSG;
    msg->msg = strdup(text);

    msg->cell = lv_mem_alloc(sizeof(ft8_cell_t));
    msg->cell->snr = snr;

    if (params.qth[0] != 0) {
        const char *qth = find_qth(text);
            
        msg->cell->dist = qth ? grid_dist(qth) : 0;
    } else {
        msg->cell->dist = 0;
    }

    event_send(table, EVENT_FT8_MSG, msg);
}

static void decode() {
    uint16_t    num_candidates = ft8_find_sync(&wf, MAX_CANDIDATES, candidate_list, MIN_SCORE);

    memset(decoded_hashtable, 0, sizeof(decoded_hashtable));
    memset(decoded, 0, sizeof(decoded));
    
    for (uint16_t idx = 0; idx < num_candidates; idx++) {
        const candidate_t *cand = &candidate_list[idx];
        
        if (cand->score < MIN_SCORE)
            continue;
            
        float freq_hz = (cand->freq_offset + (float) cand->freq_sub / wf.freq_osr) / symbol_period;
        float time_sec = (cand->time_offset + (float) cand->time_sub / wf.time_osr) * symbol_period;
        
        message_t       message;
        decode_status_t status;
        
        if (!ft8_decode(&wf, cand, &message, LDPC_ITER, &status)) {
            continue;
        }
        
        uint16_t    idx_hash = message.hash % MAX_DECODED;
        bool        found_empty_slot = false;
        bool        found_duplicate = false;
        
        do {
            if (decoded_hashtable[idx_hash] == NULL) {
                found_empty_slot = true;
            } else if (decoded_hashtable[idx_hash]->hash == message.hash && strcmp(decoded_hashtable[idx_hash]->text, message.text) == 0) {
                found_duplicate = true;
            } else {
                idx_hash = (idx_hash + 1) % MAX_DECODED;
            }
        } while (!found_empty_slot && !found_duplicate);

        if (found_empty_slot) {
            memcpy(&decoded[idx_hash], &message, sizeof(message));
            decoded_hashtable[idx_hash] = &decoded[idx_hash];

            if (params.ft8_show_all || strncmp(message.text, "CQ", 2) == 0) {
                send_text(cand->snr, message.text);
            }
        }
    }
}

void static process(float complex *frame) {
    complex float   *frame_ptr;
    int             offset = wf.num_blocks * wf.block_stride;
    int             frame_pos = 0;
    
    for (int time_sub = 0; time_sub < wf.time_osr; time_sub++) {
        windowcf_write(frame_window, &frame[frame_pos], subblock_size);
        frame_pos += subblock_size;

        windowcf_read(frame_window, &frame_ptr);
        
        for (uint32_t pos = 0; pos < nfft; pos++)
            time_buf[pos] = rx_window[pos] * frame_ptr[pos];

        fft_execute(fft);
                
        for (int freq_sub = 0; freq_sub < wf.freq_osr; freq_sub++)
            for (int bin = 0; bin < wf.num_bins; bin++) {
                int             src_bin = (bin * wf.freq_osr) + freq_sub;
                complex float   freq = freq_buf[src_bin];
                float           v = crealf(freq * conjf(freq));
                float           db = 10.0f * log10f(v);
                int             scaled = (int16_t) (db * 2.0f + 240.0f);
                
                if (scaled < 0) {
                    scaled = 0;
                } else if (scaled > 255) {
                    scaled = 255;
                }

                wf.mag[offset] = scaled;
                offset++;
            }
    }
    
    wf.num_blocks++;
}

static void * decode_thread(void *arg) {
    unsigned int    n;
    float complex   *buf;
    const size_t    size = block_size * DECIM;
    struct tm       *tm;
    time_t          now;
    
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    
    while (true) {
        pthread_mutex_lock(&audio_mutex);

        while (cbuffercf_size(audio_buf) < size) {
            pthread_cond_wait(&audio_cond, &audio_mutex);
        }
        
        pthread_mutex_unlock(&audio_mutex);
            
        while (cbuffercf_size(audio_buf) > size) {
            cbuffercf_read(audio_buf, size, &buf, &n);
    
            firdecim_crcf_execute_block(decim, buf, block_size, decim_buf);
            cbuffercf_release(audio_buf, size);

            if (rx_state == RX_IDLE) {
                now = time(NULL);
                tm = localtime(&now);
                
                bool start = false;
                
                switch (params.ft8_protocol) {
                    case PROTO_FT4:
                        switch (tm->tm_sec) {
                            case 0 ... 1:
                            case 7 ... 8:
                            case 15 ... 16:
                            case 22 ... 23:
                            case 30 ... 31:
                            case 37 ... 38:
                            case 45 ... 46:
                            case 52 ... 53:
                                start = true;
                                break;
                                
                            default:
                                start = false;
                        }
                        break;
                        
                    case PROTO_FT8:
                        switch (tm->tm_sec) {
                            case 0 ... 1:
                            case 15 ... 16:
                            case 30 ... 31:
                            case 45 ... 46:
                                start = true;
                                break;
                                
                            default:
                                start = false;
                        }
                        break;
                }
                
                if (start) {
                    timestamp = *tm;
                    rx_state = RX_PROCESS;
                    send_info("RX %s %02i:%02i:%02i", params_band.label, timestamp.tm_hour, timestamp.tm_min, timestamp.tm_sec);
                }
            }
        
            if (rx_state == RX_PROCESS) {
                process(decim_buf);
        
                if (wf.num_blocks >= wf.max_blocks) {
                    decode();
                    reset();

                    rx_state = RX_IDLE;
               }
            }
        }
    }
}

static void add_msg_cb(lv_event_t * e) {
    ft8_msg_t   *msg = (ft8_msg_t *) lv_event_get_param(e);
    int16_t     row = 0;
    int16_t     col = 0;
    bool        scroll;

    lv_table_get_selected_cell(table, &row, &col);
    scroll = table_rows == (row + 1);

#ifdef MAX_TABLE_MSG
    if (table_rows > MAX_TABLE_MSG) {
        for (uint16_t i = 1; i < table_rows; i++)
            lv_table_set_cell_value(table, i-1, 0, lv_table_get_cell_value(table, i, 0));
            
        table_rows--;
    }
#endif

    lv_table_set_cell_value(table, table_rows, 0, msg->msg);
    
    lv_table_cell_ctrl_t ctrl;

    switch (msg->type) {
        case MSG_RX_INFO:
            ctrl = LV_TABLE_CELL_CTRL_CUSTOM_1;
            break;
            
        case MSG_RX_MSG:
            ctrl = LV_TABLE_CELL_CTRL_CUSTOM_2;
            break;
    }
    
    lv_table_add_cell_ctrl(table, table_rows, 0, ctrl);

    if (msg->cell) {
        lv_table_set_cell_user_data(table, table_rows, 0, msg->cell);
    }
    
    if (scroll) {
        int32_t *c = malloc(sizeof(int32_t));
        *c = LV_KEY_DOWN;
        
        lv_event_send(table, LV_EVENT_KEY, c);
    }
    
    table_rows++;
}

static void draw_part_begin_cb(lv_event_t * e) {
    lv_obj_t                *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t  *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part == LV_PART_ITEMS) {
        uint32_t row = dsc->id / lv_table_get_col_cnt(obj);
        uint32_t col = dsc->id - row * lv_table_get_col_cnt(obj);

        if (lv_table_has_cell_ctrl(obj, row, col, LV_TABLE_CELL_CTRL_CUSTOM_1)) {           /* RX INFO */
            dsc->label_dsc->align = LV_TEXT_ALIGN_CENTER;
            dsc->rect_dsc->bg_color = lv_color_white();
            dsc->rect_dsc->bg_opa = 128;
        } else if (lv_table_has_cell_ctrl(obj, row, col, LV_TABLE_CELL_CTRL_CUSTOM_2)) {    /* RX MSG */
            const char *str = lv_table_get_cell_value(obj, row, col);
            
            if (strncmp(str, "CQ", 2) == 0) {
                dsc->rect_dsc->bg_color = lv_color_hex(0x00FF00);
                dsc->rect_dsc->bg_opa = 64;
            }
        }
    }
}


static void draw_part_end_cb(lv_event_t * e) {
    lv_obj_t                *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t  *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part == LV_PART_ITEMS) {
        uint32_t row = dsc->id / lv_table_get_col_cnt(obj);
        uint32_t col = dsc->id - row * lv_table_get_col_cnt(obj);

        if (lv_table_has_cell_ctrl(obj, row, col, LV_TABLE_CELL_CTRL_CUSTOM_2)) {    /* RX MSG */
            char                buf[64];
            const lv_coord_t    cell_top = lv_obj_get_style_pad_top(obj, LV_PART_ITEMS);
            const lv_coord_t    cell_bottom = lv_obj_get_style_pad_bottom(obj, LV_PART_ITEMS);
            lv_area_t           area;

            dsc->label_dsc->align = LV_TEXT_ALIGN_RIGHT;

            area.y1 = dsc->draw_area->y1 + cell_top;
            area.y2 = dsc->draw_area->y2 - cell_bottom;

            area.x2 = dsc->draw_area->x2 - 15;
            area.x1 = area.x2 - 120;

            ft8_cell_t *cell = lv_table_get_cell_user_data(obj, row, col);
            
            snprintf(buf, sizeof(buf), "%i dB", cell->snr);
            lv_draw_label(dsc->draw_ctx, dsc->label_dsc, &area, buf, NULL);

            if (cell->dist > 0) {
                area.x2 = area.x1 - 10;
                area.x1 = area.x2 - 200;
                
                snprintf(buf, sizeof(buf), "%i km", cell->dist);
                lv_draw_label(dsc->draw_ctx, dsc->label_dsc, &area, buf, NULL);
            }
        }
    }
}

static void selected_msg_cb(lv_event_t * e) {
    int16_t     row;
    int16_t     col;

    lv_table_get_selected_cell(table, &row, &col);
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
    done();
    
    firdecim_crcf_destroy(decim);
    free(audio_buf);

    mem_load(MEM_BACKUP_ID);

    main_screen_lock_mode(false);
    main_screen_lock_freq(false);
    main_screen_lock_band(false);
}

static void load_band() {
    uint16_t mem_id = 0;
    
    switch (params.ft8_protocol) {
        case PROTO_FT8:
            mem_id = MEM_FT8_ID;
            
            if (params.ft8_band > FT8_BANDS - 1) {
                params.ft8_band = FT8_BANDS - 1;
            }
            break;
            
        case PROTO_FT4:
            mem_id = MEM_FT4_ID;

            if (params.ft8_band > FT4_BANDS - 1) {
                params.ft8_band = FT4_BANDS - 1;
            }
            break;
    }
    
    mem_load(mem_id + params.ft8_band);
}

static void clean() {
    reset();
    rx_state = RX_IDLE;

    lv_table_set_row_cnt(table, 1);
    lv_table_set_cell_value(table, 0, 0, "Wait sync");
    lv_table_add_cell_ctrl(table, 0, 0, LV_TABLE_CELL_CTRL_CUSTOM_1);
    lv_table_clear_cell_ctrl(table, 0, 0, LV_TABLE_CELL_CTRL_CUSTOM_2);

    table_rows = 0;

    int32_t *c = malloc(sizeof(int32_t));
    *c = LV_KEY_UP;
        
    lv_event_send(table, LV_EVENT_KEY, c);
}

static void band_cb(lv_event_t * e) {
    int band = params.ft8_band;
    int max_band = 0;
    
    switch (params.ft8_protocol) {
        case PROTO_FT8:
            max_band = FT8_BANDS - 1;
            break;
            
        case PROTO_FT4:
            max_band = FT4_BANDS - 1;
            break;
    }

    if (lv_event_get_code(e) == EVENT_BAND_UP) {
        band++;
        
        if (band > max_band) {
            band = 0;
        }
    } else {
        band--;
        
        if (band < 0) {
            band = max_band;
        }
    }
    
    params_lock();
    params.ft8_band = band;
    params_unlock(&params.durty.ft8_band);
    load_band();
    clean();    
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    decim = firdecim_crcf_create_kaiser(DECIM, 16, 40.0f);
    audio_buf = cbuffercf_create(AUDIO_CAPTURE_RATE);

    table = lv_table_create(dialog.obj);
    
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_add_event_cb(table, add_msg_cb, EVENT_FT8_MSG, NULL);
    lv_obj_add_event_cb(table, selected_msg_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(table, draw_part_begin_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(table, draw_part_end_cb, LV_EVENT_DRAW_PART_END, NULL);

    lv_obj_set_size(table, 775, 325);
    
    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, 770);

    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, 0, LV_PART_ITEMS);

    lv_obj_set_style_text_color(table, lv_color_black(), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_color(table, lv_color_white(), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(table, 128, LV_PART_ITEMS | LV_STATE_EDITED);

    lv_table_set_cell_value(table, 0, 0, "Wait sync");
    lv_table_add_cell_ctrl(table, 0, 0, LV_TABLE_CELL_CTRL_CUSTOM_1);

    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    lv_obj_center(table);
    table_rows = 0;

    if (params.ft8_show_all) {
        buttons_load(0, &button_show_all);
    } else {
        buttons_load(0, &button_show_cq);
    }

    switch (params.ft8_protocol) {
        case PROTO_FT8:
            buttons_load(1, &button_mode_ft8);
            break;

        case PROTO_FT4:
            buttons_load(1, &button_mode_ft4);
            break;
    }
    
    mem_save(MEM_BACKUP_ID);
    load_band();

    main_screen_lock_mode(true);
    main_screen_lock_freq(true);
    main_screen_lock_band(true);

    init();
}

static void show_all_cb(lv_event_t * e) {
    params_lock();
    params.ft8_show_all = false;
    params_unlock(&params.durty.ft8_show_all);

    buttons_load(0, &button_show_cq);
}

static void show_cq_cb(lv_event_t * e) {
    params_lock();
    params.ft8_show_all = true;
    params_unlock(&params.durty.ft8_show_all);

    buttons_load(0, &button_show_all);
}

static void mode_ft8_cb(lv_event_t * e) {
    params_lock();
    params.ft8_protocol = PROTO_FT4;
    params_unlock(&params.durty.ft8_protocol);

    buttons_load(1, &button_mode_ft4);

    done();
    init();
    clean();
    load_band();
}

static void mode_ft4_cb(lv_event_t * e) {
    params_lock();
    params.ft8_protocol = PROTO_FT8;
    params_unlock(&params.durty.ft8_protocol);

    buttons_load(1, &button_mode_ft8);

    done();
    init();
    clean();
    load_band();
}

ft8_state_t dialog_ft8_get_state() {
    return state;
}

void dialog_ft8_put_audio_samples(unsigned int n, float complex *samples) {
    pthread_mutex_lock(&audio_mutex);
    cbuffercf_write(audio_buf, samples, n);
    pthread_cond_broadcast(&audio_cond);
    pthread_mutex_unlock(&audio_mutex);
}
