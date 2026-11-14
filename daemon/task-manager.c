#define _POSIX_C_SOURCE 200809L

#include "task-manager.h"

#include "filesystem.h"
#include "protocol.h"
#include "request-context.h"
#include <sys/stat.h>

#include <clib/memory.h>
#include <clib/mutex.h>
#include <clib/cond.h>
#include <clib/thread.h>
#include <clib/uri.h>
#include <clib/directory.h>
#include <clib/json.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool task_manager_terminated (const char* value, size_t capacity);

typedef struct ActiveTask ActiveTask;
struct ActiveTask
{
    DownloadTask task;
    bool         finishRequested;
    ActiveTask*  next;
};

typedef struct RetryEntry RetryEntry;
struct RetryEntry
{
    char        uuid[DOWNLOAD_TASK_UUID_SIZE];
    int64_t     dueAt;
    RetryEntry* next;
};

struct TaskManager
{
    Database* database;
    Scheduler* scheduler;
    CMutex    mutex;
    uint64_t  sequence;
    ActiveTask* active;
    CCond idle;
    char defaultDirectory[DOWNLOAD_TASK_PATH_SIZE];
    bool stopping;
    int maxRetries;
    int connectTimeout;
    int transferTimeout;
    CCond       retryCond;
    CThread*    retryThread;
    bool        retryStopping;
    RetryEntry* retries;
    CredentialStore* credentialStore;
    char defaultAuthProfile[DOWNLOADER_CREDENTIAL_NAME_SIZE];
    char defaultProxyProfile[DOWNLOADER_CREDENTIAL_NAME_SIZE];
    char proxyUrl[DOWNLOADER_CREDENTIAL_URL_SIZE];
    char proxyUsername[DOWNLOADER_CREDENTIAL_USER_SIZE];
    char proxyPassword[DOWNLOADER_CREDENTIAL_SECRET_SIZE];
};

int task_manager_set_transfer_options (TaskManager* manager, int maxRetries, int connectTimeout, int transferTimeout)
{
    if (manager == NULL || maxRetries < 0 || connectTimeout < 0 || transferTimeout < 0) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    manager->maxRetries = maxRetries;
    manager->connectTimeout = connectTimeout;
    manager->transferTimeout = transferTimeout;
    c_mutex_unlock (&manager->mutex);
    return 0;
}

int task_manager_set_request_policy (TaskManager* manager, CredentialStore* store, const char* defaultAuthProfile,
                                     const char* defaultProxyProfile, const char* proxyUrl,
                                     const char* proxyUsername, const char* proxyPassword)
{
    if (manager == NULL || defaultAuthProfile == NULL || defaultProxyProfile == NULL || proxyUrl == NULL ||
        proxyUsername == NULL || proxyPassword == NULL ||
        strlen (defaultAuthProfile) >= sizeof (manager->defaultAuthProfile) ||
        strlen (defaultProxyProfile) >= sizeof (manager->defaultProxyProfile) ||
        strlen (proxyUrl) >= sizeof (manager->proxyUrl) ||
        strlen (proxyUsername) >= sizeof (manager->proxyUsername) ||
        strlen (proxyPassword) >= sizeof (manager->proxyPassword)) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    manager->credentialStore = store;
    snprintf (manager->defaultAuthProfile, sizeof (manager->defaultAuthProfile), "%s", defaultAuthProfile);
    snprintf (manager->defaultProxyProfile, sizeof (manager->defaultProxyProfile), "%s", defaultProxyProfile);
    snprintf (manager->proxyUrl, sizeof (manager->proxyUrl), "%s", proxyUrl);
    snprintf (manager->proxyUsername, sizeof (manager->proxyUsername), "%s", proxyUsername);
    snprintf (manager->proxyPassword, sizeof (manager->proxyPassword), "%s", proxyPassword);
    c_mutex_unlock (&manager->mutex);
    return 0;
}

static bool task_manager_curl_protocol (const char* protocol)
{
    return strcmp (protocol, "http") == 0 || strcmp (protocol, "https") == 0 || strcmp (protocol, "ftp") == 0 ||
           strcmp (protocol, "ftps") == 0 || strcmp (protocol, "sftp") == 0 || strcmp (protocol, "baidu") == 0;
}

int task_manager_set_download_directory (TaskManager* manager, const char* directory)
{
    if (manager == NULL || directory == NULL || !directory[0] || strlen (directory) >= sizeof (manager->defaultDirectory)) {
        errno = EINVAL;
        return -1;
    }
    snprintf (manager->defaultDirectory, sizeof (manager->defaultDirectory), "%s", directory);
    return 0;
}

static ActiveTask* task_manager_find_active (TaskManager* manager, const char* uuid)
{
    for (ActiveTask* active = manager->active; active != NULL; active = active->next) {
        if (strcmp (active->task.uuid, uuid) == 0)
            return active;
    }
    return NULL;
}

static bool task_manager_active (TaskManager* manager, const char* uuid)
{
    return task_manager_find_active (manager, uuid) != NULL;
}

const char* task_manager_download_directory (const TaskManager* manager)
{
    return manager == NULL ? NULL : manager->defaultDirectory;
}

CredentialStore* task_manager_credential_store (const TaskManager* manager)
{
    return manager == NULL ? NULL : manager->credentialStore;
}

static void task_manager_release (TaskManager* manager, DownloadTask* task)
{
    ActiveTask** slot = &manager->active;
    while (*slot != NULL && &(*slot)->task != task)
        slot = &(*slot)->next;
    if (*slot != NULL)
        *slot = (*slot)->next;
    c_free (task);
    c_cond_notify_all (&manager->idle);
}

static bool task_manager_has_parent_reference (const char* value)
{
    return value != NULL && (strstr (value, "../") != NULL || strstr (value, "/..") != NULL || strcmp (value, "..") == 0);
}

