#define _POSIX_C_SOURCE 200809L

#include "credential-store.h"

#include <clib/memory.h>
#include <clib/string-util.h>

#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define CREDENTIAL_KEY_SIZE   32U
#define CREDENTIAL_NONCE_SIZE 12U
#define CREDENTIAL_TAG_SIZE   16U
#define CREDENTIAL_DATABASE_VERSION 2

struct CredentialStore
{
    sqlite3*      handle;
    unsigned char key[CREDENTIAL_KEY_SIZE];
};

static int credential_store_errno (CredentialStore* store)
{
    int errorCode = sqlite3_errcode (store->handle);

    errno = errorCode == SQLITE_NOMEM ? ENOMEM : errorCode == SQLITE_BUSY ? EBUSY : EIO;
    return -1;
}

static bool credential_text_valid (const char* value, size_t capacity)
{
    const char* end = memchr (value, '\0', capacity);

    return end != NULL && (size_t)(end - value) < capacity;
}

static int credential_key_file_exists (int descriptor)
{
    struct stat status;

    if (fstat (descriptor, &status) != 0)
        return -1;
    if (!S_ISREG (status.st_mode) || status.st_nlink != 1) {
        errno = EIO;
        return -1;
    }
    if ((status.st_mode & 0077U) != 0) {
        errno = EPERM;
        return -1;
    }
    if (status.st_size != (off_t)CREDENTIAL_KEY_SIZE) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int credential_write_all (int descriptor, const unsigned char* data, size_t size)
{
    size_t written = 0;

    while (written < size) {
        ssize_t count = write (descriptor, data + written, size - written);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        written += (size_t)count;
    }
    return 0;
}

static int credential_read_all (int descriptor, unsigned char* data, size_t size)
{
    size_t received = 0;

    while (received < size) {
        ssize_t count = read (descriptor, data + received, size - received);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0) {
            errno = EINVAL;
            return -1;
        }
        received += (size_t)count;
    }
    return 0;
}

/* The key file is independent from the task database: existing files must be
 * owner-only regular files, and new files are created exclusively as 0600. */
static int credential_load_key (const char* path, unsigned char key[CREDENTIAL_KEY_SIZE])
{
    int descriptor;

    descriptor = open (path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor >= 0) {
        if (RAND_bytes (key, (int)CREDENTIAL_KEY_SIZE) != 1) {
            int savedErrno = EIO;
            (void)close (descriptor);
            (void)unlink (path);
            errno = savedErrno;
            return -1;
        }
        if (credential_write_all (descriptor, key, CREDENTIAL_KEY_SIZE) != 0 || fsync (descriptor) != 0) {
            int savedErrno = errno;
            (void)close (descriptor);
            (void)unlink (path);
            errno = savedErrno;
            return -1;
        }
        if (close (descriptor) != 0)
            return -1;
        return 0;
    }
    if (errno != EEXIST)
        return -1;
    descriptor = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return -1;
    if (credential_key_file_exists (descriptor) != 0 || credential_read_all (descriptor, key, CREDENTIAL_KEY_SIZE) != 0) {
        int savedErrno = errno;
        (void)close (descriptor);
        errno = savedErrno;
        return -1;
    }
    return close (descriptor) == 0 ? 0 : -1;
}

