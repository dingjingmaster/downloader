#include "config.h"

#include <clib/key-file.h>
#include <clib/memory.h>
#include <clib/path.h>
#include <clib/string-view.h>
#include <clib/string-util.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef DOWNLOADER_SYSTEM_CONFIG_PATH
#define DOWNLOADER_SYSTEM_CONFIG_PATH "/etc/downloader/download.ini"
#endif

struct Config
{
    char*   path;
    char*   ipcSocket;
    char*   logLevel;
    char*   downloadDir;
    char*   database;
    char*   tempSuffix;
    char*   overwritePolicy;
    char*   webListen;
    bool    webEnabled;
    int32_t webPort;
    int32_t maxConcurrentTasks;
    int32_t maxRetries;
    int32_t connectTimeout;
    int32_t transferTimeout;
    char*   secretKeyFile;
    char*   defaultAuthProfile;
    char*   defaultProxyProfile;
    char*   proxyUrl;
    char*   proxyUsername;
    char*   proxyPassword;
    char*   webUsername;
    char*   webPassword;
    int32_t webSessionSeconds;
    char*   baiduTransferDirectory;
    int32_t baiduMaxParallelTasks;
    int32_t baiduMaxFiles;
    int32_t baiduSignCacheSeconds;
    char*   baiduApiBase;
    char*   baiduPcsBase;
};

static bool config_file_exists (const char* path)
{
    FILE* file;

    if (path == NULL)
        return false;
    file = fopen (path, "rb");
    if (file == NULL)
        return false;
    (void)fclose (file);
    return true;
}

static int config_get_string (const CKeyFile* keyFile, const char* group, const char* key, const char* fallback,
                              char** value)
{
    char*       rawValue;
    CStringView trimmedValue;

    if (c_key_file_get_string (keyFile, c_string_view_from_cstr (group), c_string_view_from_cstr (key), &rawValue) == 0) {
        trimmedValue = c_string_view_trim_ascii (c_string_view_from_cstr (rawValue));
        *value = c_string_view_to_cstr (trimmedValue);
        c_free (rawValue);
        return *value == NULL ? -1 : 0;
    }
    if (errno != ENOENT)
        return -1;
    *value = c_strdup (fallback);
    return *value == NULL ? -1 : 0;
}

static int config_get_int32 (const CKeyFile* keyFile, const char* group, const char* key, int32_t fallback,
                             int32_t* value)
{
    if (c_key_file_get_int32 (keyFile, c_string_view_from_cstr (group), c_string_view_from_cstr (key), value) == 0)
        return 0;
    if (errno != ENOENT)
        return -1;
    *value = fallback;
    return 0;
}

static int config_get_boolean (const CKeyFile* keyFile, const char* group, const char* key, bool fallback, bool* value)
{
    char*       rawValue;
    CStringView trimmedValue;

    if (c_key_file_get_string (keyFile, c_string_view_from_cstr (group), c_string_view_from_cstr (key), &rawValue) != 0) {
        if (errno != ENOENT)
            return -1;
        *value = fallback;
        return 0;
    }
    trimmedValue = c_string_view_trim_ascii (c_string_view_from_cstr (rawValue));
    if ((trimmedValue.size == 4U && strncmp (trimmedValue.data, "true", trimmedValue.size) == 0) ||
        (trimmedValue.size == 3U && strncmp (trimmedValue.data, "yes", trimmedValue.size) == 0) ||
        (trimmedValue.size == 1U && trimmedValue.data[0] == '1')) {
        *value = true;
    } else if ((trimmedValue.size == 5U && strncmp (trimmedValue.data, "false", trimmedValue.size) == 0) ||
               (trimmedValue.size == 2U && strncmp (trimmedValue.data, "no", trimmedValue.size) == 0) ||
               (trimmedValue.size == 1U && trimmedValue.data[0] == '0')) {
        *value = false;
    } else {
        c_free (rawValue);
        errno = EINVAL;
        return -1;
    }
    c_free (rawValue);
    return 0;
}

static char* config_find_explicit_path (int argc, char* argv[])
{
    int index;

    for (index = 1; index < argc; index++) {
        if (strcmp (argv[index], "--config") == 0) {
            if (index + 1 >= argc || argv[index + 1][0] == '\0') {
                errno = EINVAL;
                return NULL;
            }
            return c_strdup (argv[index + 1]);
        }
        if (strncmp (argv[index], "--config=", 9U) == 0) {
            if (argv[index][9] == '\0') {
                errno = EINVAL;
                return NULL;
            }
            return c_strdup (argv[index] + 9);
        }
    }
    return NULL;
}

