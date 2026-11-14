#define _GNU_SOURCE

#include "credential-store.h"
#include "database.h"
#include "request-context.h"
#include "task.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DownloadTask make_task (const char* url, const char* extra)
{
    DownloadTask task;

    memset (&task, 0, sizeof (task));
    snprintf (task.resourceUrl, sizeof (task.resourceUrl), "%s", url);
    snprintf (task.extraJson, sizeof (task.extraJson), "%s", extra == NULL ? "{}" : extra);
    return task;
}

static CredentialProfile credential (const char* name, DownloaderCredentialKind kind, const char* user,
                                     const char* secret)
{
    CredentialProfile profile;

    memset (&profile, 0, sizeof (profile));
    snprintf (profile.name, sizeof (profile.name), "%s", name);
    profile.kind = kind;
    snprintf (profile.username, sizeof (profile.username), "%s", user);
    snprintf (profile.secret, sizeof (profile.secret), "%s", secret);
    return profile;
}

static ProxyProfile proxy (const char* name, const char* url, const char* user, const char* secret)
{
    ProxyProfile profile;

    memset (&profile, 0, sizeof (profile));
    snprintf (profile.name, sizeof (profile.name), "%s", name);
    snprintf (profile.url, sizeof (profile.url), "%s", url);
    snprintf (profile.username, sizeof (profile.username), "%s", user);
    snprintf (profile.secret, sizeof (profile.secret), "%s", secret);
    return profile;
}

static void test_priority (CredentialStore* store)
{
    RequestContext*  context = NULL;
    DownloadTask     task;

    task = make_task ("https://cdn.example.test/file.bin", "{\"auth_profile\":\"task-basic\"}");
    assert (request_context_resolve (&context, &task, store, "default-basic", "default-proxy", NULL, NULL, NULL) == 0);
    assert (request_context_has_auth (context));
    assert (request_context_auth_kind (context) == DOWNLOADER_CREDENTIAL_BASIC);
    assert (strcmp (request_context_auth_username (context), "task-user") == 0);
    assert (strcmp (request_context_auth_secret (context), "task-secret") == 0);
    assert (request_context_has_proxy (context));
    assert (strcmp (request_context_proxy_url (context), "socks5h://site.proxy:1080") == 0);
    request_context_free (context);

    task = make_task ("https://example.test/file.bin", "{}");
    assert (request_context_resolve (&context, &task, store, "default-basic", NULL, NULL, NULL, NULL) == 0);
    assert (request_context_auth_kind (context) == DOWNLOADER_CREDENTIAL_BEARER);
    assert (strcmp (request_context_auth_secret (context), "site-token") == 0);
    assert (request_context_has_proxy (context));
    assert (strcmp (request_context_proxy_url (context), "socks5h://site.proxy:1080") == 0);
    request_context_free (context);

    task = make_task ("https://unrelated.test/file.bin", "{}");
    assert (request_context_resolve (&context, &task, store, "default-basic", "default-proxy", NULL, NULL, NULL) == 0);
    assert (strcmp (request_context_auth_username (context), "default-user") == 0);
    assert (strcmp (request_context_auth_secret (context), "default-secret") == 0);
    assert (strcmp (request_context_proxy_url (context), "http://default.proxy:8080") == 0);
    request_context_free (context);

    task = make_task ("https://unrelated.test/file.bin", "{}");
    assert (request_context_resolve (&context, &task, store, NULL, NULL, "http://global.proxy:3128", "global",
                                     "global-secret") == 0);
    assert (!request_context_has_auth (context));
    assert (request_context_has_proxy (context));
    assert (strcmp (request_context_proxy_username (context), "global") == 0);
    assert (strcmp (request_context_proxy_secret (context), "global-secret") == 0);
    request_context_free (context);

    task = make_task ("https://unrelated.test/file.bin", "{}");
    assert (request_context_resolve (&context, &task, store, NULL, NULL, NULL, NULL, NULL) == 0);
    assert (!request_context_has_auth (context));
    assert (!request_context_has_proxy (context));
    request_context_free (context);
}

