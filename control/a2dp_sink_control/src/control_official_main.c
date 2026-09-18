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
#include "control_variant.h"
#include "control_official_i2s.h"

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(6, 1, 0)
#error "This control baseline requires ESP-IDF 6.1.0"
#endif
#if !CONFIG_IDF_TARGET_ESP32 || CONFIG_BT_A2DP_USE_EXTERNAL_CODEC
#error "This control baseline requires classic ESP32 and the internal SBC decoder"
#endif

#if CONTROL_VARIANT != 9
#error "Official entry point belongs only to variant 9"
#endif
#define DEVICE_NAME "Faital A2DP Control"
static const char *TAG = "CONTROL";

typedef enum { AUDIO_STOPPED, AUDIO_SUSPENDED, AUDIO_STARTED } audio_state_t;

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

/* The service owns the five relaxed PCM counters and packet admission. */
static void official_pcm_data_callback(const uint8_t *data, uint32_t len)
{
    control_official_i2s_output(data, len);
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

    ESP_ERROR_CHECK(control_official_i2s_codec_update(mcc));
}

static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const uint8_t *bda = param->conn_stat.remote_bda;
        const esp_a2d_connection_state_t state = param->conn_stat.state;
        ESP_LOGI(TAG, "connection_state=%s, remote=%02x:%02x:%02x:%02x:%02x:%02x",
                 connection_state_name(state), bda[0], bda[1], bda[2],
                 bda[3], bda[4], bda[5]);
        if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "audio_state=stopped (disconnected)");
            ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                                    ESP_BT_GENERAL_DISCOVERABLE));
            control_official_i2s_stop();
            control_official_i2s_close();
        } else if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP audio_mtu=%u", (unsigned)param->conn_stat.audio_mtu);
            ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                                    ESP_BT_NON_DISCOVERABLE));
            /* Idempotent open also handles a peer skipping CONNECTING. */
            ESP_ERROR_CHECK(control_official_i2s_open());
            ESP_ERROR_CHECK(control_official_i2s_start());
        } else if (state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            ESP_ERROR_CHECK(control_official_i2s_open());
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
    ESP_LOGI(TAG, "CONTROL variant=%s, build=a2dp_sink_control, device=%s, ESP-IDF=%s",
             CONTROL_VARIANT_NAME, DEVICE_NAME, esp_get_idf_version());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(control_official_i2s_init());
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

    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_callback));
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(official_pcm_data_callback));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                            ESP_BT_GENERAL_DISCOVERABLE));
    ESP_LOGI(TAG, "ready: discoverable as %s; variant=%s, audio_state=stopped",
             DEVICE_NAME, CONTROL_VARIANT_NAME);
}