static int task_manager_validate (const char* resourceUrl, const char* fileName, const char* targetDirectory,
                                  const char* protocol)
{
    if (resourceUrl == NULL || resourceUrl[0] == '\0' || fileName == NULL || fileName[0] == '\0' ||
        targetDirectory == NULL || targetDirectory[0] == '\0' || protocol == NULL || protocol[0] == '\0' ||
        strchr (fileName, '/') != NULL || strchr (fileName, '\\') != NULL || task_manager_has_parent_reference (fileName) ||
        task_manager_has_parent_reference (targetDirectory)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int64_t task_manager_now (void)
{
    return (int64_t)time (NULL);
}

static int task_manager_update_state (TaskManager* manager, const char* uuid, DownloadTaskState state)
{
    DownloadTask task;
    int          result;

    if (database_load (manager->database, uuid, &task) != 0)
        return -1;
    if (state == DOWNLOAD_TASK_STATE_QUEUED && task.state == DOWNLOAD_TASK_STATE_CREATED)
        task.startedAt = task_manager_now ();
    if (state == DOWNLOAD_TASK_STATE_PAUSED)
        task.pausedAt = task_manager_now ();
    task.updatedAt = task_manager_now ();
    task.state = state;
    result = database_update (manager->database, &task);
    return result;
}

static void task_manager_retry_remove_locked (TaskManager* manager, const char* uuid)
{
    RetryEntry** slot = &manager->retries;
    while (*slot != NULL) {
        RetryEntry* entry = *slot;
        if (strcmp (entry->uuid, uuid) == 0) {
            *slot = entry->next;
            c_free (entry);
        } else {
            slot = &entry->next;
        }
    }
}

static int64_t task_manager_retry_delay (int retryCount)
{
    int     shift = retryCount <= 1 ? 0 : retryCount - 1;
    int64_t delay;

    if (shift > 6)
        shift = 6;
    delay = (int64_t)1 << shift;
    return delay > 60 ? 60 : delay;
}

/* Caller holds manager->mutex. */
static void task_manager_schedule_retry_locked (TaskManager* manager, const char* uuid, int retryCount)
{
    RetryEntry* entry;

    task_manager_retry_remove_locked (manager, uuid);
    entry = (RetryEntry*)c_malloc0 (sizeof (*entry));
    if (entry == NULL)
        return;
    snprintf (entry->uuid, sizeof (entry->uuid), "%s", uuid);
    entry->dueAt = task_manager_now () + task_manager_retry_delay (retryCount);
    entry->next = manager->retries;
    manager->retries = entry;
    c_cond_notify_all (&manager->retryCond);
}

static bool task_manager_error_retryable (int error)
{
    return error == ETIMEDOUT || error == ECONNRESET || error == ECONNREFUSED || error == EHOSTUNREACH ||
           error == ENETUNREACH || error == ENETDOWN || error == EPIPE || error == EAGAIN ||
           error == ECONNABORTED;
}

/* Caller holds manager->mutex. */
static int task_manager_submit_active_locked (TaskManager* manager, const DownloadTask* task)
{
    ActiveTask* queued;

    if (manager->scheduler == NULL) {
        errno = EINVAL;
        return -1;
    }
    queued = (ActiveTask*)c_malloc0 (sizeof (*queued));
    if (queued == NULL)
        return -1;
    queued->task = *task;
    queued->finishRequested = false;
    queued->next = manager->active;
    manager->active = queued;
    if (scheduler_submit (manager->scheduler, &queued->task) != 0) {
        manager->active = queued->next;
        c_free (queued);
        return -1;
    }
    return 0;
}

static void task_manager_retry_thread (void* userData)
{
    TaskManager* manager = (TaskManager*)userData;

    c_mutex_lock (&manager->mutex);
    while (!manager->retryStopping) {
        int64_t     now = task_manager_now ();
        RetryEntry** slot = &manager->retries;
        while (*slot != NULL) {
            RetryEntry* entry = *slot;
            bool        reschedule = false;
            if (entry->dueAt > now) {
                slot = &entry->next;
                continue;
            }
            *slot = entry->next;
            if (!manager->stopping && manager->scheduler != NULL) {
                DownloadTask task;
                if (database_load (manager->database, entry->uuid, &task) == 0 &&
                    task.state == DOWNLOAD_TASK_STATE_QUEUED && !task_manager_active (manager, entry->uuid) &&
                    task_manager_submit_active_locked (manager, &task) != 0)
                    reschedule = true;
            }
            if (reschedule) {
                entry->dueAt = now + 5;
                entry->next = manager->retries;
                manager->retries = entry;
            } else {
                c_free (entry);
            }
        }
        c_cond_wait_timeout (&manager->retryCond, &manager->mutex, 500);
    }
    c_mutex_unlock (&manager->mutex);
}

TaskManager* task_manager_new (Database* database, Scheduler* scheduler)
{
    TaskManager* manager;

    if (database == NULL) {
        errno = EINVAL;
        return NULL;
    }
    manager = (TaskManager*)c_malloc0 (sizeof (*manager));
    if (manager == NULL)
        return NULL;
    if (!c_mutex_init (&manager->mutex)) {
        c_free (manager);
        errno = ENOMEM;
        return NULL;
    }
    manager->database = database;
    manager->maxRetries = 3;
    manager->connectTimeout = 30;
    strcpy (manager->defaultDirectory, ".");
    if (!c_cond_init (&manager->idle)) {
        c_mutex_clear (&manager->mutex);
        c_free (manager);
        return NULL;
    }
    manager->scheduler = scheduler;
    if (!c_cond_init (&manager->retryCond)) {
        c_cond_clear (&manager->idle);
        c_mutex_clear (&manager->mutex);
        c_free (manager);
        errno = ENOMEM;
        return NULL;
    }
    manager->retryThread = c_thread_new_named ("downloader-retry", task_manager_retry_thread, manager);
    if (manager->retryThread == NULL) {
        c_cond_clear (&manager->retryCond);
        c_cond_clear (&manager->idle);
        c_mutex_clear (&manager->mutex);
        c_free (manager);
        errno = ENOMEM;
        return NULL;
    }
    return manager;
}

void task_manager_free (TaskManager* manager)
{
    if (manager == NULL)
        return;
    if (manager->retryThread != NULL) {
        c_mutex_lock (&manager->mutex);
        manager->retryStopping = true;
        c_cond_notify_all (&manager->retryCond);
        c_mutex_unlock (&manager->mutex);
        (void)c_thread_join (manager->retryThread);
        (void)c_thread_free (manager->retryThread);
        manager->retryThread = NULL;
    }
    while (manager->retries != NULL) {
        RetryEntry* entry = manager->retries;
        manager->retries = entry->next;
        c_free (entry);
    }
    c_cond_clear (&manager->retryCond);
    c_cond_clear (&manager->idle);
    c_mutex_clear (&manager->mutex);
    c_free (manager);
}

static int task_manager_infer_name (const char* resourceUrl, const char* fallback, char* fileName, size_t capacity)
{
    CUri        uri;
    size_t      uriError;
    CStringView basename;
    char*       decoded = NULL;
    size_t      decodedSize;

    if (resourceUrl == NULL || fileName == NULL || capacity == 0 ||
        c_uri_parse (c_string_view_from_cstr (resourceUrl), &uri, &uriError) != 0) {
        errno = EINVAL;
        return -1;
    }
    basename = uri.path;
    for (size_t index = 0; index < uri.path.size; index++) {
        if (uri.path.data[index] == '/') {
            basename.data = uri.path.data + index + 1;
            basename.size = uri.path.size - index - 1;
        }
    }
    if (c_uri_unescape_string (basename, &decoded, &decodedSize) != 0)
        return -1;
    if (decodedSize >= capacity || memchr (decoded, '\0', decodedSize) != NULL) {
        c_free (decoded);
        errno = EINVAL;
        return -1;
    }
    (void)snprintf (fileName, capacity, "%s", decodedSize ? decoded : fallback);
    c_free (decoded);
    return 0;
}

static void task_manager_set_create_error (TaskCreateResult* result, const char* code, const char* message)
{
    if (result == NULL)
        return;
    (void)snprintf (result->errorCode, sizeof (result->errorCode), "%s", code == NULL ? "" : code);
    (void)snprintf (result->errorMessage, sizeof (result->errorMessage), "%s", message == NULL ? "" : message);
}

static void task_manager_next_id (TaskManager* manager, const char* prefix, char* destination, size_t capacity)
{
    int64_t now = task_manager_now ();

    c_mutex_lock (&manager->mutex);
    manager->sequence++;
    (void)snprintf (destination, capacity, "%s-%lld-%ld-%llu", prefix, (long long)now, (long)getpid (),
                    (unsigned long long)manager->sequence);
    c_mutex_unlock (&manager->mutex);
}

static bool task_manager_valid_filename (const char* name)
{
    if (name == NULL || name[0] == '\0' || strchr (name, '/') != NULL || strchr (name, '\\') != NULL ||
        strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
        return false;
    for (const unsigned char* letter = (const unsigned char*)name; *letter; letter++) {
        if (*letter < 32 || *letter == 127)
            return false;
    }
    return true;
}

static bool task_manager_valid_subdirectory (const char* path)
{
    const char* part;

    if (path == NULL || path[0] == '/' || strchr (path, '\\') != NULL)
        return false;
    if (path[0] == '\0')
        return true;
    part = path;
    while (part != NULL && *part != '\0') {
        const char* separator = strchr (part, '/');
        size_t      length = separator == NULL ? strlen (part) : (size_t)(separator - part);
        if (length == 0 || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.'))
            return false;
        for (size_t index = 0; index < length; index++) {
            if ((unsigned char)part[index] < 32 || (unsigned char)part[index] == 127)
                return false;
        }
        part = separator == NULL ? NULL : separator + 1;
    }
    return true;
}

/* Copies explicit auth/proxy profile names from the request into an item's JSON
 * object so every expanded task resolves the same policy. */
static int task_manager_item_extra (const char* requestExtra, char* itemExtra, size_t capacity)
{
    static const char* const keys[] = { "auth_profile", "proxy_profile" };
    CJsonDocument* requestDocument = NULL;
    CJsonDocument* itemDocument = NULL;
    CJsonValue*    requestRoot = NULL;
    CJsonValue*    itemRoot = NULL;
    char*          text = NULL;
    size_t         errorOffset;
    int            result = -1;

    requestDocument = c_json_document_new_from_data (
        c_string_view_from_cstr (requestExtra == NULL || requestExtra[0] == '\0' ? "{}" : requestExtra),
        C_JSON_PARSE_NONE, &errorOffset);
    itemDocument = c_json_document_new_from_data (
        c_string_view_from_cstr (itemExtra == NULL || itemExtra[0] == '\0' ? "{}" : itemExtra),
        C_JSON_PARSE_NONE, &errorOffset);
    if (requestDocument == NULL || itemDocument == NULL) {
        errno = EINVAL;
        goto done;
    }
    requestRoot = c_json_document_root (requestDocument);
    itemRoot = c_json_document_root (itemDocument);
    if (c_json_value_type (requestRoot) != C_JSON_TYPE_OBJECT ||
        c_json_value_type (itemRoot) != C_JSON_TYPE_OBJECT) {
        errno = EINVAL;
        goto done;
    }
    for (size_t index = 0; index < sizeof (keys) / sizeof (keys[0]); index++) {
        CJsonValue* value;
        CStringView textValue;

        requestRoot = c_json_document_root (requestDocument);
        itemRoot = c_json_document_root (itemDocument);
        value = c_json_object_get (requestRoot, c_string_view_from_cstr (keys[index]));
        if (value == NULL || c_json_object_get (itemRoot, c_string_view_from_cstr (keys[index])) != NULL)
            continue;
        textValue = c_json_value_get_string (value);
        if (textValue.data == NULL || textValue.size == 0)
            continue;
        if (c_json_object_set (itemRoot, c_string_view_from_cstr (keys[index]),
                               c_json_value_new_string (itemDocument, textValue)) != 0) {
            errno = ENOMEM;
            goto done;
        }
    }
    text = c_json_document_to_data (itemDocument, C_JSON_WRITE_NONE, NULL);
    if (text == NULL || strlen (text) >= capacity) {
        errno = text == NULL ? ENOMEM : ENAMETOOLONG;
        goto done;
    }
    (void)snprintf (itemExtra, capacity, "%s", text);
    result = 0;
done:
    c_free (text);
    c_json_document_free (requestDocument);
    c_json_document_free (itemDocument);
    return result;
}

int task_manager_create (TaskManager* manager, const char* resourceUrl, const char* fileName, const char* targetDirectory,
                         const char* protocol, const char* extraJson, DownloadTask* result)
{
    DownloadTask task;
    int64_t      now;
    char inferredProtocol[DOWNLOAD_TASK_PROTOCOL_SIZE];
    char inferredName[DOWNLOAD_TASK_FILE_NAME_SIZE];

    if (manager == NULL || resourceUrl == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_detect (resourceUrl, inferredProtocol, sizeof (inferredProtocol)) != 0)
        return -1;
    protocol = inferredProtocol;
    if (targetDirectory == NULL || !targetDirectory[0])
        targetDirectory = manager->defaultDirectory;
    if (fileName == NULL || !fileName[0]) {
        if (task_manager_infer_name (resourceUrl, "download.bin", inferredName, sizeof (inferredName)) != 0)
            return -1;
        fileName = inferredName;
    }

    if (manager == NULL || result == NULL || task_manager_validate (resourceUrl, fileName, targetDirectory, protocol) != 0)
        return -1;
    if (strlen (resourceUrl) >= sizeof (task.resourceUrl) || strlen (fileName) >= sizeof (task.fileName) ||
        strlen (targetDirectory) >= sizeof (task.targetDirectory) ||
        (extraJson != NULL && strlen (extraJson) >= sizeof (task.extraJson))) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset (&task, 0, sizeof (task));
    now = task_manager_now ();
    c_mutex_lock (&manager->mutex);
    manager->sequence++;
    (void)snprintf (task.uuid, sizeof (task.uuid), "task-%lld-%ld-%llu", (long long)now, (long)getpid(),
                    (unsigned long long)manager->sequence);
    (void)snprintf (task.fileName, sizeof (task.fileName), "%s", fileName);
    (void)snprintf (task.targetDirectory, sizeof (task.targetDirectory), "%s", targetDirectory);
    (void)snprintf (task.resourceUrl, sizeof (task.resourceUrl), "%s", resourceUrl);
    (void)snprintf (task.protocol, sizeof (task.protocol), "%s", protocol);
    (void)snprintf (task.extraJson, sizeof (task.extraJson), "%s", extraJson == NULL ? "{}" : extraJson);
    task.state = DOWNLOAD_TASK_STATE_CREATED;
    task.createdAt = now;
    task.updatedAt = now;
    task.maxRetries = manager->maxRetries;
    if (filesystem_prepare (&task) != 0 || database_insert (manager->database, &task) != 0) {
        int savedErrno = errno;
        c_mutex_unlock (&manager->mutex);
        errno = savedErrno;
        return -1;
    }
    *result = task;
    c_mutex_unlock (&manager->mutex);
    return 0;
}

static const char* task_manager_create_error_code (int error)
{
    switch (error) {
    case EINVAL:
        return "invalid_url";
    case ENOTSUP:
        return "unsupported_protocol";
    case ENOENT:
        return "auth_profile_unavailable";
    case ENOMEM:
        return "out_of_memory";
    default:
        return "auth_profile_unavailable";
    }
}

int task_manager_create_expanded (TaskManager* manager, const char* resourceUrl, const char* fileName,
                                  const char* targetDirectory, const char* extraJson, TaskCreateResult* result)
{
    DownloadTask     request;
    DownloadTask*    tasks = NULL;
    ProtocolItem*    items = NULL;
    size_t           itemCount = 0;
    size_t           used = 0;
    size_t           returned;
    ProtocolError    protocolError;
    RequestContext*  requestContext = NULL;
    const ProtocolOps* ops;
    CredentialStore* store;
    char             protocol[DOWNLOAD_TASK_PROTOCOL_SIZE];
    char             inferredName[DOWNLOAD_TASK_FILE_NAME_SIZE];
    char             baseDirectory[DOWNLOAD_TASK_PATH_SIZE];
    char             groupName[DOWNLOAD_TASK_GROUP_NAME_SIZE];
    char             groupUuid[DOWNLOAD_TASK_GROUP_UUID_SIZE];
    char             defaultAuth[DOWNLOADER_CREDENTIAL_NAME_SIZE];
    char             defaultProxy[DOWNLOADER_CREDENTIAL_NAME_SIZE];
    char             proxyUrl[DOWNLOADER_CREDENTIAL_URL_SIZE];
    char             proxyUser[DOWNLOADER_CREDENTIAL_USER_SIZE];
    char             proxyPass[DOWNLOADER_CREDENTIAL_SECRET_SIZE];
    int64_t          now;
    int              outcome = -1;

    if (manager == NULL || resourceUrl == NULL || result == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset (result, 0, sizeof (*result));
    if (protocol_detect (resourceUrl, protocol, sizeof (protocol)) != 0) {
        task_manager_set_create_error (result, task_manager_create_error_code (errno), strerror (errno));
        return -1;
    }
    if (targetDirectory == NULL || !targetDirectory[0])
        targetDirectory = manager->defaultDirectory;
    if (targetDirectory == NULL || !targetDirectory[0] ||
        strlen (targetDirectory) >= sizeof (baseDirectory)) {
        task_manager_set_create_error (result, "invalid_url", "target directory is invalid");
        errno = EINVAL;
        return -1;
    }
    (void)snprintf (baseDirectory, sizeof (baseDirectory), "%s", targetDirectory);
    if (fileName == NULL || !fileName[0]) {
        if (task_manager_infer_name (resourceUrl, "download.bin", inferredName, sizeof (inferredName)) != 0) {
            task_manager_set_create_error (result, "invalid_url", "cannot infer a file name");
            return -1;
        }
        fileName = inferredName;
    }
    if (strlen (fileName) >= sizeof (request.fileName) ||
        (extraJson != NULL && strlen (extraJson) >= sizeof (request.extraJson))) {
        task_manager_set_create_error (result, "invalid_url", "task field is too long");
        errno = ENAMETOOLONG;
        return -1;
    }
    memset (&request, 0, sizeof (request));
    groupUuid[0] = '\0';
    (void)snprintf (request.resourceUrl, sizeof (request.resourceUrl), "%s", resourceUrl);
    (void)snprintf (request.fileName, sizeof (request.fileName), "%s", fileName);
    (void)snprintf (request.protocol, sizeof (request.protocol), "%s", protocol);
    (void)snprintf (request.targetDirectory, sizeof (request.targetDirectory), "%s", baseDirectory);
    (void)snprintf (request.extraJson, sizeof (request.extraJson), "%s", extraJson == NULL ? "{}" : extraJson);
    request.state = DOWNLOAD_TASK_STATE_CREATED;

    c_mutex_lock (&manager->mutex);
    store = manager->credentialStore;
    request.connectTimeout = manager->connectTimeout;
    request.transferTimeout = manager->transferTimeout;
    (void)snprintf (defaultAuth, sizeof (defaultAuth), "%s", manager->defaultAuthProfile);
    (void)snprintf (defaultProxy, sizeof (defaultProxy), "%s", manager->defaultProxyProfile);
    (void)snprintf (proxyUrl, sizeof (proxyUrl), "%s", manager->proxyUrl);
    (void)snprintf (proxyUser, sizeof (proxyUser), "%s", manager->proxyUsername);
    (void)snprintf (proxyPass, sizeof (proxyPass), "%s", manager->proxyPassword);
    c_mutex_unlock (&manager->mutex);

    if (request_context_resolve (&requestContext, &request, store, defaultAuth, defaultProxy,
                                 proxyUrl[0] ? proxyUrl : NULL, proxyUser, proxyPass) != 0) {
        int savedErrno = errno;
        task_manager_set_create_error (result, task_manager_create_error_code (savedErrno), strerror (savedErrno));
        errno = savedErrno;
        return -1;
    }
    request.requestContext = requestContext;
    ops = protocol_find (resourceUrl, protocol);
    memset (&protocolError, 0, sizeof (protocolError));
    memset (groupName, 0, sizeof (groupName));
    if (ops != NULL && ops->expand != NULL) {
        if (ops->expand (&request, &items, &itemCount, groupName, sizeof (groupName), &protocolError) != 0) {
            task_manager_set_create_error (result,
                                           protocolError.code[0] ? protocolError.code : "expand_failed",
                                           protocolError.message[0] ? protocolError.message : strerror (errno));
            goto cleanup;
        }
    } else {
        items = (ProtocolItem*)c_malloc0 (sizeof (*items));
        if (items == NULL) {
            task_manager_set_create_error (result, "out_of_memory", strerror (ENOMEM));
            goto cleanup;
        }
        itemCount = 1;
        (void)snprintf (items[0].resourceUrl, sizeof (items[0].resourceUrl), "%s", resourceUrl);
        (void)snprintf (items[0].fileName, sizeof (items[0].fileName), "%s", fileName);
        (void)snprintf (items[0].extraJson, sizeof (items[0].extraJson), "%s",
                        extraJson == NULL ? "{}" : extraJson);
    }
    request.requestContext = NULL;
    request_context_free (requestContext);
    requestContext = NULL;
    groupName[sizeof (groupName) - 1] = '\0';
    if (items == NULL || itemCount == 0) {
        task_manager_set_create_error (result, "expand_empty", "resource contains no files");
        errno = ENODATA;
        goto cleanup;
    }
    if (itemCount > 1 || groupName[0] != '\0') {
        task_manager_next_id (manager, "group", groupUuid, sizeof (groupUuid));
        (void)snprintf (result->groupUuid, sizeof (result->groupUuid), "%s", groupUuid);
        (void)snprintf (result->groupName, sizeof (result->groupName), "%s", groupName);
    }
    tasks = (DownloadTask*)c_malloc0 (itemCount * sizeof (*tasks));
    if (tasks == NULL) {
        task_manager_set_create_error (result, "out_of_memory", strerror (ENOMEM));
        goto cleanup;
    }
    now = task_manager_now ();
    for (size_t index = 0; index < itemCount; index++) {
        ProtocolItem* item = &items[index];
        DownloadTask* task = &tasks[used];
        char          directory[DOWNLOAD_TASK_PATH_SIZE];
        struct stat   status;
        int           written;

        if (!task_manager_terminated (item->resourceUrl, sizeof (item->resourceUrl)) ||
            !task_manager_terminated (item->fileName, sizeof (item->fileName)) ||
            !task_manager_terminated (item->targetSubdirectory, sizeof (item->targetSubdirectory)) ||
            !task_manager_terminated (item->extraJson, sizeof (item->extraJson)) ||
            !task_manager_valid_filename (item->fileName) ||
            !task_manager_valid_subdirectory (item->targetSubdirectory) ||
            strlen (item->resourceUrl) >= sizeof (task->resourceUrl)) {
            task_manager_set_create_error (result, "invalid_name", "expanded item has an invalid name or path");
            errno = EINVAL;
            goto cleanup;
        }
        written = item->targetSubdirectory[0] == '\0'
            ? snprintf (directory, sizeof (directory), "%s", baseDirectory)
            : snprintf (directory, sizeof (directory), "%s/%s", baseDirectory, item->targetSubdirectory);
        if (written < 0 || (size_t)written >= sizeof (directory)) {
            task_manager_set_create_error (result, "invalid_name", "expanded item path is too long");
            errno = ENAMETOOLONG;
            goto cleanup;
        }
        memset (task, 0, sizeof (*task));
        task_manager_next_id (manager, "task", task->uuid, sizeof (task->uuid));
        (void)snprintf (task->fileName, sizeof (task->fileName), "%s", item->fileName);
        (void)snprintf (task->targetDirectory, sizeof (task->targetDirectory), "%s", directory);
        (void)snprintf (task->resourceUrl, sizeof (task->resourceUrl), "%s", item->resourceUrl);
        (void)snprintf (task->protocol, sizeof (task->protocol), "%s", protocol);
        if (task_manager_item_extra (request.extraJson, item->extraJson, sizeof (item->extraJson)) != 0) {
            task_manager_set_create_error (result, "invalid_extra", "expanded item extra is invalid");
            errno = EINVAL;
            goto cleanup;
        }
        (void)snprintf (task->extraJson, sizeof (task->extraJson), "%s", item->extraJson);
        task->state = DOWNLOAD_TASK_STATE_CREATED;
        task->createdAt = now;
        task->updatedAt = now;
        task->maxRetries = manager->maxRetries;
        task->totalBytes = item->totalBytes;
        (void)snprintf (task->checksumType, sizeof (task->checksumType), "%s", item->checksumType);
        (void)snprintf (task->checksumValue, sizeof (task->checksumValue), "%s", item->checksumValue);
        if (groupUuid[0] != '\0') {
            (void)snprintf (task->groupUuid, sizeof (task->groupUuid), "%s", groupUuid);
            (void)snprintf (task->groupName, sizeof (task->groupName), "%s", groupName);
        }
        if (filesystem_prepare (task) != 0) {
            task_manager_set_create_error (result, "invalid_path", strerror (errno));
            goto cleanup;
        }
        if (lstat (task->targetPath, &status) == 0) {
            result->skippedCount++;
            continue;
        }
        if (errno != ENOENT) {
            task_manager_set_create_error (result, "target_check_failed", strerror (errno));
            goto cleanup;
        }
        used++;
    }
    if (used == 0) {
        task_manager_set_create_error (result, "target_files_exist", "all target files already exist");
        errno = EEXIST;
        goto cleanup;
    }
    returned = used < DOWNLOADER_CREATE_RESULT_TASKS ? used : DOWNLOADER_CREATE_RESULT_TASKS;
    result->tasks = (DownloadTask*)c_malloc0 (returned * sizeof (*result->tasks));
    if (result->tasks == NULL) {
        task_manager_set_create_error (result, "out_of_memory", strerror (ENOMEM));
        goto cleanup;
    }
    c_mutex_lock (&manager->mutex);
    if (database_insert_many (manager->database, tasks, used) != 0) {
        int savedErrno = errno;
        c_mutex_unlock (&manager->mutex);
        task_manager_set_create_error (result, "insert_failed", strerror (savedErrno));
        errno = savedErrno;
        goto cleanup;
    }
    c_mutex_unlock (&manager->mutex);
    memcpy (result->tasks, tasks, returned * sizeof (*result->tasks));
    result->taskCount = used;
    result->returnedCount = returned;
    result->truncated = used > returned;
    outcome = 0;
cleanup:
    if (outcome != 0 && result->tasks != NULL) {
        c_free (result->tasks);
        result->tasks = NULL;
    }
    c_free (tasks);
    c_free (items);
    request_context_free (requestContext);
    return outcome;
}

int task_manager_group_summary (TaskManager* manager, const char* groupUuid, DownloadTaskGroupSummary* summary)
{
    int result;

    if (manager == NULL || groupUuid == NULL || summary == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    result = database_group_summary (manager->database, groupUuid, summary);
    c_mutex_unlock (&manager->mutex);
    return result;
}

int task_manager_group_action (TaskManager* manager, const char* groupUuid, DownloadGroupAction action,
                               size_t* affected)
{
    DownloadTask* tasks = NULL;
    char (*uuids)[DOWNLOAD_TASK_UUID_SIZE] = NULL;
    size_t count = 0;
    size_t succeeded = 0;
    int    result;

    if (manager == NULL || groupUuid == NULL || affected == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    result = database_list_group (manager->database, groupUuid, &tasks, &count);
    c_mutex_unlock (&manager->mutex);
    if (result != 0)
        return -1;
    if (count > 0) {
        uuids = (char (*)[DOWNLOAD_TASK_UUID_SIZE])c_malloc0 (count * sizeof (*uuids));
        if (uuids == NULL) {
            c_free (tasks);
            return -1;
        }
        for (size_t index = 0; index < count; index++)
            (void)snprintf (uuids[index], sizeof (uuids[index]), "%s", tasks[index].uuid);
    }
    c_free (tasks);
    for (size_t index = 0; index < count; index++) {
        int outcome;
        switch (action) {
        case DOWNLOAD_GROUP_ACTION_START:
            outcome = task_manager_start (manager, uuids[index]);
            break;
        case DOWNLOAD_GROUP_ACTION_PAUSE:
            outcome = task_manager_pause (manager, uuids[index]);
            break;
        case DOWNLOAD_GROUP_ACTION_RESUME:
            outcome = task_manager_resume (manager, uuids[index]);
            break;
        case DOWNLOAD_GROUP_ACTION_DELETE:
            outcome = task_manager_delete (manager, uuids[index]);
            break;
        default:
            outcome = -1;
            errno = EINVAL;
            break;
        }
        if (outcome == 0)
            succeeded++;
    }
    c_free (uuids);
    *affected = succeeded;
    return 0;
}

int task_manager_delete (TaskManager* manager, const char* uuid)
{
    DownloadTask task;
    int          result;

    if (manager == NULL || uuid == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    if (database_load (manager->database, uuid, &task) != 0) {
        result = -1;
    } else {
        task_manager_retry_remove_locked (manager, uuid);
        if (task_manager_active (manager, uuid)) {
            if (task_manager_update_state (manager, uuid, DOWNLOAD_TASK_STATE_CANCELLED) != 0) {
                c_mutex_unlock (&manager->mutex);
                return -1;
            }
            while (task_manager_active (manager, uuid))
                c_cond_wait (&manager->idle, &manager->mutex);
        }
        result = filesystem_remove_partial (&task);
        if (result == 0)
            result = database_delete (manager->database, uuid);
    }
    {
        int savedErrno = errno;
        c_mutex_unlock (&manager->mutex);
        errno = savedErrno;
    }
    return result;
}

int task_manager_start (TaskManager* manager, const char* uuid)
{
    int result;
    DownloadTask* queued;

    if (manager == NULL || uuid == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    if (manager->stopping) {
        c_mutex_unlock (&manager->mutex);
        errno = ESHUTDOWN;
        return -1;
    }
    if (task_manager_active (manager, uuid)) {
        c_mutex_unlock (&manager->mutex);
        errno = EBUSY;
        return -1;
    }
    queued = c_malloc0 (sizeof (ActiveTask));
    if (queued == NULL || database_load (manager->database, uuid, queued) != 0) {
        c_free (queued);
        c_mutex_unlock (&manager->mutex);
        return -1;
    }
    if (queued->state == DOWNLOAD_TASK_STATE_DOWNLOADING || queued->state == DOWNLOAD_TASK_STATE_COMPLETED) {
        c_free (queued);
        c_mutex_unlock (&manager->mutex);
        errno = EBUSY;
        return -1;
    }
    result = task_manager_update_state (manager, uuid, DOWNLOAD_TASK_STATE_QUEUED);
    if (result == 0 && manager->scheduler != NULL) {
        ((ActiveTask*)queued)->next = manager->active;
        manager->active = (ActiveTask*)queued;
        result = scheduler_submit (manager->scheduler, queued);
        if (result != 0) {
            manager->active = ((ActiveTask*)queued)->next;
            database_update (manager->database, queued);
        }
    }
    if (result != 0 || manager->scheduler == NULL)
        c_free (queued);
    c_mutex_unlock (&manager->mutex);
    return result;
}

typedef struct
{
    TaskManager* manager;
    DownloadTask* task;
    ProtocolTask* context;
    const ProtocolOps* ops;
    int errorCode;
    char errorMessage[DOWNLOAD_TASK_ERROR_SIZE];
} DownloadExecution;

static void task_manager_failed (void* userData, int code, const char* message)
{
    DownloadExecution* execution = userData;
    execution->errorCode = code;
    snprintf (execution->errorMessage, sizeof (execution->errorMessage), "%s", message == NULL ? "" : message);
}

/* Caller holds manager->mutex. Control remains on the protocol worker thread. */
static int task_manager_callback_task (DownloadExecution* execution, DownloadTask* current)
{
    if (database_load (execution->manager->database, execution->task->uuid, current) != 0) {
        execution->ops->cancel (execution->context);
        return -1;
    }
    if (current->state == DOWNLOAD_TASK_STATE_CANCELLED) {
        execution->ops->cancel (execution->context);
    } else if (current->state == DOWNLOAD_TASK_STATE_PAUSED || execution->manager->stopping) {
        execution->ops->pause (execution->context);
    } else {
        return 0;
    }
    errno = ECANCELED;
    return -1;
}

static bool task_manager_terminated (const char* value, size_t capacity)
{
    return memchr (value, '\0', capacity) != NULL;
}

/* Truncation discards data that was never proven durable. */
static int task_manager_truncate_file (const char* path, uint64_t size)
{
    struct stat status;
    int         descriptor;
    int         result;

    if (path == NULL || path[0] == '\0' || size > (uint64_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    descriptor = open (path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return -1;
    result = fstat (descriptor, &status);
    if (result == 0 && (!S_ISREG (status.st_mode) || status.st_nlink != 1 || (uint64_t)status.st_size < size)) {
        result = -1;
        errno = EINVAL;
    }
    if (result == 0 && ftruncate (descriptor, (off_t)size) != 0)
        result = -1;
    if (result == 0 && fsync (descriptor) != 0)
        result = -1;
    {
        int savedErrno = errno;
        if (close (descriptor) != 0 && result == 0) {
            savedErrno = errno;
            result = -1;
        }
        errno = savedErrno;
    }
    return result;
}

static int task_manager_discard_checkpoint (TaskManager* manager, DownloadTask* task, bool truncate)
{
    struct stat status;

    if (truncate) {
        if (lstat (task->tempPath, &status) == 0) {
            if (task_manager_truncate_file (task->tempPath, 0) != 0)
                return -1;
        } else if (errno != ENOENT) {
            return -1;
        }
    }
    if (database_delete_checkpoint (manager->database, task->uuid) != 0)
        return -1;
    task->etag[0] = '\0';
    task->lastModified[0] = '\0';
    task->downloadedBytes = 0;
    memset (&task->checkpoint, 0, sizeof (task->checkpoint));
    return 0;
}

/* Bytes beyond the checkpoint were never fsynced, so resuming at the file
 * size could append bytes the failed run never proved durable. */
static int task_manager_restore_checkpoint (TaskManager* manager, DownloadTask* task)
{
    DownloadTaskCheckpoint checkpoint;
    struct stat            status;
    bool                   regular;

    if (database_load_checkpoint (manager->database, task->uuid, &checkpoint) != 0)
        return -1;
    memset (&task->checkpoint, 0, sizeof (task->checkpoint));
    if (checkpoint.version == 0) {
        /* Legacy tasks have no checkpoint and resume through persisted columns. */
        if (stat (task->tempPath, &status) == 0 && S_ISREG (status.st_mode))
            task->downloadedBytes = (uint64_t)status.st_size;
        return 0;
    }
    if (checkpoint.version != DOWNLOAD_TASK_CHECKPOINT_VERSION ||
        !task_manager_terminated (checkpoint.effectiveUrl, sizeof (checkpoint.effectiveUrl)) ||
        !task_manager_terminated (checkpoint.etag, sizeof (checkpoint.etag)) ||
        !task_manager_terminated (checkpoint.lastModified, sizeof (checkpoint.lastModified)) ||
        !task_manager_terminated (checkpoint.requestContext, sizeof (checkpoint.requestContext))) {
        return task_manager_discard_checkpoint (manager, task, true);
    }
    regular = stat (task->tempPath, &status) == 0 && S_ISREG (status.st_mode) && status.st_nlink == 1;
    if (!regular) {
        /* No data, but the persisted resource identity is still usable. */
        if (checkpoint.durableBytes == 0) {
            task->checkpoint = checkpoint;
            task->downloadedBytes = 0;
            snprintf (task->etag, sizeof (task->etag), "%s", checkpoint.etag);
            snprintf (task->lastModified, sizeof (task->lastModified), "%s", checkpoint.lastModified);
            return 0;
        }
        return task_manager_discard_checkpoint (manager, task, false);
    }
    if ((uint64_t)status.st_size < checkpoint.durableBytes)
        return task_manager_discard_checkpoint (manager, task, true);
    if ((uint64_t)status.st_size > checkpoint.durableBytes &&
        task_manager_truncate_file (task->tempPath, checkpoint.durableBytes) != 0)
        return -1;
    task->checkpoint = checkpoint;
    task->downloadedBytes = checkpoint.durableBytes;
    snprintf (task->etag, sizeof (task->etag), "%s", checkpoint.etag);
    snprintf (task->lastModified, sizeof (task->lastModified), "%s", checkpoint.lastModified);
    return 0;
}

static int task_manager_checkpoint (void* userData, const DownloadTaskCheckpoint* checkpoint)
{
    DownloadExecution* execution = userData;
    DownloadTask       current;
    struct stat        status;
    int                result;

    if (execution == NULL || checkpoint == NULL ||
        checkpoint->version != DOWNLOAD_TASK_CHECKPOINT_VERSION ||
        !task_manager_terminated (checkpoint->effectiveUrl, sizeof (checkpoint->effectiveUrl)) ||
        !task_manager_terminated (checkpoint->etag, sizeof (checkpoint->etag)) ||
        !task_manager_terminated (checkpoint->lastModified, sizeof (checkpoint->lastModified)) ||
        !task_manager_terminated (checkpoint->requestContext, sizeof (checkpoint->requestContext))) {
        errno = EINVAL;
        return -1;
    }
    /* The protocol contract requires fsync before this callback; the file size
     * is the independent evidence that the reported bytes actually exist. */
    if (stat (execution->task->tempPath, &status) != 0 || !S_ISREG (status.st_mode) || status.st_nlink != 1 ||
        (uint64_t)status.st_size != checkpoint->durableBytes) {
        errno = EPROTO;
        return -1;
    }
    memset (&current, 0, sizeof (current));
    c_mutex_lock (&execution->manager->mutex);
    result = task_manager_callback_task (execution, &current);
    if (result == 0 || current.state == DOWNLOAD_TASK_STATE_PAUSED) {
        /* A pause still needs the last durable position, but the abort result
         * stays so the worker yields without declaring completion. */
        int savedErrno;
        if (database_save_checkpoint (execution->manager->database, execution->task->uuid, checkpoint) != 0 &&
            result == 0)
            result = -1;
        savedErrno = errno;
        c_mutex_unlock (&execution->manager->mutex);
        errno = savedErrno;
        return result;
    }
    {
        int savedErrno = errno;
        c_mutex_unlock (&execution->manager->mutex);
        errno = savedErrno;
    }
    return result;
}

static int task_manager_metadata (void* userData, const char* etag, const char* lastModified)
{
    DownloadExecution* execution = userData;
    DownloadTask current;
    int result;
    if (etag == NULL || lastModified == NULL || strlen (etag) >= sizeof (current.etag) ||
        strlen (lastModified) >= sizeof (current.lastModified)) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&execution->manager->mutex);
    result = task_manager_callback_task (execution, &current);
    if (result == 0) {
        snprintf (current.etag, sizeof (current.etag), "%s", etag);
        snprintf (current.lastModified, sizeof (current.lastModified), "%s", lastModified);
        current.updatedAt = task_manager_now ();
        result = database_update (execution->manager->database, &current);
    }
    int savedErrno = errno;
    c_mutex_unlock (&execution->manager->mutex);
    errno = savedErrno;
    return result;
}

static int task_manager_retry (void* userData, int attempt)
{
    DownloadExecution* execution = userData;
    DownloadTask current;
    int result;
    if (attempt <= 0) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&execution->manager->mutex);
    result = task_manager_callback_task (execution, &current);
    if (result == 0) {
        if (attempt > current.retryCount)
            current.retryCount = attempt;
        current.speedBytesSec = 0;
        current.updatedAt = task_manager_now ();
        result = database_update (execution->manager->database, &current);
    }
    int savedErrno = errno;
    c_mutex_unlock (&execution->manager->mutex);
    errno = savedErrno;
    return result;
}

static void task_manager_progress (void* userData, uint64_t total, uint64_t downloaded, uint64_t speed)
{
    DownloadExecution* execution = userData;
    DownloadTask current;
    c_mutex_lock (&execution->manager->mutex);
    if (database_load (execution->manager->database, execution->task->uuid, &current) != 0) {
        execution->ops->cancel (execution->context);
    } else if (current.state == DOWNLOAD_TASK_STATE_CANCELLED) {
        execution->ops->cancel (execution->context);
    } else if (current.state == DOWNLOAD_TASK_STATE_PAUSED || execution->manager->stopping) {
        execution->ops->pause (execution->context);
    } else {
        current.totalBytes = total;
        current.downloadedBytes = downloaded;
        current.speedBytesSec = speed;
        current.progress = total == 0 ? 0 : 100.0 * (downloaded > total ? total : downloaded) / total;
        current.updatedAt = task_manager_now ();
        if (database_update (execution->manager->database, &current) != 0) {
            execution->ops->cancel (execution->context);
        } else {
            ActiveTask* active = task_manager_find_active (execution->manager, execution->task->uuid);
            if (active != NULL && active->finishRequested) {
                if (execution->ops->stop != NULL)
                    execution->ops->stop (execution->context);
                else
                    execution->ops->cancel (execution->context);
            }
        }
    }
    c_mutex_unlock (&execution->manager->mutex);
}

static void task_manager_recording (void* userData, int64_t durationMillis, uint32_t gaps)
{
    DownloadExecution* execution = userData;
    DownloadTask       current;

    if (execution == NULL || durationMillis < 0)
        return;
    c_mutex_lock (&execution->manager->mutex);
    if (database_load (execution->manager->database, execution->task->uuid, &current) != 0) {
        execution->ops->cancel (execution->context);
    } else if (current.state == DOWNLOAD_TASK_STATE_CANCELLED) {
        execution->ops->cancel (execution->context);
    } else if (current.state == DOWNLOAD_TASK_STATE_PAUSED || execution->manager->stopping) {
        execution->ops->pause (execution->context);
    } else {
        current.durationMillis = durationMillis;
        current.gapCount = gaps;
        current.updatedAt = task_manager_now ();
        if (database_update (execution->manager->database, &current) != 0)
            execution->ops->cancel (execution->context);
    }
    c_mutex_unlock (&execution->manager->mutex);
}

static void task_manager_filename (void* userData, const char* name)
{
    DownloadExecution* execution = userData;
    DownloadTask current;
    if (!name[0] || strlen (name) >= sizeof (current.fileName) || strchr (name, '/') || strchr (name, '\\') ||
        strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
        return;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        if (*p < 32 || *p == 127)
            return;
    }
    c_mutex_lock (&execution->manager->mutex);
    if (database_load (execution->manager->database, execution->task->uuid, &current) == 0 &&
        snprintf (current.targetPath, sizeof (current.targetPath), "%s/%s", current.targetDirectory, name) < (int)sizeof (current.targetPath)) {
        strcpy (current.fileName, name);
        if (database_update (execution->manager->database, &current) == 0) {
            strcpy (execution->task->fileName, current.fileName);
            strcpy (execution->task->targetPath, current.targetPath);
        }
    }
    c_mutex_unlock (&execution->manager->mutex);
}

int task_manager_execute (TaskManager* manager, DownloadTask* task)
{
    DownloadExecution execution = { .manager = manager, .task = task };
    ProtocolCallbacks callbacks = { .onProgress = task_manager_progress, .onFileName = task_manager_filename,
                                    .onFailed = task_manager_failed, .onMetadata = task_manager_metadata,
                                    .onCheckpoint = task_manager_checkpoint, .onRetry = task_manager_retry,
                                    .onRecording = task_manager_recording };
    DownloadTask current;
    struct stat status;
    int result = -1;
    int savedErrno;
    bool publishedWarning = false;
    c_mutex_lock (&manager->mutex);
    if (database_load (manager->database, task->uuid, &current) != 0 || current.state != DOWNLOAD_TASK_STATE_QUEUED) {
        task_manager_release (manager, task);
        c_mutex_unlock (&manager->mutex);
        return -1;
    }
    *task = current;
    task->maxRetries = manager->maxRetries;
    task->connectTimeout = manager->connectTimeout;
    task->transferTimeout = manager->transferTimeout;
    if (task_manager_restore_checkpoint (manager, task) != 0) {
        int savedErrno = errno;
        task_manager_release (manager, task);
        c_mutex_unlock (&manager->mutex);
        errno = savedErrno;
        return -1;
    }
    {
        RequestContext* requestContext = NULL;
        int             policyError = 0;
        if (request_context_resolve (&requestContext, task, manager->credentialStore, manager->defaultAuthProfile,
                                     manager->defaultProxyProfile,
                                     manager->proxyUrl[0] ? manager->proxyUrl : NULL, manager->proxyUsername,
                                     manager->proxyPassword) != 0) {
            policyError = errno ? errno : EIO;
        } else if (!task_manager_curl_protocol (task->protocol) &&
                   (request_context_has_auth (requestContext) || request_context_has_proxy (requestContext))) {
            /* Never silently fall back to a direct connection for an engine
             * that cannot honor the configured auth/proxy policy. */
            policyError = ENOTSUP;
        } else {
            task->requestContext = requestContext;
            requestContext = NULL;
        }
        if (policyError != 0) {
            current.state = DOWNLOAD_TASK_STATE_FAILED;
            current.errorCode = policyError;
            current.updatedAt = task_manager_now ();
            snprintf (current.errorMessage, sizeof (current.errorMessage), "%s",
                      policyError == ENOTSUP
                          ? "Request policy (auth/proxy) is not supported by this protocol"
                          : strerror (policyError));
            (void)database_update (manager->database, &current);
            request_context_free (requestContext);
            task_manager_release (manager, task);
            c_mutex_unlock (&manager->mutex);
            errno = policyError;
            return -1;
        }
    }
    task->state = DOWNLOAD_TASK_STATE_DOWNLOADING;
    task->startedAt = task_manager_now ();
    task->errorCode = 0;
    task->errorMessage[0] = '\0';
    result = database_update (manager->database, task);
    c_mutex_unlock (&manager->mutex);
    execution.ops = protocol_find (task->resourceUrl, task->protocol);
    bool retryable = false;
    if (result == 0 && execution.ops != NULL &&
        execution.ops->open (&execution.context, task, &callbacks, &execution) == 0) {
        result = execution.ops->run (execution.context);
        savedErrno = errno;
        if (result < 0) {
            if (execution.ops->retryable != NULL)
                retryable = execution.ops->retryable (execution.context, savedErrno, execution.errorMessage);
            else
                retryable = task_manager_error_retryable (savedErrno);
        }
        execution.ops->close (execution.context);
        errno = savedErrno;
    } else {
        result = -1;
        retryable = task_manager_error_retryable (errno);
    }
    request_context_free (task->requestContext);
    task->requestContext = NULL;
    savedErrno = errno;
    c_mutex_lock (&manager->mutex);
    if (database_load (manager->database, task->uuid, &current) == 0) {
        if (stat (task->tempPath, &status) == 0 && S_ISREG (status.st_mode))
            current.downloadedBytes = (uint64_t)status.st_size;
        if (result == 1 && current.state == DOWNLOAD_TASK_STATE_DOWNLOADING) {
            current.state = DOWNLOAD_TASK_STATE_PAUSED;
            current.pausedAt = task_manager_now ();
        }
        if (current.state != DOWNLOAD_TASK_STATE_PAUSED && current.state != DOWNLOAD_TASK_STATE_CANCELLED) {
            if (result != 0 && retryable && !manager->stopping) {
                current.state = DOWNLOAD_TASK_STATE_QUEUED;
                current.retryCount = current.retryCount + 1;
                current.errorCode = savedErrno != 0 ? savedErrno : EIO;
                snprintf (current.errorMessage, sizeof (current.errorMessage), "%s",
                          execution.errorMessage[0] ? execution.errorMessage : strerror (current.errorCode));
                current.updatedAt = task_manager_now ();
                task_manager_schedule_retry_locked (manager, current.uuid, current.retryCount);
                current.speedBytesSec = 0;
                result = database_update (manager->database, &current);
                task_manager_release (manager, task);
                c_mutex_unlock (&manager->mutex);
                return result;
            }
            if (result == 0) {
                result = filesystem_commit (task);
                savedErrno = errno;
                if (result == 1) {
                    publishedWarning = true;
                    result = 0;
                }
            }
            current.state = result == 0 ? DOWNLOAD_TASK_STATE_COMPLETED : DOWNLOAD_TASK_STATE_FAILED;
            current.errorCode = result == 0 ? 0 : (savedErrno == 0 ? EIO : savedErrno);
            snprintf (current.errorMessage, sizeof (current.errorMessage), "%s", result == 0 ? "" :
                      (execution.errorMessage[0] ? execution.errorMessage : strerror (current.errorCode)));
            if (publishedWarning) {
                current.errorCode = savedErrno;
                snprintf (current.errorMessage, sizeof (current.errorMessage),
                          "Output published; parent directory sync failed: %s", strerror (savedErrno));
            }
            if (result == 0) {
                current.completedAt = task_manager_now ();
                current.progress = 100;
                if (current.totalBytes == 0)
                    current.totalBytes = current.downloadedBytes;
            }
        }
        current.speedBytesSec = 0;
        result = database_update (manager->database, &current);
        /* A completed task can never resume; a stale row would be harmless but
         * misleading, so drop it (it also disappears with the task row). */
        if (result == 0 && current.state == DOWNLOAD_TASK_STATE_COMPLETED)
            (void)database_delete_checkpoint (manager->database, task->uuid);
    }
    task_manager_release (manager, task);
    c_mutex_unlock (&manager->mutex);
    return result;
}

int task_manager_stop (TaskManager* manager)
{
    int savedErrno = 0;
    if (manager == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    manager->stopping = true;
    manager->retryStopping = true;
    while (manager->retries != NULL) {
        RetryEntry* entry = manager->retries;
        manager->retries = entry->next;
        c_free (entry);
    }
    c_cond_notify_all (&manager->retryCond);
    for (ActiveTask* active = manager->active; active != NULL; active = active->next) {
        if (task_manager_update_state (manager, active->task.uuid, DOWNLOAD_TASK_STATE_PAUSED) != 0)
            savedErrno = errno;
    }
    c_mutex_unlock (&manager->mutex);
    errno = savedErrno;
    return savedErrno == 0 ? 0 : -1;
}

int task_manager_pause (TaskManager* manager, const char* uuid)
{
    int result;

    if (manager == NULL || uuid == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    DownloadTask current;
    if (database_load (manager->database, uuid, &current) != 0 ||
        current.state == DOWNLOAD_TASK_STATE_COMPLETED || current.state == DOWNLOAD_TASK_STATE_CANCELLED) {
        c_mutex_unlock (&manager->mutex);
        errno = EINVAL;
        return -1;
    }
    task_manager_retry_remove_locked (manager, uuid);
    result = task_manager_update_state (manager, uuid, DOWNLOAD_TASK_STATE_PAUSED);
    while (result == 0 && task_manager_active (manager, uuid))
        c_cond_wait (&manager->idle, &manager->mutex);
    c_mutex_unlock (&manager->mutex);
    return result;
}

int task_manager_resume (TaskManager* manager, const char* uuid)
{
    return task_manager_start (manager, uuid);
}

int task_manager_finish (TaskManager* manager, const char* uuid)
{
    DownloadTask       task;
    const ProtocolOps* ops;
    ActiveTask*        active;

    if (manager == NULL || uuid == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    if (database_load (manager->database, uuid, &task) != 0) {
        c_mutex_unlock (&manager->mutex);
        return -1;
    }
    ops = protocol_find (task.resourceUrl, task.protocol);
    if (ops == NULL || ops->stop == NULL) {
        c_mutex_unlock (&manager->mutex);
        errno = ENOTSUP;
        return -1;
    }
    active = task_manager_find_active (manager, uuid);
    if (active == NULL) {
        c_mutex_unlock (&manager->mutex);
        errno = ENOENT;
        return -1;
    }
    active->finishRequested = true;
    c_mutex_unlock (&manager->mutex);
    return 0;
}

int task_manager_resume_queued (TaskManager* manager)
{
    const size_t pageSize = 100;
    size_t       offset = 0;
    size_t       total = 0;
    int          savedErrno = 0;

    if (manager == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    do {
        DownloadTask* tasks = NULL;
        size_t        count = 0;
        if (database_list (manager->database, offset, pageSize, &tasks, &count, &total) != 0) {
            savedErrno = errno;
            break;
        }
        for (size_t index = 0; index < count; index++) {
            if (tasks[index].state == DOWNLOAD_TASK_STATE_QUEUED &&
                !task_manager_active (manager, tasks[index].uuid) &&
                task_manager_submit_active_locked (manager, &tasks[index]) != 0 && savedErrno == 0)
                savedErrno = errno;
        }
        c_free (tasks);
        offset += count;
        if (count == 0)
            break;
    } while (offset < total);
    c_mutex_unlock (&manager->mutex);
    errno = savedErrno;
    return savedErrno == 0 ? 0 : -1;
}

int task_manager_get (TaskManager* manager, const char* uuid, DownloadTask* task)
{
    int result;

    if (manager == NULL || uuid == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    result = database_load (manager->database, uuid, task);
    c_mutex_unlock (&manager->mutex);
    return result;
}

int task_manager_list (TaskManager* manager, size_t page, size_t pageSize, DownloadTask** tasks, size_t* count,
                       size_t* total)
{
    size_t offset;
    int    result;

    if (manager == NULL || page == 0 || pageSize == 0 || page > SIZE_MAX / pageSize || tasks == NULL || count == NULL ||
        total == NULL) {
        errno = EINVAL;
        return -1;
    }
    offset = (page - 1U) * pageSize;
    c_mutex_lock (&manager->mutex);
    result = database_list (manager->database, offset, pageSize, tasks, count, total);
    c_mutex_unlock (&manager->mutex);
    return result;
}

int task_manager_clear_completed (TaskManager* manager, size_t* deleted)
{
    int result;

    if (manager == NULL || deleted == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    result = database_clear_completed (manager->database);
    if (result >= 0)
        *deleted = (size_t)result;
    c_mutex_unlock (&manager->mutex);
    return result < 0 ? -1 : 0;
}
