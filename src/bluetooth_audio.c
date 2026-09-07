#include "bluetooth_audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_config.h"
#include "audio_pipeline.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "status_led.h"

static const char *TAG = APP_LOG_TAG;

static QueueHandle_t bda_save_queue;
static portMUX_TYPE bluetooth_state_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_bd_addr_t connected_bda;
static bool connected_bda_valid;
static bool have_last_bda;
static esp_bd_addr_t last_bda;

static const char *connection_state_name(esp_a2d_connection_state_t state)
{
    switch (state)
    {
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
        return "disconnected";
    case ESP_A2D_CONNECTION_STATE_CONNECTING:
        return "connecting";
    case ESP_A2D_CONNECTION_STATE_CONNECTED:
        return "connected";
    case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
        return "disconnecting";
    default:
        return "unknown";
    }
}

static void log_bda(const char *prefix, const esp_bd_addr_t bda)
{
    ESP_LOGI(TAG,
             "%s %02x:%02x:%02x:%02x:%02x:%02x",
             prefix,
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static void load_last_bda(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return;
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Cannot open Bluetooth NVS: %s", esp_err_to_name(err));
        status_led_signal_error();
        return;
    }

    size_t size = sizeof(last_bda);
    err = nvs_get_blob(handle, NVS_LAST_BDA_KEY, last_bda, &size);
    nvs_close(handle);

    if (err == ESP_OK && size == sizeof(last_bda))
    {
        have_last_bda = true;
        log_bda("Saved phone:", last_bda);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(TAG, "No saved phone MAC in NVS");
    }
    else
    {
        ESP_LOGW(TAG,
                 "Cannot load saved phone MAC: error=%s, size=%u",
                 esp_err_to_name(err),
                 (unsigned)size);
        status_led_signal_error();
    }
}

static void save_last_bda(const esp_bd_addr_t bda)
{
    if (have_last_bda && memcmp(last_bda, bda, sizeof(last_bda)) == 0)
    {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "Cannot open Bluetooth NVS for writing: %s",
                 esp_err_to_name(err));
        status_led_signal_error();
        return;
    }

    err = nvs_set_blob(handle, NVS_LAST_BDA_KEY, bda, sizeof(esp_bd_addr_t));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK)
    {
        memcpy(last_bda, bda, sizeof(last_bda));
        have_last_bda = true;
        log_bda("Saved connected phone:", last_bda);
    }
    else
    {
        ESP_LOGW(TAG, "Cannot save phone address: %s", esp_err_to_name(err));
        status_led_signal_error();
    }
}

static uint32_t sample_rate_from_sbc_config(const esp_a2d_mcc_t *mcc)
{
    if (mcc == NULL || mcc->type != ESP_A2D_MCT_SBC)
    {
        return DEFAULT_SAMPLE_RATE_HZ;
    }

    const uint8_t sampling_frequency = mcc->cie.sbc_info.samp_freq;
    if ((sampling_frequency & ESP_A2D_SBC_CIE_SF_48K) != 0)
    {
        return 48000U;
    }
    if ((sampling_frequency & ESP_A2D_SBC_CIE_SF_44K) != 0)
    {
        return 44100U;
    }
    if ((sampling_frequency & ESP_A2D_SBC_CIE_SF_32K) != 0)
    {
        return 32000U;
    }
    if ((sampling_frequency & ESP_A2D_SBC_CIE_SF_16K) != 0)
    {
        return 16000U;
    }
    return DEFAULT_SAMPLE_RATE_HZ;
}

static const char *audio_state_name(esp_a2d_audio_state_t state)
{
    switch (state)
    {
    case ESP_A2D_AUDIO_STATE_SUSPEND:
        return "suspended";
    case ESP_A2D_AUDIO_STATE_STARTED:
        return "started";
    default:
        return "unknown";
    }
}

static const char *sbc_channel_mode_name(uint8_t channel_mode)
{
    if ((channel_mode & ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO) != 0)
    {
        return "joint-stereo";
    }
    if ((channel_mode & ESP_A2D_SBC_CIE_CH_MODE_STEREO) != 0)
    {
        return "stereo";
    }
    if ((channel_mode & ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL) != 0)
    {
        return "dual-channel";
    }
    if ((channel_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) != 0)
    {
        return "mono";
    }
    return "unknown";
}

