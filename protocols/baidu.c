#define _POSIX_C_SOURCE 200809L

#include "baidu.h"

#include "baidu-parse.h"
#include "baidu-sign.h"
#include "request-context.h"

#include <clib/checksum.h>
#include <clib/cond.h>
#include <clib/json.h>
#include <clib/memory.h>
#include <clib/mutex.h>
#include <clib/once.h>
#include <clib/string-view.h>
#include <clib/uri.h>

#include <curl/curl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BAIDU_URL_SIZE              8192U
#define BAIDU_RESPONSE_LIMIT        (8U * 1024U * 1024U)
#define BAIDU_CHECKPOINT_INTERVAL   (4U * 1024U * 1024U)
#define BAIDU_PROGRESS_INTERVAL_MS  200LL
#define BAIDU_MAX_LIST_PAGES        200
#define BAIDU_LIST_PAGE_SIZE        1000
#define BAIDU_TRANSFER_CHUNK_SIZE   80

typedef struct BaiduSignEntry BaiduSignEntry;
struct BaiduSignEntry
{
    char             key[65];
    char             sign[256];
    char             timestamp[32];
    int64_t          expiresAt;
    BaiduSignEntry*  next;
};

typedef struct
{
    char transferDirectory[DOWNLOAD_TASK_PATH_SIZE];
    int  maxParallelTasks;
    int  maxFiles;
    int  signCacheSeconds;
    char apiBase[DOWNLOAD_TASK_URL_SIZE];
    char pcsBase[DOWNLOAD_TASK_URL_SIZE];
} BaiduConfig;

static BaiduConfig      gBaiduConfig = {
    .transferDirectory = "/downloader",
    .maxParallelTasks = 2,
    .maxFiles = 2000,
    .signCacheSeconds = 3600,
    .apiBase = "https://pan.baidu.com",
    .pcsBase = "https://pcs.baidu.com"
};
static COnce            gBaiduOnce = C_ONCE_INIT;
static CMutex           gBaiduMutex;
static CCond            gBaiduSlotCond;
static bool             gBaiduInitialized;
static int              gBaiduActiveSlots;
static BaiduSignEntry*  gBaiduSigns;

typedef struct
{
    DownloadTask      source;
    ProtocolCallbacks callbacks;
    void*             userData;
    bool              pauseRequested;
    bool              cancelRequested;
    CURLcode          lastResult;
    long              status;
    char              cookie[DOWNLOADER_CREDENTIAL_SECRET_SIZE];
    char              apiPath[DOWNLOAD_TASK_PATH_SIZE];
    char              errorMessage[DOWNLOAD_TASK_ERROR_SIZE];
} BaiduTask;

typedef struct
{
    char*  data;
    size_t size;
    size_t capacity;
} BaiduBuffer;

typedef struct
{
    CURL*  curl;
    char   cookie[DOWNLOADER_CREDENTIAL_SECRET_SIZE];
    char   error[CURL_ERROR_SIZE];
    char   location[1024];
    long   status;
} BaiduSession;

typedef struct
{
    BaiduTask* task;
    FILE*      file;
    uint64_t   offset;
    uint64_t   attemptOffset;
    uint64_t   written;
    uint64_t   expectedSize;
    uint64_t   lastCheckpoint;
    int64_t    lastProgressMs;
    bool       localError;
    char       md5[DOWNLOAD_TASK_CHECKSUM_SIZE];
} BaiduDownload;

static void baidu_init_once (void* unused)
{
    (void)unused;
    gBaiduInitialized = c_mutex_init (&gBaiduMutex) && c_cond_init (&gBaiduSlotCond);
}

static bool baidu_ensure_init (void)
{
    c_once_call (&gBaiduOnce, baidu_init_once, NULL);
    return gBaiduInitialized;
}

static void baidu_copy_base (char* destination, size_t capacity, const char* value)
{
    size_t length;

    if (value == NULL) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf (destination, capacity, "%s", value);
    length = strlen (destination);
    while (length > 0 && destination[length - 1] == '/')
        destination[--length] = '\0';
}

