#define _POSIX_C_SOURCE 200809L

#include "baidu-parse.h"

#include <clib/json.h>
#include <clib/memory.h>
#include <clib/string-view.h>
#include <clib/uri.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool baidu_scheme_is (const char* scheme, const char* expected)
{
    return scheme != NULL && strcasecmp (scheme, expected) == 0;
}

static bool baidu_host_is_share (const char* host)
{
    return host != NULL &&
           (strcasecmp (host, "pan.baidu.com") == 0 || strcasecmp (host, "yun.baidu.com") == 0 ||
            strcasecmp (host, "eyun.baidu.com") == 0);
}

static bool baidu_valid_surl (const char* value)
{
    size_t length;

    if (value == NULL || value[0] == '\0')
        return false;
    length = strlen (value);
    if (length >= BAIDU_SURL_SIZE)
        return false;
    for (size_t index = 0; index < length; index++) {
        char letter = value[index];
        if (!((letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'z') ||
              (letter >= 'A' && letter <= 'Z') || letter == '_' || letter == '-'))
            return false;
    }
    return true;
}

static int baidu_query_value (const char* query, const char* key, char* destination, size_t capacity)
{
    const char* token = query;
    size_t      keyLength = strlen (key);

    if (key == NULL || destination == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    if (query == NULL) {
        errno = ENOENT;
        return -1;
    }
    while (token != NULL && *token != '\0') {
        const char* end = strchr (token, '&');
        size_t      length = end == NULL ? strlen (token) : (size_t)(end - token);

        if (length > keyLength + 1U && strncmp (token, key, keyLength) == 0 && token[keyLength] == '=') {
            CStringView value = { .data = token + keyLength + 1U, .size = length - keyLength - 1U };
            char*       decoded = NULL;
            size_t      decodedSize = 0;
            if (c_uri_unescape_string (value, &decoded, &decodedSize) != 0)
                return -1;
            if (decodedSize >= capacity) {
                c_free (decoded);
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy (destination, decoded, decodedSize);
            destination[decodedSize] = '\0';
            c_free (decoded);
            return 0;
        }
        token = end == NULL ? NULL : end + 1;
    }
    errno = ENOENT;
    return -1;
}

static void baidu_split_free (char** scheme, char** userInfo, char** host, char** path, char** query,
                              char** fragment)
{
    c_free (*scheme);
    c_free (*userInfo);
    c_free (*host);
    c_free (*path);
    c_free (*query);
    c_free (*fragment);
    *scheme = *userInfo = *host = *path = *query = *fragment = NULL;
}

bool baidu_share_url (const char* url, char* surl, size_t capacity)
{
    char*  scheme = NULL;
    char*  userInfo = NULL;
    char*  host = NULL;
    char*  path = NULL;
    char*  query = NULL;
    char*  fragment = NULL;
    int    port = 0;
    size_t errorOffset = 0;
    bool   result = false;

    if (url == NULL || surl == NULL || capacity == 0) {
        errno = EINVAL;
        return false;
    }
    if (c_uri_split (c_string_view_from_cstr (url), C_URI_FLAGS_NONE, &scheme, &userInfo, &host, &port, &path,
                     &query, &fragment, &errorOffset) != 0) {
        errno = EINVAL;
        return false;
    }
    if ((baidu_scheme_is (scheme, "https") || baidu_scheme_is (scheme, "http")) && baidu_host_is_share (host) &&
        path != NULL) {
        if (strncmp (path, "/s/", 3U) == 0 && baidu_valid_surl (path + 3) &&
            strlen (path + 3) < capacity) {
            (void)snprintf (surl, capacity, "%s", path + 3);
            result = true;
        } else if (strcmp (path, "/share/init") == 0 && query != NULL &&
                   baidu_query_value (query, "surl", surl, capacity) == 0 && baidu_valid_surl (surl)) {
            result = true;
        }
    }
    baidu_split_free (&scheme, &userInfo, &host, &path, &query, &fragment);
    return result;
}

int baidu_share_url_pwd (const char* url, char* pwd, size_t capacity)
{
    char*  scheme = NULL;
    char*  userInfo = NULL;
    char*  host = NULL;
    char*  path = NULL;
    char*  query = NULL;
    char*  fragment = NULL;
    int    port = 0;
    size_t errorOffset = 0;
    int    result = -1;

    if (url == NULL || pwd == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    if (c_uri_split (c_string_view_from_cstr (url), C_URI_FLAGS_NONE, &scheme, &userInfo, &host, &port, &path,
                     &query, &fragment, &errorOffset) == 0) {
        result = baidu_query_value (query, "pwd", pwd, capacity);
    }
    baidu_split_free (&scheme, &userInfo, &host, &path, &query, &fragment);
    return result;
}

static bool baidu_has_parent_reference (const char* path)
{
    const char* part = path;

    while (part != NULL && *part != '\0') {
        const char* separator = strchr (part, '/');
        size_t      length = separator == NULL ? strlen (part) : (size_t)(separator - part);
        if (length == 2U && strncmp (part, "..", 2U) == 0)
            return true;
        part = separator == NULL ? NULL : separator + 1;
    }
    return false;
}

bool baidu_own_url (const char* url, char* path, size_t capacity)
{
    char*  scheme = NULL;
    char*  userInfo = NULL;
    char*  host = NULL;
    char*  uriPath = NULL;
    char*  query = NULL;
    char*  fragment = NULL;
    int    port = 0;
    size_t errorOffset = 0;
    char*  decoded = NULL;
    size_t decodedSize = 0;
    bool   result = false;

    if (url == NULL || path == NULL || capacity == 0) {
        errno = EINVAL;
        return false;
    }
    if (c_uri_split (c_string_view_from_cstr (url), C_URI_FLAGS_NONE, &scheme, &userInfo, &host, &port, &uriPath,
                     &query, &fragment, &errorOffset) != 0) {
        errno = EINVAL;
        return false;
    }
    if (!baidu_scheme_is (scheme, "baidu") || uriPath == NULL || uriPath[0] != '/')
        goto done;
    if (c_uri_unescape_string (c_string_view_from_cstr (uriPath), &decoded, &decodedSize) != 0)
        goto done;
    if (decodedSize < 2U || decodedSize >= capacity || memchr (decoded, '\0', decodedSize) != NULL ||
        strchr (decoded, '\\') != NULL || baidu_has_parent_reference (decoded))
        goto done;
    memcpy (path, decoded, decodedSize);
    path[decodedSize] = '\0';
    result = true;
done:
    c_free (decoded);
    baidu_split_free (&scheme, &userInfo, &host, &uriPath, &query, &fragment);
    return result;
}

static int baidu_copy_json_string (const CJsonValue* object, const char* key, char* destination, size_t capacity)
{
    CJsonValue* value = c_json_object_get (object, c_string_view_from_cstr (key));
    CStringView text;

    if (value == NULL)
        return 0;
    if (c_json_value_type (value) != C_JSON_TYPE_STRING) {
        errno = EINVAL;
        return -1;
    }
    text = c_json_value_get_string (value);
    if (text.size >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy (destination, text.data, text.size);
    destination[text.size] = '\0';
    return 0;
}

int baidu_task_options (const char* extraJson, BaiduTaskOptions* options)
{
    CJsonDocument* document;
    CJsonValue*    root;
    CJsonValue*    baidu;
    size_t         errorOffset = 0;
    int            result = -1;

    if (options == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset (options, 0, sizeof (*options));
    if (extraJson == NULL || extraJson[0] == '\0')
        return 0;
    document = c_json_document_new_from_data (c_string_view_from_cstr (extraJson), C_JSON_PARSE_NONE, &errorOffset);
    if (document == NULL) {
        errno = EINVAL;
        return -1;
    }
    root = c_json_document_root (document);
    if (c_json_value_type (root) != C_JSON_TYPE_OBJECT) {
        errno = EINVAL;
        goto done;
    }
    baidu = c_json_object_get (root, c_string_view_from_cstr ("baidu"));
    if (baidu != NULL) {
        if (c_json_value_type (baidu) != C_JSON_TYPE_OBJECT) {
            errno = EINVAL;
            goto done;
        }
        if (baidu_copy_json_string (baidu, "pwd", options->pwd, sizeof (options->pwd)) != 0 ||
            baidu_copy_json_string (baidu, "path", options->path, sizeof (options->path)) != 0)
            goto done;
        if (options->path[0] != '\0' && options->path[0] != '/') {
            errno = EINVAL;
            goto done;
        }
    }
    result = 0;
done:
    c_json_document_free (document);
    return result;
}

int baidu_cookie_login (const char* cookie)
{
    const char* token;
    bool        hasBduss = false;
    bool        hasStoken = false;

    if (cookie == NULL) {
        errno = EINVAL;
        return -1;
    }
    token = cookie;
    while (token != NULL && *token != '\0') {
        const char* end;
        size_t      length;

        while (*token == ' ' || *token == '\t' || *token == ';')
            token++;
        end = strchr (token, ';');
        length = end == NULL ? strlen (token) : (size_t)(end - token);
        while (length > 0 && (token[length - 1] == ' ' || token[length - 1] == '\t'))
            length--;
        if (length > 6U && strncmp (token, "BDUSS=", 6U) == 0 && length > 6U)
            hasBduss = true;
        if (length > 7U && strncmp (token, "STOKEN=", 7U) == 0)
            hasStoken = true;
        token = end == NULL ? NULL : end + 1;
    }
    if (!hasBduss || !hasStoken) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static int baidu_extract_json_scalar (const char* html, const char* key, char* destination, size_t capacity)
{
    char        needle[128];
    const char* start;
    size_t      length = 0;

    if (snprintf (needle, sizeof (needle), "\"%s\":", key) >= (int)sizeof (needle))
        return -1;
    start = strstr (html, needle);
    if (start == NULL)
        return -1;
    start += strlen (needle);
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (*start == '"') {
        start++;
        while (start[length] != '\0' && start[length] != '"')
            length++;
        if (start[length] != '"')
            return -1;
    } else {
        while (start[length] != '\0' && start[length] != ',' && start[length] != '}' &&
               start[length] != ' ' && start[length] != '\t' && start[length] != '\r' && start[length] != '\n')
            length++;
    }
    if (length == 0 || length >= capacity)
        return -1;
    memcpy (destination, start, length);
    destination[length] = '\0';
    return 0;
}

int baidu_share_page_tokens (const char* html, BaiduShareTokens* tokens)
{
    if (html == NULL || tokens == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset (tokens, 0, sizeof (*tokens));
    if (strstr (html, "error-404") != NULL) {
        errno = ENOENT;
        return -1;
    }
    if (strstr (html, "platform-non-found") != NULL) {
        errno = ESTALE;
        return -1;
    }
    if (baidu_extract_json_scalar (html, "bdstoken", tokens->bdstoken, sizeof (tokens->bdstoken)) != 0 ||
        baidu_extract_json_scalar (html, "uk", tokens->uk, sizeof (tokens->uk)) != 0 ||
        baidu_extract_json_scalar (html, "share_uk", tokens->shareUk, sizeof (tokens->shareUk)) != 0 ||
        baidu_extract_json_scalar (html, "shareid", tokens->shareid, sizeof (tokens->shareid)) != 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}
