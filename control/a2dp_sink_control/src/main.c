/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Adapted from ESP-IDF v6.1 a2dp_sink_stream and its bredr_app_common_utils
 * and a2dp_sink_int_codec_utils helpers. See ../README.md for all deviations.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(6, 1, 0)
#error "This control baseline requires ESP-IDF 6.1.0"
#endif
#if !CONFIG_IDF_TARGET_ESP32 || CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
#error "This control baseline requires classic ESP32 and the internal SBC decoder"
#endif

#define STATS_PERIOD_US INT64_C(5000000)
#define DEVICE_NAME "Faital A2DP Control"

static const char *TAG = "CONTROL";

/* IDF 6.1 exposes only SUSPEND and STARTED; STOPPED is local/disconnected. */
typedef enum {
    AUDIO_STOPPED,
    AUDIO_SUSPENDED,
    AUDIO_STARTED,
} audio_state_t;

typedef struct {
    int64_t max_us;
    uint32_t gt_30ms;
    uint32_t gt_50ms;
    uint32_t gt_100ms;
    uint32_t gt_200ms;
} gap_stats_t;

typedef struct {
    uint64_t callbacks;
    uint64_t pcm_total;
    uint32_t last_size;
    int64_t first_pcm_us;
    int64_t last_pcm_us;
    int64_t segment_start_us;
    int64_t window_start_us;
    uint64_t base_callbacks;
    uint64_t base_pcm;
    gap_stats_t gaps;
    uint32_t sample_rate;
    uint8_t channel_mode;
    audio_state_t audio_state;
} control_stats_t;

/* ESP32 is 32-bit: a shared uint64_t needs synchronization, not just volatile.
 * Every holder does only bounded scalar updates or a small metadata snapshot.
 * No logging, division, PCM access, allocation or scheduler calls under lock.
 */
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static control_stats_t s_stats;

static const char *audio_state_name(audio_state_t state)
{
    switch (state) {
    case AUDIO_STARTED: return "started";
    case AUDIO_SUSPENDED: return "suspended";
    default: return "stopped";
    }
}

static const char *connection_state_name(esp_a2d_connection_state_t state)
{
    switch (state) {
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED: return "disconnected";
    case ESP_A2D_CONNECTION_STATE_CONNECTING: return "connecting";
    case ESP_A2D_CONNECTION_STATE_CONNECTED: return "connected";
    case ESP_A2D_CONNECTION_STATE_DISCONNECTING: return "disconnecting";
    default: return "unknown";
    }
}

static uint32_t sbc_sample_rate(uint8_t frequency)
{
    /* A negotiated configuration must contain one frequency, not a mask of many. */
    switch (frequency) {
    case ESP_A2D_SBC_CIE_SF_16K: return 16000;
    case ESP_A2D_SBC_CIE_SF_32K: return 32000;
    case ESP_A2D_SBC_CIE_SF_44K: return 44100;
    case ESP_A2D_SBC_CIE_SF_48K: return 48000;
    default: return 0;
    }
}

static const char *sbc_channel_mode(uint8_t mode)
{
    switch (mode) {
    case ESP_A2D_SBC_CIE_CH_MODE_MONO: return "mono";
    case ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL: return "dual_channel";
    case ESP_A2D_SBC_CIE_CH_MODE_STEREO: return "stereo";
    case ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO: return "joint_stereo";
    default: return "invalid";
    }
}

/* Caller holds s_stats_lock. Exclude pause/disconnect/reconfiguration time. */
static void reset_measurement_locked(int64_t now_us)
{
    s_stats.segment_start_us = now_us;
    s_stats.window_start_us = now_us;
    s_stats.base_callbacks = s_stats.callbacks;
    s_stats.base_pcm = s_stats.pcm_total;
    s_stats.last_pcm_us = 0;
    s_stats.gaps = (gap_stats_t){0};
}

