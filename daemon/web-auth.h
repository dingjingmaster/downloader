#ifndef DOWNLOADER_WEB_AUTH_H
#define DOWNLOADER_WEB_AUTH_H

#include <clib/macro.h>

#include <stdbool.h>
#include <stddef.h>

#define DOWNLOADER_WEB_SESSION_SIZE 65U
#define DOWNLOADER_WEB_CSRF_SIZE    65U

C_BEGIN_EXTERN_C

typedef struct WebAuth WebAuth;

/* Empty username/password disables authentication (local-only legacy mode). */
WebAuth* web_auth_new (const char* username, const char* password, int sessionSeconds);
void     web_auth_free (WebAuth* auth);
bool     web_auth_enabled (const WebAuth* auth);
/* On success returns 0 and writes the session/csrf tokens (hex, NUL-terminated).
 * Wrong credentials and expired sessions return -1 with EACCES. */
int      web_auth_login (WebAuth* auth, const char* username, const char* password, char* session,
                         size_t sessionCapacity, char* csrf, size_t csrfCapacity);
int      web_auth_session (WebAuth* auth, const char* session, char* csrf, size_t csrfCapacity);
int      web_auth_logout (WebAuth* auth, const char* session);
/* Extracts "downloader_session" from a Cookie header. */
int      web_auth_cookie (const char* cookieHeader, char* session, size_t capacity);
bool     web_auth_csrf_matches (const WebAuth* auth, const char* expected, const char* provided);

C_END_EXTERN_C

#endif
