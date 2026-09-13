#include "app_diagnostics.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = APP_LOG_TAG;
static TaskHandle_t task_handles[DIAG_TASK_COUNT];
static int32_t saved_stacks[DIAG_TASK_COUNT] = {-1, -1, -1, -1, -1, -1, -1};

enum { ACL_INCOMPLETE, ACL_ORPHAN, SBC_QUEUE_DROP, SBC_SEQUENCE, SBC_DECODE, BT_FAULT_COUNT };
static const char *const fault_formats[BT_FAULT_COUNT] = {
    "found unfinished packet for handle with start packet",
    "got continuation for unknown packet",
    "Pkt dropped",
    "Sequence numbers error",
    "Decoding failure:"
};
static portMUX_TYPE fault_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t fault_counts[BT_FAULT_COUNT];
static int64_t fault_last_log_us[BT_FAULT_COUNT];
static vprintf_like_t previous_log_output = vprintf;
static uint32_t allocation_failures;

static void allocation_failed(size_t size, uint32_t caps, const char *function_name)
{
    (void)size;
    (void)caps;
    (void)function_name;
    /* Public heap hook: no output, allocation, or blocking on the failure path. */
    __atomic_fetch_add(&allocation_failures, 1U, __ATOMIC_RELAXED);
}

static int diagnostic_log_output(const char *format, va_list args)
{
    /* IDF 6.0.1/6.1 BT_PRINT_* passes the literal message in format. Match it
     * without formatting/copying a packet, allocating, or consuming va_list.
     * This hook is reentrant; never log or call the output sink under the lock. */
    for (unsigned i = 0; i < BT_FAULT_COUNT; ++i)
    {
        if (strstr(format, fault_formats[i]) == NULL)
        {
            continue;
        }
        int64_t now_us = esp_timer_get_time();
        portENTER_CRITICAL(&fault_lock);
        uint64_t count = ++fault_counts[i];
        bool emit = BT_DIAGNOSTIC_VERBOSE_STACK_LOGS || count == 1 ||
                    now_us - fault_last_log_us[i] >= (int64_t)AUDIO_STATS_INTERVAL_MS * 1000;
        if (emit)
        {
            fault_last_log_us[i] = now_us;
        }
        portEXIT_CRITICAL(&fault_lock);
        /* Normal mode shows the first/recent example and every occurrence in
         * the five-second totals. Verbose mode also prints every fault line. */
        return emit ? previous_log_output(format, args) : 0;
    }
    return previous_log_output(format, args);
}

void app_diagnostics_init(void)
{
    /* Called once from app_main, before any application tasks / Bluetooth. */
    previous_log_output = esp_log_set_vprintf(diagnostic_log_output);
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(allocation_failed));
    ESP_LOGI(TAG, "Build: IDF=%s, BT minimal=%d, verbose=%d, tone=%d",
             esp_get_idf_version(), BT_DIAGNOSTIC_MINIMAL_MODE,
             BT_DIAGNOSTIC_VERBOSE_STACK_LOGS, AUDIO_DIAGNOSTIC_TONE_ENABLED);
}

void app_diagnostics_track_task(diagnostic_task_id_t id)
{
    configASSERT(id < DIAG_TASK_RECONNECT);
    __atomic_store_n(&task_handles[id], xTaskGetCurrentTaskHandle(), __ATOMIC_RELEASE);
}

void app_diagnostics_record_stack(diagnostic_task_id_t id)
{
    configASSERT(id == DIAG_TASK_RECONNECT || id == DIAG_TASK_I2S_INIT);
    __atomic_store_n(&saved_stacks[id],
                     (int32_t)uxTaskGetStackHighWaterMark(NULL), __ATOMIC_RELEASE);
}

void app_diagnostics_log(void)
{
    static uint64_t previous_faults[BT_FAULT_COUNT];
    uint64_t faults[BT_FAULT_COUNT];
    uint64_t delta[BT_FAULT_COUNT];
    portENTER_CRITICAL(&fault_lock);
    memcpy(faults, fault_counts, sizeof(faults));
    portEXIT_CRITICAL(&fault_lock);
    for (unsigned i = 0; i < BT_FAULT_COUNT; ++i)
    {
        delta[i] = faults[i] - previous_faults[i];
        previous_faults[i] = faults[i];
    }
    ESP_LOGI(TAG,
             "BT fault logs total(+interval): acl_incomplete=%" PRIu64 "(+%" PRIu64 ")"
             ", acl_orphan=%" PRIu64 "(+%" PRIu64 "), sbc_queue_drop=%" PRIu64 "(+%" PRIu64 ")"
             ", sbc_sequence=%" PRIu64 "(+%" PRIu64 "), sbc_decode=%" PRIu64 "(+%" PRIu64 ")",
             faults[ACL_INCOMPLETE], delta[ACL_INCOMPLETE], faults[ACL_ORPHAN], delta[ACL_ORPHAN],
             faults[SBC_QUEUE_DROP], delta[SBC_QUEUE_DROP], faults[SBC_SEQUENCE], delta[SBC_SEQUENCE],
             faults[SBC_DECODE], delta[SBC_DECODE]);

    int32_t stack[DIAG_TASK_COUNT];
    for (unsigned i = 0; i < DIAG_TASK_COUNT; ++i)
    {
        TaskHandle_t handle = __atomic_load_n(&task_handles[i], __ATOMIC_ACQUIRE);
        /* Do not hold a spinlock while scanning stacks. Transient tasks never
         * publish handles, so a deleted TCB cannot be sampled here. */
        stack[i] = handle != NULL ? (int32_t)uxTaskGetStackHighWaterMark(handle)
                                 : __atomic_load_n(&saved_stacks[i], __ATOMIC_ACQUIRE);
    }
    ESP_LOGI(TAG,
             "Resources: heap_free=%" PRIu32 ", heap_min=%" PRIu32 ", largest_8bit=%u"
             ", allocation_failures=%" PRIu32
             ", stack_free_min_bytes(writer/stats/led/storage/rssi/reconnect_final/i2s_init_final)="
             "%" PRId32 "/%" PRId32 "/%" PRId32 "/%" PRId32 "/%" PRId32 "/%" PRId32 "/%" PRId32,
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             __atomic_load_n(&allocation_failures, __ATOMIC_RELAXED),
             stack[0], stack[1], stack[2], stack[3], stack[4], stack[5], stack[6]);
}
