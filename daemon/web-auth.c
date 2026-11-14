#define _POSIX_C_SOURCE 200809L

#include "web-auth.h"

#include <clib/memory.h>
#include <clib/mutex.h>

#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WEB_AUTH_SESSION_CAPACITY 64
#define WEB_AUTH_TOKEN_BYTES      32U
#define WEB_AUTH_DIGEST_SIZE      32U
#define WEB_AUTH_DEFAULT_SECONDS  28800

typedef struct
{
    char   session[DOWNLOADER_WEB_SESSION_SIZE];
    char   csrf[DOWNLOADER_WEB_CSRF_SIZE];
    int64_t expiresAt;
} WebAuthSession;

struct WebAuth
{
    CMutex          mutex;
    bool            enabled;
    unsigned char   usernameHash[WEB_AUTH_DIGEST_SIZE];
    unsigned char   passwordHash[WEB_AUTH_DIGEST_SIZE];
    int64_t         sessionSeconds;
    WebAuthSession  sessions[WEB_AUTH_SESSION_CAPACITY];
};

static int64_t web_auth_now (void)
{
    struct timespec now;

    if (clock_gettime (CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (int64_t)now.tv_sec;
}

static int web_auth_digest (const char* value, unsigned char digest[WEB_AUTH_DIGEST_SIZE])
{
    unsigned int length = 0;

    if (EVP_Digest (value, strlen (value), digest, &length, EVP_sha256 (), NULL) != 1 ||
        length != WEB_AUTH_DIGEST_SIZE)
        return -1;
    return 0;
}

static int web_auth_token (char* destination, size_t capacity)
{
    unsigned char bytes[WEB_AUTH_TOKEN_BYTES];
    size_t        index;

    if (capacity < WEB_AUTH_TOKEN_BYTES * 2U + 1U)
        return -1;
    if (RAND_bytes (bytes, (int)sizeof (bytes)) != 1)
        return -1;
    for (index = 0; index < sizeof (bytes); index++)
        snprintf (destination + index * 2U, 3U, "%02x", bytes[index]);
    destination[WEB_AUTH_TOKEN_BYTES * 2U] = '\0';
    return 0;
}

WebAuth* web_auth_new (const char* username, const char* password, int sessionSeconds)
{
    WebAuth* auth = (WebAuth*)c_malloc0 (sizeof (*auth));

    if (auth == NULL)
        return NULL;
    if (!c_mutex_init (&auth->mutex)) {
        c_free (auth);
        errno = ENOMEM;
        return NULL;
    }
    auth->sessionSeconds = sessionSeconds > 0 ? sessionSeconds : WEB_AUTH_DEFAULT_SECONDS;
    {
        bool hasUsername = username != NULL && username[0] != '\0';
        bool hasPassword = password != NULL && password[0] != '\0';
        if (hasUsername != hasPassword) {
            c_mutex_clear (&auth->mutex);
            c_free (auth);
            errno = EINVAL;
            return NULL;
        }
    }
    if (username != NULL && username[0] != '\0' && password != NULL && password[0] != '\0') {
        if (web_auth_digest (username, auth->usernameHash) != 0 ||
            web_auth_digest (password, auth->passwordHash) != 0) {
            c_mutex_clear (&auth->mutex);
            c_free (auth);
            errno = EIO;
            return NULL;
        }
        auth->enabled = true;
    }
    return auth;
}

void web_auth_free (WebAuth* auth)
{
    if (auth == NULL)
        return;
    c_mutex_clear (&auth->mutex);
    OPENSSL_cleanse (auth, sizeof (*auth));
    c_free (auth);
}

bool web_auth_enabled (const WebAuth* auth)
{
    return auth != NULL && auth->enabled;
}

static WebAuthSession* web_auth_slot (WebAuth* auth, int64_t now)
{
    WebAuthSession* oldest = &auth->sessions[0];

    for (size_t index = 0; index < WEB_AUTH_SESSION_CAPACITY; index++) {
        if (auth->sessions[index].session[0] == '\0' || auth->sessions[index].expiresAt <= now)
            return &auth->sessions[index];
        if (auth->sessions[index].expiresAt < oldest->expiresAt)
            oldest = &auth->sessions[index];
    }
    return oldest;
}

int web_auth_login (WebAuth* auth, const char* username, const char* password, char* session,
                    size_t sessionCapacity, char* csrf, size_t csrfCapacity)
{
    unsigned char   usernameHash[WEB_AUTH_DIGEST_SIZE];
    unsigned char   passwordHash[WEB_AUTH_DIGEST_SIZE];
    WebAuthSession* slot;
    int64_t         now;

    if (auth == NULL || session == NULL || csrf == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!auth->enabled || username == NULL || password == NULL) {
        errno = EACCES;
        return -1;
    }
    if (web_auth_digest (username, usernameHash) != 0 || web_auth_digest (password, passwordHash) != 0 ||
        CRYPTO_memcmp (usernameHash, auth->usernameHash, WEB_AUTH_DIGEST_SIZE) != 0 ||
        CRYPTO_memcmp (passwordHash, auth->passwordHash, WEB_AUTH_DIGEST_SIZE) != 0) {
        OPENSSL_cleanse (usernameHash, sizeof (usernameHash));
        OPENSSL_cleanse (passwordHash, sizeof (passwordHash));
        errno = EACCES;
        return -1;
    }
    OPENSSL_cleanse (usernameHash, sizeof (usernameHash));
    OPENSSL_cleanse (passwordHash, sizeof (passwordHash));
    now = web_auth_now ();
    c_mutex_lock (&auth->mutex);
    slot = web_auth_slot (auth, now);
    if (web_auth_token (slot->session, sizeof (slot->session)) != 0 ||
        web_auth_token (slot->csrf, sizeof (slot->csrf)) != 0) {
        c_mutex_unlock (&auth->mutex);
        errno = EIO;
        return -1;
    }
    slot->expiresAt = now + auth->sessionSeconds;
    snprintf (session, sessionCapacity, "%s", slot->session);
    snprintf (csrf, csrfCapacity, "%s", slot->csrf);
    c_mutex_unlock (&auth->mutex);
    return 0;
}

static WebAuthSession* web_auth_find (WebAuth* auth, const char* session, int64_t now)
{
    size_t length;

    if (session == NULL || session[0] == '\0')
        return NULL;
    length = strlen (session);
    for (size_t index = 0; index < WEB_AUTH_SESSION_CAPACITY; index++) {
        WebAuthSession* slot = &auth->sessions[index];
        if (slot->session[0] != '\0' && strlen (slot->session) == length &&
            CRYPTO_memcmp (slot->session, session, length) == 0) {
            if (slot->expiresAt <= now) {
                OPENSSL_cleanse (slot, sizeof (*slot));
                return NULL;
            }
            return slot;
        }
    }
    return NULL;
}

int web_auth_session (WebAuth* auth, const char* session, char* csrf, size_t csrfCapacity)
{
    WebAuthSession* slot;

    if (auth == NULL || csrf == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!auth->enabled) {
        errno = EACCES;
        return -1;
    }
    c_mutex_lock (&auth->mutex);
    slot = web_auth_find (auth, session, web_auth_now ());
    if (slot == NULL) {
        c_mutex_unlock (&auth->mutex);
        errno = EACCES;
        return -1;
    }
    snprintf (csrf, csrfCapacity, "%s", slot->csrf);
    c_mutex_unlock (&auth->mutex);
    return 0;
}

int web_auth_logout (WebAuth* auth, const char* session)
{
    WebAuthSession* slot;

    if (auth == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&auth->mutex);
    slot = web_auth_find (auth, session, web_auth_now ());
    if (slot != NULL)
        OPENSSL_cleanse (slot, sizeof (*slot));
    c_mutex_unlock (&auth->mutex);
    return slot == NULL ? -1 : 0;
}

int web_auth_cookie (const char* cookieHeader, char* session, size_t capacity)
{
    static const char name[] = "downloader_session=";
    const char*       token;

    if (cookieHeader == NULL || session == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    session[0] = '\0';
    token = cookieHeader;
    while (token != NULL && *token != '\0') {
        const char* end = strchr (token, ';');
        size_t      length = end == NULL ? strlen (token) : (size_t)(end - token);
        const char* value = token;
        while (length > 0 && (*value == ' ' || *value == '\t')) {
            value++;
            length--;
        }
        if (length > sizeof (name) - 1U && strncmp (value, name, sizeof (name) - 1U) == 0) {
            size_t valueLength = length - (sizeof (name) - 1U);
            if (valueLength >= capacity) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy (session, value + sizeof (name) - 1U, valueLength);
            session[valueLength] = '\0';
            return 0;
        }
        token = end == NULL ? NULL : end + 1;
    }
    errno = ENOENT;
    return -1;
}

bool web_auth_csrf_matches (const WebAuth* auth, const char* expected, const char* provided)
{
    size_t length;

    if (auth == NULL || expected == NULL || provided == NULL)
        return false;
    length = strlen (expected);
    if (length == 0 || strlen (provided) != length)
        return false;
    return CRYPTO_memcmp (expected, provided, length) == 0;
}
