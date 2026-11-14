#include "web-api.h"

#include <clib/json.h>
#include <clib/memory.h>
#include <clib/string-view.h>
#include <clib/string-util.h>
#include <clib/directory.h>
#include <clib/path.h>
#include <clib/utf8.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define WEB_API_RESPONSE_CAPACITY (1024U * 1024U)

static int web_api_set_body (WebResponse* response, int status, const char* body)
{
    response->status = status;
    response->contentType = "application/json";
    response->body = c_strdup (body);
    response->bodySize = response->body == NULL ? 0 : strlen (response->body);
    response->bodyOwned = response->body != NULL;
    return response->body == NULL ? -1 : 0;
}

static int web_api_set_error (WebResponse* response, int status, const char* error)
{
    char body[512];

    if (snprintf (body, sizeof (body), "{\"ok\":false,\"error\":\"%s\"}", error) < 0)
        return -1;
    return web_api_set_body (response, status, body);
}

static int web_api_append (char* buffer, size_t capacity, size_t* offset, const char* format, ...);
static int web_api_append_json_string (char* buffer, size_t capacity, size_t* offset, const char* value);

static int web_api_set_error_message (WebResponse* response, int status, const char* error, const char* message)
{
    char   buffer[2048];
    size_t size = 0;

    if (web_api_append (buffer, sizeof (buffer), &size, "{\"ok\":false,\"error\":") != 0 ||
        web_api_append_json_string (buffer, sizeof (buffer), &size, error == NULL ? "" : error) != 0 ||
        web_api_append (buffer, sizeof (buffer), &size, ",\"message\":") != 0 ||
        web_api_append_json_string (buffer, sizeof (buffer), &size, message == NULL ? "" : message) != 0 ||
        web_api_append (buffer, sizeof (buffer), &size, "}") != 0)
        return -1;
    return web_api_set_body (response, status, buffer);
}

static int web_api_create_error_status (const char* code)
{
    static const struct
    {
        const char* code;
        int         status;
    } mapping[] = {
        { "invalid_url", 400 },
        { "baidu_password_required", 400 },
        { "baidu_password_invalid", 400 },
        { "auth_kind_unsupported", 400 },
        { "unsupported_protocol", 400 },
        { "baidu_share_empty", 400 },
        { "baidu_auth_missing", 401 },
        { "baidu_auth_expired", 403 },
        { "baidu_challenge_required", 403 },
        { "baidu_share_not_found", 404 },
        { "baidu_share_expired", 404 },
        { "baidu_share_path_not_found", 404 },
        { "baidu_file_not_found", 404 },
        { "auth_profile_unavailable", 404 },
        { "baidu_transfer_conflict", 409 },
        { "target_files_exist", 409 },
        { "baidu_share_too_large", 413 },
        { "baidu_network_error", 502 },
        { "baidu_transfer_failed", 502 },
        { "baidu_dlink_failed", 502 }
    };

    if (code != NULL) {
        for (size_t index = 0; index < sizeof (mapping) / sizeof (mapping[0]); index++) {
            if (strcmp (code, mapping[index].code) == 0)
                return mapping[index].status;
        }
    }
    return 500;
}

static int web_api_append (char* buffer, size_t capacity, size_t* offset, const char* format, ...)
{
    va_list arguments;
    int     written;

    va_start (arguments, format);
    written = vsnprintf (buffer + *offset, capacity - *offset, format, arguments);
    va_end (arguments);
    if (written < 0 || (size_t)written >= capacity - *offset) {
        errno = ENOSPC;
        return -1;
    }
    *offset += (size_t)written;
    return 0;
}

static int web_api_append_json_string (char* buffer, size_t capacity, size_t* offset, const char* value)
{
    const unsigned char* text = (const unsigned char*)(value == NULL ? "" : value);
    bool validUtf8 = c_utf8_validate (c_string_view_from_cstr ((const char*)text));

    if (web_api_append (buffer, capacity, offset, "\"") != 0)
        return -1;
    while (*text != '\0') {
        switch (*text) {
        case '"':
            if (web_api_append (buffer, capacity, offset, "\\\"") != 0)
                return -1;
            break;
        case '\\':
            if (web_api_append (buffer, capacity, offset, "\\\\") != 0)
                return -1;
            break;
        case '\n':
            if (web_api_append (buffer, capacity, offset, "\\n") != 0)
                return -1;
            break;
        case '\r':
            if (web_api_append (buffer, capacity, offset, "\\r") != 0)
                return -1;
            break;
        case '\t':
            if (web_api_append (buffer, capacity, offset, "\\t") != 0)
                return -1;
            break;
        default:
            if (*text < 0x20U || (!validUtf8 && *text > 0x7eU)) {
                if (web_api_append (buffer, capacity, offset, "\\u%04x", *text) != 0)
                    return -1;
            } else if (web_api_append (buffer, capacity, offset, "%c", *text) != 0) {
                return -1;
            }
            break;
        }
        text++;
    }
    return web_api_append (buffer, capacity, offset, "\"");
}

