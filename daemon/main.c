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
#include "credential-store.h"
#include "database.h"
#include "ipc.h"
#include "protocol.h"
#include "scheduler.h"
#include "task-manager.h"
#include "web-auth.h"
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
extern const ProtocolOps* smb1_protocol_ops (void);
extern const ProtocolOps* baidu_protocol_ops (void);
extern int baidu_protocol_configure (const char* transferDirectory, int maxParallelTasks, int maxFiles,
                                     int signCacheSeconds, const char* apiBase, const char* pcsBase);

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
    CredentialStore* credentials;
    Scheduler* scheduler;
    TaskManager* manager;
    Ipc*       ipc;
    Web*       web;
    WebAuth*   webAuth;
} Daemon;

static int daemon_run_task (DownloadTask* task, void* userData)
{
    Daemon* daemon = userData;
    return task_manager_execute (daemon->manager, task);
}

static int daemon_web_reply (WebResponse* response, int status, const char* body)
{
    response->status = status;
    response->contentType = "application/json";
    response->body = c_strdup (body);
    response->bodySize = response->body == NULL ? 0 : strlen (response->body);
    response->bodyOwned = response->body != NULL;
    return response->body == NULL ? -1 : 0;
}

static int daemon_web_json_string (const CJsonValue* root, const char* key, char* destination, size_t capacity)
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

static int daemon_web_login (Daemon* daemon, const WebRequest* request, WebResponse* response)
{
    CJsonDocument* document;
    CJsonValue*    root;
    char           username[DOWNLOAD_TASK_UUID_SIZE];
    char           password[DOWNLOAD_TASK_UUID_SIZE];
    char           session[DOWNLOADER_WEB_SESSION_SIZE];
    char           csrf[DOWNLOADER_WEB_CSRF_SIZE];
    char           body[256];
    size_t         errorOffset;

    if (!web_auth_enabled (daemon->webAuth))
        return daemon_web_reply (response, 404, "{\"ok\":false,\"error\":\"auth_disabled\"}");
    document = c_json_document_new_from_data (
        c_string_view_from_data (request->body == NULL ? "" : request->body, request->bodySize), C_JSON_PARSE_NONE,
        &errorOffset);
    if (document == NULL) {
        return daemon_web_reply (response, 400, "{\"ok\":false,\"error\":\"invalid_json\"}");
    }
    root = c_json_document_root (document);
    if (c_json_value_type (root) != C_JSON_TYPE_OBJECT ||
        daemon_web_json_string (root, "username", username, sizeof (username)) != 0 ||
        daemon_web_json_string (root, "password", password, sizeof (password)) != 0) {
        c_json_document_free (document);
        return daemon_web_reply (response, 400, "{\"ok\":false,\"error\":\"missing_credentials\"}");
    }
    c_json_document_free (document);
    if (web_auth_login (daemon->webAuth, username, password, session, sizeof (session), csrf, sizeof (csrf)) != 0)
        return daemon_web_reply (response, 401, "{\"ok\":false,\"error\":\"invalid_credentials\"}");
    (void)snprintf (response->setCookie, sizeof (response->setCookie),
                    "downloader_session=%s; HttpOnly; SameSite=Lax; Path=/", session);
    (void)snprintf (body, sizeof (body), "{\"ok\":true,\"csrf_token\":\"%s\"}", csrf);
    return daemon_web_reply (response, 200, body);
}

static int daemon_web_logout (Daemon* daemon, const WebRequest* request, WebResponse* response)
{
    char session[DOWNLOADER_WEB_SESSION_SIZE];

    if (web_auth_cookie (request->cookie, session, sizeof (session)) == 0)
        (void)web_auth_logout (daemon->webAuth, session);
    (void)snprintf (response->setCookie, sizeof (response->setCookie),
                    "downloader_session=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0");
    return daemon_web_reply (response, 200, "{\"ok\":true}");
}

