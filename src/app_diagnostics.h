#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum
{
    DIAG_TASK_WRITER,
    DIAG_TASK_STATS,
    DIAG_TASK_LED,
    DIAG_TASK_STORAGE,
    DIAG_TASK_RSSI,
    DIAG_TASK_RECONNECT,
    DIAG_TASK_I2S_INIT,
    DIAG_TASK_COUNT
} diagnostic_task_id_t;

void app_diagnostics_init(void);
/* Persistent tasks only: these handles must remain valid until reset. */
void app_diagnostics_track_task(diagnostic_task_id_t id);
/* Transient tasks publish their own watermark before deleting themselves. */
void app_diagnostics_record_stack(diagnostic_task_id_t id);
/* Called only by audio_stats_task, at most once every five seconds. */
void app_diagnostics_log(void);