/* URL userinfo can carry passwords for SMB/FTP; never return the password
 * through the API even though the task row stores the original URL. */
static int web_api_append_json_url (char* buffer, size_t capacity, size_t* offset, const char* url)
{
    const char* scheme = strstr (url, "://");
    const char* authority;
    const char* authorityEnd;
    size_t      authorityLength;
    const char* at;
    const char* colon;
    char        redacted[DOWNLOAD_TASK_URL_SIZE + 8];
    size_t      prefix;
    size_t      rest;

    if (scheme == NULL)
        return web_api_append_json_string (buffer, capacity, offset, url);
    authority = scheme + 3;
    authorityEnd = strpbrk (authority, "/?#");
    authorityLength = authorityEnd == NULL ? strlen (authority) : (size_t)(authorityEnd - authority);
    at = (const char*)memchr (authority, '@', authorityLength);
    if (at == NULL)
        return web_api_append_json_string (buffer, capacity, offset, url);
    colon = (const char*)memchr (authority, ':', (size_t)(at - authority));
    if (colon == NULL)
        return web_api_append_json_string (buffer, capacity, offset, url);
    prefix = (size_t)(colon - url) + 1U;
    rest = strlen (at);
    if (prefix + 3U + rest >= sizeof (redacted))
        return web_api_append_json_string (buffer, capacity, offset, url);
    memcpy (redacted, url, prefix);
    memcpy (redacted + prefix, "***", 3U);
    memcpy (redacted + prefix + 3U, at, rest + 1U);
    return web_api_append_json_string (buffer, capacity, offset, redacted);
}

static int web_api_append_task (char* buffer, size_t capacity, size_t* offset, const DownloadTask* task)
{
    if (web_api_append (buffer, capacity, offset, "{\"id\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->uuid) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"file_name\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->fileName) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"target_directory\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->targetDirectory) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"resource_url\":") != 0 ||
        web_api_append_json_url (buffer, capacity, offset, task->resourceUrl) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"protocol\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->protocol) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"state\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, download_task_state_name (task->state)) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"error_message\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->errorMessage) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"group_id\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->groupUuid) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"group_name\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->groupName) != 0 ||
        web_api_append (buffer, capacity, offset,
                        ",\"total_bytes\":%llu,\"downloaded_bytes\":%llu,\"progress\":%.3f,"
                        "\"speed_bytes_sec\":%llu,\"retry_count\":%d,\"max_retries\":%d,\"created_at\":%lld,"
                        "\"completed_at\":%lld,\"duration_seconds\":%.3f,\"gap_count\":%u}",
                        (unsigned long long)task->totalBytes, (unsigned long long)task->downloadedBytes, task->progress,
                        (unsigned long long)task->speedBytesSec, task->retryCount, task->maxRetries,
                        (long long)task->createdAt, (long long)task->completedAt,
                        (double)task->durationMillis / 1000.0, task->gapCount) != 0)
        return -1;
    return 0;
}

static int web_api_response_from_buffer (WebResponse* response, int status, char* buffer, size_t size)
{
    response->status = status;
    response->contentType = "application/json";
    response->body = buffer;
    response->bodySize = size;
    response->bodyOwned = true;
    return 0;
}

static int web_api_parse_query_size (const char* query, const char* key, size_t fallback, size_t maximum, size_t* result)
{
    const char* token = query;
    size_t      keySize = strlen (key);

    *result = fallback;
    while (token != NULL && *token != '\0') {
        const char* value = token;
        const char* end = strchr (token, '&');
        size_t      tokenSize = end == NULL ? strlen (token) : (size_t)(end - token);
        char        number[32];
        char*       numberEnd;
        unsigned long long parsed;

        if (tokenSize > keySize + 1U && strncmp (value, key, keySize) == 0 && value[keySize] == '=') {
            size_t numberSize = tokenSize - keySize - 1U;
            if (numberSize == 0 || numberSize >= sizeof (number)) {
                errno = EINVAL;
                return -1;
            }
            memcpy (number, value + keySize + 1U, numberSize);
            number[numberSize] = '\0';
            errno = 0;
            parsed = strtoull (number, &numberEnd, 10);
            if (errno != 0 || numberEnd == number || *numberEnd != '\0' || parsed == 0 || parsed > maximum) {
                errno = EINVAL;
                return -1;
            }
            *result = (size_t)parsed;
            return 0;
        }
        token = end == NULL ? NULL : end + 1;
    }
    return 0;
}

