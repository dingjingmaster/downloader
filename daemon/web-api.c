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

static int web_api_append_task (char* buffer, size_t capacity, size_t* offset, const DownloadTask* task)
{
    if (web_api_append (buffer, capacity, offset, "{\"id\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->uuid) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"file_name\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->fileName) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"target_directory\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->targetDirectory) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"resource_url\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->resourceUrl) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"protocol\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->protocol) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"state\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, download_task_state_name (task->state)) != 0 ||
        web_api_append (buffer, capacity, offset, ",\"error_message\":") != 0 ||
        web_api_append_json_string (buffer, capacity, offset, task->errorMessage) != 0 ||
        web_api_append (buffer, capacity, offset,
                        ",\"total_bytes\":%llu,\"downloaded_bytes\":%llu,\"progress\":%.3f,"
                        "\"speed_bytes_sec\":%llu,\"retry_count\":%d,\"max_retries\":%d,\"created_at\":%lld,\"completed_at\":%lld}",
                        (unsigned long long)task->totalBytes, (unsigned long long)task->downloadedBytes, task->progress,
                        (unsigned long long)task->speedBytesSec, task->retryCount, task->maxRetries,
                        (long long)task->createdAt, (long long)task->completedAt) != 0)
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
    CJsonDocument* document;
    CJsonValue*    root;
    DownloadTask   task;
    char           url[DOWNLOAD_TASK_URL_SIZE];
    char           fileName[DOWNLOAD_TASK_FILE_NAME_SIZE];
    char           directory[DOWNLOAD_TASK_PATH_SIZE];
    char           protocol[DOWNLOAD_TASK_PROTOCOL_SIZE];
    const char*    extra = "{}";
    char*          extraText = NULL;
    size_t         errorOffset;
    char*          buffer;
    size_t         size = 0;

    document = c_json_document_new_from_data (c_string_view_from_data (request->body, request->bodySize), C_JSON_PARSE_NONE,
                                              &errorOffset);
    if (document == NULL)
        return web_api_set_error (response, 400, "invalid_json");
    root = c_json_document_root (document);
    if (web_api_extract_string (root, "url", url, sizeof (url)) != 0) {
        c_json_document_free (document);
        return web_api_set_error (response, 400, "missing_task_field");
    }
    fileName[0] = directory[0] = protocol[0] = '\0';
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
    if (task_manager_create (manager, url, fileName, directory, protocol, extra, &task) != 0) {
        c_free (extraText);
        c_json_document_free (document);
        return web_api_set_error (response, errno == EINVAL ? 400 : 500, "create_failed");
    }
    c_free (extraText);
    c_json_document_free (document);
    buffer = (char*)c_malloc (WEB_API_RESPONSE_CAPACITY);
    if (buffer == NULL)
        return -1;
    if (web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "{\"ok\":true,\"task\":") != 0 ||
        web_api_append_task (buffer, WEB_API_RESPONSE_CAPACITY, &size, &task) != 0 ||
        web_api_append (buffer, WEB_API_RESPONSE_CAPACITY, &size, "}") != 0) {
        c_free (buffer);
        return web_api_set_error (response, 500, "response_too_large");
    }
    return web_api_response_from_buffer (response, 201, buffer, size);
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
    else {
        errno = ENOENT;
        return web_api_set_error (response, 404, "not_found");
    }
    if (result != 0)
        return web_api_set_error (response, errno == ENOENT ? 404 : 400, "task_action_failed");
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
    if (strcmp (request->path, "/api/directories") == 0) {
        if (strcmp (request->method, "POST") != 0)
            return web_api_set_error (response, 405, "method_not_allowed");
        return web_api_directories (manager, request, response);
    }
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
