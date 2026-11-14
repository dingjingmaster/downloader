#ifndef DOWNLOADER_CREDENTIAL_STORE_H
#define DOWNLOADER_CREDENTIAL_STORE_H

#include <clib/macro.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DOWNLOADER_CREDENTIAL_NAME_SIZE     64U
#define DOWNLOADER_CREDENTIAL_USER_SIZE     256U
#define DOWNLOADER_CREDENTIAL_SECRET_SIZE   4096U
#define DOWNLOADER_CREDENTIAL_URL_SIZE      2048U
#define DOWNLOADER_CREDENTIAL_DOMAIN_SIZE   256U

C_BEGIN_EXTERN_C

typedef enum
{
    DOWNLOADER_CREDENTIAL_BASIC,
    DOWNLOADER_CREDENTIAL_DIGEST,
    DOWNLOADER_CREDENTIAL_BEARER,
    DOWNLOADER_CREDENTIAL_COOKIE
} DownloaderCredentialKind;

/* Secrets are decrypted only into runtime records; they are never persisted in
 * plaintext and never returned by list APIs. */
typedef struct
{
    char                     name[DOWNLOADER_CREDENTIAL_NAME_SIZE];
    DownloaderCredentialKind kind;
    char                     username[DOWNLOADER_CREDENTIAL_USER_SIZE];
    char                     secret[DOWNLOADER_CREDENTIAL_SECRET_SIZE];
    char                     scope[DOWNLOADER_CREDENTIAL_DOMAIN_SIZE];
    int64_t                  createdAt;
    int64_t                  updatedAt;
} CredentialProfile;

typedef struct
{
    char domain[DOWNLOADER_CREDENTIAL_DOMAIN_SIZE];
    char authProfile[DOWNLOADER_CREDENTIAL_NAME_SIZE];
    char proxyProfile[DOWNLOADER_CREDENTIAL_NAME_SIZE];
} SiteProfile;

typedef struct
{
    char    name[DOWNLOADER_CREDENTIAL_NAME_SIZE];
    char    url[DOWNLOADER_CREDENTIAL_URL_SIZE];
    char    username[DOWNLOADER_CREDENTIAL_USER_SIZE];
    char    secret[DOWNLOADER_CREDENTIAL_SECRET_SIZE];
    int64_t createdAt;
    int64_t updatedAt;
} ProxyProfile;

typedef struct CredentialStore CredentialStore;

/* Opens the profile tables and the AES-256-GCM key. keyPath NULL means
 * "<databasePath>.key". The key file must be a regular 0600 file with nlink 1;
 * it is generated exclusively on first use. */
CredentialStore* credential_store_open (const char* databasePath, const char* keyPath);
void             credential_store_close (CredentialStore* store);
void             credential_profile_clear (CredentialProfile* profile);
void             proxy_profile_clear (ProxyProfile* profile);
const char*      credential_kind_name (DownloaderCredentialKind kind);
/* True when the URL authority contains userinfo (user[:password]@host). */
bool             credential_url_has_userinfo (const char* url);
int              credential_kind_parse (const char* name, DownloaderCredentialKind* kind);

int credential_store_save_credential (CredentialStore* store, const CredentialProfile* profile);
/* Returns 0 on success; ENOENT when absent; EIO when decryption fails. */
int credential_store_load_credential (CredentialStore* store, const char* name, CredentialProfile* profile);
int credential_store_delete_credential (CredentialStore* store, const char* name);
/* Returns records with secret fields zeroed. */
int credential_store_list_credentials (CredentialStore* store, CredentialProfile** profiles, size_t* count);

int credential_store_save_proxy (CredentialStore* store, const ProxyProfile* profile);
int credential_store_load_proxy (CredentialStore* store, const char* name, ProxyProfile* profile);
int credential_store_delete_proxy (CredentialStore* store, const char* name);
int credential_store_list_proxies (CredentialStore* store, ProxyProfile** profiles, size_t* count);

/* Site overrides are matched by longest case-insensitive domain suffix. */
int credential_store_set_site (CredentialStore* store, const char* domain, const char* authProfile,
                               const char* proxyProfile);
int credential_store_resolve_site (CredentialStore* store, const char* host, char* authProfile,
                                   size_t authCapacity, char* proxyProfile, size_t proxyCapacity);
int credential_store_list_sites (CredentialStore* store, SiteProfile** sites, size_t* count);

C_END_EXTERN_C

#endif