static int web_api_list (TaskManager* manager, const WebRequest* request, WebResponse* response)
{
    DownloadTask* tasks;
    size_t        page = 1;
    size_t        pageSize = 20;
    size_t        count;
    size_t        total;
    size_t        offset = 0;
    char*         buffer;
    size_t        size = 0;
    size_t        index;

    if (request->query != NULL && request->query[0] != '\0') {
        if (web_api_parse_query_size (request->query, "page", 1, SIZE_MAX, &page) != 0)
            return web_api_set_error (response, 400, "invalid_page");
        if (web_api_parse_query_size (request->query, "page_size", 20, 100, &pageSize) != 0)
            return web_api_set_error (response, 400, "invalid_page_size");
    }
    if (task_manager_list (manager, page, pageSize, &tasks, &count, &total) != 0)
        return web_api_set_error (response, 500, "list_failed");
    buffer = (char*)c_malloc (WEB_API_RESPONSE_CAPACITY);
    if (buffer == NULL) {
        c_free (tasks);
        return -1;
    }
    if (total > 0)
        offset = (total + pageSize - 1U) / pageSize;
    if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"ok\":true,\"page\":%zu,\"page_size\":%zu,\"total\":%zu,\"pages\":%zu,\"tasks\":[", page,
                        pageSize, total, offset) != 0)
        goto failed;
    for (index = 0; index < count; index++) {
        if (index > 0 && web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",") != 0)
            goto failed;
        if (web_api_append_task (buffer, WEB_API_RESPONSE_CAPACITY, &size, &tasks[index]) != 0)
            goto failed;
    }
    if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "],\"groups\":[") != 0)
        goto failed;
    {
        bool firstGroup = true;
        for (size_t taskIndex = 0; taskIndex < count; taskIndex++) {
            bool                      duplicate = false;
            DownloadTaskGroupSummary  summary;

            if (tasks[taskIndex].groupUuid[0] == '\0')
                continue;
            for (size_t previous = 0; previous < taskIndex; previous++) {
                if (strcmp (tasks[previous].groupUuid, tasks[taskIndex].groupUuid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;
            if (task_manager_group_summary (manager, tasks[taskIndex].groupUuid, &summary) != 0)
                goto failed;
            if ((!firstGroup && web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",") != 0) ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"id\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, summary.groupUuid) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"name\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, summary.groupName) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size,
                                ",\"task_count\":%zu,\"completed\":%zu,\"failed\":%zu,\"total_bytes\":%llu,"
                                "\"downloaded_bytes\":%llu}",
                                summary.taskCount, summary.completedCount, summary.failedCount,
                                (unsigned long long)summary.totalBytes,
                                (unsigned long long)summary.downloadedBytes) != 0)
                goto failed;
            firstGroup = false;
        }
    }
    if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "]}") != 0)
        goto failed;
    c_free (tasks);
    return web_api_response_from_buffer (response, 200, buffer, size);
failed:
    c_free (tasks);
    c_free (buffer);
    return web_api_set_error (response, 500, "response_too_large");
}

static int web_api_extract_string (const CJsonValue* root, const char* key, char* destination, size_t capacity)
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

