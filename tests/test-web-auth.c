#define _POSIX_C_SOURCE 200809L

#include "web-auth.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void test_login_and_session (void)
{
    WebAuth* auth = web_auth_new ("admin", "correct horse", 3600);
    char     session[DOWNLOADER_WEB_SESSION_SIZE];
    char     csrf[DOWNLOADER_WEB_CSRF_SIZE];
    char     checked[DOWNLOADER_WEB_CSRF_SIZE];
    char     otherSession[DOWNLOADER_WEB_SESSION_SIZE];
    char     otherCsrf[DOWNLOADER_WEB_CSRF_SIZE];

    assert (auth != NULL);
    assert (web_auth_enabled (auth));
    errno = 0;
    assert (web_auth_login (auth, "admin", "wrong", session, sizeof (session), csrf, sizeof (csrf)) != 0);
    assert (errno == EACCES);
    errno = 0;
    assert (web_auth_login (auth, "other", "correct horse", session, sizeof (session), csrf, sizeof (csrf)) != 0);
    assert (errno == EACCES);
    assert (web_auth_login (auth, "admin", "correct horse", session, sizeof (session), csrf, sizeof (csrf)) == 0);
    assert (strlen (session) == DOWNLOADER_WEB_SESSION_SIZE - 1U);
    assert (strlen (csrf) == DOWNLOADER_WEB_CSRF_SIZE - 1U);
    assert (web_auth_session (auth, session, checked, sizeof (checked)) == 0);
    assert (strcmp (checked, csrf) == 0);
    assert (web_auth_csrf_matches (auth, csrf, csrf));
    assert (!web_auth_csrf_matches (auth, csrf, "wrong"));
    assert (!web_auth_csrf_matches (auth, csrf, ""));
    errno = 0;
    assert (web_auth_session (auth, "not-a-session", checked, sizeof (checked)) != 0);
    assert (errno == EACCES);
    assert (web_auth_login (auth, "admin", "correct horse", otherSession, sizeof (otherSession), otherCsrf,
                            sizeof (otherCsrf)) == 0);
    assert (strcmp (session, otherSession) != 0);
    assert (strcmp (csrf, otherCsrf) != 0);
    assert (web_auth_logout (auth, session) == 0);
    errno = 0;
    assert (web_auth_session (auth, session, checked, sizeof (checked)) != 0);
    assert (errno == EACCES);
    assert (web_auth_session (auth, otherSession, checked, sizeof (checked)) == 0);
    assert (web_auth_cookie ("theme=dark; downloader_session=cookie-token; other=1", checked,
                            sizeof (checked)) == 0);
    assert (strcmp (checked, "cookie-token") == 0);
    assert (web_auth_cookie ("theme=dark", checked, sizeof (checked)) != 0);
    web_auth_free (auth);
}

static void test_expiry (void)
{
    WebAuth* auth = web_auth_new ("admin", "secret", 1);
    char     session[DOWNLOADER_WEB_SESSION_SIZE];
    char     csrf[DOWNLOADER_WEB_CSRF_SIZE];
    char     checked[DOWNLOADER_WEB_CSRF_SIZE];
    struct timespec delay = { .tv_sec = 1, .tv_nsec = 200000000L };

    assert (auth != NULL);
    assert (web_auth_login (auth, "admin", "secret", session, sizeof (session), csrf, sizeof (csrf)) == 0);
    nanosleep (&delay, NULL);
    errno = 0;
    assert (web_auth_session (auth, session, checked, sizeof (checked)) != 0);
    assert (errno == EACCES);
    web_auth_free (auth);
}

static void test_disabled (void)
{
    WebAuth* auth = web_auth_new ("", "", 3600);

    assert (auth != NULL && !web_auth_enabled (auth));
    web_auth_free (auth);
    auth = web_auth_new (NULL, NULL, 3600);
    assert (auth != NULL && !web_auth_enabled (auth));
    web_auth_free (auth);
}

static void test_partial_configuration_is_rejected (void)
{
    errno = 0;
    assert (web_auth_new ("admin", "", 3600) == NULL);
    assert (errno == EINVAL);
    errno = 0;
    assert (web_auth_new ("", "secret", 3600) == NULL);
    assert (errno == EINVAL);
}

int main (void)
{
    test_login_and_session ();
    test_partial_configuration_is_rejected ();
    test_expiry ();
    test_disabled ();
    return 0;
}
