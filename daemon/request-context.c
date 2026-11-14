#define _POSIX_C_SOURCE 200809L

#include "request-context.h"

#include <clib/json.h>
#include <clib/memory.h>
#include <clib/uri.h>

#include <errno.h>
#include <openssl/crypto.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

struct RequestContext
{
    bool                     auth;
    DownloaderCredentialKind authKind;
    char                     authUsername[DOWNLOADER_CREDENTIAL_USER_SIZE];
    char                     authSecret[DOWNLOADER_CREDENTIAL_SECRET_SIZE];
    bool                     proxy;
    char                     proxyUrl[DOWNLOADER_CREDENTIAL_URL_SIZE];
    char                     proxyUsername[DOWNLOADER_CREDENTIAL_USER_SIZE];
    char                     proxySecret[DOWNLOADER_CREDENTIAL_SECRET_SIZE];
};

static bool request_text_safe (const char* value, size_t length, size_t capacity)
{
    return length < capacity && memchr (value, '\0', length) == NULL && memchr (value, '\r', length) == NULL &&
           memchr (value, '\n', length) == NULL;
}

static int request_extra_profiles (const DownloadTask* task, char* authProfile, size_t authCapacity,
                                   char* proxyProfile, size_t proxyCapacity)
{
    static const char* const keys[] = { "auth_profile", "proxy_profile" };
    char*                     destinations[] = { authProfile, proxyProfile };
    size_t                    capacities[] = { authCapacity, proxyCapacity };
    CJsonDocument*            document;
    CJsonValue*               root;
    int                       result = -1;

    authProfile[0] = '\0';
    proxyProfile[0] = '\0';
    if (task->extraJson[0] == '\0')
        return 0;
    document = c_json_document_new_from_data (c_string_view_from_cstr (task->extraJson), C_JSON_PARSE_NONE, NULL);
    if (document == NULL) {
        errno = EINVAL;
        return -1;
    }
    root = c_json_document_root (document);
    if (c_json_value_type (root) != C_JSON_TYPE_OBJECT) {
        errno = EINVAL;
        goto done;
    }
    for (size_t index = 0; index < 2; index++) {
        CJsonValue*  value = c_json_object_get (root, c_string_view_from_cstr (keys[index]));
        CStringView  text;
        if (value == NULL)
            continue;
        text = c_json_value_get_string (value);
        if (c_json_value_type (value) != C_JSON_TYPE_STRING ||
            !request_text_safe (text.data, text.size, capacities[index])) {
            errno = EINVAL;
            goto done;
        }
        memcpy (destinations[index], text.data, text.size);
        destinations[index][text.size] = '\0';
    }
    result = 0;
done:
    c_json_document_free (document);
    return result;
}