static int web_api_create (TaskManager* manager, const WebRequest* request, WebResponse* response)
{
    CJsonDocument*   document;
    CJsonValue*      root;
    TaskCreateResult created;
    char             url[DOWNLOAD_TASK_URL_SIZE];
    char             fileName[DOWNLOAD_TASK_FILE_NAME_SIZE];
    char             directory[DOWNLOAD_TASK_PATH_SIZE];
    const char*      extra = "{}";
    char*            extraText = NULL;
    size_t           errorOffset;
    char*            buffer;
    size_t           size = 0;

    document = c_json_document_new_from_data (c_string_view_from_data (request->body, request->bodySize), C_JSON_PARSE_NONE,
                                              &errorOffset);
    if (document == NULL)
        return web_api_set_error (response, 400, "invalid_json");
    root = c_json_document_root (document);
    if (web_api_extract_string (root, "url", url, sizeof (url)) != 0) {
        c_json_document_free (document);
        return web_api_set_error (response, 400, "missing_task_field");
    }
    fileName[0] = directory[0] = '\0';
    const char* keys[] = { "file_name", "target_directory" };
    char* destinations[] = { fileName, directory };
    size_t capacities[] = { sizeof (fileName), sizeof (directory) };
    for (size_t i = 0; i < 2; i++) {
        CJsonValue* value = c_json_object_get (root, c_string_view_from_cstr (keys[i]));
        if (value == NULL)
            continue;
        CStringView text = c_json_value_get_string (value);
        if (c_json_value_type (value) != C_JSON_TYPE_STRING || text.size >= capacities[i] ||
            memchr (text.data, '\0', text.size) != NULL) {
            c_json_document_free (document);
            return web_api_set_error (response, 400, "invalid_task_field");
        }
        memcpy (destinations[i], text.data, text.size);
        destinations[i][text.size] = '\0';
    }
    {
        CJsonValue* extraValue = c_json_object_get (root, c_string_view_from_cstr ("extra"));
        if (extraValue != NULL) {
            extraText = c_json_value_to_data (extraValue, C_JSON_WRITE_NONE, NULL);
            if (extraText == NULL) {
                c_json_document_free (document);
                return web_api_set_error (response, 400, "invalid_extra");
            }
            extra = extraText;
        }
    }
    memset (&created, 0, sizeof (created));
    if (task_manager_create_expanded (manager, url, fileName, directory, extra, &created) != 0) {
        int status = web_api_create_error_status (created.errorCode);
        c_free (extraText);
        c_json_document_free (document);
        c_free (created.tasks);
        return web_api_set_error_message (response, status,
                                          created.errorCode[0] ? created.errorCode : "create_failed",
                                          created.errorMessage);
    }
    c_free (extraText);
    c_json_document_free (document);
    buffer = (char*)c_malloc (WEB_API_RESPONSE_CAPACITY);
    if (buffer == NULL) {
        c_free (created.tasks);
        return -1;
    }
    if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"ok\":true") != 0)
        goto failed;
    if (created.groupUuid[0] != '\0') {
        if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"group\":{\"id\":") != 0 ||
            web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, created.groupUuid) != 0 ||
            web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"name\":") != 0 ||
            web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, created.groupName) != 0 ||
            web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size,
                            ",\"task_count\":%zu,\"skipped\":%zu,\"truncated\":%s}", created.taskCount,
                            created.skippedCount, created.truncated ? "true" : "false") != 0)
            goto failed;
    }
    if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"tasks\":[") != 0)
        goto failed;
    for (size_t index = 0; index < created.returnedCount; index++) {
        if ((index > 0 && web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",") != 0) ||
            web_api_append_task (buffer, WEB_API_RESPONSE_CAPACITY, &size, &created.tasks[index]) != 0)
            goto failed;
    }
    if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "],\"task\":") != 0)
        goto failed;
    if (created.returnedCount > 0) {
        if (web_api_append_task (buffer, WEB_API_RESPONSE_CAPACITY, &size, &created.tasks[0]) != 0)
            goto failed;
    } else if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "null") != 0) {
        goto failed;
    }
    if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "}") != 0)
        goto failed;
    c_free (created.tasks);
    return web_api_response_from_buffer (response, 201, buffer, size);
failed:
    c_free (created.tasks);
    c_free (buffer);
    return web_api_set_error (response, 500, "response_too_large");
}

static int web_api_task_action (TaskManager* manager, const char* uuid, const char* action, WebResponse* response)
{
    int result;

    if (strcmp (action, "start") == 0)
        result = task_manager_start (manager, uuid);
    else if (strcmp (action, "pause") == 0)
        result = task_manager_pause (manager, uuid);
    else if (strcmp (action, "resume") == 0)
        result = task_manager_resume (manager, uuid);
    else if (strcmp (action, "finish") == 0)
        result = task_manager_finish (manager, uuid);
    else {
        errno = ENOENT;
        return web_api_set_error (response, 404, "not_found");
    }
    if (result != 0)
        return web_api_set_error (response, errno == ENOENT ? 404 : errno == ENOTSUP ? 400 : 400,
                                  errno == ENOTSUP ? "finish_unsupported" : "task_action_failed");
    return web_api_set_body (response, 200, "{\"ok\":true}");
}