int baidu_protocol_configure (const char* transferDirectory, int maxParallelTasks, int maxFiles,
                              int signCacheSeconds, const char* apiBase, const char* pcsBase)
{
    if (!baidu_ensure_init ()) {
        errno = ENOMEM;
        return -1;
    }
    if (transferDirectory == NULL || transferDirectory[0] != '/' || maxParallelTasks <= 0 || maxFiles <= 0 ||
        signCacheSeconds < 0 || apiBase == NULL || apiBase[0] == '\0' || pcsBase == NULL || pcsBase[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&gBaiduMutex);
    (void)snprintf (gBaiduConfig.transferDirectory, sizeof (gBaiduConfig.transferDirectory), "%s",
                    transferDirectory);
    gBaiduConfig.maxParallelTasks = maxParallelTasks;
    gBaiduConfig.maxFiles = maxFiles;
    gBaiduConfig.signCacheSeconds = signCacheSeconds;
    baidu_copy_base (gBaiduConfig.apiBase, sizeof (gBaiduConfig.apiBase), apiBase);
    baidu_copy_base (gBaiduConfig.pcsBase, sizeof (gBaiduConfig.pcsBase), pcsBase);
    c_mutex_unlock (&gBaiduMutex);
    return 0;
}

static int baidu_set_error (BaiduTask* task, int code, const char* message)
{
    errno = code;
    (void)snprintf (task->errorMessage, sizeof (task->errorMessage), "%s", message == NULL ? "" : message);
    return -1;
}

/* A 5xx from the web API is a transport-level failure worth retrying; the
 * callers' own classification (auth expired, rejected share, ...) only fits
 * the 4xx family. Returns 0 when the status is not an error. */
static int baidu_status_error (BaiduTask* task, long status, const char* message)
{
    if (status < 400)
        return 0;
    if (status >= 500)
        return baidu_set_error (task, EAGAIN, "baidu_network_error: server returned HTTP 5xx");
    return baidu_set_error (task, EACCES, message);
}

static int baidu_now_ms (void)
{
    struct timespec value;

    (void)clock_gettime (CLOCK_MONOTONIC, &value);
    return (int)(value.tv_sec * 1000LL + value.tv_nsec / 1000000LL);
}

static bool baidu_share_supported_url (const char* url)
{
    char surl[BAIDU_SURL_SIZE];
    return baidu_share_url (url, surl, sizeof (surl));
}

static bool baidu_can_handle (const char* url, const char* protocol)
{
    char path[DOWNLOAD_TASK_PATH_SIZE];

    if (url == NULL || protocol == NULL || strcmp (protocol, "baidu") != 0)
        return false;
    return baidu_own_url (url, path, sizeof (path)) || baidu_share_supported_url (url);
}

static size_t baidu_buffer_write (void* data, size_t size, size_t count, void* userData)
{
    BaiduBuffer* buffer = (BaiduBuffer*)userData;
    size_t       length = size * count;
    size_t       needed;
    size_t       capacity;

    if (buffer == NULL || data == NULL || length == 0)
        return length;
    needed = buffer->size + length;
    if (needed > BAIDU_RESPONSE_LIMIT)
        return 0;
    if (needed + 1U > buffer->capacity) {
        capacity = buffer->capacity == 0 ? 4096U : buffer->capacity;
        while (capacity < needed + 1U && capacity < BAIDU_RESPONSE_LIMIT)
            capacity *= 2U;
        if (capacity < needed + 1U)
            return 0;
        char* grown = (char*)c_realloc (buffer->data, capacity);
        if (grown == NULL)
            return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy (buffer->data + buffer->size, data, length);
    buffer->size = needed;
    buffer->data[buffer->size] = '\0';
    return length;
}

static size_t baidu_header_cb (char* buffer, size_t size, size_t count, void* userData)
{
    BaiduSession* session = (BaiduSession*)userData;
    size_t        length = size * count;

    if (session == NULL || buffer == NULL)
        return length;
    if (length >= 5U && strncasecmp (buffer, "HTTP/", 5U) == 0) {
        const char* space = (const char*)memchr (buffer, ' ', length);
        session->location[0] = '\0';
        session->status = 0;
        if (space != NULL) {
            char number[8];
            size_t digits = strcspn (space + 1, " \r\n");
            if (digits < sizeof (number)) {
                memcpy (number, space + 1, digits);
                number[digits] = '\0';
                session->status = strtol (number, NULL, 10);
            }
        }
    } else if (length >= 9U && strncasecmp (buffer, "Location:", 9U) == 0) {
        const char* value = buffer + 9;
        const char* end = buffer + length;
        size_t      valueLength;
        while (value < end && (*value == ' ' || *value == '\t'))
            value++;
        valueLength = (size_t)(end - value);
        while (valueLength > 0 && (value[valueLength - 1] == '\r' || value[valueLength - 1] == '\n' ||
                                   value[valueLength - 1] == ' '))
            valueLength--;
        if (valueLength < sizeof (session->location)) {
            memcpy (session->location, value, valueLength);
            session->location[valueLength] = '\0';
        }
    }
    return length;
}

static int baidu_session_init (BaiduTask* task, BaiduSession* session)
{
    memset (session, 0, sizeof (*session));
    session->curl = curl_easy_init ();
    if (session->curl == NULL) {
        errno = ENOMEM;
        return -1;
    }
    (void)snprintf (session->cookie, sizeof (session->cookie), "%s", task->cookie);
    curl_easy_setopt (session->curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt (session->curl, CURLOPT_COOKIE, session->cookie);
    curl_easy_setopt (session->curl, CURLOPT_USERAGENT, "pan.baidu.com");
    curl_easy_setopt (session->curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt (session->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt (session->curl, CURLOPT_ERRORBUFFER, session->error);
    curl_easy_setopt (session->curl, CURLOPT_HEADERFUNCTION, baidu_header_cb);
    curl_easy_setopt (session->curl, CURLOPT_HEADERDATA, session);
    if (task->source.connectTimeout > 0)
        curl_easy_setopt (session->curl, CURLOPT_CONNECTTIMEOUT, (long)task->source.connectTimeout);
    if (task->source.transferTimeout > 0)
        curl_easy_setopt (session->curl, CURLOPT_TIMEOUT, (long)task->source.transferTimeout);
    if (task->source.requestContext != NULL && request_context_has_proxy (task->source.requestContext)) {
        curl_easy_setopt (session->curl, CURLOPT_PROXY, request_context_proxy_url (task->source.requestContext));
        curl_easy_setopt (session->curl, CURLOPT_NOPROXY, "");
        if (request_context_proxy_username (task->source.requestContext)[0] != '\0') {
            char credentials[DOWNLOADER_CREDENTIAL_USER_SIZE + DOWNLOADER_CREDENTIAL_SECRET_SIZE + 2];
            (void)snprintf (credentials, sizeof (credentials), "%s:%s",
                            request_context_proxy_username (task->source.requestContext),
                            request_context_proxy_secret (task->source.requestContext));
            curl_easy_setopt (session->curl, CURLOPT_PROXYUSERPWD, credentials);
        }
    }
    return 0;
}

static void baidu_session_close (BaiduSession* session)
{
    if (session->curl != NULL)
        curl_easy_cleanup (session->curl);
    session->curl = NULL;
}

static CURLcode baidu_session_request (BaiduSession* session, const char* url, const char* postBody,
                                       const char* referer, BaiduBuffer* buffer)
{
    CURLcode result;

    buffer->size = 0;
    if (buffer->data == NULL) {
        buffer->data = (char*)c_malloc0 (1);
        buffer->capacity = buffer->data == NULL ? 0 : 1;
    }
    if (buffer->data != NULL)
        buffer->data[0] = '\0';
    session->location[0] = '\0';
    session->status = 0;
    curl_easy_setopt (session->curl, CURLOPT_URL, url);
    if (referer != NULL)
        curl_easy_setopt (session->curl, CURLOPT_REFERER, referer);
    else
        curl_easy_setopt (session->curl, CURLOPT_REFERER, NULL);
    if (postBody != NULL) {
        curl_easy_setopt (session->curl, CURLOPT_POST, 1L);
        curl_easy_setopt (session->curl, CURLOPT_POSTFIELDS, postBody);
        curl_easy_setopt (session->curl, CURLOPT_POSTFIELDSIZE, (long)strlen (postBody));
    } else {
        curl_easy_setopt (session->curl, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt (session->curl, CURLOPT_WRITEFUNCTION, baidu_buffer_write);
    curl_easy_setopt (session->curl, CURLOPT_WRITEDATA, buffer);
    result = curl_easy_perform (session->curl);
    if (result == CURLE_OK)
        (void)curl_easy_getinfo (session->curl, CURLINFO_RESPONSE_CODE, &session->status);
    return result;
}

static void baidu_buffer_clear (BaiduBuffer* buffer)
{
    c_free (buffer->data);
    memset (buffer, 0, sizeof (*buffer));
}

static void baidu_cookie_key (const char* cookie, char key[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char     digest[C_SHA256_DIGEST_SIZE];

    (void)c_checksum_sha256 (cookie, strlen (cookie), digest);
    for (size_t index = 0; index < sizeof (digest); index++) {
        key[index * 2U] = hex[digest[index] >> 4];
        key[index * 2U + 1U] = hex[digest[index] & 0x0f];
    }
    key[64] = '\0';
}

static void baidu_sign_invalidate (const char* cookie)
{
    char           key[65];
    BaiduSignEntry** slot;

    baidu_cookie_key (cookie, key);
    c_mutex_lock (&gBaiduMutex);
    slot = &gBaiduSigns;
    while (*slot != NULL) {
        if (strcmp ((*slot)->key, key) == 0) {
            BaiduSignEntry* removed = *slot;
            *slot = removed->next;
            c_free (removed);
            break;
        }
        slot = &(*slot)->next;
    }
    c_mutex_unlock (&gBaiduMutex);
}

static int baidu_sign_get (BaiduTask* task, char* sign, size_t signCapacity, char* timestamp,
                           size_t timestampCapacity)
{
    char          key[65];
    int64_t       now;
    BaiduSignEntry* entry;
    BaiduSession  session;
    BaiduBuffer   buffer;
    char          url[BAIDU_URL_SIZE];

    baidu_cookie_key (task->cookie, key);
    now = (int64_t)time (NULL);
    c_mutex_lock (&gBaiduMutex);
    for (entry = gBaiduSigns; entry != NULL; entry = entry->next) {
        if (strcmp (entry->key, key) == 0 && entry->expiresAt > now) {
            (void)snprintf (sign, signCapacity, "%s", entry->sign);
            (void)snprintf (timestamp, timestampCapacity, "%s", entry->timestamp);
            c_mutex_unlock (&gBaiduMutex);
            return 0;
        }
    }
    c_mutex_unlock (&gBaiduMutex);

    memset (&buffer, 0, sizeof (buffer));
    if (baidu_session_init (task, &session) != 0)
        return baidu_set_error (task, ENOMEM, "baidu_auth_missing: cannot create HTTP session");
    (void)snprintf (url, sizeof (url), "%s/disk/home", gBaiduConfig.apiBase);
    CURLcode result = baidu_session_request (&session, url, NULL, "https://pan.baidu.com/disk/home", &buffer);
    if (result != CURLE_OK) {
        baidu_session_close (&session);
        baidu_buffer_clear (&buffer);
        return baidu_set_error (task, EAGAIN, "baidu_network_error: cannot read /disk/home");
    }
    if (session.location[0] != '\0' &&
        (strstr (session.location, "passport.baidu.com") != NULL || strcmp (session.location, "/") == 0)) {
        baidu_session_close (&session);
        baidu_buffer_clear (&buffer);
        return baidu_set_error (task, EACCES, "baidu_auth_expired: cookie is no longer valid");
    }
    if (session.status >= 400) {
        baidu_session_close (&session);
        baidu_buffer_clear (&buffer);
        return baidu_status_error (task, session.status, "baidu_auth_expired: cookie is no longer valid");
    }
    char sign1[128];
    char sign3[128];
    char stamp[32];
    if (buffer.data == NULL ||
        baidu_page_extract_sign (buffer.data, sign1, sizeof (sign1), sign3, sizeof (sign3), stamp,
                                 sizeof (stamp)) != 0) {
        baidu_session_close (&session);
        baidu_buffer_clear (&buffer);
        return baidu_set_error (task, EPROTO, "baidu_sign_unavailable: /disk/home did not contain sign fields");
    }
    char computed[256];
    if (baidu_compute_sign (sign3, sign1, computed, sizeof (computed)) != 0) {
        baidu_session_close (&session);
        baidu_buffer_clear (&buffer);
        return baidu_set_error (task, EPROTO, "baidu_sign_unavailable: cannot compute sign");
    }
    c_mutex_lock (&gBaiduMutex);
    entry = NULL;
    for (BaiduSignEntry* current = gBaiduSigns; current != NULL; current = current->next) {
        if (strcmp (current->key, key) == 0) {
            entry = current;
            break;
        }
    }
    if (entry == NULL) {
        entry = (BaiduSignEntry*)c_malloc0 (sizeof (*entry));
        if (entry != NULL) {
            (void)snprintf (entry->key, sizeof (entry->key), "%s", key);
            entry->next = gBaiduSigns;
            gBaiduSigns = entry;
        }
    }
    if (entry != NULL) {
        (void)snprintf (entry->sign, sizeof (entry->sign), "%s", computed);
        (void)snprintf (entry->timestamp, sizeof (entry->timestamp), "%s", stamp);
        entry->expiresAt = now + gBaiduConfig.signCacheSeconds;
        (void)snprintf (sign, signCapacity, "%s", entry->sign);
        (void)snprintf (timestamp, timestampCapacity, "%s", entry->timestamp);
    }
    c_mutex_unlock (&gBaiduMutex);
    baidu_session_close (&session);
    baidu_buffer_clear (&buffer);
    if (entry == NULL)
        return baidu_set_error (task, ENOMEM, "baidu_sign_unavailable: out of memory");
    return 0;
}

static int baidu_json_errno (const CJsonValue* root)
{
    CJsonValue* value = c_json_object_get (root, c_string_view_from_cstr ("errno"));
    int64_t     code;

    if (value != NULL && c_json_value_get_int64 (value, &code) == 0)
        return (int)code;
    return 0;
}

static int baidu_json_scalar (const CJsonValue* object, const char* key, char* destination, size_t capacity)
{
    CJsonValue* value = c_json_object_get (object, c_string_view_from_cstr (key));
    CJsonType   type;

    if (value == NULL)
        return -1;
    type = c_json_value_type (value);
    if (type == C_JSON_TYPE_STRING) {
        CStringView text = c_json_value_get_string (value);
        if (text.size >= capacity)
            return -1;
        memcpy (destination, text.data, text.size);
        destination[text.size] = '\0';
        return 0;
    }
    if (type == C_JSON_TYPE_INT64) {
        int64_t number;
        if (c_json_value_get_int64 (value, &number) != 0)
            return -1;
        (void)snprintf (destination, capacity, "%lld", (long long)number);
        return 0;
    }
    if (type == C_JSON_TYPE_UINT64) {
        uint64_t number;
        if (c_json_value_get_uint64 (value, &number) != 0)
            return -1;
        (void)snprintf (destination, capacity, "%llu", (unsigned long long)number);
        return 0;
    }
    return -1;
}

static int baidu_json_int64 (const CJsonValue* object, const char* key, int64_t* result)
{
    CJsonValue* value = c_json_object_get (object, c_string_view_from_cstr (key));

    if (value == NULL)
        return -1;
    if (c_json_value_type (value) == C_JSON_TYPE_INT64)
        return c_json_value_get_int64 (value, result);
    if (c_json_value_type (value) == C_JSON_TYPE_UINT64) {
        uint64_t number;
        if (c_json_value_get_uint64 (value, &number) != 0)
            return -1;
        *result = (int64_t)number;
        return 0;
    }
    return -1;
}

typedef struct
{
    char     name[DOWNLOAD_TASK_FILE_NAME_SIZE];
    char     fsid[64];
    char     path[DOWNLOAD_TASK_PATH_SIZE];
    uint64_t size;
    char     md5[DOWNLOAD_TASK_CHECKSUM_SIZE];
    bool     isdir;
} BaiduEntry;

static int baidu_list_page (BaiduTask* task, const char* directory, int page, BaiduEntry** entries,
                            size_t* count)
{
    BaiduSession   session;
    BaiduBuffer    buffer = { 0 };
    char*          escaped;
    char           url[BAIDU_URL_SIZE];
    CURLcode       code;
    size_t         errorOffset = 0;
    CJsonDocument* document;
    CJsonValue*    root;
    CJsonValue*    list;
    size_t         itemCount;
    BaiduEntry*    values = NULL;
    size_t         used = 0;
    int            remote;
    int            result = -1;

    *entries = NULL;
    *count = 0;
    if (baidu_session_init (task, &session) != 0)
        return baidu_set_error (task, ENOMEM, "baidu_auth_missing: cannot create HTTP session");
    escaped = curl_easy_escape (session.curl, directory, 0);
    if (escaped == NULL) {
        baidu_session_close (&session);
        return baidu_set_error (task, ENOMEM, "baidu_network_error: cannot encode list path");
    }
    (void)snprintf (url, sizeof (url), "%s/api/list?dir=%s&num=%d&page=%d&order=name&desc=0",
                    gBaiduConfig.apiBase, escaped, BAIDU_LIST_PAGE_SIZE, page);
    curl_free (escaped);
    code = baidu_session_request (&session, url, NULL, NULL, &buffer);
    if (code != CURLE_OK) {
        baidu_session_close (&session);
        baidu_buffer_clear (&buffer);
        return baidu_set_error (task, EAGAIN, "baidu_network_error: cannot list the directory");
    }
    if (session.status >= 400) {
        baidu_session_close (&session);
        baidu_buffer_clear (&buffer);
        return baidu_status_error (task, session.status, "baidu_auth_expired: list request was rejected");
    }
    baidu_session_close (&session);
    document = c_json_document_new_from_data (
        c_string_view_from_data (buffer.data == NULL ? "" : buffer.data, buffer.size), C_JSON_PARSE_NONE,
        &errorOffset);
    if (document == NULL) {
        baidu_buffer_clear (&buffer);
        return baidu_set_error (task, EPROTO, "baidu_dlink_failed: list response is not JSON");
    }
    root = c_json_document_root (document);
    remote = baidu_json_errno (root);
    if (remote != 0) {
        c_json_document_free (document);
        baidu_buffer_clear (&buffer);
        if (remote == -6)
            return baidu_set_error (task, EACCES, "baidu_auth_expired: cookie is no longer valid");
        if (remote == 8001)
            return baidu_set_error (task, EACCES, "baidu_challenge_required: Baidu requires verification");
        return baidu_set_error (task, ENOENT, "baidu_file_not_found: directory is unavailable");
    }
    list = c_json_object_get (root, c_string_view_from_cstr ("list"));
    itemCount = list == NULL ? 0 : c_json_array_size (list);
    if (itemCount > 0) {
        values = (BaiduEntry*)c_malloc0 (itemCount * sizeof (*values));
        if (values == NULL) {
            c_json_document_free (document);
            baidu_buffer_clear (&buffer);
            return baidu_set_error (task, ENOMEM, "baidu_network_error: out of memory");
        }
    }
    for (size_t index = 0; index < itemCount; index++) {
        CJsonValue* item = c_json_array_get (list, index);
        BaiduEntry* entry = &values[used];
        int64_t     isDirectory = 0;
        int64_t     sizeValue = 0;
        if (item == NULL || baidu_json_scalar (item, "server_filename", entry->name, sizeof (entry->name)) != 0 ||
            baidu_json_scalar (item, "fs_id", entry->fsid, sizeof (entry->fsid)) != 0)
            continue;
        (void)baidu_json_int64 (item, "isdir", &isDirectory);
        (void)baidu_json_int64 (item, "size", &sizeValue);
        entry->isdir = isDirectory != 0;
        entry->size = sizeValue > 0 ? (uint64_t)sizeValue : 0;
        entry->md5[0] = '\0';
        (void)baidu_json_scalar (item, "md5", entry->md5, sizeof (entry->md5));
        if (baidu_json_scalar (item, "path", entry->path, sizeof (entry->path)) != 0) {
            if (strcmp (directory, "/") == 0)
                (void)snprintf (entry->path, sizeof (entry->path), "/%s", entry->name);
            else
                (void)snprintf (entry->path, sizeof (entry->path), "%s/%s", directory, entry->name);
        }
        used++;
    }
    c_json_document_free (document);
    baidu_buffer_clear (&buffer);
    *entries = values;
    *count = used;
    result = 0;
    return result;
}

/* List every page of a directory. Reuse decisions must see the whole transfer
 * directory: a share with more than one page of roots would otherwise be
 * treated as missing and re-transferred on every creation. */
static int baidu_list_all (BaiduTask* task, const char* directory, BaiduEntry** entries, size_t* count)
{
    BaiduEntry* values = NULL;
    size_t      used = 0;
    size_t      capacity = 0;
    bool        complete = false;

    *entries = NULL;
    *count = 0;
    for (int page = 1; page <= BAIDU_MAX_LIST_PAGES; page++) {
        BaiduEntry* pageEntries = NULL;
        size_t      pageCount = 0;
        if (baidu_list_page (task, directory, page, &pageEntries, &pageCount) != 0)
            goto fail;
        if (pageCount == 0) {
            c_free (pageEntries);
            complete = true;
            break;
        }
        if (used + pageCount > capacity) {
            size_t      next = capacity == 0 ? pageCount : capacity;
            BaiduEntry* grown;
            while (next < used + pageCount)
                next *= 2;
            grown = (BaiduEntry*)c_realloc (values, next * sizeof (*values));
            if (grown == NULL) {
                c_free (pageEntries);
                baidu_set_error (task, ENOMEM, "baidu_network_error: out of memory");
                goto fail;
            }
            values = grown;
            capacity = next;
        }
        memcpy (values + used, pageEntries, pageCount * sizeof (*values));
        used += pageCount;
        c_free (pageEntries);
        if (pageCount < BAIDU_LIST_PAGE_SIZE) {
            complete = true;
            break;
        }
    }
    if (!complete) {
        /* The page cap is a hard bound; the caller works with what it has. */
        (void)baidu_set_error (task, EFBIG, "baidu_share_too_large: transfer directory has too many entries");
        goto fail;
    }
    *entries = values;
    *count = used;
    return 0;
fail:
    c_free (values);
    return -1;
}

static int baidu_split_parent (const char* path, char* parent, size_t parentCapacity, const char** name)
{
    const char* slash = strrchr (path, '/');
    size_t      length;

    if (slash == NULL || slash[1] == '\0')
        return -1;
    *name = slash + 1;
    length = (size_t)(slash - path);
    if (length == 0) {
        if (parentCapacity < 2U)
            return -1;
        (void)snprintf (parent, parentCapacity, "/");
        return 0;
    }
    if (length >= parentCapacity)
        return -1;
    memcpy (parent, path, length);
    parent[length] = '\0';
    return 0;
}

static int baidu_resolve_entry (BaiduTask* task, const char* path, BaiduEntry* entry)
{
    char        parent[DOWNLOAD_TASK_PATH_SIZE];
    const char* name;
    int         result = -1;

    if (baidu_split_parent (path, parent, sizeof (parent), &name) != 0) {
        errno = EINVAL;
        return -1;
    }
    for (int page = 1; page <= BAIDU_MAX_LIST_PAGES; page++) {
        BaiduEntry* entries = NULL;
        size_t      count = 0;
        if (baidu_list_page (task, parent, page, &entries, &count) != 0)
            return -1;
        for (size_t index = 0; index < count; index++) {
            if (strcmp (entries[index].name, name) == 0) {
                *entry = entries[index];
                result = 0;
                break;
            }
        }
        c_free (entries);
        if (result == 0 || count < 1000U)
            break;
    }
    if (result != 0)
        return baidu_set_error (task, ENOENT, "baidu_file_not_found: path does not exist");
    return 0;
}

static int baidu_resolve_file (BaiduTask* task, char* fsid, size_t fsidCapacity, uint64_t* size,
                               char* md5, size_t md5Capacity)
{
    BaiduEntry entry;

    if (baidu_resolve_entry (task, task->apiPath, &entry) != 0)
        return -1;
    if (entry.isdir)
        return baidu_set_error (task, ENOENT, "baidu_file_not_found: path is a directory");
    (void)snprintf (fsid, fsidCapacity, "%s", entry.fsid);
    (void)snprintf (md5, md5Capacity, "%s", entry.md5);
    *size = entry.size;
    return 0;
}

typedef struct
{
    BaiduTask*    task;
    ProtocolItem* items;
    size_t        count;
    size_t        capacity;
    size_t        maxFiles;
} BaiduExpansion;

static bool baidu_valid_entry_name (const char* name)
{
    if (name == NULL || name[0] == '\0' || strchr (name, '/') != NULL || strchr (name, '\\') != NULL ||
        strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
        return false;
    for (const unsigned char* letter = (const unsigned char*)name; *letter != '\0'; letter++) {
        if (*letter < 32 || *letter == 127)
            return false;
    }
    return true;
}

static int baidu_escape_json (const char* value, char* destination, size_t capacity)
{
    size_t written = 0;

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
        {
            size_t length = escape == NULL ? 1U : strlen (escape);
            if (written + length >= capacity)
                return -1;
            if (escape == NULL)
                destination[written] = (char)*letter;
            else
                memcpy (destination + written, escape, length);
            written += length;
        }
    }
    destination[written] = '\0';
    return 0;
}

static int baidu_item_extra (const BaiduEntry* entry, const char* apiPath, const char* sourceShare,
                             char* destination, size_t capacity)
{
    char fsid[512];
    char path[8192];
    char md5[1024];
    char share[512];
    int  written;

    if (baidu_escape_json (entry->fsid, fsid, sizeof (fsid)) != 0 ||
        baidu_escape_json (apiPath, path, sizeof (path)) != 0 ||
        baidu_escape_json (entry->md5, md5, sizeof (md5)) != 0 ||
        (sourceShare != NULL && baidu_escape_json (sourceShare, share, sizeof (share)) != 0)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    written = snprintf (destination, capacity,
                        "{\"baidu\":{\"kind\":\"own\",\"fs_id\":\"%s\",\"api_path\":\"%s\",\"md5\":\"%s\","
                        "\"size\":%llu%s%s%s}}",
                        fsid, path, md5, (unsigned long long)entry->size,
                        sourceShare != NULL ? ",\"source_share\":\"" : "",
                        sourceShare != NULL ? share : "",
                        sourceShare != NULL ? "\"" : "");
    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int baidu_expand_add (BaiduExpansion* expansion, const BaiduEntry* entry, const char* apiPath,
                             const char* targetSubdirectory, const char* sourceShare)
{
    ProtocolItem* item;
    char*         escaped = NULL;
    size_t        escapedSize = 0;
    char          url[DOWNLOAD_TASK_URL_SIZE];

    if (expansion->count >= expansion->maxFiles)
        return baidu_set_error (expansion->task, EFBIG, "baidu_share_too_large: too many files");
    if (expansion->count == expansion->capacity) {
        size_t        next = expansion->capacity == 0 ? 32U : expansion->capacity * 2U;
        ProtocolItem* grown = (ProtocolItem*)c_realloc_array (expansion->items, next, sizeof (*grown));
        if (grown == NULL)
            return baidu_set_error (expansion->task, ENOMEM, "baidu_network_error: out of memory");
        expansion->items = grown;
        expansion->capacity = next;
    }
    if (apiPath == NULL || apiPath[0] != '/')
        return baidu_set_error (expansion->task, EINVAL, "invalid_url: expanded path is not absolute");
    item = &expansion->items[expansion->count];
    memset (item, 0, sizeof (*item));
    if (c_uri_escape_string (c_string_view_from_cstr (apiPath + 1), &escaped, &escapedSize) != 0)
        return baidu_set_error (expansion->task, ENOMEM, "baidu_network_error: cannot encode the path");
    int written = snprintf (url, sizeof (url), "baidu:///%s", escaped);
    c_free (escaped);
    if (written < 0 || (size_t)written >= sizeof (url))
        return baidu_set_error (expansion->task, ENAMETOOLONG, "invalid_name: expanded url is too long");
    (void)snprintf (item->resourceUrl, sizeof (item->resourceUrl), "%s", url);
    (void)snprintf (item->fileName, sizeof (item->fileName), "%s", entry->name);
    (void)snprintf (item->targetSubdirectory, sizeof (item->targetSubdirectory), "%s",
                    targetSubdirectory == NULL ? "" : targetSubdirectory);
    if (baidu_item_extra (entry, apiPath, sourceShare, item->extraJson, sizeof (item->extraJson)) != 0)
        return baidu_set_error (expansion->task, EINVAL, "invalid_extra: cannot serialize item metadata");
    item->totalBytes = entry->size;
    (void)snprintf (item->checksumType, sizeof (item->checksumType), "md5");
    (void)snprintf (item->checksumValue, sizeof (item->checksumValue), "%s", entry->md5);
    expansion->count++;
    return 0;
}

static int baidu_parent_subdirectory (const char* fullPath, char* destination, size_t capacity)
{
    const char* slash;
    size_t      length;

    if (fullPath == NULL || fullPath[0] != '/' || (slash = strrchr (fullPath, '/')) == NULL)
        return -1;
    length = (size_t)(slash - fullPath);
    if (length == 0) {
        destination[0] = '\0';
        return 0;
    }
    if (length - 1U >= capacity)
        return -1;
    memcpy (destination, fullPath + 1, length - 1U);
    destination[length - 1U] = '\0';
    return 0;
}

static int baidu_expand_directory (BaiduExpansion* expansion, const char* directory, int depth)
{
    char (*seen)[64];
    size_t seenCount = 0;
    int    result = 0;

    if (depth > 64)
        return baidu_set_error (expansion->task, EFBIG, "baidu_share_too_large: directory nesting is too deep");
    seen = (char (*)[64])c_malloc0 (expansion->maxFiles * 64U);
    if (seen == NULL)
        return baidu_set_error (expansion->task, ENOMEM, "baidu_network_error: out of memory");
    for (int page = 1; page <= BAIDU_MAX_LIST_PAGES && result == 0; page++) {
        BaiduEntry* entries = NULL;
        size_t      count = 0;
        if (baidu_list_page (expansion->task, directory, page, &entries, &count) != 0) {
            result = -1;
            break;
        }
        for (size_t index = 0; index < count && result == 0; index++) {
            BaiduEntry* entry = &entries[index];
            bool        duplicate = false;
            char        fullPath[DOWNLOAD_TASK_PATH_SIZE];
            int         written;

            if (!baidu_valid_entry_name (entry->name))
                continue;
            for (size_t previous = 0; previous < seenCount; previous++) {
                if (strcmp (seen[previous], entry->fsid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;
            if (seenCount < expansion->maxFiles)
                (void)snprintf (seen[seenCount++], 64, "%s", entry->fsid);
            written = strcmp (directory, "/") == 0
                ? snprintf (fullPath, sizeof (fullPath), "/%s", entry->name)
                : snprintf (fullPath, sizeof (fullPath), "%s/%s", directory, entry->name);
            if (written < 0 || (size_t)written >= sizeof (fullPath)) {
                result = baidu_set_error (expansion->task, ENAMETOOLONG, "invalid_name: path is too long");
                break;
            }
            if (entry->isdir) {
                result = baidu_expand_directory (expansion, fullPath, depth + 1);
            } else {
                char subdirectory[DOWNLOAD_TASK_PATH_SIZE];
                if (baidu_parent_subdirectory (fullPath, subdirectory, sizeof (subdirectory)) != 0) {
                    result = baidu_set_error (expansion->task, ENAMETOOLONG, "invalid_name: path is too long");
                    break;
                }
                result = baidu_expand_add (expansion, entry, fullPath, subdirectory, NULL);
            }
        }
        c_free (entries);
        if (count < 1000U)
            break;
    }
    c_free (seen);
    return result;
}

static void baidu_copy_protocol_error (const BaiduTask* task, ProtocolError* error)
{
    const char* message = task->errorMessage;
    const char* colon = strchr (message, ':');
    size_t      codeLength = colon == NULL ? strlen (message) : (size_t)(colon - message);

    if (codeLength >= sizeof (error->code))
        codeLength = sizeof (error->code) - 1U;
    memcpy (error->code, message, codeLength);
    error->code[codeLength] = '\0';
    (void)snprintf (error->message, sizeof (error->message), "%s", message);
}

static int baidu_share_page_get (BaiduTask* task, BaiduSession* session, const char* surl,
                                 BaiduShareTokens* tokens)
{
    char        url[BAIDU_URL_SIZE];
    char        referer[BAIDU_URL_SIZE];
    BaiduBuffer buffer = { 0 };
    CURLcode    code;

    (void)snprintf (url, sizeof (url), "%s/s/%s", gBaiduConfig.apiBase, surl);
    (void)snprintf (referer, sizeof (referer), "%s/disk/home", gBaiduConfig.apiBase);
    code = baidu_session_request (session, url, NULL, referer, &buffer);
    if (code != CURLE_OK) {
        baidu_buffer_clear (&buffer);
        return baidu_set_error (task, EAGAIN, "baidu_network_error: cannot open the share page");
    }
    if (session->status >= 400) {
        baidu_buffer_clear (&buffer);
        return baidu_status_error (task, session->status, "baidu_auth_expired: share page was rejected");
    }
    if (buffer.data == NULL || baidu_share_page_tokens (buffer.data, tokens) != 0) {
        int savedErrno = errno;
        baidu_buffer_clear (&buffer);
        if (savedErrno == ESTALE)
            return baidu_set_error (task, ESTALE, "baidu_share_expired: share link is no longer valid");
        if (savedErrno == ENOENT)
            return baidu_set_error (task, ENOENT, "baidu_share_not_found: share link does not exist");
        return baidu_set_error (task, EPROTO, "baidu_share_not_found: cannot parse the share page");
    }
    baidu_buffer_clear (&buffer);
    return 0;
}

static int baidu_share_verify (BaiduTask* task, BaiduSession* session, const BaiduShareTokens* tokens,
                               const char* pwd)
{
    char        url[BAIDU_URL_SIZE];
    char        body[1024];
    char*       escapedPwd;
    char*       escapedToken;
    BaiduBuffer buffer = { 0 };
    CURLcode    code;
    int         result = -1;

    escapedPwd = curl_easy_escape (session->curl, pwd, 0);
    escapedToken = curl_easy_escape (session->curl, tokens->bdstoken, 0);
    if (escapedPwd == NULL || escapedToken == NULL) {
        curl_free (escapedPwd);
        curl_free (escapedToken);
        return baidu_set_error (task, ENOMEM, "baidu_network_error: cannot encode verify parameters");
    }
    (void)snprintf (url, sizeof (url), "%s/share/verify?shareid=%s&time=%lld&clienttype=1&uk=%s",
                    gBaiduConfig.apiBase, tokens->shareid, (long long)time (NULL) * 1000LL, tokens->shareUk);
    (void)snprintf (body, sizeof (body), "pwd=%s&vcode=null&vcode_str=null&bdstoken=%s", escapedPwd,
                    escapedToken);
    curl_free (escapedPwd);
    curl_free (escapedToken);
    code = baidu_session_request (session, url, body, NULL, &buffer);
    if (code != CURLE_OK) {
        result = baidu_set_error (task, EAGAIN, "baidu_network_error: cannot verify the share password");
        goto done;
    }
    if (session->status >= 500) {
        result = baidu_set_error (task, EAGAIN, "baidu_network_error: server returned HTTP 5xx");
        goto done;
    }
    {
        size_t         errorOffset = 0;
        CJsonDocument* document = c_json_document_new_from_data (
            c_string_view_from_data (buffer.data == NULL ? "" : buffer.data, buffer.size), C_JSON_PARSE_NONE,
            &errorOffset);
        int remote;
        if (document == NULL) {
            result = baidu_set_error (task, EPROTO, "baidu_share_not_found: verify response is not JSON");
            goto done;
        }
        remote = baidu_json_errno (c_json_document_root (document));
        c_json_document_free (document);
        if (remote == 0) {
            result = 0;
        } else if (remote == -9) {
            result = baidu_set_error (task, EINVAL, "baidu_password_invalid: extraction code is wrong");
        } else if (remote == 8001) {
            result = baidu_set_error (task, EACCES, "baidu_challenge_required: Baidu requires verification");
        } else {
            char message[DOWNLOAD_TASK_ERROR_SIZE];
            (void)snprintf (message, sizeof (message), "baidu_transfer_failed: verify errno %d", remote);
            result = baidu_set_error (task, EIO, message);
        }
    }
done:
    baidu_buffer_clear (&buffer);
    return result;
}

static int baidu_share_list (BaiduTask* task, BaiduSession* session, const char* surl,
                             const BaiduShareTokens* tokens, BaiduEntry** entries, size_t* count)
{
    char        url[BAIDU_URL_SIZE];
    char*       escapedToken;
    BaiduBuffer buffer = { 0 };
    CURLcode    code;
    int         result = -1;

    *entries = NULL;
    *count = 0;
    escapedToken = curl_easy_escape (session->curl, tokens->bdstoken, 0);
    if (escapedToken == NULL)
        return baidu_set_error (task, ENOMEM, "baidu_network_error: cannot encode the share token");
    (void)snprintf (url, sizeof (url),
                    "%s/share/list?bdstoken=%s&root=1&web=5&app_id=250528&shorturl=%s&channel=chunlei",
                    gBaiduConfig.apiBase, escapedToken, surl);
    curl_free (escapedToken);
    code = baidu_session_request (session, url, NULL, NULL, &buffer);
    if (code != CURLE_OK) {
        result = baidu_set_error (task, EAGAIN, "baidu_network_error: cannot list the share root");
        goto done;
    }
    if (session->status >= 500) {
        result = baidu_set_error (task, EAGAIN, "baidu_network_error: server returned HTTP 5xx");
        goto done;
    }
    {
        size_t         errorOffset = 0;
        CJsonDocument* document = c_json_document_new_from_data (
            c_string_view_from_data (buffer.data == NULL ? "" : buffer.data, buffer.size), C_JSON_PARSE_NONE,
            &errorOffset);
        CJsonValue* root;
        CJsonValue* list;
        size_t      itemCount;
        BaiduEntry* values = NULL;
        size_t      used = 0;
        int         remote;

        if (document == NULL) {
            result = baidu_set_error (task, EPROTO, "baidu_share_not_found: share list is not JSON");
            goto done;
        }
        root = c_json_document_root (document);
        remote = baidu_json_errno (root);
        if (remote == -9) {
            c_json_document_free (document);
            result = -2;
            goto done;
        }
        if (remote == 8001) {
            c_json_document_free (document);
            result = baidu_set_error (task, EACCES, "baidu_challenge_required: Baidu requires verification");
            goto done;
        }
        if (remote != 0) {
            char message[DOWNLOAD_TASK_ERROR_SIZE];
            c_json_document_free (document);
            (void)snprintf (message, sizeof (message), "baidu_share_not_found: list errno %d", remote);
            result = baidu_set_error (task, ENOENT, message);
            goto done;
        }
        list = c_json_object_get (root, c_string_view_from_cstr ("list"));
        itemCount = list == NULL ? 0 : c_json_array_size (list);
        if (itemCount > 0) {
            values = (BaiduEntry*)c_malloc0 (itemCount * sizeof (*values));
            if (values == NULL) {
                c_json_document_free (document);
                result = baidu_set_error (task, ENOMEM, "baidu_network_error: out of memory");
                goto done;
            }
        }
        for (size_t index = 0; index < itemCount; index++) {
            CJsonValue* item = c_json_array_get (list, index);
            BaiduEntry* entry = &values[used];
            int64_t     isDirectory = 0;
            int64_t     sizeValue = 0;
            if (item == NULL ||
                baidu_json_scalar (item, "server_filename", entry->name, sizeof (entry->name)) != 0 ||
                baidu_json_scalar (item, "fs_id", entry->fsid, sizeof (entry->fsid)) != 0)
                continue;
            (void)baidu_json_int64 (item, "isdir", &isDirectory);
            (void)baidu_json_int64 (item, "size", &sizeValue);
            entry->isdir = isDirectory != 0;
            entry->size = sizeValue > 0 ? (uint64_t)sizeValue : 0;
            entry->md5[0] = '\0';
            (void)baidu_json_scalar (item, "md5", entry->md5, sizeof (entry->md5));
            if (baidu_json_scalar (item, "path", entry->path, sizeof (entry->path)) != 0)
                (void)snprintf (entry->path, sizeof (entry->path), "/%s", entry->name);
            used++;
        }
        c_json_document_free (document);
        *entries = values;
        *count = used;
        result = 0;
    }
done:
    baidu_buffer_clear (&buffer);
    return result;
}

static int baidu_share_mkdir (BaiduTask* task, BaiduSession* session, const char* path)
{
    char*       escaped = curl_easy_escape (session->curl, path, 0);
    char        url[BAIDU_URL_SIZE];
    BaiduBuffer buffer = { 0 };
    CURLcode    code;
    int         result = -1;

    if (escaped == NULL)
        return baidu_set_error (task, ENOMEM, "baidu_network_error: cannot encode the transfer path");
    (void)snprintf (url, sizeof (url), "%s/rest/2.0/pcs/file?method=mkdir&path=%s&app_id=250528",
                    gBaiduConfig.pcsBase, escaped);
    curl_free (escaped);
    code = baidu_session_request (session, url, "", NULL, &buffer);
    if (code != CURLE_OK) {
        result = baidu_set_error (task, EAGAIN, "baidu_network_error: cannot create the transfer directory");
        goto done;
    }
    if (session->status >= 500) {
        result = baidu_set_error (task, EAGAIN, "baidu_network_error: server returned HTTP 5xx");
        goto done;
    }
    {
        size_t         errorOffset = 0;
        CJsonDocument* document = c_json_document_new_from_data (
            c_string_view_from_data (buffer.data == NULL ? "" : buffer.data, buffer.size), C_JSON_PARSE_NONE,
            &errorOffset);
        int remote = 0;
        if (document != NULL) {
            remote = baidu_json_errno (c_json_document_root (document));
            c_json_document_free (document);
        }
        if (remote == 0 || remote == -8) {
            result = 0;
        } else {
            char message[DOWNLOAD_TASK_ERROR_SIZE];
            (void)snprintf (message, sizeof (message), "baidu_transfer_failed: mkdir errno %d", remote);
            result = baidu_set_error (task, EIO, message);
        }
    }
done:
    baidu_buffer_clear (&buffer);
    return result;
}

static int baidu_share_transfer (BaiduTask* task, BaiduSession* session, const char* surl,
                                 const BaiduShareTokens* tokens, const BaiduEntry* entries, size_t count,
                                 const char* targetDirectory, char* transferred, size_t transferredCapacity)
{
    char  fsids[2048];
    char  url[BAIDU_URL_SIZE];
    char  body[BAIDU_URL_SIZE];
    char* escapedFsids;
    char* escapedTarget;
    char* escapedToken;
    char* escapedName;
    BaiduBuffer buffer = { 0 };
    CURLcode    code;
    size_t      offset = 0;
    int         result = -1;

    fsids[offset++] = '[';
    for (size_t index = 0; index < count && offset < sizeof (fsids); index++) {
        int written = snprintf (fsids + offset, sizeof (fsids) - offset, "%s%s", index == 0 ? "" : ",",
                                entries[index].fsid);
        if (written < 0 || (size_t)written >= sizeof (fsids) - offset)
            return baidu_set_error (task, ENAMETOOLONG, "baidu_transfer_failed: too many files");
        offset += (size_t)written;
    }
    if (offset + 2U > sizeof (fsids))
        return baidu_set_error (task, ENAMETOOLONG, "baidu_transfer_failed: too many files");
    fsids[offset++] = ']';
    fsids[offset] = '\0';
    escapedFsids = curl_easy_escape (session->curl, fsids, 0);
    escapedTarget = curl_easy_escape (session->curl, targetDirectory, 0);
    escapedToken = curl_easy_escape (session->curl, tokens->bdstoken, 0);
    escapedName = curl_easy_escape (session->curl, entries[0].name, 0);
    if (escapedFsids == NULL || escapedTarget == NULL || escapedToken == NULL || escapedName == NULL) {
        curl_free (escapedFsids);
        curl_free (escapedTarget);
        curl_free (escapedToken);
        curl_free (escapedName);
        return baidu_set_error (task, ENOMEM, "baidu_transfer_failed: cannot encode transfer parameters");
    }
    (void)snprintf (url, sizeof (url),
                    "%s/share/transfer?app_id=250528&channel=chunlei&clienttype=0&web=1&shareid=%s&from=%s"
                    "&bdstoken=%s&filename=%s&item_num=%zu&fs_id=%s&path=%s",
                    gBaiduConfig.apiBase, tokens->shareid, tokens->shareUk, escapedToken, escapedName, count,
                    escapedFsids, escapedTarget);
    (void)snprintf (body, sizeof (body), "fsidlist=%s&path=%s", escapedFsids, escapedTarget);
    curl_free (escapedFsids);
    curl_free (escapedTarget);
    curl_free (escapedToken);
    curl_free (escapedName);
    code = CURLE_OK;
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t         errorOffset = 0;
        CJsonDocument* document;
        CJsonValue*    root;
        CJsonValue*    info;
        size_t         infoCount;
        int            remote;
        int            failure;

        code = baidu_session_request (session, url, body, NULL, &buffer);
        if (code != CURLE_OK || session->status >= 500) {
            if (attempt == 0)
                continue;
            result = baidu_set_error (task, EAGAIN,
                                      code != CURLE_OK ? "baidu_network_error: transfer request failed"
                                                       : "baidu_network_error: server returned HTTP 5xx");
            goto done;
        }
        document = c_json_document_new_from_data (
            c_string_view_from_data (buffer.data == NULL ? "" : buffer.data, buffer.size), C_JSON_PARSE_NONE,
            &errorOffset);
        if (document == NULL) {
            if (attempt == 0)
                continue;
            result = baidu_set_error (task, EPROTO, "baidu_transfer_failed: transfer response is not JSON");
            goto done;
        }
        root = c_json_document_root (document);
        remote = baidu_json_errno (root);
        info = c_json_object_get (root, c_string_view_from_cstr ("info"));
        infoCount = info == NULL ? 0 : c_json_array_size (info);
        /* Baidu reports "already exists" as errno 4 for a whole request and as
         * errno 12 with per-entry errno -30 inside info[]; both mean the target
         * is already in the transfer directory, which is what reuse wants. */
        failure = remote;
        if (remote == 0 || remote == 4) {
            failure = 0;
        } else if (remote == 12 && infoCount > 0) {
            failure = 0;
            for (size_t index = 0; index < infoCount; index++) {
                CJsonValue* item = c_json_array_get (info, index);
                int64_t     entryErrno = 0;
                if (item == NULL || baidu_json_int64 (item, "errno", &entryErrno) != 0 ||
                    (entryErrno != 0 && entryErrno != -30)) {
                    failure = item != NULL && baidu_json_int64 (item, "errno", &entryErrno) == 0
                                  ? (int)entryErrno
                                  : remote;
                    break;
                }
            }
        }
        if (failure != 0) {
            c_json_document_free (document);
            if (attempt == 0 && failure != 8001 && failure != -9)
                continue;
            if (failure == 8001) {
                result = baidu_set_error (task, EACCES, "baidu_challenge_required: Baidu requires verification");
            } else {
                char message[DOWNLOAD_TASK_ERROR_SIZE];
                (void)snprintf (message, sizeof (message), "baidu_transfer_failed: transfer errno %d", failure);
                result = baidu_set_error (task, EIO, message);
            }
            goto done;
        }
        {
            CJsonValue* first = infoCount > 0 ? c_json_array_get (info, 0) : NULL;
            char        path[DOWNLOAD_TASK_PATH_SIZE];
            if (first != NULL && baidu_json_scalar (first, "path", path, sizeof (path)) == 0 && path[0] != '\0') {
                (void)snprintf (transferred, transferredCapacity, "%s", path);
            } else if (count == 1) {
                (void)snprintf (transferred, transferredCapacity, "%s/%s", targetDirectory, entries[0].name);
            } else {
                (void)snprintf (transferred, transferredCapacity, "%s", targetDirectory);
            }
        }
        c_json_document_free (document);
        result = 0;
        break;
    }
done:
    baidu_buffer_clear (&buffer);
    return result;
}

static bool baidu_share_filter_matches (const char* sharePath, const char* filter)
{
    size_t length;

    if (filter == NULL || filter[0] == '\0')
        return true;
    if (strcmp (sharePath, filter) == 0)
        return true;
    length = strlen (filter);
    return strncmp (sharePath, filter, length) == 0 && sharePath[length] == '/';
}

static BaiduEntry* baidu_find_entry (BaiduEntry* entries, size_t count, const char* name)
{
    for (size_t index = 0; index < count; index++) {
        if (strcmp (entries[index].name, name) == 0)
            return &entries[index];
    }
    return NULL;
}

static int baidu_expand_share_tree (BaiduExpansion* expansion, BaiduTask* task, const char* transferredDirectory,
                                    const char* sharePrefix, const char* filter, const char* surl, int depth,
                                    bool* matched)
{
    char (*seen)[64];
    size_t seenCount = 0;
    int    result = 0;

    if (depth > 64)
        return baidu_set_error (task, EFBIG, "baidu_share_too_large: directory nesting is too deep");
    seen = (char (*)[64])c_malloc0 (expansion->maxFiles * 64U);
    if (seen == NULL)
        return baidu_set_error (task, ENOMEM, "baidu_network_error: out of memory");
    for (int page = 1; page <= BAIDU_MAX_LIST_PAGES && result == 0; page++) {
        BaiduEntry* entries = NULL;
        size_t      count = 0;
        if (baidu_list_page (task, transferredDirectory, page, &entries, &count) != 0) {
            result = -1;
            break;
        }
        for (size_t index = 0; index < count && result == 0; index++) {
            BaiduEntry* entry = &entries[index];
            bool        duplicate = false;
            char        sharePath[DOWNLOAD_TASK_PATH_SIZE];
            char        transferredPath[DOWNLOAD_TASK_PATH_SIZE];
            int         written;

            if (!baidu_valid_entry_name (entry->name))
                continue;
            for (size_t previous = 0; previous < seenCount; previous++) {
                if (strcmp (seen[previous], entry->fsid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;
            if (seenCount < expansion->maxFiles)
                (void)snprintf (seen[seenCount++], 64, "%s", entry->fsid);
            written = snprintf (sharePath, sizeof (sharePath), "%s/%s", sharePrefix, entry->name);
            if (written < 0 || (size_t)written >= sizeof (sharePath)) {
                result = baidu_set_error (task, ENAMETOOLONG, "invalid_name: share path is too long");
                break;
            }
            written = snprintf (transferredPath, sizeof (transferredPath), "%s/%s", transferredDirectory,
                                entry->name);
            if (written < 0 || (size_t)written >= sizeof (transferredPath)) {
                result = baidu_set_error (task, ENAMETOOLONG, "invalid_name: transfer path is too long");
                break;
            }
            if (entry->isdir) {
                result = baidu_expand_share_tree (expansion, task, transferredPath, sharePath, filter, surl,
                                                  depth + 1, matched);
            } else if (baidu_share_filter_matches (sharePath, filter)) {
                char subdirectory[DOWNLOAD_TASK_PATH_SIZE];
                if (baidu_parent_subdirectory (sharePath, subdirectory, sizeof (subdirectory)) != 0) {
                    result = baidu_set_error (task, ENAMETOOLONG, "invalid_name: share path is too long");
                    break;
                }
                result = baidu_expand_add (expansion, entry, transferredPath, subdirectory, surl);
                if (result == 0)
                    *matched = true;
            }
        }
        c_free (entries);
        if (count < 1000U)
            break;
    }
    c_free (seen);
    return result;
}

static int baidu_expand_share (BaiduTask* task, const DownloadTask* request, const char* surl,
                               BaiduExpansion* expansion, char* groupName, size_t groupNameCapacity)
{
    BaiduTaskOptions options;
    BaiduSession     session;
    BaiduShareTokens tokens;
    BaiduEntry*      entries = NULL;
    size_t           count = 0;
    BaiduEntry*      existing = NULL;
    size_t           existingCount = 0;
    size_t*          missing = NULL;
    BaiduEntry*      missingEntries = NULL;
    size_t           missingCount = 0;
    char             transferDirectory[DOWNLOAD_TASK_PATH_SIZE];
    bool             matched = false;
    int              listResult;
    int              result = -1;

    if (baidu_task_options (request->extraJson, &options) != 0)
        return baidu_set_error (task, EINVAL, "invalid_url: cannot parse baidu task options");
    if (options.pwd[0] == '\0')
        (void)baidu_share_url_pwd (request->resourceUrl, options.pwd, sizeof (options.pwd));
    if (baidu_session_init (task, &session) != 0)
        return baidu_set_error (task, ENOMEM, "baidu_auth_missing: cannot create HTTP session");
    if (baidu_share_page_get (task, &session, surl, &tokens) != 0)
        goto done;
    if (options.pwd[0] != '\0') {
        if (baidu_share_verify (task, &session, &tokens, options.pwd) != 0)
            goto done;
        if (baidu_share_page_get (task, &session, surl, &tokens) != 0)
            goto done;
    }
    listResult = baidu_share_list (task, &session, surl, &tokens, &entries, &count);
    if (listResult == -2) {
        if (options.pwd[0] == '\0')
            result = baidu_set_error (task, EINVAL, "baidu_password_required: extraction code is required");
        else
            result = baidu_set_error (task, EINVAL, "baidu_password_invalid: extraction code is wrong");
        goto done;
    }
    if (listResult != 0)
        goto done;
    if (count == 0) {
        result = baidu_set_error (task, ENODATA, "baidu_share_empty: share contains no files");
        goto done;
    }
    (void)snprintf (transferDirectory, sizeof (transferDirectory), "%s/%s", gBaiduConfig.transferDirectory,
                    surl);
    if (baidu_share_mkdir (task, &session, transferDirectory) != 0)
        goto done;
    if (baidu_list_all (task, transferDirectory, &existing, &existingCount) != 0)
        goto done;
    missing = (size_t*)c_malloc0 (count * sizeof (*missing));
    if (missing == NULL) {
        result = baidu_set_error (task, ENOMEM, "baidu_network_error: out of memory");
        goto done;
    }
    for (size_t index = 0; index < count; index++) {
        BaiduEntry* found = baidu_find_entry (existing, existingCount, entries[index].name);
        if (found == NULL) {
            missing[missingCount++] = index;
            continue;
        }
        if (found->isdir != entries[index].isdir ||
            (!entries[index].isdir &&
             (found->size != entries[index].size ||
              (entries[index].md5[0] != '\0' && found->md5[0] != '\0' &&
               strcasecmp (entries[index].md5, found->md5) != 0)))) {
            result = baidu_set_error (task, EEXIST,
                                      "baidu_transfer_conflict: transfer directory has a different file");
            goto done;
        }
    }
    if (missingCount > 0) {
        char scratch[DOWNLOAD_TASK_PATH_SIZE];
        missingEntries = (BaiduEntry*)c_malloc0 (missingCount * sizeof (*missingEntries));
        if (missingEntries == NULL) {
            result = baidu_set_error (task, ENOMEM, "baidu_network_error: out of memory");
            goto done;
        }
        for (size_t index = 0; index < missingCount; index++)
            missingEntries[index] = entries[missing[index]];
        /* The transfer endpoint rejects very large fs_id lists and the web API
         * has a per-request item limit; split the roots into bounded batches. */
        for (size_t offset = 0; offset < missingCount; offset += BAIDU_TRANSFER_CHUNK_SIZE) {
            size_t chunk = missingCount - offset;
            if (chunk > BAIDU_TRANSFER_CHUNK_SIZE)
                chunk = BAIDU_TRANSFER_CHUNK_SIZE;
            if (baidu_share_transfer (task, &session, surl, &tokens, missingEntries + offset, chunk,
                                      transferDirectory, scratch, sizeof (scratch)) != 0)
                goto done;
        }
    }
    for (size_t index = 0; index < count; index++) {
        char transferredRoot[DOWNLOAD_TASK_PATH_SIZE];
        int  written = snprintf (transferredRoot, sizeof (transferredRoot), "%s%s", transferDirectory,
                                 entries[index].path);
        if (written < 0 || (size_t)written >= sizeof (transferredRoot)) {
            result = baidu_set_error (task, ENAMETOOLONG, "invalid_name: transfer path is too long");
            goto done;
        }
        if (entries[index].isdir) {
            if (baidu_expand_share_tree (expansion, task, transferredRoot, entries[index].path, options.path,
                                         surl, 0, &matched) != 0)
                goto done;
        } else if (baidu_share_filter_matches (entries[index].path, options.path)) {
            BaiduEntry transferredEntry;
            char       subdirectory[DOWNLOAD_TASK_PATH_SIZE];
            if (baidu_resolve_entry (task, transferredRoot, &transferredEntry) != 0)
                goto done;
            if (baidu_parent_subdirectory (entries[index].path, subdirectory, sizeof (subdirectory)) != 0) {
                result = baidu_set_error (task, ENAMETOOLONG, "invalid_name: share path is too long");
                goto done;
            }
            if (baidu_expand_add (expansion, &transferredEntry, transferredRoot, subdirectory, surl) != 0)
                goto done;
            matched = true;
        }
    }
    if (options.path[0] != '\0' && !matched) {
        result = baidu_set_error (task, ENOENT, "baidu_share_path_not_found: selected share path is empty");
        goto done;
    }
    if (expansion->count == 0) {
        result = baidu_set_error (task, ENODATA, "baidu_share_empty: share contains no files");
        goto done;
    }
    if (count == 1)
        (void)snprintf (groupName, groupNameCapacity, "%s", entries[0].name);
    else
        (void)snprintf (groupName, groupNameCapacity, "百度分享-%s", surl);
    result = 0;
done:
    baidu_session_close (&session);
    c_free (entries);
    c_free (existing);
    c_free (missing);
    c_free (missingEntries);
    return result;
}

static int baidu_expand (const DownloadTask* request, ProtocolItem** items, size_t* count,
                         char* groupName, size_t groupNameCapacity, ProtocolError* error)
{
    BaiduTask      task;
    char           path[DOWNLOAD_TASK_PATH_SIZE];
    char           share[BAIDU_SURL_SIZE];
    BaiduEntry     entry;
    BaiduExpansion expansion;

    if (request == NULL || items == NULL || count == NULL || groupName == NULL || error == NULL) {
        errno = EINVAL;
        return -1;
    }
    *items = NULL;
    *count = 0;
    groupName[0] = '\0';
    memset (error, 0, sizeof (*error));
    if (!baidu_ensure_init ()) {
        (void)snprintf (error->code, sizeof (error->code), "out_of_memory");
        return -1;
    }
    memset (&task, 0, sizeof (task));
    task.source = *request;
    if (request->requestContext == NULL || !request_context_has_auth (request->requestContext) ||
        request_context_auth_kind (request->requestContext) != DOWNLOADER_CREDENTIAL_COOKIE) {
        (void)snprintf (error->code, sizeof (error->code), "baidu_auth_missing");
        (void)snprintf (error->message, sizeof (error->message),
                        "baidu_auth_missing: select a cookie credential profile");
        errno = EPERM;
        return -1;
    }
    (void)snprintf (task.cookie, sizeof (task.cookie), "%s",
                    request_context_auth_secret (request->requestContext));
    if (baidu_cookie_login (task.cookie) != 0) {
        (void)snprintf (error->code, sizeof (error->code), "baidu_auth_missing");
        (void)snprintf (error->message, sizeof (error->message),
                        "baidu_auth_missing: cookie needs BDUSS and STOKEN");
        errno = EPERM;
        return -1;
    }
    if (!baidu_own_url (request->resourceUrl, path, sizeof (path))) {
        if (baidu_share_url (request->resourceUrl, share, sizeof (share))) {
            memset (&expansion, 0, sizeof (expansion));
            expansion.task = &task;
            expansion.maxFiles = (size_t)gBaiduConfig.maxFiles;
            if (baidu_expand_share (&task, request, share, &expansion, groupName, groupNameCapacity) != 0) {
                c_free (expansion.items);
                baidu_copy_protocol_error (&task, error);
                return -1;
            }
            *items = expansion.items;
            *count = expansion.count;
            return 0;
        }
        (void)snprintf (error->code, sizeof (error->code), "invalid_url");
        (void)snprintf (error->message, sizeof (error->message), "invalid_url: unsupported baidu path");
        errno = EINVAL;
        return -1;
    }
    if (baidu_resolve_entry (&task, path, &entry) != 0) {
        baidu_copy_protocol_error (&task, error);
        return -1;
    }
    memset (&expansion, 0, sizeof (expansion));
    expansion.task = &task;
    expansion.maxFiles = (size_t)gBaiduConfig.maxFiles;
    if (entry.isdir) {
        const char* rootName = strrchr (path, '/');
        if (rootName == NULL || rootName[1] == '\0') {
            (void)snprintf (error->code, sizeof (error->code), "invalid_url");
            errno = EINVAL;
            return -1;
        }
        (void)snprintf (groupName, groupNameCapacity, "%s", rootName + 1);
        if (baidu_expand_directory (&expansion, path, 0) != 0) {
            c_free (expansion.items);
            baidu_copy_protocol_error (&task, error);
            return -1;
        }
        if (expansion.count == 0) {
            c_free (expansion.items);
            (void)snprintf (error->code, sizeof (error->code), "baidu_file_not_found");
            (void)snprintf (error->message, sizeof (error->message), "baidu_file_not_found: directory is empty");
            errno = ENOENT;
            return -1;
        }
    } else {
        char subdirectory[DOWNLOAD_TASK_PATH_SIZE];
        if (baidu_parent_subdirectory (path, subdirectory, sizeof (subdirectory)) != 0 ||
            baidu_expand_add (&expansion, &entry, path, subdirectory, NULL) != 0) {
            c_free (expansion.items);
            baidu_copy_protocol_error (&task, error);
            return -1;
        }
    }
    *items = expansion.items;
    *count = expansion.count;
    return 0;
}

static int baidu_resolve_dlink (BaiduTask* task, const char* fsid, char* dlink, size_t capacity)
{
    BaiduSession session;
    BaiduBuffer  buffer = { 0 };
    int          result = -1;

    if (baidu_session_init (task, &session) != 0)
        return baidu_set_error (task, ENOMEM, "baidu_auth_missing: cannot create HTTP session");
    for (int attempt = 0; attempt < 2; attempt++) {
        char sign[256];
        char timestamp[32];
        char url[BAIDU_URL_SIZE];
        if (baidu_sign_get (task, sign, sizeof (sign), timestamp, sizeof (timestamp)) != 0) {
            result = -1;
            break;
        }
        char* escapedSign = curl_easy_escape (session.curl, sign, 0);
        char* escapedStamp = curl_easy_escape (session.curl, timestamp, 0);
        char  body[1024];
        if (escapedSign == NULL || escapedStamp == NULL) {
            curl_free (escapedSign);
            curl_free (escapedStamp);
            result = baidu_set_error (task, ENOMEM, "baidu_dlink_failed: cannot encode sign");
            break;
        }
        (void)snprintf (body, sizeof (body), "sign=%s&timestamp=%s&fidlist=%%5B%s%%5D", escapedSign, escapedStamp,
                        fsid);
        curl_free (escapedSign);
        curl_free (escapedStamp);
        (void)snprintf (url, sizeof (url), "%s/api/download", gBaiduConfig.apiBase);
        CURLcode code = baidu_session_request (&session, url, body, NULL, &buffer);
        if (code != CURLE_OK) {
            result = baidu_set_error (task, EAGAIN, "baidu_network_error: cannot resolve a dlink");
            break;
        }
        if (session.status == 401 || session.status == 403) {
            result = baidu_set_error (task, EACCES, "baidu_challenge_required: download request was rejected");
            break;
        }
        if (session.status >= 500) {
            result = baidu_set_error (task, EAGAIN, "baidu_network_error: server returned HTTP 5xx");
            break;
        }
        size_t errorOffset = 0;
        CJsonDocument* document = c_json_document_new_from_data (
            c_string_view_from_data (buffer.data == NULL ? "" : buffer.data, buffer.size), C_JSON_PARSE_NONE,
            &errorOffset);
        if (document == NULL) {
            result = baidu_set_error (task, EIO, "baidu_dlink_failed: download response is not JSON");
            break;
        }
        CJsonValue* root = c_json_document_root (document);
        int         remote = baidu_json_errno (root);
        if (remote == 0) {
            CJsonValue* links = c_json_object_get (root, c_string_view_from_cstr ("dlink"));
            CJsonValue* first = links == NULL ? NULL : c_json_array_get (links, 0);
            if (first == NULL || baidu_json_scalar (first, "dlink", dlink, capacity) != 0) {
                c_json_document_free (document);
                result = baidu_set_error (task, EIO, "baidu_dlink_failed: response has no dlink");
                break;
            }
            c_json_document_free (document);
            result = 0;
            break;
        }
        c_json_document_free (document);
        if (remote == 112 || remote == 113) {
            baidu_sign_invalidate (task->cookie);
            if (attempt == 0)
                continue;
            result = baidu_set_error (task, EIO, "baidu_dlink_failed: sign stayed invalid");
            break;
        }
        if (remote == 31066 || remote == 31297) {
            result = baidu_set_error (task, ENOENT, "baidu_file_not_found: the cloud file no longer exists");
            break;
        }
        if (remote == 8001) {
            result = baidu_set_error (task, EACCES, "baidu_challenge_required: Baidu requires verification");
            break;
        }
        char message[DOWNLOAD_TASK_ERROR_SIZE];
        (void)snprintf (message, sizeof (message), "baidu_dlink_failed: remote errno %d", remote);
        result = baidu_set_error (task, EIO, message);
        break;
    }
    baidu_session_close (&session);
    baidu_buffer_clear (&buffer);
    return result;
}

static int baidu_checkpoint (BaiduDownload* download, uint64_t downloaded)
{
    DownloadTaskCheckpoint checkpoint;

    if (download->task->callbacks.onCheckpoint == NULL)
        return 0;
    if (fflush (download->file) != 0)
        return -1;
    if (fsync (fileno (download->file)) != 0)
        return -1;
    memset (&checkpoint, 0, sizeof (checkpoint));
    checkpoint.version = DOWNLOAD_TASK_CHECKPOINT_VERSION;
    checkpoint.durableBytes = downloaded;
    (void)snprintf (checkpoint.effectiveUrl, sizeof (checkpoint.effectiveUrl), "%s",
                    download->task->source.resourceUrl);
    (void)snprintf (checkpoint.etag, sizeof (checkpoint.etag), "%s", download->md5);
    (void)snprintf (checkpoint.lastModified, sizeof (checkpoint.lastModified), "%llu",
                    (unsigned long long)download->expectedSize);
    if (download->task->callbacks.onCheckpoint (download->task->userData, &checkpoint) != 0)
        return -1;
    download->lastCheckpoint = downloaded;
    return 0;
}

typedef struct
{
    BaiduDownload* download;
} BaiduProgress;

static size_t baidu_download_write (void* data, size_t size, size_t count, void* userData)
{
    BaiduDownload* download = (BaiduDownload*)userData;
    size_t         length = size * count;
    uint64_t       absolute;

    if (download->task->pauseRequested || download->task->cancelRequested)
        return 0;
    if (length == 0)
        return 0;
    if (fwrite (data, size, count, download->file) != count) {
        download->localError = true;
        return 0;
    }
    download->written += length;
    absolute = download->offset + download->written;
    if (absolute - download->lastCheckpoint >= BAIDU_CHECKPOINT_INTERVAL &&
        baidu_checkpoint (download, absolute) != 0) {
        download->localError = true;
        return 0;
    }
    return length;
}

static int baidu_progress_cb (void* userData, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                              curl_off_t ulnow)
{
    BaiduDownload* download = (BaiduDownload*)userData;
    int            now;
    uint64_t       downloaded;

    (void)dltotal;
    (void)ultotal;
    (void)ulnow;
    if (download->task->pauseRequested || download->task->cancelRequested)
        return 1;
    if (download->task->callbacks.onProgress == NULL)
        return 0;
    now = baidu_now_ms ();
    if (now - download->lastProgressMs < BAIDU_PROGRESS_INTERVAL_MS && dlnow < dltotal)
        return 0;
    downloaded = download->offset + (uint64_t)(dlnow > 0 ? dlnow : 0);
    if (downloaded > download->expectedSize)
        downloaded = download->expectedSize;
    download->task->callbacks.onProgress (download->task->userData, download->expectedSize, downloaded, 0);
    download->lastProgressMs = now;
    return 0;
}

static CURLcode baidu_download_perform (BaiduTask* task, BaiduDownload* download, const char* dlink)
{
    BaiduSession session;
    char         range[64];

    if (baidu_session_init (task, &session) != 0)
        return CURLE_FAILED_INIT;
    curl_easy_setopt (session.curl, CURLOPT_URL, dlink);
    curl_easy_setopt (session.curl, CURLOPT_COOKIE, task->cookie);
    curl_easy_setopt (session.curl, CURLOPT_USERAGENT, "pan.baidu.com");
    curl_easy_setopt (session.curl, CURLOPT_REFERER, NULL);
    if (download->offset > 0) {
        (void)snprintf (range, sizeof (range), "%llu-", (unsigned long long)download->offset);
        curl_easy_setopt (session.curl, CURLOPT_RANGE, range);
    }
    curl_easy_setopt (session.curl, CURLOPT_WRITEFUNCTION, baidu_download_write);
    curl_easy_setopt (session.curl, CURLOPT_WRITEDATA, download);
    curl_easy_setopt (session.curl, CURLOPT_XFERINFOFUNCTION, baidu_progress_cb);
    curl_easy_setopt (session.curl, CURLOPT_XFERINFODATA, download);
    curl_easy_setopt (session.curl, CURLOPT_NOPROGRESS, 0L);
    download->attemptOffset = download->offset;
    download->localError = false;
    CURLcode result = curl_easy_perform (session.curl);
    if (result == CURLE_OK)
        (void)curl_easy_getinfo (session.curl, CURLINFO_RESPONSE_CODE, &session.status);
    task->status = session.status;
    task->lastResult = result;
    if (result != CURLE_OK)
        (void)snprintf (task->errorMessage, sizeof (task->errorMessage), "baidu_network_error: %s",
                        session.error[0] != '\0' ? session.error : curl_easy_strerror (result));
    baidu_session_close (&session);
    return result;
}

static int baidu_verify_md5 (BaiduDownload* download)
{
    CChecksum* context;
    unsigned char buffer[65536];
    size_t        received;
    char          actual[DOWNLOAD_TASK_CHECKSUM_SIZE];
    size_t        required = 0;
    int           result = -1;

    if (download->md5[0] == '\0')
        return 0;
    if (fseeko (download->file, 0, SEEK_SET) != 0)
        return -1;
    context = c_checksum_new (C_CHECKSUM_MD5);
    if (context == NULL)
        return -1;
    while ((received = fread (buffer, 1, sizeof (buffer), download->file)) > 0) {
        if (c_checksum_update (context, buffer, received) != 0)
            goto done;
    }
    if (ferror (download->file))
        goto done;
    if (c_checksum_final_hex (context, actual, sizeof (actual), &required) != 0)
        goto done;
    result = strcasecmp (actual, download->md5) == 0 ? 0 : -1;
done:
    c_checksum_free (context);
    (void)fseeko (download->file, 0, SEEK_END);
    return result;
}

static int baidu_run_impl (BaiduTask* task)
{
    char          fsid[64];
    uint64_t      expectedSize = 0;
    char          expectedMd5[DOWNLOAD_TASK_CHECKSUM_SIZE];
    BaiduDownload download;
    int           fd;
    struct stat   status;
    int           result = -1;

    memset (&download, 0, sizeof (download));
    memset (expectedMd5, 0, sizeof (expectedMd5));
    if (baidu_resolve_file (task, fsid, sizeof (fsid), &expectedSize, expectedMd5, sizeof (expectedMd5)) != 0)
        return -1;
    if (expectedSize > (uint64_t)INT64_MAX)
        return baidu_set_error (task, EOVERFLOW, "baidu_file_not_found: file is too large");
    fd = open (task->source.tempPath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return baidu_set_error (task, errno, "baidu_dlink_failed: cannot open the temporary file");
    if (fstat (fd, &status) != 0 || !S_ISREG (status.st_mode)) {
        int savedErrno = errno;
        (void)close (fd);
        return baidu_set_error (task, savedErrno == 0 ? EINVAL : savedErrno,
                                "baidu_dlink_failed: temporary file is invalid");
    }
    download.task = task;
    download.file = fdopen (fd, "r+b");
    if (download.file == NULL) {
        int savedErrno = errno;
        (void)close (fd);
        return baidu_set_error (task, savedErrno, "baidu_dlink_failed: cannot use the temporary file");
    }
    download.expectedSize = expectedSize;
    (void)snprintf (download.md5, sizeof (download.md5), "%s", expectedMd5);
    if (fseeko (download.file, 0, SEEK_END) != 0) {
        result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot seek the temporary file");
        goto done;
    }
    download.offset = (uint64_t)ftello (download.file);
    if (download.offset > expectedSize)
        download.offset = 0;
    if (task->source.etag[0] != '\0' && expectedMd5[0] != '\0' &&
        strcasecmp (task->source.etag, expectedMd5) != 0)
        download.offset = 0;
    if (download.offset == 0) {
        if (ftruncate (fileno (download.file), 0) != 0) {
            result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot reset the temporary file");
            goto done;
        }
        (void)fseeko (download.file, 0, SEEK_SET);
        download.lastCheckpoint = 0;
        if (task->source.checkpoint.version != 0)
            (void)baidu_checkpoint (&download, 0);
    } else {
        (void)fseeko (download.file, (off_t)download.offset, SEEK_SET);
        download.lastCheckpoint = download.offset;
    }
    for (int pass = 0; pass < 2; pass++) {
        char dlink[DOWNLOAD_TASK_URL_SIZE];
        bool downloaded = false;
        if (baidu_resolve_dlink (task, fsid, dlink, sizeof (dlink)) != 0) {
            result = -1;
            goto done;
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            download.written = 0;
            CURLcode code = baidu_download_perform (task, &download, dlink);
            bool     success = code == CURLE_OK && task->status >= 200 && task->status < 300;

            if (!success) {
                if (task->status >= 400 || download.localError) {
                    /* libcurl delivers error pages/JSON to the write callback too;
                     * discard them so a retry resumes from the last proven offset.
                     * Flush first: fseeko would otherwise replay the buffered body
                     * after the truncation and silently keep it. */
                    if (fflush (download.file) != 0 ||
                        ftruncate (fileno (download.file), (off_t)download.attemptOffset) != 0 ||
                        fseeko (download.file, (off_t)download.attemptOffset, SEEK_SET) != 0) {
                        result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot reset the temporary file");
                        goto done;
                    }
                    download.offset = download.attemptOffset;
                    download.written = 0;
                    download.lastCheckpoint = download.attemptOffset;
                } else if (download.offset + download.written > 0) {
                    /* A transport failure in the middle of a valid payload leaves a
                     * usable prefix; record it so the next attempt resumes there. */
                    if (baidu_checkpoint (&download, download.offset + download.written) != 0) {
                        result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot save the checkpoint");
                        goto done;
                    }
                }
                if (download.localError) {
                    result = baidu_set_error (task, EIO, "baidu_dlink_failed: local write failed");
                    goto done;
                }
            } else if (task->status == 200 && download.offset > 0) {
                /* The server ignored Range; restart from zero. */
                if (fflush (download.file) != 0 ||
                    ftruncate (fileno (download.file), 0) != 0 ||
                    fseeko (download.file, 0, SEEK_SET) != 0) {
                    result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot reset the temporary file");
                    goto done;
                }
                download.offset = 0;
                download.written = 0;
                download.lastCheckpoint = 0;
                continue;
            }
            if (task->pauseRequested) {
                (void)baidu_checkpoint (&download, download.offset + download.written);
                result = 1;
                goto done;
            }
            if (task->cancelRequested) {
                result = baidu_set_error (task, ECANCELED, "baidu_dlink_failed: cancelled");
                goto done;
            }
            if (!success) {
                if (code == CURLE_OK && (task->status == 401 || task->status == 403) && attempt == 0) {
                    if (baidu_resolve_dlink (task, fsid, dlink, sizeof (dlink)) != 0) {
                        result = -1;
                        goto done;
                    }
                    continue;
                }
                break;
            }
            downloaded = true;
            break;
        }
        if (!downloaded) {
            if (task->lastResult != CURLE_OK) {
                char message[DOWNLOAD_TASK_ERROR_SIZE];
                (void)snprintf (message, sizeof (message), "%s", task->errorMessage);
                result = baidu_set_error (task, EAGAIN, message);
            } else if (task->status >= 500) {
                result = baidu_set_error (task, EAGAIN, "baidu_network_error: server returned HTTP 5xx");
            } else {
                char message[DOWNLOAD_TASK_ERROR_SIZE];
                (void)snprintf (message, sizeof (message), "baidu_dlink_failed: HTTP %ld", task->status);
                result = baidu_set_error (task, EIO, message);
            }
            goto done;
        }
        {
            struct stat fileStatus;
            uint64_t    actualSize;
            if (fflush (download.file) != 0) {
                result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot flush the temporary file");
                goto done;
            }
            if (fstat (fileno (download.file), &fileStatus) != 0) {
                result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot inspect the temporary file");
                goto done;
            }
            actualSize = (uint64_t)fileStatus.st_size;
            if (baidu_checkpoint (&download, actualSize) != 0) {
                result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot save the checkpoint");
                goto done;
            }
            if (actualSize == expectedSize && baidu_verify_md5 (&download) == 0) {
                if (task->callbacks.onProgress != NULL)
                    task->callbacks.onProgress (task->userData, expectedSize, expectedSize, 0);
                result = 0;
                goto done;
            }
        }
        /* Verification failed: discard and try once more from the beginning. */
        if (fflush (download.file) != 0 ||
            ftruncate (fileno (download.file), 0) != 0 ||
            fseeko (download.file, 0, SEEK_SET) != 0) {
            result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot reset the temporary file");
            goto done;
        }
        download.offset = 0;
        download.written = 0;
        if (baidu_checkpoint (&download, 0) != 0) {
            result = baidu_set_error (task, EIO, "baidu_dlink_failed: cannot reset the checkpoint");
            goto done;
        }
        if (pass == 1)
            result = baidu_set_error (task, EIO, "baidu_checksum_mismatch: size or md5 did not match");
    }
done:
    if (download.file != NULL)
        (void)fclose (download.file);
    return result;
}

static int baidu_slot_acquire (BaiduTask* task)
{
    c_mutex_lock (&gBaiduMutex);
    while (gBaiduActiveSlots >= gBaiduConfig.maxParallelTasks) {
        if (task->cancelRequested) {
            c_mutex_unlock (&gBaiduMutex);
            errno = ECANCELED;
            return -1;
        }
        if (task->pauseRequested) {
            c_mutex_unlock (&gBaiduMutex);
            return 1;
        }
        c_cond_wait (&gBaiduSlotCond, &gBaiduMutex);
    }
    gBaiduActiveSlots++;
    c_mutex_unlock (&gBaiduMutex);
    return 0;
}

static void baidu_slot_release (void)
{
    c_mutex_lock (&gBaiduMutex);
    if (gBaiduActiveSlots > 0)
        gBaiduActiveSlots--;
    c_cond_notify_all (&gBaiduSlotCond);
    c_mutex_unlock (&gBaiduMutex);
}

static int baidu_run (ProtocolTask* context)
{
    BaiduTask* task = (BaiduTask*)context;
    int        result;

    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!baidu_ensure_init ())
        return baidu_set_error (task, ENOMEM, "baidu_auth_missing: global initialization failed");
    result = baidu_slot_acquire (task);
    if (result != 0)
        return result;
    result = baidu_run_impl (task);
    baidu_slot_release ();
    if (result < 0 && task->callbacks.onFailed != NULL)
        task->callbacks.onFailed (task->userData, errno == 0 ? EIO : errno, task->errorMessage);
    return result;
}

static int baidu_open (ProtocolTask** result, const DownloadTask* source, const ProtocolCallbacks* callbacks,
                       void* userData)
{
    BaiduTask* task;
    char       share[BAIDU_SURL_SIZE];

    if (result == NULL || source == NULL || callbacks == NULL) {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    if (!baidu_ensure_init ()) {
        errno = ENOMEM;
        return -1;
    }
    task = (BaiduTask*)c_malloc0 (sizeof (*task));
    if (task == NULL)
        return -1;
    task->source = *source;
    task->callbacks = *callbacks;
    task->userData = userData;
    if (source->requestContext == NULL || !request_context_has_auth (source->requestContext) ||
        request_context_auth_kind (source->requestContext) != DOWNLOADER_CREDENTIAL_COOKIE) {
        if (callbacks->onFailed != NULL)
            callbacks->onFailed (userData, EPERM, "baidu_auth_missing: select a cookie credential profile");
        errno = EPERM;
        c_free (task);
        return -1;
    }
    (void)snprintf (task->cookie, sizeof (task->cookie), "%s", request_context_auth_secret (source->requestContext));
    if (baidu_cookie_login (task->cookie) != 0) {
        if (callbacks->onFailed != NULL)
            callbacks->onFailed (userData, EPERM, "baidu_auth_missing: cookie needs BDUSS and STOKEN");
        errno = EPERM;
        c_free (task);
        return -1;
    }
    if (baidu_own_url (source->resourceUrl, task->apiPath, sizeof (task->apiPath))) {
        *result = (ProtocolTask*)task;
        return 0;
    }
    if (baidu_share_url (source->resourceUrl, share, sizeof (share))) {
        if (callbacks->onFailed != NULL)
            callbacks->onFailed (userData, ENOTSUP, "baidu share support pending");
        errno = ENOTSUP;
        c_free (task);
        return -1;
    }
    if (callbacks->onFailed != NULL)
        callbacks->onFailed (userData, EINVAL, "baidu unsupported URL");
    errno = EINVAL;
    c_free (task);
    return -1;
}

static int baidu_pause (ProtocolTask* context)
{
    BaiduTask* task = (BaiduTask*)context;

    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&gBaiduMutex);
    task->pauseRequested = true;
    c_cond_notify_all (&gBaiduSlotCond);
    c_mutex_unlock (&gBaiduMutex);
    return 0;
}

static int baidu_resume (ProtocolTask* context)
{
    BaiduTask* task = (BaiduTask*)context;

    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&gBaiduMutex);
    task->pauseRequested = false;
    c_mutex_unlock (&gBaiduMutex);
    return 0;
}

static int baidu_cancel (ProtocolTask* context)
{
    BaiduTask* task = (BaiduTask*)context;

    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&gBaiduMutex);
    task->cancelRequested = true;
    c_cond_notify_all (&gBaiduSlotCond);
    c_mutex_unlock (&gBaiduMutex);
    return 0;
}

static void baidu_close (ProtocolTask* context)
{
    c_free (context);
}

static bool baidu_retryable (ProtocolTask* context, int errorCode, const char* message)
{
    BaiduTask* task = (BaiduTask*)context;

    (void)message;
    if (task == NULL)
        return false;
    if (task->status == 408 || task->status == 429 || (task->status >= 500 && task->status < 600))
        return true;
    switch (task->lastResult) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
        return true;
    default:
        break;
    }
    return errorCode == EAGAIN || errorCode == ETIMEDOUT || errorCode == ECONNRESET || errorCode == ECONNREFUSED ||
           errorCode == EHOSTUNREACH || errorCode == ENETUNREACH || errorCode == ENETDOWN || errorCode == EPIPE ||
           errorCode == ECONNABORTED;
}

static const ProtocolOps gcBaiduOps = {
    .name = "baidu",
    .canHandle = baidu_can_handle,
    .open = baidu_open,
    .run = baidu_run,
    .pause = baidu_pause,
    .resume = baidu_resume,
    .cancel = baidu_cancel,
    .close = baidu_close,
    .retryable = baidu_retryable,
    .expand = baidu_expand
};

const ProtocolOps* baidu_protocol_ops (void)
{
    return &gcBaiduOps;
}
