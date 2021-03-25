// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_attr.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/sens_reg.h"
#include <math.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "iot_touchpad.h"
#include "sdkconfig.h"

#include "hal/touch_sensor_hal.h"

#ifdef CONFIG_DATA_SCOPE_DEBUG
#include "touch_tune_tool.h"
#endif

#define USE_ESP_TIMER       CONFIG_TOUCH_PAD_USE_ESP_TIMER

#define IOT_CHECK(tag, a, ret)  if(!(a)) {                                             \
        ESP_LOGE(tag,"%s:%d (%s)", __FILE__, __LINE__, __FUNCTION__);      \
        return (ret);                                                                   \
        }

#define ERR_ASSERT(tag, param)  IOT_CHECK(tag, (param) == ESP_OK, ESP_FAIL)
#define POINT_ASSERT(tag, param)    IOT_CHECK(tag, (param) != NULL, ESP_FAIL)
#define RES_ASSERT(tag, res, ret)   IOT_CHECK(tag, (res) != pdFALSE, ret)
#define TIMER_CALLBACK_MAX_WAIT_TICK    (0)

/*************Fixed Parameters********************/
#define SLDER_POS_FILTER_FACTOR_DEFAULT             4       /**< Slider IIR filter parameters. */

#define TOUCHPAD_MEAS_PERIOD_MS                     20
#define TOUCHPAD_MEAS_CYCLE_US                      8192    /* 8192 - max */
#define TOUCHPAD_SLEEP_CYCLE_MS                     (TOUCHPAD_MEAS_PERIOD_MS - TOUCHPAD_MEAS_CYCLE_US/1000)     /* 437 - max */

#define TOUCHPAD_STATE_SWITCH_DEBOUNCE              80     /**< 80ms; Debounce threshold max coun value. */
#define TOUCHPAD_BASELINE_RESET_COUNT_THRESHOLD     5       /**< 5 count number; All channels; */
#define TOUCHPAD_BASELINE_UPDATE_COUNT_THRESHOLD    800     /**< 800ms; Baseline update cycle. */
#define TOUCHPAD_TOUCH_LOW_SENSE_THRESHOLD          ((float)0.03)    /**< 3% ; Set the low sensitivity threshold.
                                                                 When less than this threshold, remove the jitter processing. */
#define TOUCHPAD_TOUCH_THRESHOLD_PERCENT            ((float)0.75)    /**< 75%; This is button type triggering threshold, should be larger than noise threshold.
                                                                 The threshold determines the sensitivity of the touch. */
#define TOUCHPAD_NOISE_THRESHOLD_PERCENT            ((float)0.20)    /**< 20%; The threshold is used to determine whether to update the baseline.
                                                                 The touch system has a signal-to-noise ratio of at least 5:1. */
#define TOUCHPAD_HYSTERESIS_THRESHOLD_PERCENT       ((float)0.10)    /**< 10%; The threshold prevents frequent triggering. */
#define TOUCHPAD_BASELINE_RESET_THRESHOLD_PERCENT   ((float)0.20)    /**< 20%; If the touch data exceed this threshold
                                                                 for 'RESET_COUNT_THRESHOLD' times, then reset baseline to raw data. */
#define TOUCHPAD_SLIDER_TRIGGER_THRESHOLD_PERCENT   ((float)0.50)    /**< 50%; This is slider type triggering threshold, should large than noise threshold.
                                                                 when diff-value exceeded this threshold, a sliding operation has occurred. */

#define TOUCHPAD_FILTER_FACTOR_DEFAULT   (4)   // IIR filter coefficient.
#define TOUCHPAD_SHIFT_DEFAULT           (4)   // Increase computing accuracy.
#define TOUCHPAD_SHIFT_ROUND_DEFAULT     (8)   // ROUND = 2^(n-1); rounding off for fractional.

typedef struct tp_custom_cb tp_custom_cb_t;
typedef enum {
    TOUCHPAD_STATE_IDLE = 0,
    TOUCHPAD_STATE_PUSH,
    TOUCHPAD_STATE_PRESS,
    TOUCHPAD_STATE_RELEASE,
} tp_status_t;

typedef enum {
    TOUCHPAD_SINGLE_BUTTON = 0,
    TOUCHPAD_MATRIX_BUTTON,
#if CONFIG_TOUCH_PAD_USE_SLIDER
    TOUCHPAD_LINEAR_SLIDER,
    TOUCHPAD_DUPLEX_SLIDER,
    TOUCHPAD_WHEEL_SLIDER,
#endif
    TOUCHPAD_TYPE_MAX
} tp_type_t;

typedef struct {
    tp_cb cb;
    void *arg;
} tp_cb_t;

typedef struct {
    touch_pad_t touch_pad_num;  //Touch pad channel.
    tp_status_t state;          //The button touch status.
    tp_type_t button_type;      //Matrix or single button.
    float touchChange;          //User setting. Stores the rate of touch data changes when touched.
    float diff_rate;            //diff_rate = value change of touch / baseline.
    float touch_thr;            //Touch trigger threshold.
    float noise_thr;            //Basedata update threshold.
    float hysteresis_thr;       //The threshold prevents frequent triggering.
    float baseline_reset_thr;   //Basedata reset threshold.
    float slide_trigger_thr;    //Slide trigger threshold.
    uint16_t baseline;          //Base data update from filtered data. solve temperature drift.
    uint16_t debounce_count;    //Debounce count variable.
    uint16_t debounce_th;       //Debounce threshold. If exceeded, confirm the trigger.
    uint16_t bl_reset_count;    //Basedata reset count variable.
    uint16_t bl_reset_count_th; //Basedata reset threshold. If exceeded, reset basedata.
    uint16_t bl_update_count;   //Basedata update count variable.
    uint16_t bl_update_count_th;//Basedata update threshold. If exceeded, update basedata.
    #if USE_ESP_TIMER
    uint32_t interval_ms;
    esp_timer_handle_t serial_tmr;
    #else
    TimerHandle_t serial_tmr;
    #endif
    tp_cb_t *cb_group[TOUCHPAD_CB_MAX]; //Stores global variables for each channel parameter.
    #if CONFIG_TOUCH_PAD_USE_CB_SERIAL
    tp_cb_t serial_cb;          //A callback.
    /*Serial trigger parameter*/
    uint32_t serial_thres_sec;  //Continuously triggered threshold parameters.
    uint32_t serial_interval_ms;//Continuously triggered counting parameters.
    uint32_t sum_ms;            //Long press parameter.
    #endif
    #if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
    tp_custom_cb_t *custom_cbs; //User-defined callback function.
    #endif
} tp_dev_t;

#if CONFIG_TOUCH_PAD_USE_SLIDER
typedef struct {
    float pos_scale;
    float pos_range;
    uint8_t tp_num;
    uint32_t slide_pos;
    float *calc_val;
    tp_handle_t *tp_handles;
} tp_slide_t;
#endif

#if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
struct tp_custom_cb {
    tp_cb cb;
    void *arg;
    #if USE_ESP_TIMER
    uint32_t interval_ms;
    esp_timer_handle_t tmr;
    #else
    TimerHandle_t tmr;
    #endif
    tp_dev_t *tp_dev;
    tp_custom_cb_t *next_cb;
};
#endif

typedef enum {
    TOUCHPAD_MATRIX_ROW = 0,
    TOUCHPAD_MATRIX_COLUMN,
    TOUCHPAD_MATRIX_TYPE_MAX,
} tp_matrix_type_t;

typedef struct tp_matrix_cus_cb tp_matrix_cus_cb_t;

typedef struct {
    tp_matrix_cb cb;
    void *arg;
} tp_matrix_cb_t;

typedef struct tp_matrix_arg tp_matrix_arg_t;

typedef struct {
    tp_handle_t *x_tps;
    tp_handle_t *y_tps;
    tp_matrix_arg_t *matrix_args;
    tp_matrix_arg_t *matrix_args_y;
    tp_matrix_cb_t *cb_group[TOUCHPAD_CB_MAX];
    #if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
    tp_matrix_cus_cb_t *custom_cbs;
    #endif

    #if CONFIG_TOUCH_PAD_USE_CB_SERIAL
    tp_matrix_cb_t serial_cb;
    #endif
    uint32_t serial_thres_sec;
    uint32_t serial_interval_ms;
    #if USE_ESP_TIMER
    esp_timer_handle_t serial_tmr;
    #else
    TimerHandle_t serial_tmr;
    #endif
    tp_status_t active_state;
    uint8_t active_idx;
    uint8_t x_num;
    uint8_t y_num;
} tp_matrix_t;