static void set_audio_state(audio_state_t state)
{
    portENTER_CRITICAL(&s_stats_lock);
    if (s_stats.audio_state != state) {
        s_stats.audio_state = state;
        reset_measurement_locked(esp_timer_get_time());
    }
    if (state == AUDIO_STOPPED) {
        s_stats.sample_rate = 0;
        s_stats.channel_mode = 0;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

/* Drain-only: data is owned by Bluedroid; never read, copy, retain or free it. */
static void pcm_data_callback(const uint8_t *data, uint32_t len)
{
    (void)data;
    portENTER_CRITICAL(&s_stats_lock);
    const int64_t now_us = esp_timer_get_time();
    if (s_stats.callbacks == 0) {
        s_stats.first_pcm_us = now_us;
    }
    ++s_stats.callbacks;
    s_stats.pcm_total += len;
    s_stats.last_size = len;
    if (s_stats.audio_state == AUDIO_STARTED && s_stats.last_pcm_us != 0) {
        const int64_t gap_us = now_us - s_stats.last_pcm_us;
        if (gap_us > s_stats.gaps.max_us) {
            s_stats.gaps.max_us = gap_us;
        }
        s_stats.gaps.gt_30ms += gap_us > 30000;
        s_stats.gaps.gt_50ms += gap_us > 50000;
        s_stats.gaps.gt_100ms += gap_us > 100000;
        s_stats.gaps.gt_200ms += gap_us > 200000;
    }
    s_stats.last_pcm_us = now_us;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stats_task(void *arg)
{
    (void)arg;
    bool first_pcm_logged = false;
    for (;;) {
        /* One low-priority task. Poll only to defer the first-PCM log and to
         * notice a new measurement window; print stats no more often than 5 s.
         */
        vTaskDelay(pdMS_TO_TICKS(100));
        control_stats_t snapshot;
        bool report = false;
        portENTER_CRITICAL(&s_stats_lock);
        const int64_t now_us = esp_timer_get_time();
        const int64_t first_pcm_us = s_stats.first_pcm_us;
        if (s_stats.audio_state == AUDIO_STARTED &&
            now_us - s_stats.window_start_us >= STATS_PERIOD_US) {
            snapshot = s_stats;
            s_stats.window_start_us = now_us;
            s_stats.base_callbacks = s_stats.callbacks;
            s_stats.base_pcm = s_stats.pcm_total;
            s_stats.gaps = (gap_stats_t){0};
            report = true;
        }
        portEXIT_CRITICAL(&s_stats_lock);

        if (!first_pcm_logged && first_pcm_us != 0) {
            ESP_LOGI(TAG, "first PCM callback: timestamp_us=%" PRId64
                     " (reported by stats task)", first_pcm_us);
            first_pcm_logged = true;
        }
        if (!report) {
            continue;
        }
        const int64_t elapsed_us = now_us - snapshot.window_start_us;
        const uint64_t pcm_interval = snapshot.pcm_total - snapshot.base_pcm;
        const uint64_t pcm_rate = pcm_interval * UINT64_C(1000000) / elapsed_us;
        const int64_t last_us = snapshot.last_pcm_us != 0 ?
                                snapshot.last_pcm_us : snapshot.segment_start_us;
        const int64_t no_pcm_us = now_us - last_us;
        /* A stalled source must remain visible even before the next callback. */
        const int64_t max_gap_us = snapshot.gaps.max_us > no_pcm_us ?
                                   snapshot.gaps.max_us : no_pcm_us;
        ESP_LOGI(TAG, "stats: elapsed_ms=%" PRId64
                 ", callbacks=%" PRIu64 " (+%" PRIu64 ")"
                 ", pcm_total=%" PRIu64 ", pcm_interval=%" PRIu64
                 ", pcm_rate=%" PRIu64 " B/s, expected_rate=%" PRIu32
                 ", last_size=%" PRIu32 ", no_pcm_now_ms=%" PRId64
                 ", max_gap_ms=%" PRId64
                 ", gaps_gt_30ms=%" PRIu32 ", gaps_gt_50ms=%" PRIu32
                 ", gaps_gt_100ms=%" PRIu32 ", gaps_gt_200ms=%" PRIu32
                 ", sample_rate=%" PRIu32 ", channel_mode=%s, audio_state=%s",
                 elapsed_us / 1000, snapshot.callbacks,
                 snapshot.callbacks - snapshot.base_callbacks,
                 snapshot.pcm_total, pcm_interval, pcm_rate,
                 snapshot.sample_rate * 2U * 2U, snapshot.last_size,
                 no_pcm_us / 1000, max_gap_us / 1000,
                 snapshot.gaps.gt_30ms, snapshot.gaps.gt_50ms,
                 snapshot.gaps.gt_100ms, snapshot.gaps.gt_200ms,
                 snapshot.sample_rate, sbc_channel_mode(snapshot.channel_mode),
                 audio_state_name(snapshot.audio_state));
    }
}

static void handle_audio_config(const esp_a2d_mcc_t *mcc)
{
    uint32_t sample_rate = 0;
    uint8_t channel_mode = 0;
    if (mcc->type == ESP_A2D_MCT_SBC) {
        const esp_a2d_cie_sbc_t *sbc = &mcc->cie.sbc_info;
        const uint8_t *raw = (const uint8_t *)sbc;
        sample_rate = sbc_sample_rate(sbc->samp_freq);
        channel_mode = sbc->ch_mode;
        ESP_LOGI(TAG, "SBC raw=%02x %02x %02x %02x, sample_rate=%" PRIu32
                 ", channel_mode=%s, PCM=s16le interleaved, stereo_frame_bytes=4",
                 raw[0], raw[1], raw[2], raw[3], sample_rate,
                 sbc_channel_mode(channel_mode));
        if (sample_rate == 0 || channel_mode == ESP_A2D_SBC_CIE_CH_MODE_MONO ||
            (channel_mode != ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL &&
             channel_mode != ESP_A2D_SBC_CIE_CH_MODE_STEREO &&
             channel_mode != ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO)) {
            ESP_LOGW(TAG, "Unexpected SBC format; verify before comparing with"
                     " the sample_rate * 2 channels * 2 bytes stereo baseline");
        }
    } else {
        ESP_LOGW(TAG, "Unexpected codec type=%u; expected SBC", (unsigned)mcc->type);
    }

    portENTER_CRITICAL(&s_stats_lock);
    const uint32_t previous_rate = s_stats.sample_rate;
    const uint8_t previous_mode = s_stats.channel_mode;
    if (previous_rate != sample_rate || previous_mode != channel_mode) {
        s_stats.sample_rate = sample_rate;
        s_stats.channel_mode = channel_mode;
        reset_measurement_locked(esp_timer_get_time());
    }
    portEXIT_CRITICAL(&s_stats_lock);
    if (previous_rate != 0 &&
        (previous_rate != sample_rate || previous_mode != channel_mode)) {
        ESP_LOGW(TAG, "SBC configuration changed: sample_rate=%" PRIu32 " -> %" PRIu32
                 ", channel_mode=%s -> %s; measurement window restarted",
                 previous_rate, sample_rate, sbc_channel_mode(previous_mode),
                 sbc_channel_mode(channel_mode));
    }
}

static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const uint8_t *bda = param->conn_stat.remote_bda;
        const esp_a2d_connection_state_t state = param->conn_stat.state;
        if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            set_audio_state(AUDIO_STOPPED);
        }
        ESP_LOGI(TAG, "connection_state=%s, remote=%02x:%02x:%02x:%02x:%02x:%02x",
                 connection_state_name(state), bda[0], bda[1], bda[2],
                 bda[3], bda[4], bda[5]);
        if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "audio_state=stopped (disconnected)");
            ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                    ESP_BT_GENERAL_DISCOVERABLE));
        } else if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP audio_mtu=%u", (unsigned)param->conn_stat.audio_mtu);
            ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                                    ESP_BT_NON_DISCOVERABLE));
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
        handle_audio_config(&param->audio_cfg.mcc);
        break;
    case ESP_A2D_AUDIO_STATE_EVT: {
        audio_state_t state;
        switch (param->audio_stat.state) {
        case ESP_A2D_AUDIO_STATE_STARTED: state = AUDIO_STARTED; break;
        case ESP_A2D_AUDIO_STATE_SUSPEND: state = AUDIO_SUSPENDED; break;
        default:
            ESP_LOGW(TAG, "Unknown A2DP audio state=%d", param->audio_stat.state);
            return;
        }
        set_audio_state(state);
        ESP_LOGI(TAG, "audio_state=%s", audio_state_name(state));
        break;
    }
    case ESP_A2D_PROF_STATE_EVT:
        ESP_LOGI(TAG, "A2DP Sink profile=%s",
                 param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS ?
                 "initialized" : "deinitialized");
        break;
    default:
        break;
    }
}

/* Minimal pairing support retained from bredr_app_common_utils. */
static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "pairing status=%d", param->auth_cmpl.stat);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirmation=%06" PRIu32, param->cfm_req.num_val);
        ESP_ERROR_CHECK(esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true));
        break;
    default:
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "build=a2dp_sink_control (drain-only), device=%s, ESP-IDF=%s",
             DEVICE_NAME, esp_get_idf_version());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "Bluetooth Classic controller starting (default configuration)");
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_LOGI(TAG, "Bluetooth Classic controller enabled");

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "Bluedroid host starting (default configuration, SSP enabled)");
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_LOGI(TAG, "Bluedroid host enabled");

    /* Same SSP I/O capability and legacy PIN as the official common helper. */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap,
                                                 sizeof(uint8_t)));
    esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin_code));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(DEVICE_NAME));
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_callback));

    if (xTaskCreate(stats_task, "control_stats", 4096, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create statistics task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_callback));
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(pcm_data_callback));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                            ESP_BT_GENERAL_DISCOVERABLE));
    ESP_LOGI(TAG, "ready: discoverable as %s; PCM drain-only, audio_state=stopped",
             DEVICE_NAME);
}