static int web_api_directories (TaskManager* manager, const WebRequest* request, WebResponse* response)
{
    size_t errorOffset;
    CJsonDocument* input = c_json_document_new_from_data (
        c_string_view_from_data (request->body, request->bodySize), C_JSON_PARSE_NONE, &errorOffset);
    if (input == NULL)
        return web_api_set_error (response, 400, "invalid_json");
    CJsonValue* value = c_json_object_get (c_json_document_root (input), c_string_view_from_cstr ("path"));
    CStringView view = c_json_value_get_string (value);
    if (c_json_value_type (c_json_document_root (input)) != C_JSON_TYPE_OBJECT ||
        (value != NULL && (c_json_value_type (value) != C_JSON_TYPE_STRING || view.size >= DOWNLOAD_TASK_PATH_SIZE ||
         memchr (view.data, '\0', view.size) != NULL))) {
        c_json_document_free (input);
        return web_api_set_error (response, 400, "invalid_path");
    }
    char* requested = view.size ? c_string_view_to_cstr (view) : c_strdup (task_manager_download_directory (manager));
    char* path = requested ? c_path_canonicalize (requested, NULL) : NULL;
    c_free (requested);
    c_json_document_free (input);
    if (path == NULL)
        return web_api_set_error (response, 500, "directory_failed");
    CDirectory* directory = c_directory_open (path);
    if (directory == NULL) {
        c_free (path);
        return web_api_set_error (response, 400, "无法打开文件夹");
    }
    CJsonDocument* output = c_json_document_new ();
    CJsonValue* root = c_json_value_new_object (output);
    CJsonValue* entries = NULL;
    char* parent = c_path_dirname (path);
    int result = root && parent ? 0 : -1;
    if (result == 0)
        result = c_json_document_set_root (output, root);
    if (result == 0)
        result = c_json_object_set (c_json_document_root (output), c_string_view_from_cstr ("ok"), c_json_value_new_bool (output, true));
    if (result == 0)
        result = c_json_object_set (c_json_document_root (output), c_string_view_from_cstr ("path"), c_json_value_new_string (output, c_string_view_from_cstr (path)));
    if (result == 0)
        result = c_json_object_set (c_json_document_root (output), c_string_view_from_cstr ("parent"), c_json_value_new_string (output, c_string_view_from_cstr (parent)));
    if (result == 0)
        result = c_json_object_set (c_json_document_root (output), c_string_view_from_cstr ("directories"), c_json_value_new_array (output));
    const char* name;
    int next = 0;
    size_t count = 0;
    while (result == 0 && (next = c_directory_next (directory, &name)) == 1) {
        char* child = c_path_join (path, name);
        struct stat status;
        if (child == NULL) {
            result = -1;
            break;
        }
        if (stat (child, &status) == 0 && S_ISDIR (status.st_mode)) {
            if (++count > 10000)
                result = -1;
            else {
                entries = c_json_object_get (c_json_document_root (output), c_string_view_from_cstr ("directories"));
                result = c_json_array_append (entries, c_json_value_new_string (output, c_string_view_from_cstr (name)));
            }
        }
        c_free (child);
    }
    char* body = result == 0 && next >= 0 ? c_json_document_to_data (output, C_JSON_WRITE_NONE, NULL) : NULL;
    c_json_document_free (output);
    c_directory_free (directory);
    c_free (parent);
    c_free (path);
    if (body == NULL)
        return web_api_set_error (response, 500, "directory_failed");
    return web_api_response_from_buffer (response, 200, body, strlen (body));
}

static int web_api_optional_string (const CJsonValue* root, const char* key, char* destination, size_t capacity)
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

static int web_api_parse_object (const WebRequest* request, CJsonDocument** document)
{
    size_t errorOffset;

    *document = c_json_document_new_from_data (
        c_string_view_from_data (request->body == NULL ? "" : request->body, request->bodySize), C_JSON_PARSE_NONE,
        &errorOffset);
    if (*document == NULL || c_json_value_type (c_json_document_root (*document)) != C_JSON_TYPE_OBJECT) {
        if (*document != NULL)
            c_json_document_free (*document);
        *document = NULL;
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int web_api_auth_profiles (TaskManager* manager, const WebRequest* request, WebResponse* response)
{
    CredentialStore* store = task_manager_credential_store (manager);
    if (store == NULL)
        return web_api_set_error (response, 503, "credential_store_unavailable");
    if (strcmp (request->method, "GET") == 0) {
        CredentialProfile* profiles = NULL;
        size_t             count = 0;
        char*              buffer;
        size_t             size = 0;
        if (credential_store_list_credentials (store, &profiles, &count) != 0)
            return web_api_set_error (response, 500, "list_failed");
        buffer = (char*)c_malloc (WEB_API_RESPONSE_CAPACITY);
        if (buffer == NULL) {
            c_free (profiles);
            return -1;
        }
        if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"ok\":true,\"credentials\":[") != 0)
            goto failed;
        for (size_t index = 0; index < count; index++) {
            const char* kind = credential_kind_name (profiles[index].kind);
            if ((index > 0 && web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",") != 0) ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"name\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, profiles[index].name) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"kind\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, kind == NULL ? "" : kind) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"username\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, profiles[index].username) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"scope\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, profiles[index].scope) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "}") != 0)
                goto failed;
        }
        if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "]}") != 0)
            goto failed;
        c_free (profiles);
        return web_api_response_from_buffer (response, 200, buffer, size);