static void test_failures (CredentialStore* store)
{
    RequestContext* context = NULL;
    DownloadTask    task;

    task = make_task ("https://example.test/file.bin", "{\"auth_profile\":\"missing\"}");
    errno = 0;
    assert (request_context_resolve (&context, &task, store, NULL, NULL, NULL, NULL, NULL) != 0);
    assert (errno == ENOENT);
    assert (context == NULL);

    task = make_task ("https://example.test/file.bin", "not-json");
    errno = 0;
    assert (request_context_resolve (&context, &task, store, NULL, NULL, NULL, NULL, NULL) != 0);
    assert (errno == EINVAL);

    task = make_task ("https://example.test/file.bin", "{\"proxy_profile\":\"missing-proxy\"}");
    errno = 0;
    assert (request_context_resolve (&context, &task, store, NULL, NULL, NULL, NULL, NULL) != 0);
    assert (errno == ENOENT);

    task = make_task ("https://unrelated.test/file.bin", "{}");
    errno = 0;
    assert (request_context_resolve (&context, &task, store, NULL, NULL, "ftp://not-a-proxy", NULL, NULL) != 0);
    assert (errno == EINVAL);
}

static void test_task_override (CredentialStore* store)
{
    RequestContext* context = NULL;
    DownloadTask    task = make_task ("https://example.test/file.bin",
                                      "{\"auth_profile\":\"task-basic\",\"proxy_profile\":\"other-proxy\"}");

    assert (request_context_resolve (&context, &task, store, "default-basic", NULL, NULL, NULL, NULL) == 0);
    assert (strcmp (request_context_auth_username (context), "task-user") == 0);
    assert (strcmp (request_context_proxy_url (context), "http://other.proxy:8081") == 0);
    request_context_free (context);
}

static void test_cookie_and_baidu (CredentialStore* store)
{
    RequestContext* context = NULL;
    DownloadTask    task;

    task = make_task ("baidu:///%E8%B5%84%E6%96%99/a.bin", "{\"auth_profile\":\"baidu-acct\"}");
    assert (request_context_resolve (&context, &task, store, "", "", NULL, NULL, NULL) == 0);
    assert (request_context_has_auth (context));
    assert (request_context_auth_kind (context) == DOWNLOADER_CREDENTIAL_COOKIE);
    assert (request_context_auth_username (context)[0] == '\0');
    assert (strcmp (request_context_auth_secret (context), "BDUSS=x; STOKEN=y") == 0);
    request_context_free (context);

    assert (credential_store_set_site (store, "pan.baidu.com", "baidu-acct", "") == 0);
    task = make_task ("baidu:///%E8%B5%84%E6%96%99/a.bin", "{}");
    assert (request_context_resolve (&context, &task, store, "", "", NULL, NULL, NULL) == 0);
    assert (request_context_auth_kind (context) == DOWNLOADER_CREDENTIAL_COOKIE);
    request_context_free (context);
}

int main (void)
{
    char             templatePath[] = "/tmp/downloader-request-context-XXXXXX";
    char*            directory = mkdtemp (templatePath);
    char             databasePath[512];
    Database*        database;
    CredentialStore* store;

    assert (directory != NULL);
    snprintf (databasePath, sizeof (databasePath), "%s/meta.db", directory);
    assert (database_open (databasePath, &database) == 0);
    store = credential_store_open (databasePath, NULL);
    assert (store != NULL);
    {
        CredentialProfile profile = credential ("task-basic", DOWNLOADER_CREDENTIAL_BASIC, "task-user", "task-secret");
        assert (credential_store_save_credential (store, &profile) == 0);
        profile = credential ("site-bearer", DOWNLOADER_CREDENTIAL_BEARER, "", "site-token");
        assert (credential_store_save_credential (store, &profile) == 0);
        profile = credential ("default-basic", DOWNLOADER_CREDENTIAL_BASIC, "default-user", "default-secret");
        assert (credential_store_save_credential (store, &profile) == 0);
        profile = credential ("baidu-acct", DOWNLOADER_CREDENTIAL_COOKIE, "", "BDUSS=x; STOKEN=y");
        assert (credential_store_save_credential (store, &profile) == 0);
        ProxyProfile office = proxy ("default-proxy", "http://default.proxy:8080", "proxy-user", "proxy-secret");
        assert (credential_store_save_proxy (store, &office) == 0);
        office = proxy ("other-proxy", "http://other.proxy:8081", "", "");
        assert (credential_store_save_proxy (store, &office) == 0);
        office = proxy ("site-proxy", "socks5h://site.proxy:1080", "", "");
        assert (credential_store_save_proxy (store, &office) == 0);
        assert (credential_store_set_site (store, "example.test", "site-bearer", "site-proxy") == 0);
    }
    test_priority (store);
    test_failures (store);
    test_task_override (store);
    test_cookie_and_baidu (store);
    credential_store_close (store);
    database_close (database);
    return 0;
}
