#define _GNU_SOURCE

#include "credential-store.h"
#include "database.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static CredentialProfile credential (const char* name, DownloaderCredentialKind kind, const char* user,
                                     const char* secret, const char* scope)
{
    CredentialProfile profile;

    memset (&profile, 0, sizeof (profile));
    snprintf (profile.name, sizeof (profile.name), "%s", name);
    profile.kind = kind;
    snprintf (profile.username, sizeof (profile.username), "%s", user);
    snprintf (profile.secret, sizeof (profile.secret), "%s", secret);
    snprintf (profile.scope, sizeof (profile.scope), "%s", scope);
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

static void assert_mode (const char* path, mode_t expected)
{
    struct stat status;

    assert (lstat (path, &status) == 0);
    assert (S_ISREG (status.st_mode));
    assert ((status.st_mode & 07777U) == expected);
    assert (status.st_nlink == 1);
}

static void assert_not_plaintext (const char* databasePath, const char* table, const char* column, const char* secret)
{
    sqlite3*      raw;
    sqlite3_stmt* statement;
    const void*   blob;
    int           size;
    char          sql[256];

    assert (sqlite3_open_v2 (databasePath, &raw, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    snprintf (sql, sizeof (sql), "SELECT %s FROM %s;", column, table);
    assert (sqlite3_prepare_v2 (raw, sql, -1, &statement, NULL) == SQLITE_OK);
    assert (sqlite3_step (statement) == SQLITE_ROW);
    blob = sqlite3_column_blob (statement, 0);
    size = sqlite3_column_bytes (statement, 0);
    assert (blob != NULL && size > 0);
    assert (memmem (blob, (size_t)size, secret, strlen (secret)) == NULL);
    sqlite3_finalize (statement);
    assert (sqlite3_close (raw) == SQLITE_OK);
}

static void test_roundtrip_and_storage (const char* directory)
{
    char                databasePath[512];
    char                keyPath[512];
    Database*           database;
    CredentialStore*    store;
    CredentialProfile   saved;
    CredentialProfile   loaded;
    CredentialProfile*  list = NULL;
    size_t              count = 0;
    ProxyProfile        savedProxy;
    ProxyProfile        loadedProxy;
    ProxyProfile*       proxies = NULL;
    size_t              proxyCount = 0;

    snprintf (databasePath, sizeof (databasePath), "%s/meta.db", directory);
    snprintf (keyPath, sizeof (keyPath), "%s/secret.key", directory);
    assert (database_open (databasePath, &database) == 0);
    store = credential_store_open (databasePath, keyPath);
    assert (store != NULL);
    assert_mode (keyPath, 0600);
    {
        struct stat status;
        assert (stat (keyPath, &status) == 0);
        assert (status.st_size == 32);
    }

    saved = credential ("site-a", DOWNLOADER_CREDENTIAL_BASIC, "alice", "s3cret-value", "example.test");
    assert (credential_store_save_credential (store, &saved) == 0);
    memset (&loaded, 0, sizeof (loaded));
    assert (credential_store_load_credential (store, "site-a", &loaded) == 0);
    assert (loaded.kind == DOWNLOADER_CREDENTIAL_BASIC);
    assert (strcmp (loaded.username, "alice") == 0);
    assert (strcmp (loaded.secret, "s3cret-value") == 0);
    assert (strcmp (loaded.scope, "example.test") == 0);
    credential_profile_clear (&loaded);
    assert_not_plaintext (databasePath, "credential_profiles", "secret", "s3cret-value");

    assert (credential_store_list_credentials (store, &list, &count) == 0);
    assert (count == 1);
    assert (strcmp (list[0].name, "site-a") == 0);
    assert (list[0].secret[0] == '\0');
    free (list);

    saved = credential ("site-a", DOWNLOADER_CREDENTIAL_BEARER, "alice", "rotated-token", "example.test");
    assert (credential_store_save_credential (store, &saved) == 0);
    assert (credential_store_load_credential (store, "site-a", &loaded) == 0);
    assert (loaded.kind == DOWNLOADER_CREDENTIAL_BEARER);
    assert (strcmp (loaded.secret, "rotated-token") == 0);
    credential_profile_clear (&loaded);

    assert (strcmp (credential_kind_name (DOWNLOADER_CREDENTIAL_COOKIE), "cookie") == 0);
    {
        DownloaderCredentialKind kind;
        assert (credential_kind_parse ("cookie", &kind) == 0 && kind == DOWNLOADER_CREDENTIAL_COOKIE);
    }
    saved = credential ("baidu", DOWNLOADER_CREDENTIAL_COOKIE, "", "BDUSS=x; STOKEN=y", "pan.baidu.com");
    assert (credential_store_save_credential (store, &saved) == 0);
    assert (credential_store_load_credential (store, "baidu", &loaded) == 0);
    assert (loaded.kind == DOWNLOADER_CREDENTIAL_COOKIE);
    assert (loaded.username[0] == '\0');
    assert (strcmp (loaded.secret, "BDUSS=x; STOKEN=y") == 0);
    credential_profile_clear (&loaded);

    savedProxy = proxy ("office", "http://proxy.test:3128", "proxy-user", "proxy-secret");
    assert (credential_store_save_proxy (store, &savedProxy) == 0);
    assert (credential_store_load_proxy (store, "office", &loadedProxy) == 0);
    assert (strcmp (loadedProxy.url, "http://proxy.test:3128") == 0);
    assert (strcmp (loadedProxy.username, "proxy-user") == 0);
    assert (strcmp (loadedProxy.secret, "proxy-secret") == 0);
    proxy_profile_clear (&loadedProxy);
    assert_not_plaintext (databasePath, "proxy_profiles", "secret", "proxy-secret");
    assert (credential_store_list_proxies (store, &proxies, &proxyCount) == 0);
    assert (proxyCount == 1 && proxies[0].secret[0] == '\0');
    free (proxies);

    assert (credential_store_set_site (store, "example.test", "site-a", "office") == 0);
    assert (credential_store_set_site (store, "cdn.example.test", "site-a", "") == 0);
    {
        char auth[DOWNLOADER_CREDENTIAL_NAME_SIZE];
        char proxyName[DOWNLOADER_CREDENTIAL_NAME_SIZE];
        assert (credential_store_resolve_site (store, "images.CDN.example.test", auth, sizeof (auth),
                                               proxyName, sizeof (proxyName)) == 0);
        assert (strcmp (auth, "site-a") == 0);
        assert (proxyName[0] == '\0');
        assert (credential_store_resolve_site (store, "www.example.test", auth, sizeof (auth),
                                               proxyName, sizeof (proxyName)) == 0);
        assert (strcmp (auth, "site-a") == 0);
        assert (strcmp (proxyName, "office") == 0);
        assert (credential_store_resolve_site (store, "unrelated.test", auth, sizeof (auth),
                                               proxyName, sizeof (proxyName)) == 0);
        assert (auth[0] == '\0' && proxyName[0] == '\0');
    }

    credential_store_close (store);
    /* The key survives a reopen and still decrypts existing rows. */
    store = credential_store_open (databasePath, keyPath);
    assert (store != NULL);
    assert (credential_store_load_credential (store, "site-a", &loaded) == 0);
    assert (strcmp (loaded.secret, "rotated-token") == 0);
    credential_profile_clear (&loaded);
    assert (credential_store_delete_credential (store, "site-a") == 0);
    errno = 0;
    assert (credential_store_load_credential (store, "site-a", &loaded) != 0);
    assert (errno == ENOENT);
    assert (credential_store_delete_proxy (store, "office") == 0);
    credential_store_close (store);
    database_close (database);
}

static void test_proxy_userinfo_is_rejected (const char* directory)
{
    char             databasePath[512];
    Database*        database;
    CredentialStore* store;
    ProxyProfile     profile;

    snprintf (databasePath, sizeof (databasePath), "%s/userinfo.db", directory);
    assert (database_open (databasePath, &database) == 0);
    store = credential_store_open (databasePath, NULL);
    assert (store != NULL);
    profile = proxy ("embedded", "http://user:secret@proxy.test:3128", "", "");
    errno = 0;
    assert (credential_store_save_proxy (store, &profile) != 0);
    assert (errno == EINVAL);
    profile = proxy ("separate", "http://proxy.test:3128", "user", "secret");
    assert (credential_store_save_proxy (store, &profile) == 0);
    credential_store_close (store);
    database_close (database);
}

static void test_wrong_key_is_rejected (const char* directory)
{
    char              databasePath[512];
    char              keyPath[512];
    char              otherKeyPath[512];
    Database*         database;
    CredentialStore*  store;
    CredentialProfile saved = credential ("site-b", DOWNLOADER_CREDENTIAL_BASIC, "bob", "wrong-key-secret", "");
    CredentialProfile loaded;

    snprintf (databasePath, sizeof (databasePath), "%s/wrong.db", directory);
    snprintf (keyPath, sizeof (keyPath), "%s/wrong.key", directory);
    snprintf (otherKeyPath, sizeof (otherKeyPath), "%s/other.key", directory);
    assert (database_open (databasePath, &database) == 0);
    store = credential_store_open (databasePath, keyPath);
    assert (store != NULL);
    assert (credential_store_save_credential (store, &saved) == 0);
    credential_store_close (store);
    /* A different key file cannot authenticate the GCM tag. */
    store = credential_store_open (databasePath, otherKeyPath);
    assert (store != NULL);
    memset (&loaded, 0, sizeof (loaded));
    errno = 0;
    assert (credential_store_load_credential (store, "site-b", &loaded) != 0);
    assert (errno == EIO);
    credential_store_close (store);
    database_close (database);
}

static void test_permissive_key_is_rejected (const char* directory)
{
    char             databasePath[512];
    char             keyPath[512];
    Database*        database;
    CredentialStore* store;

    snprintf (databasePath, sizeof (databasePath), "%s/perm.db", directory);
    snprintf (keyPath, sizeof (keyPath), "%s/perm.key", directory);
    assert (database_open (databasePath, &database) == 0);
    {
        int descriptor = open (keyPath, O_WRONLY | O_CREAT | O_EXCL, 0644);
        assert (descriptor >= 0);
        assert (write (descriptor, "01234567890123456789012345678901", 32) == 32);
        assert (close (descriptor) == 0);
        assert (chmod (keyPath, 0644) == 0);
    }
    errno = 0;
    store = credential_store_open (databasePath, keyPath);
    assert (store == NULL);
    assert (errno == EPERM);
    database_close (database);
}

int main (void)
{
    char  templatePath[] = "/tmp/downloader-credential-XXXXXX";
    char* directory = mkdtemp (templatePath);

    assert (directory != NULL);
    test_roundtrip_and_storage (directory);
    test_proxy_userinfo_is_rejected (directory);
    test_wrong_key_is_rejected (directory);
    test_permissive_key_is_rejected (directory);
    return 0;
}