static int daemon_web_session (Daemon* daemon, const WebRequest* request, WebResponse* response)
{
    char session[DOWNLOADER_WEB_SESSION_SIZE];
    char csrf[DOWNLOADER_WEB_CSRF_SIZE];
    char body[256];

    if (!web_auth_enabled (daemon->webAuth))
        return daemon_web_reply (response, 200, "{\"ok\":true,\"auth\":false}");
    if (web_auth_cookie (request->cookie, session, sizeof (session)) != 0 ||
        web_auth_session (daemon->webAuth, session, csrf, sizeof (csrf)) != 0)
        return daemon_web_reply (response, 401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    (void)snprintf (body, sizeof (body), "{\"ok\":true,\"auth\":true,\"csrf_token\":\"%s\"}", csrf);
    return daemon_web_reply (response, 200, body);
}

static int daemon_handle_web_request (const WebRequest* request, WebResponse* response, void* userData)
{
    Daemon* daemon = (Daemon*)userData;

    if (strcmp (request->path, "/api/login") == 0)
        return daemon_web_login (daemon, request, response);
    if (strcmp (request->path, "/api/logout") == 0)
        return daemon_web_logout (daemon, request, response);
    if (strcmp (request->path, "/api/session") == 0)
        return daemon_web_session (daemon, request, response);
    if (web_auth_enabled (daemon->webAuth) && strncmp (request->path, "/api/", 5) == 0) {
        char session[DOWNLOADER_WEB_SESSION_SIZE];
        char csrf[DOWNLOADER_WEB_CSRF_SIZE];
        bool mutating = strcmp (request->method, "POST") == 0 || strcmp (request->method, "PUT") == 0 ||
                        strcmp (request->method, "PATCH") == 0 || strcmp (request->method, "DELETE") == 0;
        if (web_auth_cookie (request->cookie, session, sizeof (session)) != 0 ||
            web_auth_session (daemon->webAuth, session, csrf, sizeof (csrf)) != 0)
            return daemon_web_reply (response, 401, "{\"ok\":false,\"error\":\"unauthorized\"}");
        if (mutating && !web_auth_csrf_matches (daemon->webAuth, csrf, request->csrfToken))
            return daemon_web_reply (response, 403, "{\"ok\":false,\"error\":\"csrf_failed\"}");
    }
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

static int daemon_json_get_optional_string (const CJsonValue* root, const char* key, char* destination, size_t capacity)
{
    CJsonValue* value = c_json_object_get (root, c_string_view_from_cstr (key));
    CStringView text;

    destination[0] = '\0';
    if (value == NULL)
        return 0;
    text = c_json_value_get_string (value);
    if (c_json_value_type (value) != C_JSON_TYPE_STRING || text.size >= capacity ||
        memchr (text.data, '\0', text.size) != NULL) {
        errno = EINVAL;
        return -1;
    }
    memcpy (destination, text.data, text.size);
    destination[text.size] = '\0';
    return 0;
}

static int daemon_append_plain (char* buffer, size_t capacity, size_t* offset, const char* text)
{
    size_t length = strlen (text);

    if (*offset + length + 1U > capacity)
        return -1;
    memcpy (buffer + *offset, text, length);
    *offset += length;
    buffer[*offset] = '\0';
    return 0;
}

static int daemon_append_json_string (char* buffer, size_t capacity, size_t* offset, const char* value)
{
    if (*offset + 1U > capacity)
        return -1;
    buffer[(*offset)++] = '"';
    for (const unsigned char* letter = (const unsigned char*)value; *letter != '\0'; letter++) {
        const char* escape = NULL;
        char        unicode[8];

        switch (*letter) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (*letter < 32U) {
                (void)snprintf (unicode, sizeof (unicode), "\\u%04x", *letter);
                escape = unicode;
            }
            break;
        }
        size_t length = escape == NULL ? 1U : strlen (escape);
        if (*offset + length + 1U > capacity)
            return -1;
        if (escape == NULL)
            buffer[(*offset)++] = (char)*letter;
        else {
            memcpy (buffer + *offset, escape, length);
            *offset += length;
        }
    }
    if (*offset + 1U > capacity)
        return -1;
    buffer[(*offset)++] = '"';
    buffer[*offset] = '\0';
    return 0;
}

static int daemon_create_response (char* response, size_t capacity, const TaskCreateResult* created)
{
    char   part[512];
    size_t offset = 0;

    if (daemon_append_plain (response, capacity, &offset, "{\"ok\":true,\"id\":") != 0)
        return -1;
    if (created->returnedCount > 0) {
        if (daemon_append_json_string (response, capacity, &offset, created->tasks[0].uuid) != 0)
            return -1;
    } else if (daemon_append_plain (response, capacity, &offset, "null") != 0) {
        return -1;
    }
    if (created->groupUuid[0] != '\0') {
        if (daemon_append_plain (response, capacity, &offset, ",\"group\":{\"id\":") != 0 ||
            daemon_append_json_string (response, capacity, &offset, created->groupUuid) != 0 ||
            daemon_append_plain (response, capacity, &offset, ",\"name\":") != 0 ||
            daemon_append_json_string (response, capacity, &offset, created->groupName) != 0)
            return -1;
        (void)snprintf (part, sizeof (part), ",\"task_count\":%zu}", created->taskCount);
        if (daemon_append_plain (response, capacity, &offset, part) != 0)
            return -1;
    }
    (void)snprintf (part, sizeof (part), ",\"skipped\":%zu,\"truncated\":%s}", created->skippedCount,
                    created->truncated ? "true" : "false");
    if (daemon_append_plain (response, capacity, &offset, part) != 0)
        return -1;
    return (int)offset;
}

static int daemon_create_error_response (char* response, size_t capacity, const char* code, const char* message)
{
    size_t offset = 0;

    if (daemon_append_plain (response, capacity, &offset, "{\"ok\":false,\"error\":") != 0 ||
        daemon_append_json_string (response, capacity, &offset, code == NULL ? "create_failed" : code) != 0 ||
        daemon_append_plain (response, capacity, &offset, ",\"message\":") != 0 ||
        daemon_append_json_string (response, capacity, &offset, message == NULL ? "" : message) != 0 ||
        daemon_append_plain (response, capacity, &offset, "}") != 0)
        return -1;
    return (int)offset;
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
        char             url[DOWNLOAD_TASK_URL_SIZE];
        char             fileName[DOWNLOAD_TASK_FILE_NAME_SIZE];
        char             directory[DOWNLOAD_TASK_PATH_SIZE];
        const char*      extra = "{}";
        char*            extraText = NULL;
        TaskCreateResult created;

        memset (&created, 0, sizeof (created));
        if (daemon_json_get_string (root, "url", url, sizeof (url)) != 0 ||
            daemon_json_get_optional_string (root, "file_name", fileName, sizeof (fileName)) != 0 ||
            daemon_json_get_optional_string (root, "target_directory", directory, sizeof (directory)) != 0) {
            result = snprintf (response, responseCapacity, "{\"ok\":false,\"error\":\"missing_task_field\"}");
        } else {
            CJsonValue* extraValue = c_json_object_get (root, c_string_view_from_cstr ("extra"));
            if (extraValue != NULL)
                extraText = c_json_value_to_data (extraValue, C_JSON_WRITE_NONE, NULL);
            if (extraText != NULL)
                extra = extraText;
            if (task_manager_create_expanded (daemon->manager, url, fileName, directory, extra, &created) != 0)
                result = daemon_create_error_response (response, responseCapacity, created.errorCode,
                                                       created.errorMessage);
            else
                result = daemon_create_response (response, responseCapacity, &created);
            c_free (created.tasks);
            c_free (extraText);
        }
    } else if (strcmp (operationText, "start") == 0 || strcmp (operationText, "pause") == 0 ||
               strcmp (operationText, "resume") == 0 || strcmp (operationText, "finish") == 0 ||
               strcmp (operationText, "delete") == 0) {
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
        } else if (strcmp (operationText, "finish") == 0) {
            operationResult = task_manager_finish (daemon->manager, uuid);
            result = snprintf (response, responseCapacity, operationResult == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"finish_failed\"}");
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
    daemon.credentials = credential_store_open (config_database (daemon.config),
                                                config_secret_key_file (daemon.config));
    if (daemon.credentials == NULL) {
        fprintf (stderr, "downloader: open credential store failed: %s\n", strerror (errno));
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    daemon.scheduler = scheduler_new ((size_t)config_max_concurrent_tasks (daemon.config), daemon_run_task, &daemon);
    if (daemon.scheduler == NULL || scheduler_start (daemon.scheduler) != 0) {
        fprintf (stderr, "downloader: start scheduler failed: %s\n", strerror (errno));
        scheduler_stop (daemon.scheduler);
        credential_store_close (daemon.credentials);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    if (baidu_protocol_configure (config_baidu_transfer_directory (daemon.config),
                                  config_baidu_max_parallel_tasks (daemon.config),
                                  config_baidu_max_files (daemon.config),
                                  config_baidu_sign_cache_seconds (daemon.config),
                                  config_baidu_api_base (daemon.config),
                                  config_baidu_pcs_base (daemon.config)) != 0) {
        fprintf (stderr, "downloader: configure baidu failed: %s\n", strerror (errno));
        scheduler_stop (daemon.scheduler);
        credential_store_close (daemon.credentials);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    if (protocol_register (baidu_protocol_ops ()) != 0 || protocol_register (aria_protocol_ops ()) != 0 ||
        protocol_register (media_protocol_ops ()) != 0 || protocol_register (smb_protocol_ops ()) != 0 ||
        protocol_register (smb1_protocol_ops ()) != 0 || protocol_register (http_protocol_ops ()) != 0) {
        fprintf (stderr, "downloader: register protocol failed: %s\n", strerror (errno));
        scheduler_stop (daemon.scheduler);
        credential_store_close (daemon.credentials);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    daemon.manager = task_manager_new (daemon.database, daemon.scheduler);
    if (daemon.manager == NULL) {
        fprintf (stderr, "downloader: create task manager failed: %s\n", strerror (errno));
        scheduler_stop (daemon.scheduler);
        credential_store_close (daemon.credentials);
        database_close (daemon.database);
        config_free (daemon.config);
        return 1;
    }
    if (task_manager_set_download_directory (daemon.manager, config_download_dir (daemon.config)) != 0 ||
        task_manager_set_transfer_options (daemon.manager, config_max_retries (daemon.config),
                                          config_connect_timeout (daemon.config), config_transfer_timeout (daemon.config)) != 0 ||
        task_manager_set_request_policy (daemon.manager, daemon.credentials,
                                         config_default_auth_profile (daemon.config),
                                         config_default_proxy_profile (daemon.config), config_proxy_url (daemon.config),
                                         config_proxy_username (daemon.config),
                                         config_proxy_password (daemon.config)) != 0) {
        scheduler_stop (daemon.scheduler);
        task_manager_free (daemon.manager);
        credential_store_close (daemon.credentials);
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
        credential_store_close (daemon.credentials);
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
        daemon.webAuth = web_auth_new (config_web_username (daemon.config), config_web_password (daemon.config),
                                       config_web_session_seconds (daemon.config));
        daemon.web = daemon.webAuth == NULL ? NULL : web_new (&options, daemon_handle_web_request, &daemon);
        if (daemon.webAuth == NULL || daemon.web == NULL || web_start (daemon.web) != 0) {
            fprintf (stderr, "downloader: start web failed: %s\n", strerror (errno));
            web_free (daemon.web);
            web_auth_free (daemon.webAuth);
            ipc_stop (daemon.ipc);
            ipc_free (daemon.ipc);
            if (task_manager_stop (daemon.manager) != 0)
                fprintf (stderr, "downloader: save paused tasks failed: %s\n", strerror (errno));
            scheduler_stop (daemon.scheduler);
            protocol_finish_all ();
            task_manager_free (daemon.manager);
            credential_store_close (daemon.credentials);
            database_close (daemon.database);
            config_free (daemon.config);
            return 1;
        }
    }

    if (task_manager_resume_queued (daemon.manager) != 0)
        fprintf (stderr, "downloader: resume queued tasks failed: %s\n", strerror (errno));

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
    web_auth_free (daemon.webAuth);
    scheduler_stop (daemon.scheduler);
    protocol_finish_all ();
    task_manager_free (daemon.manager);
    credential_store_close (daemon.credentials);
    database_close (daemon.database);
    config_free (daemon.config);

    return 0;
}