failed:
        c_free (profiles);
        c_free (buffer);
        return web_api_set_error (response, 500, "response_too_large");
    }
    if (strcmp (request->method, "POST") == 0) {
        CJsonDocument*    document;
        CJsonValue*       root;
        CredentialProfile profile;
        char              kindName[32];
        if (web_api_parse_object (request, &document) != 0)
            return web_api_set_error (response, 400, "invalid_json");
        root = c_json_document_root (document);
        memset (&profile, 0, sizeof (profile));
        if (web_api_extract_string (root, "name", profile.name, sizeof (profile.name)) != 0 ||
            web_api_extract_string (root, "kind", kindName, sizeof (kindName)) != 0 ||
            web_api_optional_string (root, "username", profile.username, sizeof (profile.username)) != 0 ||
            web_api_optional_string (root, "secret", profile.secret, sizeof (profile.secret)) != 0 ||
            web_api_optional_string (root, "scope", profile.scope, sizeof (profile.scope)) != 0 ||
            credential_kind_parse (kindName, &profile.kind) != 0) {
            c_json_document_free (document);
            return web_api_set_error (response, 400, "invalid_profile");
        }
        if (credential_store_save_credential (store, &profile) != 0) {
            c_json_document_free (document);
            return web_api_set_error (response, errno == EINVAL ? 400 : 500, "save_failed");
        }
        c_json_document_free (document);
        return web_api_set_body (response, 201, "{\"ok\":true}");
    }
    return web_api_set_error (response, 405, "method_not_allowed");
}

static int web_api_proxy_profiles (TaskManager* manager, const WebRequest* request, WebResponse* response)
{
    CredentialStore* store = task_manager_credential_store (manager);
    if (store == NULL)
        return web_api_set_error (response, 503, "credential_store_unavailable");
    if (strcmp (request->method, "GET") == 0) {
        ProxyProfile* profiles = NULL;
        size_t        count = 0;
        char*         buffer;
        size_t        size = 0;
        if (credential_store_list_proxies (store, &profiles, &count) != 0)
            return web_api_set_error (response, 500, "list_failed");
        buffer = (char*)c_malloc (WEB_API_RESPONSE_CAPACITY);
        if (buffer == NULL) {
            c_free (profiles);
            return -1;
        }
        if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"ok\":true,\"proxies\":[") != 0)
            goto failed;
        for (size_t index = 0; index < count; index++) {
            if ((index > 0 && web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",") != 0) ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"name\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, profiles[index].name) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"url\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, profiles[index].url) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"username\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, profiles[index].username) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "}") != 0)
                goto failed;
        }
        if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "]}") != 0)
            goto failed;
        c_free (profiles);
        return web_api_response_from_buffer (response, 200, buffer, size);
failed:
        c_free (profiles);
        c_free (buffer);
        return web_api_set_error (response, 500, "response_too_large");
    }
    if (strcmp (request->method, "POST") == 0) {
        CJsonDocument* document;
        CJsonValue*    root;
        ProxyProfile   profile;
        if (web_api_parse_object (request, &document) != 0)
            return web_api_set_error (response, 400, "invalid_json");
        root = c_json_document_root (document);
        memset (&profile, 0, sizeof (profile));
        if (web_api_extract_string (root, "name", profile.name, sizeof (profile.name)) != 0 ||
            web_api_extract_string (root, "url", profile.url, sizeof (profile.url)) != 0 ||
            web_api_optional_string (root, "username", profile.username, sizeof (profile.username)) != 0 ||
            web_api_optional_string (root, "secret", profile.secret, sizeof (profile.secret)) != 0) {
            c_json_document_free (document);
            return web_api_set_error (response, 400, "invalid_profile");
        }
        if (credential_store_save_proxy (store, &profile) != 0) {
            c_json_document_free (document);
            return web_api_set_error (response, errno == EINVAL ? 400 : 500, "save_failed");
        }
        c_json_document_free (document);
        return web_api_set_body (response, 201, "{\"ok\":true}");
    }
    return web_api_set_error (response, 405, "method_not_allowed");
}