typedef struct tp_matrix_arg {
    tp_matrix_t *tp_matrix;
    uint8_t tp_idx;
    tp_matrix_type_t type;
} tp_matrix_arg_t;

struct tp_matrix_cus_cb {
    tp_matrix_cb cb;
    void *arg;
    #if USE_ESP_TIMER
    esp_timer_handle_t tmr;
    uint32_t interval_ms;
    #else
    TimerHandle_t tmr;
    #endif
    tp_matrix_t *tp_matrix;
    tp_matrix_cus_cb_t *next_cb;
};

#define SLIDE_POS_INF   ((1 << 8) - 1)                  // Record time of last touch, used to eliminate jitter
static const char *TAG = "touchpad";                    // Debug tag in esp log
static bool g_init_flag = false;                        // Judge if initialized the global setting of touch.
static tp_dev_t *tp_group[TOUCH_PAD_MAX];               // Buffer of each button.
static xSemaphoreHandle s_tp_mux = NULL;
static uint16_t tp_value[TOUCH_PAD_MAX];

static TaskHandle_t task_handle;

static IRAM_ATTR void tp_rtc_irq_handler(void *arg)
{
    touch_pad_clear_status();
    for (int i = 0; i < TOUCH_PAD_MAX; i++) {
        if (tp_group[i] != NULL) {
            tp_value[i] = touch_hal_read_raw_data(i);
        }
    }

    BaseType_t task_woken = pdFALSE;
    BaseType_t res = xTaskNotifyFromISR(task_handle, 0, eNoAction, &task_woken);
    assert(res == pdTRUE);
    if (task_woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

#if USE_ESP_TIMER
static void stop_timer(esp_timer_handle_t tmr) {
    esp_err_t res;
    res = esp_timer_stop(tmr);
    if (res != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(res);
}

void delete_timer(TimerHandle_t tmr) {
  esp_err_t res;
  if (tmr != NULL)
  {
      res = esp_timer_stop(tmr);
      if (res != ESP_ERR_INVALID_STATE)
          ESP_ERROR_CHECK(res);
      res = esp_timer_delete(tmr);
      ESP_ERROR_CHECK(res);
  }
}
#else
static void stop_timer(TimerHandle_t tmr) {
    xTimerStop(tmr, portMAX_DELAY);
}

static void delete_timer(TimerHandle_t tmr) {
    xTimerDelete(tmr, portMAX_DELAY);
}
#endif

#if CONFIG_TOUCH_PAD_USE_SLIDER
/* IIR filter for silder position. */
static uint32_t _slider_filter_iir(uint32_t in_now, uint32_t out_last, uint32_t k)
{
    if (k == 0) {
        return in_now;
    } else {
        uint32_t out_now = (in_now + (k - 1) * out_last) / k;
        return out_now;
    }
}

/* callback function triggered by 'push', 'tap' and 'release' event. */
static void tp_slide_pos_cb(void *arg)
{
    tp_dev_t *tp_dev = NULL;
    tp_slide_t *tp_slide = (tp_slide_t *) arg;
    float val_sum = 0;
    float pos = 0;
    float weight_sum = 0;
    uint8_t non0_cnt = 0;
    uint32_t max_idx = 0;
    uint32_t slide_pos_temp = tp_slide->slide_pos;
    static uint32_t slide_pos_last = 0;

    for (int i = 0; i < tp_slide->tp_num; i++) {
        tp_dev = (tp_dev_t *) tp_slide->tp_handles[i];
        weight_sum += tp_dev->slide_trigger_thr;
        tp_slide->calc_val[i] = tp_dev->diff_rate - tp_dev->slide_trigger_thr; //Calculate the actual change of each key.
        if (tp_slide->calc_val[i] < 0) {
            tp_slide->calc_val[i] = 0;
        }
    }
    for (int i = 0; i < tp_slide->tp_num; i++) {
        tp_dev = (tp_dev_t *) tp_slide->tp_handles[i];
        tp_slide->calc_val[i] = tp_slide->calc_val[i] * weight_sum / tp_dev->slide_trigger_thr; //Weights each key element, unifying the rate of change.
    }
    // Find out the three bigger and continuous data.
    for (int i = 0; i < tp_slide->tp_num; i++) {
        if (i >= 2) {
            // find the max sum of three continuous values
            float neb_sum = tp_slide->calc_val[i - 2] + tp_slide->calc_val[i - 1] + tp_slide->calc_val[i];
            if (neb_sum > val_sum) {
                // val_sum is the max value of neb_sum
                val_sum = neb_sum;
                max_idx = i - 1;
                // Check the zero data number.
                non0_cnt = 0;
                for (int j = i - 2; j <= i; j++) {
                    non0_cnt += (tp_slide->calc_val[j] > 0) ? 1 : 0;
                }
            }

        }
    }
    if (non0_cnt == 0) {
        // if the max value of neb_sum is zero, no pad is touched
        // slide_pos_temp = SLIDE_POS_INF;
    } else if (non0_cnt == 1) { // only touch one pad
        // Check the active button number.
        uint8_t no_zero = 0;
        for (int i = 0; i < tp_slide->tp_num; i++) {
            if (tp_slide->calc_val[i] > 0) {
                no_zero++;
            }
        }
        // if (no_zero > non0_cnt), May be duplex slider board. TOUCHPAD_DUPLEX_SLIDER.
        // If duplex slider board, should not identify one button touched.
        if (no_zero <= non0_cnt) {    // Linear slider. TOUCHPAD_LINEAR_SLIDER
            for (int i = max_idx - 1; i <= max_idx + 1; i++) {
                if (0 != tp_slide->calc_val[i]) {
                    if (i == tp_slide->tp_num - 1) {
                        slide_pos_temp = tp_slide->pos_range;
                    } else {
                        slide_pos_temp = (uint32_t) (i * tp_slide->pos_scale);
                    }
                    break;
                }
            }
        }
    } else if (non0_cnt == 2) { // Only touch two pad
        if (0 == tp_slide->calc_val[max_idx - 1]) {
            // return the corresponding position.
            pos = ((max_idx + 1) * tp_slide->calc_val[max_idx + 1] + (max_idx) * tp_slide->calc_val[max_idx])
                  * tp_slide->pos_scale;
            slide_pos_temp = (uint32_t) (pos / val_sum);
        } else if (0 == tp_slide->calc_val[max_idx + 1]) {
            // return the corresponding position.
            pos = ((max_idx - 1) * tp_slide->calc_val[max_idx - 1] + (max_idx) * tp_slide->calc_val[max_idx])
                  * tp_slide->pos_scale;
            slide_pos_temp = (uint32_t) (pos / val_sum);
        } else {
            // slide_pos_temp = tp_slide->slide_pos;
        }
    } else {
        // return the corresponding position.
        pos = ((max_idx - 1) * tp_slide->calc_val[max_idx - 1] + (max_idx) * tp_slide->calc_val[max_idx]
               + (max_idx + 1) * tp_slide->calc_val[max_idx + 1]) * tp_slide->pos_scale;
        slide_pos_temp = (uint32_t) (pos / val_sum);
    }
    // Improve the precision of the operation.
    slide_pos_last = slide_pos_last == 0 ? ((uint16_t) slide_pos_temp << 4) : slide_pos_last;
    slide_pos_last = _slider_filter_iir((slide_pos_temp << 4), slide_pos_last, SLDER_POS_FILTER_FACTOR_DEFAULT);
    tp_slide->slide_pos = ((slide_pos_last + 8) >> 4);

#ifdef CONFIG_DATA_SCOPE_DEBUG
    tune_dev_data_t dev_data = {0};
    for (int i = 0; i < tp_slide->tp_num; i++) {
        tp_dev = (tp_dev_t *) tp_slide->tp_handles[i];
        dev_data.ch = tp_dev->touch_pad_num;
        dev_data.baseline = tp_dev->baseline;
        dev_data.diff = tp_slide->calc_val[i] * tp_dev->baseline;
        dev_data.raw = tp_dev->baseline - tp_dev->diff_rate * tp_dev->baseline;
        dev_data.status = tp_slide->slide_pos;
        tune_tool_set_device_data(&dev_data);
    }
#endif
}
#endif

/* check and run the hooked callback function */
static inline void callback_exec(tp_dev_t *tp_dev, tp_cb_type_t cb_type)
{
    if (tp_dev->cb_group[cb_type] != NULL) {
        tp_cb_t *cb_info = tp_dev->cb_group[cb_type];
        cb_info->cb(cb_info->arg);
    }
}

#if CONFIG_TOUCH_PAD_USE_CB_SERIAL
/* check and run the hooked callback function */
static void tp_serial_timer_cb(void *tmr)
{
    #if USE_ESP_TIMER
    tp_dev_t *tp_dev = (tp_dev_t*)(tmr);
    #else
    tp_dev_t *tp_dev = (tp_dev_t *) pvTimerGetTimerID(tmr);
    #endif

    if (tp_dev->serial_cb.cb != NULL) {
        tp_dev->serial_cb.cb(tp_dev->serial_cb.arg);
    }
}
#endif

#if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
static void tp_custom_timer_cb(void *tmr)
{
    #if USE_ESP_TIMER
    tp_custom_cb_t *custom_cb = (tp_custom_cb_t*)(tmr);
    #else
    tp_custom_cb_t *custom_cb = (tp_custom_cb_t *) pvTimerGetTimerID(tmr);
    #endif

    custom_cb->tp_dev->state = TOUCHPAD_STATE_PRESS;
    custom_cb->cb(custom_cb->arg);
}

/* reset all the customed event timers */
static inline void tp_custom_reset_cb_tmrs(tp_dev_t *tp_dev)
{
    tp_custom_cb_t *custom_cb = tp_dev->custom_cbs;
    while (custom_cb != NULL) {
        if (custom_cb->tmr != NULL) {
            #if USE_ESP_TIMER
            esp_err_t res;
            res = esp_timer_stop(custom_cb->tmr);
            if (res != ESP_ERR_INVALID_STATE)
                ESP_ERROR_CHECK(res);
            res = esp_timer_start_once(custom_cb->tmr, custom_cb->interval_ms * 1000);
            ESP_ERROR_CHECK(res);
            #else
            xTimerReset(custom_cb->tmr, portMAX_DELAY);
            #endif
        }
        custom_cb = custom_cb->next_cb;
    }
}

/* stop all the customed event timers */
static inline void tp_custom_stop_cb_tmrs(tp_dev_t *tp_dev)
{
    tp_custom_cb_t *custom_cb = tp_dev->custom_cbs;
    while (custom_cb != NULL) {
        if (custom_cb->tmr != NULL) {
            stop_timer(custom_cb->tmr);
        }
        custom_cb = custom_cb->next_cb;
    }
}
#endif

/* Call this function after reading the filter once. This function should be registered. */
static void touch_pad_read_cb(uint16_t raw_data[], uint16_t filtered_data[])
{
    int16_t diff_data = 0;

    #if CONFIG_TOUCH_PAD_USE_SLIDER
    tp_dev_t *slide_trigger_dev = NULL;
    #endif

    // Main loop to check every channel raw data.
    for (int i = 0; i < TOUCH_PAD_MAX; i++) {
        if (tp_group[i] != NULL) {
            tp_dev_t *tp_dev = tp_group[i];
            // Use raw data calculate the diff data. Buttons respond fastly. Frequent button ok.
            diff_data = (int16_t) tp_dev->baseline - (int16_t) raw_data[i];
            tp_dev->diff_rate = (float) diff_data / (float) tp_dev->baseline;
            // IDLE status, wait to be pushed
            if (TOUCHPAD_STATE_IDLE == tp_dev->state || TOUCHPAD_STATE_RELEASE == tp_dev->state) {
                tp_dev->state = TOUCHPAD_STATE_IDLE;
                // If diff data less than noise threshold, update baseline value.
                if (fabs(tp_dev->diff_rate) <= tp_dev->noise_thr) {
                    tp_dev->bl_reset_count = 0; // Clean baseline reset count.
                    tp_dev->debounce_count = 0; // Clean debounce count.
                    // bl_update_count_th control the baseline update frequency
                    if (++tp_dev->bl_update_count > tp_dev->bl_update_count_th) {
                        tp_dev->bl_update_count = 0;
                        // Baseline updating can use Jitter filter ?
                        tp_dev->baseline = filtered_data[i];
                    }
                } else {
                    tp_dev->bl_update_count = 0;
                    // If the diff data is larger than the touch threshold, touch action be triggered.
                    if (tp_dev->diff_rate >= tp_dev->touch_thr + tp_dev->hysteresis_thr) {
                        tp_dev->bl_reset_count = 0;
                        // Debounce processing.
                        if (++tp_dev->debounce_count >= tp_dev->debounce_th \
                                || tp_dev->touchChange < TOUCHPAD_TOUCH_LOW_SENSE_THRESHOLD) {
                            tp_dev->debounce_count = 0;
                            tp_dev->state = TOUCHPAD_STATE_PUSH;
                            // run push event cb, reset custom event cb
                            callback_exec(tp_dev, TOUCHPAD_CB_PUSH);
                            #if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
                            tp_custom_reset_cb_tmrs(tp_dev);
                            #endif
                        }
                        // diff data exceed the baseline reset line. reset baseline to raw data.
                    } else if (tp_dev->diff_rate <= 0 - tp_dev->baseline_reset_thr) {
                        tp_dev->debounce_count = 0;
                        // Check that if do the reset action again. reset baseline value to raw data.
                        if (++tp_dev->bl_reset_count > tp_dev->bl_reset_count_th) {
                            tp_dev->bl_reset_count = 0;
                            tp_dev->baseline = raw_data[i];
                        }
                    } else {
                        tp_dev->debounce_count = 0;
                        tp_dev->bl_reset_count = 0;
                    }
                }
            } else {
                // The button is in touched status.
                // The button to be pressed continued. long press.
                if (tp_dev->diff_rate > tp_dev->touch_thr - tp_dev->hysteresis_thr) {
                    tp_dev->debounce_count = 0;
                    #if CONFIG_TOUCH_PAD_USE_CB_SERIAL
                    // sum_ms is the total time that the read value is under threshold, which means a touch event is on.
                    tp_dev->sum_ms += TOUCHPAD_MEAS_PERIOD_MS;
                    // whether this is the exact time that a serial event happens.
                    if (tp_dev->serial_thres_sec > 0
                            && tp_dev->sum_ms - TOUCHPAD_MEAS_PERIOD_MS < tp_dev->serial_thres_sec * 1000
                            && tp_dev->sum_ms >= tp_dev->serial_thres_sec * 1000) {
                        tp_dev->state = TOUCHPAD_STATE_PRESS;
                        tp_dev->serial_cb.cb(tp_dev->serial_cb.arg);
                        #if USE_ESP_TIMER == 1
                        esp_err_t res;
                        res = esp_timer_start_periodic(tp_dev->serial_tmr, tp_dev->interval_ms);
                        ESP_ERROR_CHECK(res);
                        #else
                        xTimerStart(tp_dev->serial_tmr, portMAX_DELAY);
                        #endif
                    }
                    #endif
                } else {    // Check the release action.
                    //  Debounce processing.
                    if (++tp_dev->debounce_count >= tp_dev->debounce_th \
                            || fabs(tp_dev->diff_rate) < tp_dev->noise_thr \
                            || tp_dev->touchChange < TOUCHPAD_TOUCH_LOW_SENSE_THRESHOLD) {
                        tp_dev->debounce_count = 0;
                        #if CONFIG_TOUCH_PAD_USE_CB_TAP_EVENT
                        if (tp_dev->state == TOUCHPAD_STATE_PUSH) {
                            callback_exec(tp_dev, TOUCHPAD_CB_TAP);
                        }
                        #endif
                        #if CONFIG_TOUCH_PAD_USE_CB_SERIAL
                        tp_dev->sum_ms = 0; // Clean long press count event.
                        #endif
                        tp_dev->state = TOUCHPAD_STATE_RELEASE;
                        callback_exec(tp_dev, TOUCHPAD_CB_RELEASE);
                        #if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
                        tp_custom_stop_cb_tmrs(tp_dev);
                        #endif
                        if (tp_dev->serial_tmr)
                            stop_timer(tp_dev->serial_tmr);
                    }
                }
            }

            #if CONFIG_TOUCH_PAD_USE_SLIDER
            // Check if the button also is slider element. All kind of slider. a button meanwhile is a slider.
            if (tp_dev->diff_rate > tp_dev->slide_trigger_thr && tp_dev->button_type >= TOUCHPAD_LINEAR_SLIDER) {
                slide_trigger_dev = tp_dev;
            }
            #endif
            
            #ifdef CONFIG_DATA_SCOPE_DEBUG
            if (tp_dev->button_type <= TOUCHPAD_MATRIX_BUTTON) {
                tune_dev_data_t dev_data = {0};
                dev_data.ch = i;
                dev_data.raw = raw_data[i];
                dev_data.baseline = tp_dev->baseline;
                dev_data.diff = diff_data;
                dev_data.status = (tp_dev->state == TOUCHPAD_STATE_PUSH || tp_dev->state == TOUCHPAD_STATE_PRESS) ? 1 : 0;
                tune_tool_set_device_data(&dev_data);
            }
            #endif
        }
    }

    #if CONFIG_TOUCH_PAD_USE_SLIDER
    if (NULL != slide_trigger_dev) {
        // if the pad is slide and raw data exceed noise th, it should update position.
        callback_exec(slide_trigger_dev, TOUCHPAD_CB_SLIDE);
    }
    #endif
}

static uint32_t touch_filter_iir(uint32_t in_now, uint32_t out_last, uint32_t k)
{
    if (k == 0) {
        return in_now;
    } else {
        uint32_t out_now = (in_now + (k - 1) * out_last) / k;
        return out_now;
    }
}

static void Thread(void *_context) {
    (void)_context;
    ESP_LOGI(TAG, "Thread started");

    touch_pad_intr_enable();
    while (1) {

        // Wait notification from interrupt:
        xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

        // Copy raw data:
        uint16_t raw[TOUCH_PAD_MAX];
        for (int i = 0; i < TOUCH_PAD_MAX; i++)
            raw[i] = tp_value[i];

        // Filter process:
        uint16_t filtered[TOUCH_PAD_MAX];
        static uint32_t filtered_temp[TOUCH_PAD_MAX] = {0};
        for (int i = 0; i < TOUCH_PAD_MAX; i++) {
            if (tp_group[i] != NULL) {
              filtered_temp[i] = filtered_temp[i] == 0 ? ((uint32_t)raw[i] << TOUCHPAD_SHIFT_DEFAULT) : filtered_temp[i];
              filtered_temp[i] = touch_filter_iir((raw[i] << TOUCHPAD_SHIFT_DEFAULT),
                                                     filtered_temp[i], TOUCHPAD_FILTER_FACTOR_DEFAULT);
              filtered[i] = (filtered_temp[i] + TOUCHPAD_SHIFT_ROUND_DEFAULT) >> TOUCHPAD_SHIFT_DEFAULT;
            }
        }

        // Callback:
        touch_pad_read_cb(raw, filtered);
    }
}

/* Creat a button element, init the element parameter */
tp_handle_t iot_tp_create(touch_pad_t touch_pad_num, float sensitivity)
{
    uint16_t tp_val;
    uint32_t avg = 0;
    uint8_t num = 0;
    if (g_init_flag == false) {

        // Touch Pad task create:
        BaseType_t res;
        res = xTaskCreatePinnedToCore(
                                Thread,
                                "TouchPad",
                                2048,
                                (void*)NULL,
                                2,
                                &task_handle, APP_CPU_NUM);
        IOT_CHECK(TAG, res == pdPASS, NULL);
        IOT_CHECK(TAG, task_handle != NULL, NULL);

        // global touch sensor hardware init
        s_tp_mux = xSemaphoreCreateMutex();
        IOT_CHECK(TAG, s_tp_mux != NULL, NULL);
        g_init_flag = true;
        touch_pad_init();
        touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);

        // Set FSM mode:
        uint32_t sleep_cycle = TOUCHPAD_SLEEP_CYCLE_MS * 150;   // TODO - use <rtc_clk_slow_freq_get_hz>
        if (sleep_cycle > 0xffff)
          sleep_cycle = 0xffff;
        uint32_t meas_cycle = TOUCHPAD_MEAS_CYCLE_US * 8;
        if (meas_cycle > 0xffff)
          meas_cycle = 0xffff;
        res = touch_pad_set_meas_time((uint16_t)sleep_cycle, (uint16_t)meas_cycle);
        res = touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
        ESP_ERROR_CHECK(res);

        // Register touch interrupt ISR:
        touch_pad_isr_register(tp_rtc_irq_handler, NULL);
    }
    IOT_CHECK(TAG, touch_pad_num < TOUCH_PAD_MAX, NULL);
    IOT_CHECK(TAG, sensitivity > 0, NULL);
    if (sensitivity < TOUCHPAD_TOUCH_LOW_SENSE_THRESHOLD) {
        ESP_LOGW(TAG, "The sensitivity (change rate of touch reading) is too low, \
                       please improve hardware design and improve touch performance.");
    }
    xSemaphoreTake(s_tp_mux, portMAX_DELAY);
    if (tp_group[touch_pad_num] != NULL) {
        ESP_LOGE(TAG, "touchpad create error! The pad has been used!");
        xSemaphoreGive(s_tp_mux);
        return NULL;
    }
    // Init the target touch pad:
    static bool f_thresh_set = false;
    uint16_t thresh = 0;
    if (f_thresh_set == false) {
        f_thresh_set = true;
        thresh = 0xffff;      // set max level for first pad - for one interrupt event
                              // for one measurement period of all touch pads
    }
    touch_pad_config(touch_pad_num, thresh);
    vTaskDelay(20 / portTICK_PERIOD_MS);    // Wait system into stable status.
    for (int i = 0; i < 3; i++) {
        touch_pad_read(touch_pad_num, &tp_val);
        avg += tp_val;
        ++num;
    }
    tp_val = avg / num; // Calculate the initial value.
    ESP_LOGD(TAG, "tp[%d] initial value: %d\n", touch_pad_num, tp_val);
    // Init the status variable for the touch pad.
    tp_dev_t *tp_dev = (tp_dev_t *) calloc(1, sizeof(tp_dev_t));
    tp_dev->touch_pad_num = touch_pad_num;
    #if CONFIG_TOUCH_PAD_USE_CB_SERIAL
    tp_dev->sum_ms = 0;
    tp_dev->serial_thres_sec = 0;
    tp_dev->serial_interval_ms = 0;
    #endif
    tp_dev->state = TOUCHPAD_STATE_IDLE;
    tp_dev->baseline = tp_val;
    tp_dev->touchChange = sensitivity;
    tp_dev->touch_thr = tp_dev->touchChange * TOUCHPAD_TOUCH_THRESHOLD_PERCENT;
    tp_dev->noise_thr = tp_dev->touch_thr * TOUCHPAD_NOISE_THRESHOLD_PERCENT;
    tp_dev->hysteresis_thr = tp_dev->touch_thr * TOUCHPAD_HYSTERESIS_THRESHOLD_PERCENT;
    tp_dev->baseline_reset_thr = tp_dev->touch_thr * TOUCHPAD_BASELINE_RESET_THRESHOLD_PERCENT;
    tp_dev->debounce_th = TOUCHPAD_STATE_SWITCH_DEBOUNCE / TOUCHPAD_MEAS_PERIOD_MS;
    tp_dev->bl_reset_count_th = TOUCHPAD_BASELINE_RESET_COUNT_THRESHOLD;
    tp_dev->bl_update_count_th = TOUCHPAD_BASELINE_UPDATE_COUNT_THRESHOLD / TOUCHPAD_MEAS_PERIOD_MS;
    ESP_LOGD(TAG, "Set max change rate of touch %.4f;\n\r\
                   Init data baseline %d;\n\r\
                   Touch threshold %.4f;\n\r\
                   Debounce threshold %d;\n\r\
                   Noise threshold %.4f;\n\r\
                   Hysteresis threshold %.4f;\n\r\
                   Baseline reset threshold %.4f;\n\r\
                   Baseline reset count threshold %d;\n\r", \
             tp_dev->touchChange, tp_dev->baseline, tp_dev->touch_thr, tp_dev->debounce_th, tp_dev->noise_thr, \
             tp_dev->hysteresis_thr, tp_dev->baseline_reset_thr, tp_dev->bl_reset_count_th);
    tp_group[touch_pad_num] = tp_dev;   // TouchPad device add to list.
    xSemaphoreGive(s_tp_mux);
#ifdef CONFIG_DATA_SCOPE_DEBUG
    tune_dev_info_t dev_info = {0};
    dev_info.dev_cid = TUNE_CID_ESP32;
    dev_info.dev_ver = TUNE_VERSION_V0;
    esp_base_mac_addr_get(dev_info.dev_mac);
    tune_tool_set_device_info(&dev_info);

    tune_dev_parameter_t dev_para = {0};
    dev_para.filter_period = TOUCHPAD_MEAS_PERIOD_MS;
    dev_para.debounce_ms = TOUCHPAD_STATE_SWITCH_DEBOUNCE;
    dev_para.base_reset_cnt = TOUCHPAD_BASELINE_RESET_COUNT_THRESHOLD;
    dev_para.base_update_cnt = TOUCHPAD_BASELINE_UPDATE_COUNT_THRESHOLD;
    dev_para.touch_th = TOUCHPAD_TOUCH_THRESHOLD_PERCENT * 100;
    dev_para.noise_th = TOUCHPAD_NOISE_THRESHOLD_PERCENT * 100;
    dev_para.hys_th = TOUCHPAD_HYSTERESIS_THRESHOLD_PERCENT * 100;
    dev_para.base_reset_th = TOUCHPAD_BASELINE_RESET_THRESHOLD_PERCENT * 100;
    dev_para.base_slider_th = TOUCHPAD_SLIDER_TRIGGER_THRESHOLD_PERCENT * 100;
    tune_tool_set_device_parameter(&dev_para);
#endif
    return (tp_handle_t) tp_dev;
}

/* Clean a button element and setting. */
esp_err_t iot_tp_delete(tp_handle_t tp_handle)
{
    POINT_ASSERT(TAG, tp_handle);
    tp_dev_t *tp_dev = (tp_dev_t *) tp_handle;
    tp_group[tp_dev->touch_pad_num] = NULL;
    for (int i = 0; i < TOUCHPAD_CB_MAX; i++) {
        if (tp_dev->cb_group[i] != NULL) {
            free(tp_dev->cb_group[i]);
            tp_dev->cb_group[i] = NULL;
        }
    }

    #if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
    tp_custom_cb_t *custom_cb = tp_dev->custom_cbs;
    while (custom_cb != NULL) {
        tp_custom_cb_t *cb_next = custom_cb->next_cb;
        delete_timer(custom_cb->tmr);
        custom_cb->tmr = NULL;
        free(custom_cb);
        custom_cb = cb_next;
    }
    tp_dev->custom_cbs = NULL;
    #endif

    #if CONFIG_TOUCH_PAD_USE_CB_SERIAL
    if (tp_dev->serial_tmr != NULL) {
        delete_timer(tp_dev->serial_tmr);
        tp_dev->serial_tmr = NULL;
    }
    #endif

    free(tp_handle);
    return ESP_OK;
}

/* Add callback API at each button action. */
esp_err_t iot_tp_add_cb(tp_handle_t tp_handle, tp_cb_type_t cb_type, tp_cb cb, void *arg)
{
    POINT_ASSERT(TAG, tp_handle);
    POINT_ASSERT(TAG, cb);
    IOT_CHECK(TAG, cb_type < TOUCHPAD_CB_MAX, ESP_FAIL);
    tp_dev_t *tp_dev = (tp_dev_t *) tp_handle;
    if (tp_dev->cb_group[cb_type] != NULL) {
        ESP_LOGW(TAG, "This type of touchpad callback function has already been added!");
        return ESP_FAIL;
    }
    tp_cb_t *cb_info = (tp_cb_t *) calloc(1, sizeof(tp_cb_t));
    POINT_ASSERT(TAG, cb_info);
    cb_info->cb = cb;
    cb_info->arg = arg;
    tp_dev->cb_group[cb_type] = cb_info;
    return ESP_OK;
}

#if CONFIG_TOUCH_PAD_USE_CB_SERIAL

/* Add callback API for long press action. */
esp_err_t iot_tp_set_serial_trigger(tp_handle_t tp_handle, uint32_t trigger_thres_sec, uint32_t interval_ms, tp_cb cb, void *arg)
{
    POINT_ASSERT(TAG, tp_handle);
    POINT_ASSERT(TAG, cb);
    IOT_CHECK(TAG, trigger_thres_sec != 0, ESP_FAIL);
    IOT_CHECK(TAG, interval_ms > portTICK_RATE_MS, ESP_FAIL);   // TODO
    tp_dev_t *tp_dev = (tp_dev_t *) tp_handle;
    if (tp_dev->serial_tmr == NULL) {
        #if USE_ESP_TIMER == 1
        tp_dev->interval_ms = interval_ms;
        esp_timer_create_args_t timer_args;
        timer_args.arg = tp_dev;
        timer_args.callback = tp_serial_timer_cb;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "serial_tmr";
        esp_timer_create(&timer_args, &tp_dev->serial_tmr);
        #else
        tp_dev->serial_tmr = xTimerCreate("serial_tmr", interval_ms / portTICK_RATE_MS, pdTRUE, tp_dev, tp_serial_timer_cb);
        POINT_ASSERT(TAG, tp_dev->serial_tmr);
        #endif
    } else {
        #if USE_ESP_TIMER
        #else
        xTimerChangePeriod(tp_dev->serial_tmr, interval_ms / portTICK_RATE_MS, portMAX_DELAY);
        #endif
    }
    tp_dev->serial_thres_sec = trigger_thres_sec;
    tp_dev->serial_interval_ms = interval_ms;
    tp_dev->serial_cb.cb = cb;
    tp_dev->serial_cb.arg = arg;
    return ESP_OK;
}
#endif

#if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
esp_err_t iot_tp_add_custom_cb(tp_handle_t tp_handle, uint32_t press_sec, tp_cb cb, void  *arg)
{
    POINT_ASSERT(TAG, tp_handle);
    POINT_ASSERT(TAG, cb);
    IOT_CHECK(TAG, press_sec != 0, ESP_FAIL);
    tp_dev_t *tp_dev = (tp_dev_t *) tp_handle;
    tp_custom_cb_t *cb_new = (tp_custom_cb_t *) calloc(1, sizeof(tp_custom_cb_t));
    POINT_ASSERT(TAG, cb_new);
    cb_new->cb = cb;
    cb_new->arg = arg;
    cb_new->tp_dev = tp_dev;
    #if USE_ESP_TIMER == 1
    cb_new->interval_ms = press_sec * 1000;
    esp_timer_create_args_t timer_args;
    timer_args.arg = cb_new;
    timer_args.callback = tp_custom_timer_cb;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "custom_cb_tmr";
    esp_timer_create(&timer_args, &cb_new->tmr);
    #else
    cb_new->tmr = xTimerCreate("custom_cb_tmr", press_sec * 1000 / portTICK_RATE_MS, pdFALSE, cb_new, tp_custom_timer_cb);
    #endif
    if (cb_new->tmr == NULL) {
        ESP_LOGE(TAG, "timer create fail! %s:%d (%s)", __FILE__, __LINE__, __FUNCTION__);
        free(cb_new);
        return ESP_FAIL;
    }
    cb_new->next_cb = tp_dev->custom_cbs;
    tp_dev->custom_cbs = cb_new;
    return ESP_OK;
}
#endif

touch_pad_t iot_tp_num_get(const tp_handle_t tp_handle)
{
    tp_dev_t *tp_dev = (tp_dev_t *) tp_handle;
    return tp_dev->touch_pad_num;
}

esp_err_t iot_tp_set_threshold(const tp_handle_t tp_handle, float threshold)
{
    POINT_ASSERT(TAG, tp_handle);
    tp_dev_t *tp_dev = (tp_dev_t *) tp_handle;
    ERR_ASSERT(TAG, touch_pad_config(tp_dev->touch_pad_num, threshold));
    // updata all the threshold and other related value.
    tp_dev->touch_thr = threshold;
    tp_dev->noise_thr = tp_dev->touch_thr * TOUCHPAD_NOISE_THRESHOLD_PERCENT;
    tp_dev->hysteresis_thr = tp_dev->touch_thr * TOUCHPAD_HYSTERESIS_THRESHOLD_PERCENT;
    tp_dev->baseline_reset_thr = tp_dev->touch_thr * TOUCHPAD_BASELINE_RESET_THRESHOLD_PERCENT;
    tp_dev->slide_trigger_thr = tp_dev->touch_thr * TOUCHPAD_SLIDER_TRIGGER_THRESHOLD_PERCENT;
    return ESP_OK;
}

esp_err_t iot_tp_get_threshold(const tp_handle_t tp_handle, float *threshold)
{
    POINT_ASSERT(TAG, tp_handle);
    tp_dev_t *tp_dev = (tp_dev_t *) tp_handle;
    *threshold = tp_dev->touch_thr;
    return ESP_OK;
}

esp_err_t iot_tp_read(const tp_handle_t tp_handle, uint16_t *touch_value_ptr)
{
    POINT_ASSERT(TAG, tp_handle);
    tp_dev_t *tp_dev = (tp_dev_t *) tp_handle;
    return touch_pad_read_filtered(tp_dev->touch_pad_num, touch_value_ptr);
}

esp_err_t tp_read_raw(const tp_handle_t tp_handle, uint16_t *touch_value_ptr)
{
    POINT_ASSERT(TAG, tp_handle);
    tp_dev_t *tp_dev = (tp_dev_t *) tp_handle;
    return touch_pad_read_raw_data(tp_dev->touch_pad_num, touch_value_ptr);
}

#if CONFIG_TOUCH_PAD_USE_SLIDER

tp_slide_handle_t iot_tp_slide_create(uint8_t num, const touch_pad_t *tps, uint8_t pos_range,
                                      const float *p_sensitivity)
{
    IOT_CHECK(TAG, tps != NULL, NULL);
    IOT_CHECK(TAG, pos_range >= num, NULL);

    tp_slide_t *tp_slide = (tp_slide_t *) calloc(1, sizeof(tp_slide_t));
    IOT_CHECK(TAG, tp_slide != NULL, NULL);
    IOT_CHECK(TAG, p_sensitivity != NULL, NULL);
    tp_slide->tp_num = num;
    // pos_range: the position range of each pad, Must be a multiple of (num-1).
    tp_slide->pos_scale = (float) pos_range / (num - 1);
    tp_slide->pos_range = (float) pos_range;
    tp_slide->slide_pos = SLIDE_POS_INF;
    tp_slide->calc_val = (float *) calloc(tp_slide->tp_num, sizeof(float));
    tp_slide->tp_handles = (tp_handle_t *) calloc(num, sizeof(tp_handle_t));
    if (tp_slide->tp_handles == NULL) {
        ESP_LOGE(TAG, "touchpad slide calloc error!");
        free(tp_slide);
        return NULL;
    }
    for (int i = 0; i < num; i++) {
        if (tp_group[tps[i]] != NULL) {
            tp_slide->tp_handles[i] = tp_group[tps[i]];
        } else {
            if (p_sensitivity) {
                //p_thresh_abs should not be zero.
                tp_slide->tp_handles[i] = iot_tp_create(tps[i], p_sensitivity[i]);
            } else {
                tp_slide->tp_handles[i] = iot_tp_create(tps[i], 0);
            }
            // in each event callback function, it will calculate the related position
        }
    }
    float temp = 0;
    for (int i = 0; i < num; i++) {
        tp_dev_t *tp_dev = tp_slide->tp_handles[i];
        tp_dev->button_type = TOUCHPAD_LINEAR_SLIDER;
        tp_dev->slide_trigger_thr = tp_dev->touch_thr * TOUCHPAD_SLIDER_TRIGGER_THRESHOLD_PERCENT;
        ESP_LOGD(TAG, "Set touch [%d] slide trigger threshold is %.4f", tp_dev->touch_pad_num,
                 tp_dev->slide_trigger_thr);
        iot_tp_add_cb(tp_slide->tp_handles[i], TOUCHPAD_CB_SLIDE, tp_slide_pos_cb, tp_slide);
        temp += tp_dev->slide_trigger_thr;
    }
    return (tp_slide_handle_t *) tp_slide;
}

esp_err_t iot_tp_slide_delete(tp_slide_handle_t tp_slide_handle)
{
    POINT_ASSERT(TAG, tp_slide_handle);
    tp_slide_t *tp_slide = (tp_slide_t *) tp_slide_handle;
    for (int i = 0; i < tp_slide->tp_num; i++) {
        if (tp_slide->tp_handles[i]) {
            iot_tp_delete(tp_slide->tp_handles[i]);
            for (int j = i + 1; j < tp_slide->tp_num; j++) {
                if (tp_slide->tp_handles[i] == tp_slide->tp_handles[j]) {
                    tp_slide->tp_handles[j] = NULL;
                }
            }
            tp_slide->tp_handles[i] = NULL;
        }
    }
    free(tp_slide->tp_handles);
    free(tp_slide->calc_val);
    free(tp_slide);
    return ESP_OK;
}

uint8_t iot_tp_slide_position(tp_slide_handle_t tp_slide_handle)
{
    IOT_CHECK(TAG, tp_slide_handle != NULL, SLIDE_POS_INF);
    tp_slide_t *tp_slide = (tp_slide_t *) tp_slide_handle;
    return (uint8_t) tp_slide->slide_pos;
}
#endif /* CONFIG_TOUCH_PAD_USE_SLIDER */

#if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
// reset all the cumstom timers of matrix object
static inline void matrix_reset_cb_tmrs(tp_matrix_t *tp_matrix)
{
    tp_matrix_cus_cb_t *custom_cb = tp_matrix->custom_cbs;
    while (custom_cb != NULL) {
        if (custom_cb->tmr != NULL) {
            #if USE_ESP_TIMER == 1
            esp_err_t res;
            res = esp_timer_stop(custom_cb->tmr);
            if (res != ESP_ERR_INVALID_STATE)
                ESP_ERROR_CHECK(res);
            res = esp_timer_start_once(custom_cb->tmr, custom_cb->interval_ms * 1000);
            ESP_ERROR_CHECK(res);
            #else
            xTimerReset(custom_cb->tmr, portMAX_DELAY);
            #endif
        }
        custom_cb = custom_cb->next_cb;
    }
}

// stop all the cumstom timers of matrix object
static inline void matrix_stop_cb_tmrs(tp_matrix_t *tp_matrix)
{
    tp_matrix_cus_cb_t *custom_cb = tp_matrix->custom_cbs;
    while (custom_cb != NULL) {
        if (custom_cb->tmr != NULL) {
            stop_timer(custom_cb->tmr);
        }
        custom_cb = custom_cb->next_cb;
    }
}
#endif

static void tp_matrix_push_cb(void *arg)
{
    tp_matrix_arg_t *matrix_arg = (tp_matrix_arg_t *) arg;
    tp_matrix_t *tp_matrix = matrix_arg->tp_matrix;
    int x_idx, y_idx;
    tp_dev_t *tp_dev;
    int idx = -1;
    if (tp_matrix->active_state != TOUCHPAD_STATE_IDLE) {
        return;
    }
    if (matrix_arg->type == TOUCHPAD_MATRIX_ROW) {  // this is the 'x' index of pad.
        x_idx = matrix_arg->tp_idx;
        for (int j = 0; j < tp_matrix->y_num; j++) {
            tp_dev = (tp_dev_t *) tp_matrix->y_tps[j];
            ESP_LOGD(TAG, "y[%d] tp[%d] thresh: %02f; diff data: %02f; state: %d", j,
                     tp_dev->touch_pad_num, tp_dev->touch_thr, tp_dev->diff_rate, tp_dev->state);
            if (tp_dev->state == TOUCHPAD_STATE_PUSH) {
                if (idx < 0) {
                    // this is the 'y' index
                    idx = x_idx * tp_matrix->y_num + j;
                    ESP_LOGD(TAG, "matrix idx: %d", idx);
                } else {
                    // to make sure only one sensor is touched.
                    return;
                }
            }
        }
    } else if (matrix_arg->type == TOUCHPAD_MATRIX_COLUMN) { // this is the 'y' index of pad.
        y_idx = matrix_arg->tp_idx;
        for (int j = 0; j < tp_matrix->x_num; j++) {
            tp_dev = (tp_dev_t *) tp_matrix->x_tps[j];
            ESP_LOGD(TAG, "x[%d] tp[%d] thresh: %02f; diff data: %02f; state: %d", j,
                     tp_dev->touch_pad_num, tp_dev->touch_thr, tp_dev->diff_rate, tp_dev->state);
            if (tp_dev->state == TOUCHPAD_STATE_PUSH) {
                if (idx < 0) {
                    // this is the 'x' index
                    idx = j * tp_matrix->y_num + y_idx;
                    ESP_LOGD(TAG, "matrix idx: %d", idx);
                } else {
                    // to make sure only one sensor is touched.
                    return;
                }
            }
        }
    } else {
        ESP_LOGE(TAG, "matrix cb err");
    }

    // find only one active pad
    if (idx >= 0) {
        tp_matrix->active_state = TOUCHPAD_STATE_PUSH;
        tp_matrix->active_idx = idx;
        if (tp_matrix->cb_group[TOUCHPAD_CB_PUSH] != NULL) {
            tp_matrix_cb_t *cb_info = tp_matrix->cb_group[TOUCHPAD_CB_PUSH];
            cb_info->cb(cb_info->arg, idx / tp_matrix->y_num, idx % tp_matrix->y_num);
        }
        #if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
        matrix_reset_cb_tmrs(tp_matrix);
        #endif
        #if CONFIG_TOUCH_PAD_USE_CB_SERIAL
        if (tp_matrix->serial_tmr != NULL) {
            #if USE_ESP_TIMER == 1
            #else
            xTimerChangePeriod(tp_matrix->serial_tmr, tp_matrix->serial_thres_sec * 1000 / portTICK_RATE_MS, portMAX_DELAY);
            #endif
        }
        #endif
    }
}

static void tp_matrix_release_cb(void *arg)
{
    tp_matrix_arg_t *matrix_arg = (tp_matrix_arg_t *) arg;
    tp_matrix_t *tp_matrix = matrix_arg->tp_matrix;

    // Check release action only with x index.
    if (matrix_arg->type != TOUCHPAD_MATRIX_ROW \
            && matrix_arg->tp_idx != tp_matrix->active_idx / tp_matrix->y_num) {
        return;
    }

    if (tp_matrix->active_state != TOUCHPAD_STATE_IDLE) {
        tp_matrix->active_state = TOUCHPAD_STATE_IDLE;
        uint8_t idx = tp_matrix->active_idx;
        if (tp_matrix->cb_group[TOUCHPAD_CB_RELEASE] != NULL) {
            tp_matrix_cb_t *cb_info = tp_matrix->cb_group[TOUCHPAD_CB_RELEASE];
            cb_info->cb(cb_info->arg, idx / tp_matrix->y_num, idx % tp_matrix->y_num);
        }
        #if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
        matrix_stop_cb_tmrs(tp_matrix);
        #endif
        #if CONFIG_TOUCH_PAD_USE_CB_SERIAL
        if (tp_matrix->serial_tmr != NULL) {
            stop_timer(tp_matrix->serial_tmr);
        }
        #endif
    }
}

#if CONFIG_TOUCH_PAD_USE_CB_TAP
static void tp_matrix_tap_cb(void *arg)
{
    tp_matrix_arg_t *matrix_arg = (tp_matrix_arg_t *) arg;
    tp_matrix_t *tp_matrix = matrix_arg->tp_matrix;
    // make sure this is the same active sensor
    if (matrix_arg->type != TOUCHPAD_MATRIX_ROW \
            && matrix_arg->tp_idx != tp_matrix->active_idx / tp_matrix->y_num) {
        return;
    }
    if (tp_matrix->active_state == TOUCHPAD_STATE_PUSH) {
        uint8_t idx = tp_matrix->active_idx;
        if (tp_matrix->cb_group[TOUCHPAD_CB_TAP] != NULL) {
            tp_matrix_cb_t *cb_info = tp_matrix->cb_group[TOUCHPAD_CB_TAP];
            cb_info->cb(cb_info->arg, idx / tp_matrix->y_num, idx % tp_matrix->y_num);
        }
    }
}
#endif

static void tp_matrix_cus_tmr_cb(void *tmr)
{
    #if USE_ESP_TIMER == 1
    tp_matrix_cus_cb_t *tp_matrix_cb = (tp_matrix_cus_cb_t*)tmr;
    #else
    tp_matrix_cus_cb_t *tp_matrix_cb = (tp_matrix_cus_cb_t*)pvTimerGetTimerID(tmr);
    #endif

    tp_matrix_t *tp_matrix = tp_matrix_cb->tp_matrix;
    if (tp_matrix->active_state != TOUCHPAD_STATE_IDLE) {
        tp_matrix->active_state = TOUCHPAD_STATE_PRESS;
        uint8_t idx = tp_matrix->active_idx;
        tp_matrix_cb->cb(tp_matrix_cb->arg, idx / tp_matrix->y_num, idx % tp_matrix->y_num);
    }
}

#if CONFIG_TOUCH_PAD_USE_CB_SERIAL
static void tp_matrix_serial_trigger_cb(void *tmr)
{
    #if USE_ESP_TIMER == 1
    tp_matrix_t *tp_matrix = (tp_matrix_t *)tmr;
    #else
    tp_matrix_t *tp_matrix = (tp_matrix_t *)pvTimerGetTimerID(tmr);
    #endif

    if (tp_matrix->active_state != TOUCHPAD_STATE_IDLE) {
        tp_matrix->active_state = TOUCHPAD_STATE_PRESS;
        uint8_t idx = tp_matrix->active_idx;
        tp_matrix->serial_cb.cb(tp_matrix->serial_cb.arg, idx / tp_matrix->y_num, idx % tp_matrix->y_num);

        #if USE_ESP_TIMER == 1
        #else
        xTimerChangePeriod(tp_matrix->serial_tmr, tp_matrix->serial_interval_ms / portTICK_RATE_MS, portMAX_DELAY);
        #endif
    }
}
#endif

tp_matrix_handle_t iot_tp_matrix_create(uint8_t x_num, uint8_t y_num, const touch_pad_t *x_tps, \
                                        const touch_pad_t *y_tps, const float *p_sensitivity)
{
    IOT_CHECK(TAG, x_num != 0 && x_num < TOUCH_PAD_MAX, NULL);
    IOT_CHECK(TAG, y_num != 0 && y_num < TOUCH_PAD_MAX, NULL);
    tp_matrix_t *tp_matrix = (tp_matrix_t *) calloc(1, sizeof(tp_matrix_t));
    IOT_CHECK(TAG, tp_matrix != NULL, NULL);
    IOT_CHECK(TAG, p_sensitivity != NULL, NULL);
    tp_matrix->x_tps = (tp_handle_t *) calloc(x_num, sizeof(tp_handle_t));
    if (tp_matrix->x_tps == NULL) {
        ESP_LOGE(TAG, "create touchpad matrix error! no available memory!");
        free(tp_matrix);
        return NULL;
    }
    tp_matrix->y_tps = (tp_handle_t *) calloc(y_num, sizeof(tp_handle_t));
    if (tp_matrix->y_tps == NULL) {
        ESP_LOGE(TAG, "create touchpad matrix error! no available memory!");
        free(tp_matrix->x_tps);
        free(tp_matrix);
        return NULL;
    }
    tp_matrix->matrix_args = (tp_matrix_arg_t *) calloc(x_num + y_num, sizeof(tp_matrix_arg_t));
    if (tp_matrix->matrix_args == NULL) {
        ESP_LOGE(TAG, "create touchpad matrix error! no available memory!");
        free(tp_matrix->x_tps);
        free(tp_matrix->y_tps);
        free(tp_matrix);
        return NULL;
    }
    tp_matrix->x_num = x_num;
    tp_matrix->y_num = y_num;
    for (int i = 0; i < x_num; i++) {
        if (p_sensitivity) {
            tp_matrix->x_tps[i] = iot_tp_create(x_tps[i], p_sensitivity[i]);
        } else {
            tp_matrix->x_tps[i] = iot_tp_create(x_tps[i], 0);
        }
        if (tp_matrix->x_tps[i] == NULL) {
            goto CREATE_ERR;
        }
        tp_matrix->matrix_args[i].tp_matrix = tp_matrix;
        tp_matrix->matrix_args[i].tp_idx = i;
        tp_matrix->matrix_args[i].type = TOUCHPAD_MATRIX_ROW;
        iot_tp_add_cb(tp_matrix->x_tps[i], TOUCHPAD_CB_PUSH, tp_matrix_push_cb, tp_matrix->matrix_args + i);
        iot_tp_add_cb(tp_matrix->x_tps[i], TOUCHPAD_CB_RELEASE, tp_matrix_release_cb, tp_matrix->matrix_args + i);
        #if CONFIG_TOUCH_PAD_USE_CB_TAP
        iot_tp_add_cb(tp_matrix->x_tps[i], TOUCHPAD_CB_TAP, tp_matrix_tap_cb, tp_matrix->matrix_args + i);
        #endif
    }
    for (int i = 0; i < y_num; i++) {
        if (p_sensitivity) {
            tp_matrix->y_tps[i] = iot_tp_create(y_tps[i], p_sensitivity[x_num + i]);
        } else {
            tp_matrix->y_tps[i] = iot_tp_create(y_tps[i], 0);
        }
        if (tp_matrix->y_tps[i] == NULL) {
            goto CREATE_ERR;
        }
        tp_matrix->matrix_args[i + x_num].tp_matrix = tp_matrix;
        tp_matrix->matrix_args[i + x_num].tp_idx = i;
        tp_matrix->matrix_args[i + x_num].type = TOUCHPAD_MATRIX_COLUMN;
        iot_tp_add_cb(tp_matrix->y_tps[i], TOUCHPAD_CB_PUSH, tp_matrix_push_cb, &tp_matrix->matrix_args[i + x_num]);
        iot_tp_add_cb(tp_matrix->y_tps[i], TOUCHPAD_CB_RELEASE, tp_matrix_release_cb, &tp_matrix->matrix_args[i + x_num]);
        #if CONFIG_TOUCH_PAD_USE_CB_TAP
        iot_tp_add_cb(tp_matrix->y_tps[i], TOUCHPAD_CB_TAP, tp_matrix_tap_cb, &tp_matrix->matrix_args[i + x_num]);
        #endif
    }
    tp_matrix->active_state = TOUCHPAD_STATE_IDLE;
    return (tp_matrix_handle_t)tp_matrix;

CREATE_ERR:
    ESP_LOGE(TAG, "touchpad matrix create error!");
    for (int i = 0; i < x_num; i++) {
        if (tp_matrix->x_tps[i] != NULL) {
            iot_tp_delete(tp_matrix->x_tps[i]);
            tp_matrix->x_tps[i] = NULL;
        }
    }
    for (int i = 0; i < y_num; i++) {
        if (tp_matrix->y_tps[i] != NULL) {
            iot_tp_delete(tp_matrix->y_tps[i]);
            tp_matrix->y_tps[i] = NULL;
        }
    }
    free(tp_matrix->x_tps);
    free(tp_matrix->y_tps);
    free(tp_matrix->matrix_args);
    free(tp_matrix);
    tp_matrix = NULL;
    return NULL;
}

esp_err_t iot_tp_matrix_delete(tp_matrix_handle_t tp_matrix_hd)
{
    POINT_ASSERT(TAG, tp_matrix_hd);
    tp_matrix_t *tp_matrix = (tp_matrix_t *) tp_matrix_hd;
    for (int i = 0; i < tp_matrix->x_num; i++) {
        if (tp_matrix->x_tps[i] != NULL) {
            iot_tp_delete(tp_matrix->x_tps[i]);
            tp_matrix->x_tps[i] = NULL;
        }
    }
    for (int i = 0; i < tp_matrix->y_num; i++) {
        if (tp_matrix->y_tps[i] != NULL) {
            iot_tp_delete(tp_matrix->y_tps[i]);
            tp_matrix->y_tps[i] = NULL;
        }
    }
    for (int i = 0; i < TOUCHPAD_CB_MAX; i++) {
        if (tp_matrix->cb_group[i] != NULL) {
            free(tp_matrix->cb_group[i]);
            tp_matrix->cb_group[i] = NULL;
        }
    }

    #if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
    tp_matrix_cus_cb_t *custom_cb = tp_matrix->custom_cbs;
    while (custom_cb != NULL) {
        tp_matrix_cus_cb_t *cb_next = custom_cb->next_cb;
        delete_timer(custom_cb->tmr);
        custom_cb->tmr = NULL;
        free(custom_cb);
        custom_cb = cb_next;
    }
    tp_matrix->custom_cbs = NULL;
    #endif

    #if CONFIG_TOUCH_PAD_USE_CB_SERIAL
    if (tp_matrix->serial_tmr != NULL) {
        delete_timer(tp_matrix->serial_tmr);
        tp_matrix->serial_tmr = NULL;
    }
    #endif

    free(tp_matrix->x_tps);
    free(tp_matrix->y_tps);
    free(tp_matrix->matrix_args);
    free(tp_matrix);
    return ESP_OK;
}

esp_err_t iot_tp_matrix_add_cb(tp_matrix_handle_t tp_matrix_hd, tp_cb_type_t cb_type, tp_matrix_cb cb, void *arg)
{
    POINT_ASSERT(TAG, tp_matrix_hd);
    POINT_ASSERT(TAG, cb);
    IOT_CHECK(TAG, cb_type < TOUCHPAD_CB_MAX, ESP_FAIL);
    tp_matrix_t *tp_matrix = (tp_matrix_t *) tp_matrix_hd;
    if (tp_matrix->cb_group[cb_type] != NULL) {
        ESP_LOGW(TAG, "This type of touchpad callback function has already been added!");
        return ESP_FAIL;
    }
    tp_matrix_cb_t *cb_info = (tp_matrix_cb_t *) calloc(1, sizeof(tp_matrix_cb_t));
    POINT_ASSERT(TAG, cb_info);
    cb_info->cb = cb;
    cb_info->arg = arg;
    tp_matrix->cb_group[cb_type] = cb_info;
    return ESP_OK;
}

#if CONFIG_TOUCH_PAD_USE_CB_CUSTOM
esp_err_t iot_tp_matrix_add_custom_cb(tp_matrix_handle_t tp_matrix_hd, uint32_t press_sec, tp_matrix_cb cb, void *arg)
{
    POINT_ASSERT(TAG, tp_matrix_hd);
    POINT_ASSERT(TAG, cb);
    IOT_CHECK(TAG, press_sec != 0, ESP_FAIL);
    tp_matrix_t *tp_matrix = (tp_matrix_t *) tp_matrix_hd;
    tp_matrix_cus_cb_t *cb_new = (tp_matrix_cus_cb_t *) calloc(1, sizeof(tp_matrix_cus_cb_t));
    POINT_ASSERT(TAG, cb_new);

    #if USE_ESP_TIMER == 1
    cb_new->interval_ms = press_sec * 1000;
    esp_timer_create_args_t timer_args;
    timer_args.arg = cb_new;
    timer_args.callback = tp_matrix_cus_tmr_cb;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "custom_cb_tmr";
    esp_timer_create(&timer_args, &cb_new->tmr);
    #else
    cb_new->tmr = xTimerCreate("custom_tmr", press_sec * 1000 / portTICK_RATE_MS, pdFALSE, cb_new, tp_matrix_cus_tmr_cb);
    #endif

    if (cb_new->tmr == NULL) {
        ESP_LOGE(TAG, "timer create fail! %s:%d (%s)", __FILE__, __LINE__, __FUNCTION__);
        free(cb_new);
        return ESP_FAIL;
    }
    cb_new->cb = cb;
    cb_new->arg = arg;
    cb_new->tp_matrix = tp_matrix;
    cb_new->next_cb = tp_matrix->custom_cbs;
    tp_matrix->custom_cbs = cb_new;
    return ESP_OK;
}
#endif

#if CONFIG_TOUCH_PAD_USE_CB_SERIAL
esp_err_t iot_tp_matrix_set_serial_trigger(tp_matrix_handle_t tp_matrix_hd, uint32_t trigger_thres_sec, uint32_t interval_ms, tp_matrix_cb cb, void *arg)
{
    POINT_ASSERT(TAG, tp_matrix_hd);
    POINT_ASSERT(TAG, cb);
    IOT_CHECK(TAG, trigger_thres_sec != 0, ESP_FAIL);
    IOT_CHECK(TAG, interval_ms >= portTICK_PERIOD_MS, ESP_FAIL);
    tp_matrix_t *tp_matrix = (tp_matrix_t *) tp_matrix_hd;
    tp_matrix->serial_cb.cb = cb;
    tp_matrix->serial_cb.arg = arg;
    tp_matrix->serial_thres_sec = trigger_thres_sec;
    tp_matrix->serial_interval_ms = interval_ms;
    #if USE_ESP_TIMER == 1
    esp_timer_create_args_t timer_args;
    timer_args.arg = tp_matrix;
    timer_args.callback = tp_matrix_serial_trigger_cb;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "serial_tmr";
    esp_timer_create(&timer_args, &tp_matrix->serial_tmr);
    #else
    tp_matrix->serial_tmr = xTimerCreate("serial_tmr", trigger_thres_sec * 1000 / portTICK_RATE_MS, pdFALSE, tp_matrix, tp_matrix_serial_trigger_cb);
    #endif
    POINT_ASSERT(TAG, tp_matrix->serial_tmr);
    return ESP_OK;
}
#endif
