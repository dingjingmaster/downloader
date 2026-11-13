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
#include "task-manager.h"
#include "web.h"
#include "web-api.h"
#include "engines.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef DOWNLOADER_WEB_ROOT
#define DOWNLOADER_WEB_ROOT "web"
#endif

extern const ProtocolOps* http_protocol_ops (void);
extern const ProtocolOps* aria_protocol_ops (void);
extern const ProtocolOps* media_protocol_ops (void);
extern const ProtocolOps* smb_protocol_ops (void);

static volatile sig_atomic_t gStopRequested;

static void daemon_signal (int signalNumber)
{
    (void)signalNumber;
    gStopRequested = 1;
}

typedef struct
{
    Config*    config;
    Database*  database;
    Scheduler* scheduler;
    TaskManager* manager;
    Ipc*       ipc;
    Web*       web;
} Daemon;

static int daemon_run_task (DownloadTask* task, void* userData)
{
    Daemon* daemon = userData;
    return task_manager_execute (daemon->manager, task);
}

static int daemon_handle_web_request (const WebRequest* request, WebResponse* response, void* userData)
{
    Daemon* daemon = (Daemon*)userData;

    return web_api_handle (daemon->manager, request, response);
}

static int daemon_json_get_string (const CJsonValue* root, const char* key, char* destination, size_t capacity)
{
    CStringView value = c_json_value_get_string (c_json_object_get (root, c_string_view_from_cstr (key)));

    if (value.data == NULL || value.size == 0 || value.size >= capacity) {
        errno = EINVAL;
        return -1;
    }
    memcpy (destination, value.data, value.size);
    destination[value.size] = '\0';
    return 0;
}

static int daemon_handle_request (const char* data, size_t dataSize, char* response, size_t responseCapacity,
                                  size_t* responseSize, void* userData)
{
    Daemon*        daemon = (Daemon*)userData;
    CJsonDocument* document;
    CJsonValue*    root;
    CStringView    operationView;
    char*          operationText = NULL;
    size_t         errorOffset;
    int            result;

    document = c_json_document_new_from_data (c_string_view_from_data (data, dataSize), C_JSON_PARSE_NONE, &errorOffset);
    if (document == NULL) {
        result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"invalid_json\"}");
        goto finish;
    }
    root = c_json_document_root (document);
    operationView = c_json_value_get_string (c_json_object_get (root, c_string_view_from_cstr ("op")));
    operationText = c_string_view_to_cstr (operationView);
    if (operationText == NULL) {
        result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"missing_op\"}");
        goto finish;
    }
    if (strcmp (operationText, "ping") == 0) {
        result = snprintf (response, responseCapacity, "{\"ok\":true}");
    } else if (strcmp (operationText, "shutdown") == 0) {
        gStopRequested = 1;
        result = snprintf (response, responseCapacity, "{\"ok\":true}");
    } else if (strcmp (operationText, "create") == 0) {
        char         url[DOWNLOAD_TASK_URL_SIZE];
        char         fileName[DOWNLOAD_TASK_FILE_NAME_SIZE];
        char         directory[DOWNLOAD_TASK_PATH_SIZE];
        char         protocol[DOWNLOAD_TASK_PROTOCOL_SIZE];
        const char*  extra = "{}";
        char*        extraText = NULL;
        DownloadTask task;

        if (daemon_json_get_string (root, "url", url, sizeof (url)) != 0 ||
            daemon_json_get_string (root, "file_name", fileName, sizeof (fileName)) != 0 ||
            daemon_json_get_string (root, "target_directory", directory, sizeof (directory)) != 0 ||
            daemon_json_get_string (root, "protocol", protocol, sizeof (protocol)) != 0) {
            result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"missing_task_field\"}");
        } else {
            CJsonValue* extraValue = c_json_object_get (root, c_string_view_from_cstr ("extra"));
            if (extraValue != NULL)
                extraText = c_json_value_to_data (extraValue, C_JSON_WRITE_NONE, NULL);
            if (extraText != NULL)
                extra = extraText;
            if (task_manager_create (daemon->manager, url, fileName, directory, protocol, extra, &task) != 0)
                result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"create_failed\"}");
            else
                result = snprintf (response, responseCapacity, "{\"ok\":true,\"id\":\"%s\"}", task.uuid);
            c_free (extraText);
        }
    } else if (strcmp (operationText, "start") == 0 || strcmp (operationText, "pause") == 0 ||
               strcmp (operationText, "resume") == 0 || strcmp (operationText, "delete") == 0) {
        char uuid[DOWNLOAD_TASK_UUID_SIZE];
        int  operationResult;

        if (daemon_json_get_string (root, "id", uuid, sizeof (uuid)) != 0) {
            result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"missing_id\"}");
        } else if (strcmp (operationText, "start") == 0) {
            operationResult = task_manager_start (daemon->manager, uuid);
            result = snprintf (response, responseCapacity, operationResult == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"start_failed\"}");
        } else if (strcmp (operationText, "pause") == 0) {
            operationResult = task_manager_pause (daemon->manager, uuid);
            result = snprintf (response, responseCapacity, operationResult == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"pause_failed\"}");
        } else if (strcmp (operationText, "resume") == 0) {
            operationResult = task_manager_resume (daemon->manager, uuid);
            result = snprintf (response, responseCapacity, operationResult == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"resume_failed\"}");
        } else {
            operationResult = task_manager_delete (daemon->manager, uuid);
            result = snprintf (response, responseCapacity, operationResult == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"delete_failed\"}");
        }
    } else if (strcmp (operationText, "clear_completed") == 0) {
        size_t deleted = 0;
        if (task_manager_clear_completed (daemon->manager, &deleted) != 0)
            result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"clear_failed\"}");
        else
            result = snprintf (response, responseCapacity, "{\"ok\":true,\"deleted\":%zu}", deleted);
    } else {
        result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"not_supported\"}");
    }

