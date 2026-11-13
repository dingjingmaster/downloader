#include "scheduler.h"

#include <clib/memory.h>
#include <clib/thread-pool.h>

#include <errno.h>

struct Scheduler
{
    CThreadPool*      pool;
    SchedulerTaskFunc func;
    void*             userData;
    bool              started;
};

static void scheduler_run_task (void* value, void* userData)
{
    Scheduler* scheduler = (Scheduler*)userData;

    (void)scheduler->func ((DownloadTask*)value, scheduler->userData);
}

Scheduler* scheduler_new (size_t threadCount, SchedulerTaskFunc func, void* userData)
{
    Scheduler* scheduler;

    if (func == NULL) {
        errno = EINVAL;
        return NULL;
    }
    scheduler = (Scheduler*)c_malloc0 (sizeof (*scheduler));
    if (scheduler == NULL)
        return NULL;
    scheduler->func = func;
    scheduler->userData = userData;
    scheduler->pool = c_thread_pool_new (threadCount, scheduler_run_task, scheduler);
    if (scheduler->pool == NULL) {
        c_free (scheduler);
        return NULL;
    }
    return scheduler;
}

int scheduler_start (Scheduler* scheduler)
{
    if (scheduler == NULL) {
        errno = EINVAL;
        return -1;
    }
    scheduler->started = true;
    return 0;
}

int scheduler_submit (Scheduler* scheduler, DownloadTask* task)
{
    if (scheduler == NULL || task == NULL || !scheduler->started) {
        errno = EINVAL;
        return -1;
    }
    return c_thread_pool_push (scheduler->pool, task);
}

int scheduler_pause (Scheduler* scheduler, DownloadTask* task)
{
    (void)scheduler;
    return download_task_set_state (task, DOWNLOAD_TASK_STATE_PAUSED);
}

int scheduler_cancel (Scheduler* scheduler, DownloadTask* task)
{
    (void)scheduler;
    return download_task_set_state (task, DOWNLOAD_TASK_STATE_CANCELLED);
}

void scheduler_stop (Scheduler* scheduler)
{
    if (scheduler == NULL)
        return;
    (void)c_thread_pool_close (scheduler->pool);
    (void)c_thread_pool_join (scheduler->pool);
    (void)c_thread_pool_free (scheduler->pool);
    c_free (scheduler);
}