static char* config_find_path (int argc, char* argv[])
{
    char* explicitPath;
    char* programDirectory;
    char* programConfig;

    errno = 0;
    explicitPath = config_find_explicit_path (argc, argv);
    if (explicitPath != NULL || errno == EINVAL)
        return explicitPath;

    programDirectory = c_path_dirname (argc > 0 && argv[0] != NULL ? argv[0] : ".");
    if (programDirectory != NULL) {
        programConfig = c_path_join (programDirectory, "config/download.ini");
        c_free (programDirectory);
        if (programConfig != NULL && config_file_exists (programConfig))
            return programConfig;
        c_free (programConfig);
    }

    if (config_file_exists (DOWNLOADER_SYSTEM_CONFIG_PATH))
        return c_strdup (DOWNLOADER_SYSTEM_CONFIG_PATH);

    errno = ENOENT;
    return NULL;
}

static int config_load_values (Config* config, const CKeyFile* keyFile)
{
    if (config_get_string (keyFile, "daemon", "ipc_socket", "/run/downloader.sock", &config->ipcSocket) != 0)
        return -1;
    if (config_get_string (keyFile, "daemon", "log_level", "info", &config->logLevel) != 0)
        return -1;
    if (config_get_string (keyFile, "download", "download_dir", ".", &config->downloadDir) != 0)
        return -1;
    if (config_get_string (keyFile, "download", "database", "meta.db", &config->database) != 0)
        return -1;
    if (config_get_string (keyFile, "download", "temp_suffix", ".part", &config->tempSuffix) != 0)
        return -1;
    if (config_get_string (keyFile, "download", "overwrite_policy", "resume", &config->overwritePolicy) != 0)
        return -1;
    if (config_get_boolean (keyFile, "web", "enabled", true, &config->webEnabled) != 0)
        return -1;
    if (config_get_string (keyFile, "web", "listen", "0.0.0.0", &config->webListen) != 0)
        return -1;
    if (config_get_int32 (keyFile, "web", "port", 8000, &config->webPort) != 0)
        return -1;
    if (config_get_string (keyFile, "web", "username", "", &config->webUsername) != 0)
        return -1;
    if (config_get_string (keyFile, "web", "password", "", &config->webPassword) != 0)
        return -1;
    if (config_get_int32 (keyFile, "web", "session_seconds", 28800, &config->webSessionSeconds) != 0)
        return -1;
    if (config_get_int32 (keyFile, "download", "max_concurrent_tasks", 5, &config->maxConcurrentTasks) != 0)
        return -1;
    if (config_get_int32 (keyFile, "download", "max_retries", 3, &config->maxRetries) != 0)
        return -1;
    if (config_get_int32 (keyFile, "download", "connect_timeout", 30, &config->connectTimeout) != 0)
        return -1;
    if (config_get_int32 (keyFile, "download", "transfer_timeout", 0, &config->transferTimeout) != 0)
        return -1;
    if (config_get_string (keyFile, "download", "secret_key_file", "", &config->secretKeyFile) != 0)
        return -1;
    if (config_get_string (keyFile, "auth", "default_profile", "", &config->defaultAuthProfile) != 0)
        return -1;
    if (config_get_string (keyFile, "proxy", "default_profile", "", &config->defaultProxyProfile) != 0)
        return -1;
    if (config_get_string (keyFile, "proxy", "url", "", &config->proxyUrl) != 0)
        return -1;
    if (config_get_string (keyFile, "proxy", "username", "", &config->proxyUsername) != 0)
        return -1;
    if (config_get_string (keyFile, "proxy", "password", "", &config->proxyPassword) != 0)
        return -1;
    if (config_get_string (keyFile, "baidu", "transfer_directory", "/downloader",
                           &config->baiduTransferDirectory) != 0)
        return -1;
    if (config_get_int32 (keyFile, "baidu", "max_parallel_tasks", 2, &config->baiduMaxParallelTasks) != 0)
        return -1;
    if (config_get_int32 (keyFile, "baidu", "max_files", 2000, &config->baiduMaxFiles) != 0)
        return -1;
    if (config_get_int32 (keyFile, "baidu", "sign_cache_seconds", 3600, &config->baiduSignCacheSeconds) != 0)
        return -1;
    if (config_get_string (keyFile, "baidu", "api_base", "https://pan.baidu.com", &config->baiduApiBase) != 0)
        return -1;
    if (config_get_string (keyFile, "baidu", "pcs_base", "https://pcs.baidu.com", &config->baiduPcsBase) != 0)
        return -1;

    if ((config->webUsername[0] != '\0') != (config->webPassword[0] != '\0')) {
        /* Enabling auth with only one half would silently leave the API open. */
        errno = EINVAL;
        return -1;
    }
    if (config->maxConcurrentTasks <= 0 || config->maxRetries < 0 || config->connectTimeout < 0 ||
        config->transferTimeout < 0 || config->webListen[0] == '\0' || config->webPort < 1 || config->webPort > 65535 ||
        config->baiduTransferDirectory[0] != '/' || config->baiduMaxParallelTasks <= 0 ||
        config->baiduMaxFiles <= 0 || config->baiduSignCacheSeconds < 0 || config->baiduApiBase[0] == '\0' ||
        config->baiduPcsBase[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

Config* config_load (int argc, char* argv[])
{
    CKeyFile* keyFile;
    Config*   config;
    char*     path;
    size_t    errorOffset = 0;

    if (argc < 0 || argv == NULL) {
        errno = EINVAL;
        return NULL;
    }
    path = config_find_path (argc, argv);
    if (path == NULL)
        return NULL;

    keyFile = c_key_file_new ();
    if (keyFile == NULL) {
        c_free (path);
        return NULL;
    }
    if (c_key_file_load_from_file (keyFile, path, &errorOffset) != 0) {
        c_key_file_free (keyFile);
        c_free (path);
        errno = EINVAL;
        return NULL;
    }
    config = (Config*)c_malloc0 (sizeof (*config));
    if (config == NULL) {
        c_key_file_free (keyFile);
        c_free (path);
        return NULL;
    }
    config->path = path;
    if (config_load_values (config, keyFile) != 0) {
        c_key_file_free (keyFile);
        config_free (config);
        return NULL;
    }
    (void)errorOffset;
    c_key_file_free (keyFile);
    return config;
}

void config_free (Config* config)
{
    if (config == NULL)
        return;
    c_free (config->path);
    c_free (config->ipcSocket);
    c_free (config->logLevel);
    c_free (config->downloadDir);
    c_free (config->database);
    c_free (config->tempSuffix);
    c_free (config->overwritePolicy);
    c_free (config->webListen);
    c_free (config->secretKeyFile);
    c_free (config->defaultAuthProfile);
    c_free (config->defaultProxyProfile);
    c_free (config->proxyUrl);
    c_free (config->proxyUsername);
    c_free (config->proxyPassword);
    c_free (config->webUsername);
    c_free (config->webPassword);
    c_free (config->baiduTransferDirectory);
    c_free (config->baiduApiBase);
    c_free (config->baiduPcsBase);
    c_free (config);
}

const char* config_path (const Config* config) { return config == NULL ? NULL : config->path; }
const char* config_ipc_socket (const Config* config) { return config == NULL ? NULL : config->ipcSocket; }
const char* config_log_level (const Config* config) { return config == NULL ? NULL : config->logLevel; }
const char* config_download_dir (const Config* config) { return config == NULL ? NULL : config->downloadDir; }
const char* config_database (const Config* config) { return config == NULL ? NULL : config->database; }
const char* config_temp_suffix (const Config* config) { return config == NULL ? NULL : config->tempSuffix; }
const char* config_overwrite_policy (const Config* config) { return config == NULL ? NULL : config->overwritePolicy; }
bool config_web_enabled (const Config* config) { return config != NULL && config->webEnabled; }
const char* config_web_listen (const Config* config) { return config == NULL ? NULL : config->webListen; }
int32_t config_web_port (const Config* config) { return config == NULL ? 0 : config->webPort; }
int32_t config_max_concurrent_tasks (const Config* config) { return config == NULL ? 0 : config->maxConcurrentTasks; }
int32_t config_max_retries (const Config* config) { return config == NULL ? 0 : config->maxRetries; }
int32_t config_connect_timeout (const Config* config) { return config == NULL ? 0 : config->connectTimeout; }
int32_t config_transfer_timeout (const Config* config) { return config == NULL ? 0 : config->transferTimeout; }
const char* config_secret_key_file (const Config* config) { return config == NULL ? NULL : config->secretKeyFile; }
const char* config_default_auth_profile (const Config* config) { return config == NULL ? NULL : config->defaultAuthProfile; }
const char* config_default_proxy_profile (const Config* config) { return config == NULL ? NULL : config->defaultProxyProfile; }
const char* config_proxy_url (const Config* config) { return config == NULL ? NULL : config->proxyUrl; }
const char* config_proxy_username (const Config* config) { return config == NULL ? NULL : config->proxyUsername; }
const char* config_proxy_password (const Config* config) { return config == NULL ? NULL : config->proxyPassword; }
const char* config_web_username (const Config* config) { return config == NULL ? NULL : config->webUsername; }
const char* config_web_password (const Config* config) { return config == NULL ? NULL : config->webPassword; }
int32_t config_web_session_seconds (const Config* config) { return config == NULL ? 0 : config->webSessionSeconds; }
const char* config_baidu_transfer_directory (const Config* config)
{
    return config == NULL ? NULL : config->baiduTransferDirectory;
}
int32_t config_baidu_max_parallel_tasks (const Config* config) { return config == NULL ? 0 : config->baiduMaxParallelTasks; }
int32_t config_baidu_max_files (const Config* config) { return config == NULL ? 0 : config->baiduMaxFiles; }
int32_t config_baidu_sign_cache_seconds (const Config* config) { return config == NULL ? 0 : config->baiduSignCacheSeconds; }
const char* config_baidu_api_base (const Config* config) { return config == NULL ? NULL : config->baiduApiBase; }
const char* config_baidu_pcs_base (const Config* config) { return config == NULL ? NULL : config->baiduPcsBase; }