static void a2dp_data_callback(const uint8_t *data, uint32_t length)
{
    audio_pipeline_receive_pcm(data, length);
}

static void a2dp_event_callback(esp_a2d_cb_event_t event,
                                esp_a2d_cb_param_t *param)
{
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG,
                 "A2DP connection state: %s, handle=%u, audio_mtu=%u, "
                 "disconnect_reason=%s",
                 connection_state_name(param->conn_stat.state),
                 (unsigned)param->conn_stat.conn_hdl,
                 (unsigned)param->conn_stat.audio_mtu,
                 param->conn_stat.disc_rsn == ESP_A2D_DISC_RSN_NORMAL
                     ? "normal"
                     : "abnormal");
        log_bda("A2DP remote MAC:", param->conn_stat.remote_bda);

        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            portENTER_CRITICAL(&bluetooth_state_lock);
            memcpy(
                connected_bda,
                param->conn_stat.remote_bda,
                sizeof(connected_bda));
            connected_bda_valid = true;
            portEXIT_CRITICAL(&bluetooth_state_lock);
            audio_pipeline_update_rssi(0, false);

            status_led_set_state(STATUS_LED_CONNECTED);
            if (bda_save_queue == NULL ||
                xQueueOverwrite(
                    bda_save_queue,
                    param->conn_stat.remote_bda) != pdPASS)
            {
                ESP_LOGW(TAG, "Cannot queue connected phone MAC for NVS");
                status_led_signal_error();
            }
        }
        else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
        {
            portENTER_CRITICAL(&bluetooth_state_lock);
            connected_bda_valid = false;
            portEXIT_CRITICAL(&bluetooth_state_lock);
            audio_pipeline_update_rssi(0, false);

            audio_pipeline_set_streaming(false);
            status_led_set_state(STATUS_LED_DISCOVERABLE);
            esp_err_t err = esp_bt_gap_set_scan_mode(
                ESP_BT_CONNECTABLE,
                ESP_BT_GENERAL_DISCOVERABLE);
            if (err == ESP_OK)
            {
                ESP_LOGI(TAG,
                         "Bluetooth scan mode: connectable, general discoverable");
            }
            else
            {
                ESP_LOGE(TAG,
                         "Cannot restore Bluetooth discoverable state: %s",
                         esp_err_to_name(err));
                status_led_signal_error();
            }
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG,
                 "A2DP audio state: %s",
                 audio_state_name(param->audio_stat.state));
        log_bda("A2DP audio source MAC:", param->audio_stat.remote_bda);

        bool started =
            param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        audio_pipeline_set_streaming(started);
        status_led_set_state(
            started ? STATUS_LED_STREAMING : STATUS_LED_CONNECTED);
        break;

    case ESP_A2D_AUDIO_CFG_EVT:
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC)
        {
            const esp_a2d_cie_sbc_t *sbc =
                &param->audio_cfg.mcc.cie.sbc_info;
            const uint8_t *raw = (const uint8_t *)sbc;
            uint32_t detected_sample_rate_hz =
                sample_rate_from_sbc_config(&param->audio_cfg.mcc);
            audio_pipeline_set_sample_rate(detected_sample_rate_hz);
            ESP_LOGI(TAG,
                     "SBC config received: raw=%02x %02x %02x %02x, "
                     "sample_mask=0x%x, channel_mode=%s (0x%x), "
                     "blocks=0x%x, subbands=0x%x, allocation=0x%x, "
                     "bitpool=%u..%u",
                     raw[0],
                     raw[1],
                     raw[2],
                     raw[3],
                     sbc->samp_freq,
                     sbc_channel_mode_name(sbc->ch_mode),
                     sbc->ch_mode,
                     sbc->block_len,
                     sbc->num_subbands,
                     sbc->alloc_mthd,
                     sbc->min_bitpool,
                     sbc->max_bitpool);
            ESP_LOGI(TAG,
                     "SBC sample rate detected: %lu Hz",
                     (unsigned long)detected_sample_rate_hz);
        }
        else
        {
            ESP_LOGW(TAG,
                     "Unsupported A2DP codec type received: %d",
                     param->audio_cfg.mcc.type);
            status_led_signal_error();
        }
        break;

    default:
        break;
    }
}

