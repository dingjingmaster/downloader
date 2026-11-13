/*************************************************************************
> FileName: main.c
> Author  : DingJing
> Mail    : dingjing@live.cn
> Created Time: 2026年11月13日 星期五 11时36分44秒
 ************************************************************************/

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <clib/clib.h>
#include "config.h"
#include "database.h"
#include "ipc.h"
#include "protocol.h"
#include "scheduler.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern const ProtocolOps* http_protocol_ops (void);

typedef struct
{
    Config*    config;
    Database*  database;
    Scheduler* scheduler;
    Ipc*       ipc;
    volatile bool stopRequested;
} Daemon;

static int daemon_run_task (DownloadTask* task, void* userData)
{
    (void)task;
    (void)userData;
    errno = ENOTSUP;
    return -1;
}

static int daemon_handle_request (const char* data, size_t dataSize, char* response, size_t responseCapacity,
                                  size_t* responseSize, void* userData)
{
    Daemon*        daemon = (Daemon*)userData;
    CJsonDocument* document;
    CJsonValue*    root;
    CJsonValue*    operation;
    CStringView    operationView;
    char*          operationText;
    size_t         errorOffset;
    int            result = 0;

    document = c_json_document_new_from_data (c_string_view_from_data (data, dataSize), C_JSON_PARSE_NONE, &errorOffset);
    if (document == NULL) {
        result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"invalid_json\"}");
        goto done;
    }
    root = c_json_document_root (document);
    operation = c_json_object_get (root, c_string_view_from_cstr ("op"));
    operationView = c_json_value_get_string (operation);
    operationText = c_string_view_to_cstr (operationView);
    if (operationText == NULL) {
        result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"missing_op\"}");
        c_json_document_free (document);
        goto done;
    }
    if (strcmp (operationText, "ping") == 0) {
        result = snprintf (response, responseCapacity, "{\"ok\":true}");
    } else if (strcmp (operationText, "shutdown") == 0) {
        daemon->stopRequested = true;
        result = snprintf (response, responseCapacity, "{\"ok\":true}");
    } else {
        result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"not_supported\"}");
    }
    c_free (operationText);
    c_json_document_free (document);

done:
    if (result < 0 || (size_t)result >= responseCapacity) {
        errno = ENOSPC;
        return -1;
    }
    *responseSize = (size_t)result;
    return 0;
}

int main (int argc, char* argv[])
{
    Daemon daemon;

    memset (&daemon, 0, sizeof (daemon));
    daemon.config = config_load (argc, argv);
    if (daemon.config == NULL) {
        fprintf (stderr, "downloader: load config failed: %s\n", strerror (errno));
        return 1;
    }
    if (database_open (config_database (daemon.config), &daemon.database) != 0) {
        fprintf (stderr, "downloader: open database failed: %s\n", strerror (errno));
        config_free (daemon.config);
        return 1;
    }
    if (database_recover (daemon.database) != 0) {
        fprintf (stderr, "downloader: recover database failed: %s\n", strerror (errno));
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    daemon.scheduler = scheduler_new ((size_t)config_max_concurrent_tasks (daemon.config), daemon_run_task, &daemon);
    if (daemon.scheduler == NULL || scheduler_start (daemon.scheduler) != 0) {
        fprintf (stderr, "downloader: start scheduler failed: %s\n", strerror (errno));
        scheduler_stop (daemon.scheduler);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    if (protocol_register (http_protocol_ops ()) != 0 && errno != EEXIST) {
        fprintf (stderr, "downloader: register http protocol failed: %s\n", strerror (errno));
        scheduler_stop (daemon.scheduler);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    daemon.ipc = ipc_new (config_ipc_socket (daemon.config), daemon_handle_request, &daemon);
    if (daemon.ipc == NULL || ipc_start (daemon.ipc) != 0) {
        fprintf (stderr, "downloader: start ipc failed: %s\n", strerror (errno));
        ipc_free (daemon.ipc);
        scheduler_stop (daemon.scheduler);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }

    while (!daemon.stopRequested) {
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };
        (void)nanosleep (&delay, NULL);
    }

    ipc_stop (daemon.ipc);
    ipc_free (daemon.ipc);
    scheduler_stop (daemon.scheduler);
    database_close (daemon.database);
    config_free (daemon.config);

    return 0;
}