static int credential_version_ok (CredentialStore* store)
{
    sqlite3_stmt* statement;
    int           version = 0;
    int           result;

    if (sqlite3_prepare_v2 (store->handle, "PRAGMA user_version;", -1, &statement, NULL) != SQLITE_OK)
        return credential_store_errno (store);
    result = sqlite3_step (statement);
    if (result == SQLITE_ROW)
        version = sqlite3_column_int (statement, 0);
    sqlite3_finalize (statement);
    if (result != SQLITE_ROW)
        return credential_store_errno (store);
    if (version < CREDENTIAL_DATABASE_VERSION) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

CredentialStore* credential_store_open (const char* databasePath, const char* keyPath)
{
    CredentialStore* store;
    char*            defaultKeyPath = NULL;

    if (databasePath == NULL || databasePath[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    if (keyPath == NULL || keyPath[0] == '\0') {
        size_t size = strlen (databasePath) + 5U;
        defaultKeyPath = (char*)c_malloc (size);
        if (defaultKeyPath == NULL)
            return NULL;
        snprintf (defaultKeyPath, size, "%s.key", databasePath);
        keyPath = defaultKeyPath;
    }
    store = (CredentialStore*)c_malloc0 (sizeof (*store));
    if (store == NULL) {
        c_free (defaultKeyPath);
        return NULL;
    }
    if (sqlite3_open_v2 (databasePath, &store->handle, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK ||
        credential_version_ok (store) != 0 || credential_load_key (keyPath, store->key) != 0) {
        int savedErrno = errno;
        sqlite3_close (store->handle);
        c_free (defaultKeyPath);
        OPENSSL_cleanse (store, sizeof (*store));
        c_free (store);
        errno = savedErrno ? savedErrno : EIO;
        return NULL;
    }
    sqlite3_busy_timeout (store->handle, 5000);
    c_free (defaultKeyPath);
    return store;
}

void credential_store_close (CredentialStore* store)
{
    if (store == NULL)
        return;
    sqlite3_close (store->handle);
    OPENSSL_cleanse (store, sizeof (*store));
    c_free (store);
}

void credential_profile_clear (CredentialProfile* profile)
{
    if (profile == NULL)
        return;
    OPENSSL_cleanse (profile, sizeof (*profile));
    memset (profile, 0, sizeof (*profile));
}

void proxy_profile_clear (ProxyProfile* profile)
{
    if (profile == NULL)
        return;
    OPENSSL_cleanse (profile, sizeof (*profile));
    memset (profile, 0, sizeof (*profile));
}

const char* credential_kind_name (DownloaderCredentialKind kind)
{
    switch (kind) {
    case DOWNLOADER_CREDENTIAL_BASIC:
        return "basic";
    case DOWNLOADER_CREDENTIAL_DIGEST:
        return "digest";
    case DOWNLOADER_CREDENTIAL_BEARER:
        return "bearer";
    case DOWNLOADER_CREDENTIAL_COOKIE:
        return "cookie";
    }
    return NULL;
}

int credential_kind_parse (const char* name, DownloaderCredentialKind* kind)
{
    if (strcmp (name, "basic") == 0)
        *kind = DOWNLOADER_CREDENTIAL_BASIC;
    else if (strcmp (name, "digest") == 0)
        *kind = DOWNLOADER_CREDENTIAL_DIGEST;
    else if (strcmp (name, "bearer") == 0)
        *kind = DOWNLOADER_CREDENTIAL_BEARER;
    else if (strcmp (name, "cookie") == 0)
        *kind = DOWNLOADER_CREDENTIAL_COOKIE;
    else {
        errno = EIO;
        return -1;
    }
    return 0;
}

/* AES-256-GCM with the profile name as additional authenticated data, so a
 * ciphertext cannot be moved to another profile row silently. */
static int credential_encrypt (CredentialStore* store, const char* aad, const char* plaintext, size_t length,
                               unsigned char** blob, size_t* blobSize)
{
    unsigned char* buffer;
    EVP_CIPHER_CTX* context;
    int            written = 0;
    int            total = 0;
    int            result = -1;

    if (length > 0x7fffffffU || CREDENTIAL_NONCE_SIZE + length + CREDENTIAL_TAG_SIZE < length) {
        errno = EINVAL;
        return -1;
    }
    buffer = (unsigned char*)malloc (CREDENTIAL_NONCE_SIZE + length + CREDENTIAL_TAG_SIZE);
    context = EVP_CIPHER_CTX_new ();
    if (buffer == NULL || context == NULL) {
        free (buffer);
        EVP_CIPHER_CTX_free (context);
        errno = ENOMEM;
        return -1;
    }
    if (RAND_bytes (buffer, CREDENTIAL_NONCE_SIZE) != 1 ||
        EVP_EncryptInit_ex (context, EVP_aes_256_gcm (), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl (context, EVP_CTRL_GCM_SET_IVLEN, CREDENTIAL_NONCE_SIZE, NULL) != 1 ||
        EVP_EncryptInit_ex (context, NULL, NULL, store->key, buffer) != 1 ||
        EVP_EncryptUpdate (context, NULL, &written, (const unsigned char*)aad, (int)strlen (aad)) != 1 ||
        EVP_EncryptUpdate (context, buffer + CREDENTIAL_NONCE_SIZE, &written, (const unsigned char*)plaintext,
                           (int)length) != 1) {
        errno = EIO;
        goto done;
    }
    total = written;
    if (EVP_EncryptFinal_ex (context, buffer + CREDENTIAL_NONCE_SIZE + total, &written) != 1) {
        errno = EIO;
        goto done;
    }
    total += written;
    if (EVP_CIPHER_CTX_ctrl (context, EVP_CTRL_GCM_GET_TAG, CREDENTIAL_TAG_SIZE,
                             buffer + CREDENTIAL_NONCE_SIZE + total) != 1) {
        errno = EIO;
        goto done;
    }
    *blob = buffer;
    *blobSize = CREDENTIAL_NONCE_SIZE + (size_t)total + CREDENTIAL_TAG_SIZE;
    buffer = NULL;
    result = 0;
done:
    EVP_CIPHER_CTX_free (context);
    if (buffer != NULL) {
        OPENSSL_cleanse (buffer, CREDENTIAL_NONCE_SIZE + length + CREDENTIAL_TAG_SIZE);
        free (buffer);
    }
    return result;
}

static int credential_decrypt (CredentialStore* store, const char* aad, const void* blob, int blobSize,
                               char* plaintext, size_t capacity)
{
    const unsigned char* bytes = (const unsigned char*)blob;
    EVP_CIPHER_CTX*      context;
    int                  written = 0;
    int                  total = 0;
    int                  result = -1;
    size_t               cipherSize;

    if (blob == NULL || blobSize < (int)(CREDENTIAL_NONCE_SIZE + CREDENTIAL_TAG_SIZE)) {
        errno = EIO;
        return -1;
    }
    cipherSize = (size_t)blobSize - CREDENTIAL_NONCE_SIZE - CREDENTIAL_TAG_SIZE;
    if (cipherSize >= capacity) {
        errno = EIO;
        return -1;
    }
    context = EVP_CIPHER_CTX_new ();
    if (context == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (EVP_DecryptInit_ex (context, EVP_aes_256_gcm (), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl (context, EVP_CTRL_GCM_SET_IVLEN, CREDENTIAL_NONCE_SIZE, NULL) != 1 ||
        EVP_DecryptInit_ex (context, NULL, NULL, store->key, bytes) != 1 ||
        EVP_DecryptUpdate (context, NULL, &written, (const unsigned char*)aad, (int)strlen (aad)) != 1 ||
        EVP_DecryptUpdate (context, (unsigned char*)plaintext, &written, bytes + CREDENTIAL_NONCE_SIZE,
                           (int)cipherSize) != 1) {
        errno = EIO;
        goto done;
    }
    total = written;
    if (EVP_CIPHER_CTX_ctrl (context, EVP_CTRL_GCM_SET_TAG, CREDENTIAL_TAG_SIZE,
                             (void*)(bytes + CREDENTIAL_NONCE_SIZE + cipherSize)) != 1 ||
        EVP_DecryptFinal_ex (context, (unsigned char*)plaintext + total, &written) != 1) {
        errno = EIO;
        goto done;
    }
    total += written;
    plaintext[total] = '\0';
    result = 0;
done:
    EVP_CIPHER_CTX_free (context);
    if (result != 0)
        OPENSSL_cleanse (plaintext, capacity);
    return result;
}

static void credential_copy_text (char* destination, size_t capacity, const unsigned char* source)
{
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    snprintf (destination, capacity, "%s", (const char*)source);
}

int credential_store_save_credential (CredentialStore* store, const CredentialProfile* profile)
{
    static const char sql[] =
        "INSERT INTO credential_profiles (name,kind,username,secret,scope,created_at,updated_at) "
        "VALUES (?,?,?,?,?,CAST(strftime('%s','now') AS INTEGER),CAST(strftime('%s','now') AS INTEGER)) "
        "ON CONFLICT(name) DO UPDATE SET kind=excluded.kind,username=excluded.username,secret=excluded.secret,"
        "scope=excluded.scope,updated_at=excluded.updated_at;";
    sqlite3_stmt*   statement;
    unsigned char*  blob = NULL;
    size_t          blobSize = 0;
    const char*     kind;
    int             result;

    if (store == NULL || profile == NULL || !credential_text_valid (profile->name, sizeof (profile->name)) ||
        !credential_text_valid (profile->username, sizeof (profile->username)) ||
        !credential_text_valid (profile->secret, sizeof (profile->secret)) ||
        !credential_text_valid (profile->scope, sizeof (profile->scope)) || profile->name[0] == '\0' ||
        (kind = credential_kind_name (profile->kind)) == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (credential_encrypt (store, profile->name, profile->secret, strlen (profile->secret), &blob, &blobSize) != 0)
        return -1;
    if (sqlite3_prepare_v2 (store->handle, sql, -1, &statement, NULL) != SQLITE_OK) {
        OPENSSL_cleanse (blob, blobSize);
        free (blob);
        return credential_store_errno (store);
    }
    if (sqlite3_bind_text (statement, 1, profile->name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 2, kind, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 3, profile->username, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_blob (statement, 4, blob, (int)blobSize, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 5, profile->scope, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        OPENSSL_cleanse (blob, blobSize);
        free (blob);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    sqlite3_finalize (statement);
    OPENSSL_cleanse (blob, blobSize);
    free (blob);
    if (result != SQLITE_DONE)
        return credential_store_errno (store);
    return 0;
}

int credential_store_load_credential (CredentialStore* store, const char* name, CredentialProfile* profile)
{
    static const char sql[] =
        "SELECT kind,username,secret,scope,created_at,updated_at FROM credential_profiles WHERE name=?;";
    sqlite3_stmt* statement;
    int           result;

    if (store == NULL || name == NULL || name[0] == '\0' || profile == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset (profile, 0, sizeof (*profile));
    if (sqlite3_prepare_v2 (store->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return credential_store_errno (store);
    if (sqlite3_bind_text (statement, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    if (result == SQLITE_DONE) {
        sqlite3_finalize (statement);
        errno = ENOENT;
        return -1;
    }
    if (result != SQLITE_ROW) {
        sqlite3_finalize (statement);
        return credential_store_errno (store);
    }
    snprintf (profile->name, sizeof (profile->name), "%s", name);
    if (credential_kind_parse ((const char*)sqlite3_column_text (statement, 0), &profile->kind) != 0) {
        sqlite3_finalize (statement);
        OPENSSL_cleanse (profile, sizeof (*profile));
        return -1;
    }
    credential_copy_text (profile->username, sizeof (profile->username), sqlite3_column_text (statement, 1));
    credential_copy_text (profile->scope, sizeof (profile->scope), sqlite3_column_text (statement, 3));
    profile->createdAt = sqlite3_column_int64 (statement, 4);
    profile->updatedAt = sqlite3_column_int64 (statement, 5);
    if (credential_decrypt (store, name, sqlite3_column_blob (statement, 2), sqlite3_column_bytes (statement, 2),
                            profile->secret, sizeof (profile->secret)) != 0) {
        sqlite3_finalize (statement);
        OPENSSL_cleanse (profile, sizeof (*profile));
        errno = EIO;
        return -1;
    }
    sqlite3_finalize (statement);
    return 0;
}

int credential_store_delete_credential (CredentialStore* store, const char* name)
{
    sqlite3_stmt* statement;
    int           result;

    if (store == NULL || name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (sqlite3_prepare_v2 (store->handle, "DELETE FROM credential_profiles WHERE name=?;", -1, &statement, NULL) !=
        SQLITE_OK)
        return credential_store_errno (store);
    if (sqlite3_bind_text (statement, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE)
        return credential_store_errno (store);
    if (sqlite3_changes (store->handle) == 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int credential_store_list_credentials (CredentialStore* store, CredentialProfile** profiles, size_t* count)
{
    static const char sql[] =
        "SELECT name,kind,username,scope,created_at,updated_at FROM credential_profiles ORDER BY name;";
    sqlite3_stmt* statement;
    CredentialProfile* values;
    size_t        capacity = 8;
    size_t        used = 0;
    int           result;

    if (store == NULL || profiles == NULL || count == NULL) {
        errno = EINVAL;
        return -1;
    }
    *profiles = NULL;
    *count = 0;
    values = (CredentialProfile*)c_malloc0 (capacity * sizeof (*values));
    if (values == NULL)
        return -1;
    if (sqlite3_prepare_v2 (store->handle, sql, -1, &statement, NULL) != SQLITE_OK) {
        c_free (values);
        return credential_store_errno (store);
    }
    while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
        if (used == capacity) {
            CredentialProfile* grown;
            capacity *= 2U;
            grown = (CredentialProfile*)realloc (values, capacity * sizeof (*values));
            if (grown == NULL) {
                sqlite3_finalize (statement);
                c_free (values);
                errno = ENOMEM;
                return -1;
            }
            values = grown;
        }
        memset (&values[used], 0, sizeof (values[used]));
        credential_copy_text (values[used].name, sizeof (values[used].name), sqlite3_column_text (statement, 0));
        if (credential_kind_parse ((const char*)sqlite3_column_text (statement, 1), &values[used].kind) != 0) {
            sqlite3_finalize (statement);
            c_free (values);
            return -1;
        }
        credential_copy_text (values[used].username, sizeof (values[used].username),
                              sqlite3_column_text (statement, 2));
        credential_copy_text (values[used].scope, sizeof (values[used].scope), sqlite3_column_text (statement, 3));
        values[used].createdAt = sqlite3_column_int64 (statement, 4);
        values[used].updatedAt = sqlite3_column_int64 (statement, 5);
        used++;
    }
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE) {
        c_free (values);
        return credential_store_errno (store);
    }
    *profiles = values;
    *count = used;
    return 0;
}

bool credential_url_has_userinfo (const char* url)
{
    const char* scheme;
    const char* authority;
    const char* end;
    size_t      length;

    if (url == NULL)
        return false;
    scheme = strstr (url, "://");
    if (scheme == NULL)
        return false;
    authority = scheme + 3;
    end = strpbrk (authority, "/?#");
    length = end == NULL ? strlen (authority) : (size_t)(end - authority);
    return memchr (authority, '@', length) != NULL;
}

int credential_store_save_proxy (CredentialStore* store, const ProxyProfile* profile)
{
    static const char sql[] =
        "INSERT INTO proxy_profiles (name,url,username,secret,created_at,updated_at) "
        "VALUES (?,?,?,?,CAST(strftime('%s','now') AS INTEGER),CAST(strftime('%s','now') AS INTEGER)) "
        "ON CONFLICT(name) DO UPDATE SET url=excluded.url,username=excluded.username,secret=excluded.secret,"
        "updated_at=excluded.updated_at;";
    sqlite3_stmt*  statement;
    unsigned char* blob = NULL;
    size_t         blobSize = 0;
    int            result;

    if (store == NULL || profile == NULL || !credential_text_valid (profile->name, sizeof (profile->name)) ||
        !credential_text_valid (profile->url, sizeof (profile->url)) ||
        !credential_text_valid (profile->username, sizeof (profile->username)) ||
        !credential_text_valid (profile->secret, sizeof (profile->secret)) || profile->name[0] == '\0' ||
        profile->url[0] == '\0' || credential_url_has_userinfo (profile->url)) {
        errno = EINVAL;
        return -1;
    }
    if (credential_encrypt (store, profile->name, profile->secret, strlen (profile->secret), &blob, &blobSize) != 0)
        return -1;
    if (sqlite3_prepare_v2 (store->handle, sql, -1, &statement, NULL) != SQLITE_OK) {
        OPENSSL_cleanse (blob, blobSize);
        free (blob);
        return credential_store_errno (store);
    }
    if (sqlite3_bind_text (statement, 1, profile->name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 2, profile->url, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 3, profile->username, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_blob (statement, 4, blob, (int)blobSize, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        OPENSSL_cleanse (blob, blobSize);
        free (blob);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    sqlite3_finalize (statement);
    OPENSSL_cleanse (blob, blobSize);
    free (blob);
    if (result != SQLITE_DONE)
        return credential_store_errno (store);
    return 0;
}

int credential_store_load_proxy (CredentialStore* store, const char* name, ProxyProfile* profile)
{
    static const char sql[] =
        "SELECT url,username,secret,created_at,updated_at FROM proxy_profiles WHERE name=?;";
    sqlite3_stmt* statement;
    int           result;

    if (store == NULL || name == NULL || name[0] == '\0' || profile == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset (profile, 0, sizeof (*profile));
    if (sqlite3_prepare_v2 (store->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return credential_store_errno (store);
    if (sqlite3_bind_text (statement, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    if (result == SQLITE_DONE) {
        sqlite3_finalize (statement);
        errno = ENOENT;
        return -1;
    }
    if (result != SQLITE_ROW) {
        sqlite3_finalize (statement);
        return credential_store_errno (store);
    }
    snprintf (profile->name, sizeof (profile->name), "%s", name);
    credential_copy_text (profile->url, sizeof (profile->url), sqlite3_column_text (statement, 0));
    credential_copy_text (profile->username, sizeof (profile->username), sqlite3_column_text (statement, 1));
    profile->createdAt = sqlite3_column_int64 (statement, 3);
    profile->updatedAt = sqlite3_column_int64 (statement, 4);
    if (credential_decrypt (store, name, sqlite3_column_blob (statement, 2), sqlite3_column_bytes (statement, 2),
                            profile->secret, sizeof (profile->secret)) != 0) {
        sqlite3_finalize (statement);
        OPENSSL_cleanse (profile, sizeof (*profile));
        errno = EIO;
        return -1;
    }
    sqlite3_finalize (statement);
    return 0;
}

int credential_store_delete_proxy (CredentialStore* store, const char* name)
{
    sqlite3_stmt* statement;
    int           result;

    if (store == NULL || name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (sqlite3_prepare_v2 (store->handle, "DELETE FROM proxy_profiles WHERE name=?;", -1, &statement, NULL) !=
        SQLITE_OK)
        return credential_store_errno (store);
    if (sqlite3_bind_text (statement, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE)
        return credential_store_errno (store);
    if (sqlite3_changes (store->handle) == 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int credential_store_list_proxies (CredentialStore* store, ProxyProfile** profiles, size_t* count)
{
    static const char sql[] =
        "SELECT name,url,username,created_at,updated_at FROM proxy_profiles ORDER BY name;";
    sqlite3_stmt*  statement;
    ProxyProfile*  values;
    size_t         capacity = 8;
    size_t         used = 0;
    int            result;

    if (store == NULL || profiles == NULL || count == NULL) {
        errno = EINVAL;
        return -1;
    }
    *profiles = NULL;
    *count = 0;
    values = (ProxyProfile*)c_malloc0 (capacity * sizeof (*values));
    if (values == NULL)
        return -1;
    if (sqlite3_prepare_v2 (store->handle, sql, -1, &statement, NULL) != SQLITE_OK) {
        c_free (values);
        return credential_store_errno (store);
    }
    while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
        if (used == capacity) {
            ProxyProfile* grown;
            capacity *= 2U;
            grown = (ProxyProfile*)realloc (values, capacity * sizeof (*values));
            if (grown == NULL) {
                sqlite3_finalize (statement);
                c_free (values);
                errno = ENOMEM;
                return -1;
            }
            values = grown;
        }
        memset (&values[used], 0, sizeof (values[used]));
        credential_copy_text (values[used].name, sizeof (values[used].name), sqlite3_column_text (statement, 0));
        credential_copy_text (values[used].url, sizeof (values[used].url), sqlite3_column_text (statement, 1));
        credential_copy_text (values[used].username, sizeof (values[used].username),
                              sqlite3_column_text (statement, 2));
        values[used].createdAt = sqlite3_column_int64 (statement, 3);
        values[used].updatedAt = sqlite3_column_int64 (statement, 4);
        used++;
    }
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE) {
        c_free (values);
        return credential_store_errno (store);
    }
    *profiles = values;
    *count = used;
    return 0;
}

int credential_store_set_site (CredentialStore* store, const char* domain, const char* authProfile,
                               const char* proxyProfile)
{
    static const char sql[] =
        "INSERT INTO site_profiles (domain,auth_profile,proxy_profile,updated_at) "
        "VALUES (?,?,?,CAST(strftime('%s','now') AS INTEGER)) "
        "ON CONFLICT(domain) DO UPDATE SET auth_profile=excluded.auth_profile,"
        "proxy_profile=excluded.proxy_profile,updated_at=excluded.updated_at;";
    sqlite3_stmt* statement;
    int           result;

    if (store == NULL || domain == NULL || domain[0] == '\0' || authProfile == NULL || proxyProfile == NULL ||
        strlen (domain) >= DOWNLOADER_CREDENTIAL_DOMAIN_SIZE ||
        strlen (authProfile) >= DOWNLOADER_CREDENTIAL_NAME_SIZE ||
        strlen (proxyProfile) >= DOWNLOADER_CREDENTIAL_NAME_SIZE) {
        errno = EINVAL;
        return -1;
    }
    if (sqlite3_prepare_v2 (store->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return credential_store_errno (store);
    if (sqlite3_bind_text (statement, 1, domain, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 2, authProfile, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 3, proxyProfile, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE)
        return credential_store_errno (store);
    return 0;
}

static bool credential_domain_matches (const char* host, const char* domain)
{
    size_t hostSize = strlen (host);
    size_t domainSize = strlen (domain);

    if (domainSize == 0 || domainSize > hostSize)
        return false;
    if (hostSize == domainSize)
        return strcasecmp (host, domain) == 0;
    return host[hostSize - domainSize - 1U] == '.' &&
           strcasecmp (host + hostSize - domainSize, domain) == 0;
}

int credential_store_resolve_site (CredentialStore* store, const char* host, char* authProfile,
                                   size_t authCapacity, char* proxyProfile, size_t proxyCapacity)
{
    static const char sql[] = "SELECT domain,auth_profile,proxy_profile FROM site_profiles;";
    sqlite3_stmt* statement;
    int           result;
    size_t        best = 0;

    if (store == NULL || host == NULL || authProfile == NULL || proxyProfile == NULL || authCapacity == 0 ||
        proxyCapacity == 0) {
        errno = EINVAL;
        return -1;
    }
    authProfile[0] = '\0';
    proxyProfile[0] = '\0';
    if (sqlite3_prepare_v2 (store->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return credential_store_errno (store);
    while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
        const char* domain = (const char*)sqlite3_column_text (statement, 0);
        size_t      domainSize = strlen (domain);
        if (domainSize > best && credential_domain_matches (host, domain)) {
            const char* auth = (const char*)sqlite3_column_text (statement, 1);
            const char* proxyName = (const char*)sqlite3_column_text (statement, 2);
            if (strlen (auth) >= authCapacity || strlen (proxyName) >= proxyCapacity) {
                sqlite3_finalize (statement);
                errno = ENAMETOOLONG;
                return -1;
            }
            snprintf (authProfile, authCapacity, "%s", auth);
            snprintf (proxyProfile, proxyCapacity, "%s", proxyName);
            best = domainSize;
        }
    }
    sqlite3_finalize (statement);
    return result == SQLITE_DONE ? 0 : credential_store_errno (store);
}

int credential_store_list_sites (CredentialStore* store, SiteProfile** sites, size_t* count)
{
    static const char sql[] = "SELECT domain,auth_profile,proxy_profile FROM site_profiles ORDER BY domain;";
    sqlite3_stmt* statement;
    SiteProfile*  values;
    size_t        capacity = 8;
    size_t        used = 0;
    int           result;

    if (store == NULL || sites == NULL || count == NULL) {
        errno = EINVAL;
        return -1;
    }
    *sites = NULL;
    *count = 0;
    values = (SiteProfile*)c_malloc0 (capacity * sizeof (*values));
    if (values == NULL)
        return -1;
    if (sqlite3_prepare_v2 (store->handle, sql, -1, &statement, NULL) != SQLITE_OK) {
        c_free (values);
        return credential_store_errno (store);
    }
    while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
        if (used == capacity) {
            SiteProfile* grown;
            capacity *= 2U;
            grown = (SiteProfile*)realloc (values, capacity * sizeof (*values));
            if (grown == NULL) {
                sqlite3_finalize (statement);
                c_free (values);
                errno = ENOMEM;
                return -1;
            }
            values = grown;
        }
        memset (&values[used], 0, sizeof (values[used]));
        credential_copy_text (values[used].domain, sizeof (values[used].domain), sqlite3_column_text (statement, 0));
        credential_copy_text (values[used].authProfile, sizeof (values[used].authProfile),
                              sqlite3_column_text (statement, 1));
        credential_copy_text (values[used].proxyProfile, sizeof (values[used].proxyProfile),
                              sqlite3_column_text (statement, 2));
        used++;
    }
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE) {
        c_free (values);
        return credential_store_errno (store);
    }
    *sites = values;
    *count = used;
    return 0;
}