static const char *avrc_init_state_name(esp_avrc_init_state_t state)
{
    switch (state)
    {
    case ESP_AVRC_INIT_SUCCESS:
        return "initialized";
    case ESP_AVRC_INIT_ALREADY:
        return "already initialized";
    case ESP_AVRC_INIT_FAIL:
        return "initialization failed";
    default:
        return "unknown";
    }
}

static void avrc_controller_event_callback(
    esp_avrc_ct_cb_event_t event,
    esp_avrc_ct_cb_param_t *param)
{
    switch (event)
    {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG,
                 "AVRCP controller connection: %s",
                 param->conn_stat.connected ? "connected" : "disconnected");
        log_bda("AVRCP remote MAC:", param->conn_stat.remote_bda);
        break;

    case ESP_AVRC_CT_PROF_STATE_EVT:
        ESP_LOGI(TAG,
                 "AVRCP controller profile: %s",
                 avrc_init_state_name(param->avrc_ct_init_stat.state));
        if (param->avrc_ct_init_stat.state == ESP_AVRC_INIT_FAIL)
        {
            status_led_signal_error();
        }
        break;

    default:
        break;
    }
}

static void gap_event_callback(esp_bt_gap_cb_event_t event,
                               esp_bt_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI(TAG,
                     "Bluetooth pairing successful: device=%s",
                     (const char *)param->auth_cmpl.device_name);
            log_bda("Paired device MAC:", param->auth_cmpl.bda);
        }
        else
        {
            ESP_LOGW(TAG,
                     "Bluetooth pairing failed: status=%d, device=%s",
                     param->auth_cmpl.stat,
                     (const char *)param->auth_cmpl.device_name);
            log_bda("Pairing failure MAC:", param->auth_cmpl.bda);
            status_led_signal_error();
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT:
    {
        esp_bt_pin_code_t pin_code;
        memset(pin_code, 0, sizeof(pin_code));

        if (param->pin_req.min_16_digit)
        {
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        }
        else
        {
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG,
                 "SSP confirmation number: %lu",
                 (unsigned long)param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
        portENTER_CRITICAL(&bluetooth_state_lock);
        bool matching_connection =
            connected_bda_valid &&
            memcmp(
                connected_bda,
                param->read_rssi_delta.bda,
                sizeof(connected_bda)) == 0;
        portEXIT_CRITICAL(&bluetooth_state_lock);

        if (matching_connection)
        {
            audio_pipeline_update_rssi(
                param->read_rssi_delta.rssi_delta,
                param->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS);
        }
        break;

    default:
        break;
    }
}

static void reconnect_task(void *argument)
{
    (void)argument;

    vTaskDelay(pdMS_TO_TICKS(1200));
    if (have_last_bda)
    {
        log_bda("Trying saved phone:", last_bda);
        esp_err_t err = esp_a2d_sink_connect(last_bda);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Reconnect request failed: %s", esp_err_to_name(err));
            status_led_signal_error();
        }
        else
        {
            ESP_LOGI(TAG, "Automatic reconnect request submitted");
        }
    }
    vTaskDelete(NULL);
}

static void bluetooth_storage_task(void *argument)
{
    (void)argument;

    esp_bd_addr_t bda;
    while (true)
    {
        if (xQueueReceive(bda_save_queue, bda, portMAX_DELAY) == pdPASS)
        {
            save_last_bda(bda);
        }
    }
}