finish:
    c_free (operationText);
    if (document != NULL)
        c_json_document_free (document);
    if (result < 0 || (size_t)result >= responseCapacity) {
        errno = ENOSPC;
        return -1;
    }
    *responseSize = (size_t)result;
    return 0;
}

int main (int argc, char* argv[])
{
    if (argc == 2 && strcmp (argv[1], "--version") == 0) {
        download_engines_print_versions ();
        return 0;
    }
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
    if (protocol_register (aria_protocol_ops ()) != 0 || protocol_register (media_protocol_ops ()) != 0 ||
        protocol_register (smb_protocol_ops ()) != 0 || protocol_register (http_protocol_ops ()) != 0) {
        fprintf (stderr, "downloader: register protocol failed: %s\n", strerror (errno));
        scheduler_stop (daemon.scheduler);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    daemon.manager = task_manager_new (daemon.database, daemon.scheduler);
    if (daemon.manager == NULL) {
        fprintf (stderr, "downloader: create task manager failed: %s\n", strerror (errno));
        scheduler_stop (daemon.scheduler);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    if (task_manager_set_download_directory (daemon.manager, config_download_dir (daemon.config)) != 0 ||
        task_manager_set_transfer_options (daemon.manager, config_max_retries (daemon.config),
                                          config_connect_timeout (daemon.config), config_transfer_timeout (daemon.config)) != 0) {
        scheduler_stop (daemon.scheduler);
        task_manager_free (daemon.manager);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    daemon.ipc = ipc_new (config_ipc_socket (daemon.config), daemon_handle_request, &daemon);
    if (daemon.ipc == NULL || ipc_start (daemon.ipc) != 0) {
        fprintf (stderr, "downloader: start ipc failed: %s\n", strerror (errno));
        ipc_free (daemon.ipc);
        if (task_manager_stop (daemon.manager) != 0)
            fprintf (stderr, "downloader: save paused tasks failed: %s\n", strerror (errno));
        scheduler_stop (daemon.scheduler);
        task_manager_free (daemon.manager);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    if (config_web_enabled (daemon.config)) {
        WebOptions options = {
            .listen = config_web_listen (daemon.config),
            .port = config_web_port (daemon.config),
            .documentRoot = DOWNLOADER_WEB_ROOT
        };
        daemon.web = web_new (&options, daemon_handle_web_request, &daemon);
        if (daemon.web == NULL || web_start (daemon.web) != 0) {
            fprintf (stderr, "downloader: start web failed: %s\n", strerror (errno));
            web_free (daemon.web);
            ipc_stop (daemon.ipc);
            ipc_free (daemon.ipc);
            if (task_manager_stop (daemon.manager) != 0)
                fprintf (stderr, "downloader: save paused tasks failed: %s\n", strerror (errno));
            scheduler_stop (daemon.scheduler);
            task_manager_free (daemon.manager);
            database_close (daemon.database);
            config_free (daemon.config);
            return 1;
        }
    }

    struct sigaction action;
    memset (&action, 0, sizeof (action));
    action.sa_handler = daemon_signal;
    sigemptyset (&action.sa_mask);
    if (sigaction (SIGINT, &action, NULL) != 0 || sigaction (SIGTERM, &action, NULL) != 0)
        gStopRequested = 1;

    while (!gStopRequested) {
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };
        (void)nanosleep (&delay, NULL);
    }

    if (task_manager_stop (daemon.manager) != 0)
        fprintf (stderr, "downloader: save paused tasks failed: %s\n", strerror (errno));
    ipc_stop (daemon.ipc);
    ipc_free (daemon.ipc);
    web_stop (daemon.web);
    web_free (daemon.web);
    scheduler_stop (daemon.scheduler);
    task_manager_free (daemon.manager);
    database_close (daemon.database);
    config_free (daemon.config);

    return 0;
}