static int web_api_sites (TaskManager* manager, const WebRequest* request, WebResponse* response)
{
    CredentialStore* store = task_manager_credential_store (manager);
    if (store == NULL)
        return web_api_set_error (response, 503, "credential_store_unavailable");
    if (strcmp (request->method, "GET") == 0) {
        SiteProfile* sites = NULL;
        size_t       count = 0;
        char*        buffer;
        size_t       size = 0;
        if (credential_store_list_sites (store, &sites, &count) != 0)
            return web_api_set_error (response, 500, "list_failed");
        buffer = (char*)c_malloc (WEB_API_RESPONSE_CAPACITY);
        if (buffer == NULL) {
            c_free (sites);
            return -1;
        }
        if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"ok\":true,\"sites\":[") != 0)
            goto failed;
        for (size_t index = 0; index < count; index++) {
            if ((index > 0 && web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",") != 0) ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"domain\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, sites[index].domain) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"auth_profile\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, sites[index].authProfile) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, ",\"proxy_profile\":") != 0 ||
                web_api_append_json_string (buffer, WEB_API_RESPONSE_CAPACITY, &size, sites[index].proxyProfile) != 0 ||
                web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "}") != 0)
                goto failed;
        }
        if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "]}") != 0)
            goto failed;
        c_free (sites);
        return web_api_response_from_buffer (response, 200, buffer, size);
failed:
        c_free (sites);
        c_free (buffer);
        return web_api_set_error (response, 500, "response_too_large");
    }
    if (strcmp (request->method, "POST") == 0) {
        CJsonDocument* document;
        CJsonValue*    root;
        char           domain[DOWNLOADER_CREDENTIAL_DOMAIN_SIZE];
        char           authProfile[DOWNLOADER_CREDENTIAL_NAME_SIZE];
        char           proxyProfile[DOWNLOADER_CREDENTIAL_NAME_SIZE];
        if (web_api_parse_object (request, &document) != 0)
            return web_api_set_error (response, 400, "invalid_json");
        root = c_json_document_root (document);
        if (web_api_extract_string (root, "domain", domain, sizeof (domain)) != 0 ||
            web_api_optional_string (root, "auth_profile", authProfile, sizeof (authProfile)) != 0 ||
            web_api_optional_string (root, "proxy_profile", proxyProfile, sizeof (proxyProfile)) != 0 ||
            credential_store_set_site (store, domain, authProfile, proxyProfile) != 0) {
            c_json_document_free (document);
            return web_api_set_error (response, 400, "invalid_site");
        }
        c_json_document_free (document);
        return web_api_set_body (response, 201, "{\"ok\":true}");
    }
    return web_api_set_error (response, 405, "method_not_allowed");
}

static int web_api_profile_delete (TaskManager* manager, const char* name, bool credential, WebResponse* response)
{
    CredentialStore* store = task_manager_credential_store (manager);

    if (store == NULL)
        return web_api_set_error (response, 503, "credential_store_unavailable");
    if (name == NULL || name[0] == '\0' || strlen (name) >= DOWNLOADER_CREDENTIAL_NAME_SIZE || strchr (name, '/'))
        return web_api_set_error (response, 404, "not_found");
    if ((credential ? credential_store_delete_credential (store, name) : credential_store_delete_proxy (store, name)) !=
        0)
        return web_api_set_error (response, errno == ENOENT ? 404 : 500, "delete_failed");
    return web_api_set_body (response, 200, "{\"ok\":true}");
}

static int web_api_group_action (TaskManager* manager, const WebRequest* request, WebResponse* response)
{
    static const char prefix[] = "/api/groups/";
    const char*       identifier = request->path + strlen (prefix);
    const char*       suffix = strchr (identifier, '/');
    char              uuid[DOWNLOAD_TASK_GROUP_UUID_SIZE];
    size_t            uuidLength = suffix == NULL ? strlen (identifier) : (size_t)(suffix - identifier);
    DownloadGroupAction action;
    size_t            affected = 0;
    char              body[64];

    if (uuidLength == 0 || uuidLength >= sizeof (uuid))
        return web_api_set_error (response, 404, "not_found");
    memcpy (uuid, identifier, uuidLength);
    uuid[uuidLength] = '\0';
    if (strcmp (request->method, "DELETE") == 0) {
        if (suffix != NULL)
            return web_api_set_error (response, 405, "method_not_allowed");
        action = DOWNLOAD_GROUP_ACTION_DELETE;
    } else if (strcmp (request->method, "POST") == 0) {
        if (suffix == NULL)
            return web_api_set_error (response, 405, "method_not_allowed");
        if (strcmp (suffix + 1, "start") == 0)
            action = DOWNLOAD_GROUP_ACTION_START;
        else if (strcmp (suffix + 1, "pause") == 0)
            action = DOWNLOAD_GROUP_ACTION_PAUSE;
        else if (strcmp (suffix + 1, "resume") == 0)
            action = DOWNLOAD_GROUP_ACTION_RESUME;
        else
            return web_api_set_error (response, 404, "not_found");
    } else {
        return web_api_set_error (response, 405, "method_not_allowed");
    }
    if (task_manager_group_action (manager, uuid, action, &affected) != 0)
        return web_api_set_error (response, 500, "group_action_failed");
    (void)snprintf (body, sizeof (body), "{\"ok\":true,\"affected\":%zu}", affected);
    return web_api_set_body (response, 200, body);
}