static void bluetooth_rssi_task(void *argument)
{
    (void)argument;

    vTaskDelay(pdMS_TO_TICKS(BT_RSSI_INITIAL_DELAY_MS));
    while (true)
    {
        esp_bd_addr_t bda;
        bool have_connection;

        portENTER_CRITICAL(&bluetooth_state_lock);
        have_connection = connected_bda_valid;
        if (have_connection)
        {
            memcpy(bda, connected_bda, sizeof(bda));
        }
        portEXIT_CRITICAL(&bluetooth_state_lock);

        if (have_connection)
        {
            esp_err_t err = esp_bt_gap_read_rssi_delta(bda);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG,
                         "Cannot request Bluetooth RSSI delta: %s",
                         esp_err_to_name(err));
                status_led_signal_error();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BT_RSSI_INTERVAL_MS));
    }
}

static void bluetooth_stack_init(void)
{
    ESP_LOGI(TAG, "Starting Bluetooth Classic controller");
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t controller_config =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_LOGI(TAG,
             "Bluetooth Classic controller started; modem_sleep=%s",
#if CONFIG_BTDM_CTRL_MODEM_SLEEP
             "enabled"
#else
             "disabled"
#endif
    );

    ESP_LOGI(TAG, "Starting Bluedroid host stack");
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_LOGI(TAG, "Bluedroid host stack started");

    /* Avoid synchronous packet-by-packet warnings in the media path. */
    esp_log_level_set("BT_APPL", ESP_LOG_ERROR);
    ESP_LOGI(TAG,
             "Bluetooth media stack log level: errors only "
             "(packet sequence warnings suppressed)");

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_event_callback));

    ESP_LOGI(TAG, "Starting Bluetooth AVRCP Controller before A2DP");
    ESP_ERROR_CHECK(
        esp_avrc_ct_register_callback(avrc_controller_event_callback));
    ESP_ERROR_CHECK(esp_avrc_ct_init());

    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_event_callback));
    ESP_LOGI(TAG, "Starting Bluetooth A2DP Sink");
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(a2dp_data_callback));
    ESP_LOGI(TAG,
             "Bluetooth A2DP Sink started; internal SBC decoder and PCM "
             "callback registered");

    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(BLUETOOTH_DEVICE_NAME));
    ESP_LOGI(TAG, "Bluetooth device name: %s", BLUETOOTH_DEVICE_NAME);

    esp_bt_io_cap_t io_capability = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(
        ESP_BT_SP_IOCAP_MODE,
        &io_capability,
        sizeof(io_capability)));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(
        ESP_BT_CONNECTABLE,
        ESP_BT_GENERAL_DISCOVERABLE));

    ESP_LOGI(TAG, "Bluetooth ready: connectable=yes, discoverable=general");
    status_led_set_state(STATUS_LED_DISCOVERABLE);

    if (have_last_bda)
    {
        BaseType_t result =
            xTaskCreate(reconnect_task, "bt_reconnect", 3072, NULL, 4, NULL);
        if (result == pdPASS)
        {
            ESP_LOGI(TAG, "Task created: bt_reconnect");
        }
        else
        {
            ESP_LOGW(TAG, "Cannot create automatic reconnect task");
            status_led_signal_error();
        }
    }
}

esp_err_t bluetooth_audio_init(void)
{
    load_last_bda();

    bda_save_queue = xQueueCreate(1, sizeof(esp_bd_addr_t));
    if (bda_save_queue == NULL)
    {
        ESP_LOGE(TAG, "Cannot create Bluetooth MAC storage queue");
        status_led_set_state(STATUS_LED_ERROR);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Bluetooth MAC storage queue created");

    BaseType_t result = xTaskCreate(
        bluetooth_storage_task,
        "bt_storage",
        3072,
        NULL,
        3,
        NULL);
    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Cannot create Bluetooth storage task");
        status_led_set_state(STATUS_LED_ERROR);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Task created: bt_storage");

    result = xTaskCreate(
        bluetooth_rssi_task,
        "bt_rssi",
        3072,
        NULL,
        BT_RSSI_TASK_PRIORITY,
        NULL);
    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Cannot create Bluetooth RSSI task");
        status_led_set_state(STATUS_LED_ERROR);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "Task created: bt_rssi, priority=%d, interval=%u ms",
             BT_RSSI_TASK_PRIORITY,
             (unsigned)BT_RSSI_INTERVAL_MS);

    bluetooth_stack_init();
    return ESP_OK;
}