static int request_host (const char* url, char* host, size_t capacity)
{
    char*  scheme = NULL;
    char*  userInfo = NULL;
    char*  hostValue = NULL;
    char*  path = NULL;
    char*  query = NULL;
    char*  fragment = NULL;
    int    port = 0;
    size_t errorOffset = 0;
    int    result = -1;

    host[0] = '\0';
    if (c_uri_split (c_string_view_from_cstr (url), C_URI_FLAGS_NONE, &scheme, &userInfo, &hostValue, &port, &path,
                     &query, &fragment, &errorOffset) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (scheme != NULL && strcasecmp (scheme, "baidu") == 0) {
        if (strlen ("pan.baidu.com") >= capacity) {
            errno = ENAMETOOLONG;
            goto done;
        }
        snprintf (host, capacity, "%s", "pan.baidu.com");
        result = 0;
        goto done;
    }
    if (hostValue != NULL) {
        if (strlen (hostValue) >= capacity) {
            errno = ENAMETOOLONG;
            goto done;
        }
        snprintf (host, capacity, "%s", hostValue);
    }
    result = 0;
done:
    c_free (scheme);
    c_free (userInfo);
    c_free (hostValue);
    c_free (path);
    c_free (query);
    c_free (fragment);
    return result;
}

static bool request_proxy_url_supported (const char* url)
{
    static const char* const schemes[] = { "http", "https", "socks4", "socks4a", "socks5", "socks5h" };
    CUri   uri;
    size_t errorOffset;

    if (credential_url_has_userinfo (url))
        return false;
    if (c_uri_parse (c_string_view_from_cstr (url), &uri, &errorOffset) != 0 || uri.scheme.size == 0)
        return false;
    for (size_t index = 0; index < sizeof (schemes) / sizeof (schemes[0]); index++) {
        if (uri.scheme.size == strlen (schemes[index]) &&
            strncasecmp (uri.scheme.data, schemes[index], uri.scheme.size) == 0)
            return true;
    }
    return false;
}

static int request_auth_valid (DownloaderCredentialKind kind, const char* username, const char* secret)
{
    if (kind == DOWNLOADER_CREDENTIAL_BEARER || kind == DOWNLOADER_CREDENTIAL_COOKIE)
        return secret[0] != '\0' ? 0 : -1;
    return username[0] != '\0' ? 0 : -1;
}

void request_context_free (RequestContext* context)
{
    if (context == NULL)
        return;
    OPENSSL_cleanse (context, sizeof (*context));
    c_free (context);
}

int request_context_resolve (RequestContext** result, const DownloadTask* task, CredentialStore* store,
                             const char* defaultAuthProfile, const char* defaultProxyProfile,
                             const char* globalProxyUrl, const char* globalProxyUsername,
                             const char* globalProxySecret)
{
    RequestContext* context;
    char            authProfile[DOWNLOADER_CREDENTIAL_NAME_SIZE];
    char            proxyProfile[DOWNLOADER_CREDENTIAL_NAME_SIZE];
    char            host[DOWNLOADER_CREDENTIAL_DOMAIN_SIZE];

    if (result == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    if (request_extra_profiles (task, authProfile, sizeof (authProfile), proxyProfile, sizeof (proxyProfile)) != 0)
        return -1;
    if ((authProfile[0] == '\0' || proxyProfile[0] == '\0') && store != NULL) {
        char siteAuth[DOWNLOADER_CREDENTIAL_NAME_SIZE];
        char siteProxy[DOWNLOADER_CREDENTIAL_NAME_SIZE];
        if (request_host (task->resourceUrl, host, sizeof (host)) != 0)
            return -1;
        if (host[0] != '\0') {
            if (credential_store_resolve_site (store, host, siteAuth, sizeof (siteAuth), siteProxy,
                                               sizeof (siteProxy)) != 0)
                return -1;
            if (authProfile[0] == '\0')
                snprintf (authProfile, sizeof (authProfile), "%s", siteAuth);
            if (proxyProfile[0] == '\0')
                snprintf (proxyProfile, sizeof (proxyProfile), "%s", siteProxy);
        }
    }
    if (authProfile[0] == '\0' && defaultAuthProfile != NULL)
        snprintf (authProfile, sizeof (authProfile), "%s", defaultAuthProfile);
    if (proxyProfile[0] == '\0' && defaultProxyProfile != NULL)
        snprintf (proxyProfile, sizeof (proxyProfile), "%s", defaultProxyProfile);

    context = (RequestContext*)c_malloc0 (sizeof (*context));
    if (context == NULL)
        return -1;
    if (authProfile[0] != '\0') {
        CredentialProfile profile;
        if (store == NULL) {
            errno = ENOENT;
            goto failed;
        }
        if (credential_store_load_credential (store, authProfile, &profile) != 0)
            goto failed;
        if (request_auth_valid (profile.kind, profile.username, profile.secret) != 0) {
            credential_profile_clear (&profile);
            errno = EINVAL;
            goto failed;
        }
        context->auth = true;
        context->authKind = profile.kind;
        snprintf (context->authUsername, sizeof (context->authUsername), "%s", profile.username);
        snprintf (context->authSecret, sizeof (context->authSecret), "%s", profile.secret);
        credential_profile_clear (&profile);
    }
    if (proxyProfile[0] != '\0') {
        ProxyProfile profile;
        if (store == NULL) {
            errno = ENOENT;
            goto failed;
        }
        if (credential_store_load_proxy (store, proxyProfile, &profile) != 0)
            goto failed;
        if (profile.url[0] == '\0' || !request_proxy_url_supported (profile.url)) {
            proxy_profile_clear (&profile);
            errno = EINVAL;
            goto failed;
        }
        context->proxy = true;
        snprintf (context->proxyUrl, sizeof (context->proxyUrl), "%s", profile.url);
        snprintf (context->proxyUsername, sizeof (context->proxyUsername), "%s", profile.username);
        snprintf (context->proxySecret, sizeof (context->proxySecret), "%s", profile.secret);
        proxy_profile_clear (&profile);
    } else if (globalProxyUrl != NULL && globalProxyUrl[0] != '\0') {
        if (!request_proxy_url_supported (globalProxyUrl)) {
            errno = EINVAL;
            goto failed;
        }
        context->proxy = true;
        snprintf (context->proxyUrl, sizeof (context->proxyUrl), "%s", globalProxyUrl);
        snprintf (context->proxyUsername, sizeof (context->proxyUsername), "%s",
                  globalProxyUsername == NULL ? "" : globalProxyUsername);
        snprintf (context->proxySecret, sizeof (context->proxySecret), "%s",
                  globalProxySecret == NULL ? "" : globalProxySecret);
    }
    *result = context;
    return 0;

failed:
    {
        int savedErrno = errno;
        request_context_free (context);
        errno = savedErrno;
        return -1;
    }
}

bool request_context_has_auth (const RequestContext* context)
{
    return context != NULL && context->auth;
}

DownloaderCredentialKind request_context_auth_kind (const RequestContext* context)
{
    return context == NULL ? DOWNLOADER_CREDENTIAL_BASIC : context->authKind;
}

const char* request_context_auth_username (const RequestContext* context)
{
    return context == NULL ? "" : context->authUsername;
}

const char* request_context_auth_secret (const RequestContext* context)
{
    return context == NULL ? "" : context->authSecret;
}

bool request_context_has_proxy (const RequestContext* context)
{
    return context != NULL && context->proxy;
}

const char* request_context_proxy_url (const RequestContext* context)
{
    return context == NULL ? "" : context->proxyUrl;
}

const char* request_context_proxy_username (const RequestContext* context)
{
    return context == NULL ? "" : context->proxyUsername;
}

const char* request_context_proxy_secret (const RequestContext* context)
{
    return context == NULL ? "" : context->proxySecret;
}