int web_api_handle (TaskManager* manager, const WebRequest* request, WebResponse* response)
{
    const char* prefix = "/api/tasks/";
    const char* identifier;
    size_t      prefixSize = strlen (prefix);
    DownloadTask task;
    char        uuid[DOWNLOAD_TASK_UUID_SIZE];
    size_t      deleted;

    if (manager == NULL || request == NULL || response == NULL || request->method == NULL || request->path == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset (response, 0, sizeof (*response));
    if (strncmp (request->path, "/api/auth-profiles", 18) == 0) {
        const char* name = request->path + 18;
        if (name[0] == '\0')
            return web_api_auth_profiles (manager, request, response);
        if (name[0] == '/' && strcmp (request->method, "DELETE") == 0)
            return web_api_profile_delete (manager, name + 1, true, response);
        return web_api_set_error (response, 404, "not_found");
    }
    if (strncmp (request->path, "/api/proxy-profiles", 19) == 0) {
        const char* name = request->path + 19;
        if (name[0] == '\0')
            return web_api_proxy_profiles (manager, request, response);
        if (name[0] == '/' && strcmp (request->method, "DELETE") == 0)
            return web_api_profile_delete (manager, name + 1, false, response);
        return web_api_set_error (response, 404, "not_found");
    }
    if (strcmp (request->path, "/api/sites") == 0)
        return web_api_sites (manager, request, response);
    if (strcmp (request->path, "/api/directories") == 0) {
        if (strcmp (request->method, "POST") != 0)
            return web_api_set_error (response, 405, "method_not_allowed");
        return web_api_directories (manager, request, response);
    }
    if (strncmp (request->path, "/api/groups/", 12) == 0)
        return web_api_group_action (manager, request, response);
    if (strcmp (request->path, "/api/tasks") == 0) {
        if (strcmp (request->method, "GET") == 0)
            return web_api_list (manager, request, response);
        if (strcmp (request->method, "POST") == 0)
            return web_api_create (manager, request, response);
        return web_api_set_error (response, 405, "method_not_allowed");
    }
    if (strcmp (request->path, "/api/tasks/clear-completed") == 0 && strcmp (request->method, "POST") == 0) {
        if (task_manager_clear_completed (manager, &deleted) != 0)
            return web_api_set_error (response, 500, "clear_failed");
        return web_api_set_body (response, 200, "{\"ok\":true}");
    }
    if (strncmp (request->path, prefix, prefixSize) != 0)
        return web_api_set_error (response, 404, "not_found");
    identifier = request->path + prefixSize;
    if (strlen (identifier) >= sizeof (uuid))
        return web_api_set_error (response, 404, "not_found");
    (void)snprintf (uuid, sizeof (uuid), "%s", identifier);
    if (strcmp (request->method, "GET") == 0) {
        char*  buffer = (char*)c_malloc (WEB_API_RESPONSE_CAPACITY);
        size_t size = 0;
        if (task_manager_get (manager, uuid, &task) != 0)
            return web_api_set_error (response, 404, "task_not_found");
        if (buffer == NULL || web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"ok\":true,\"task\":") != 0 ||
            web_api_append_task (buffer, WEB_API_RESPONSE_CAPACITY, &size, &task) != 0 ||
            web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "}") != 0) {
            c_free (buffer);
            return web_api_set_error (response, 500, "response_too_large");
        }
        return web_api_response_from_buffer (response, 200, buffer, size);
    }
    if (strcmp (request->method, "DELETE") == 0) {
        if (task_manager_delete (manager, uuid) != 0)
            return web_api_set_error (response, 404, "task_not_found");
        return web_api_set_body (response, 200, "{\"ok\":true}");
    }
    if (strcmp (request->method, "POST") == 0) {
        const char* suffix = strrchr (identifier, '/');
        if (suffix == NULL)
            return web_api_set_error (response, 404, "not_found");
        if ((size_t)(suffix - identifier) >= sizeof (uuid))
            return web_api_set_error (response, 404, "not_found");
        memcpy (uuid, identifier, (size_t)(suffix - identifier));
        uuid[suffix - identifier] = '\0';
        return web_api_task_action (manager, uuid, suffix + 1, response);
    }
    return web_api_set_error (response, 405, "method_not_allowed");
}
