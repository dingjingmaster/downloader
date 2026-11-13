#include "task.h"

#include <errno.h>
#include <string.h>

const char* download_task_state_name (DownloadTaskState state)
{
    static const char* names[] = {
        "created", "queued", "downloading", "paused", "completed", "failed", "cancelled"
    };

    if (state < DOWNLOAD_TASK_STATE_CREATED || state > DOWNLOAD_TASK_STATE_CANCELLED)
        return "unknown";
    return names[state];
}

DownloadTaskState download_task_state_from_name (const char* name)
{
    DownloadTaskState state;

    if (name == NULL) {
        errno = EINVAL;
        return DOWNLOAD_TASK_STATE_FAILED;
    }
    for (state = DOWNLOAD_TASK_STATE_CREATED; state <= DOWNLOAD_TASK_STATE_CANCELLED; state++) {
        if (strcmp (name, download_task_state_name (state)) == 0)
            return state;
    }
    errno = EINVAL;
    return DOWNLOAD_TASK_STATE_FAILED;
}

int download_task_set_state (DownloadTask* task, DownloadTaskState state)
{
    if (task == NULL || state < DOWNLOAD_TASK_STATE_CREATED || state > DOWNLOAD_TASK_STATE_CANCELLED) {
        errno = EINVAL;
        return -1;
    }
    task->state = state;
    return 0;
}
