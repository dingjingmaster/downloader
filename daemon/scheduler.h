#ifndef DOWNLOADER_SCHEDULER_H
#define DOWNLOADER_SCHEDULER_H

#include "task.h"

#include <stddef.h>

typedef struct Scheduler Scheduler;
typedef int (*SchedulerTaskFunc) (DownloadTask* task, void* userData);

Scheduler* scheduler_new (size_t threadCount, SchedulerTaskFunc func, void* userData);
int        scheduler_start (Scheduler* scheduler);
int        scheduler_submit (Scheduler* scheduler, DownloadTask* task);
int        scheduler_pause (Scheduler* scheduler, DownloadTask* task);
int        scheduler_cancel (Scheduler* scheduler, DownloadTask* task);
void       scheduler_stop (Scheduler* scheduler);

#endif
