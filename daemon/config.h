#ifndef DOWNLOADER_CONFIG_H
#define DOWNLOADER_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Config Config;

Config*     config_load (int argc, char* argv[]);
void        config_free (Config* config);
const char* config_path (const Config* config);
const char* config_ipc_socket (const Config* config);
const char* config_log_level (const Config* config);
const char* config_download_dir (const Config* config);
const char* config_database (const Config* config);
const char* config_temp_suffix (const Config* config);
const char* config_overwrite_policy (const Config* config);
bool        config_web_enabled (const Config* config);
const char* config_web_listen (const Config* config);
int32_t     config_web_port (const Config* config);
int32_t     config_max_concurrent_tasks (const Config* config);
int32_t     config_max_retries (const Config* config);
int32_t     config_connect_timeout (const Config* config);
int32_t     config_transfer_timeout (const Config* config);
const char* config_secret_key_file (const Config* config);
const char* config_default_auth_profile (const Config* config);
const char* config_default_proxy_profile (const Config* config);
const char* config_proxy_url (const Config* config);
const char* config_proxy_username (const Config* config);
const char* config_proxy_password (const Config* config);
const char* config_web_username (const Config* config);
const char* config_web_password (const Config* config);
int32_t     config_web_session_seconds (const Config* config);
const char* config_baidu_transfer_directory (const Config* config);
int32_t     config_baidu_max_parallel_tasks (const Config* config);
int32_t     config_baidu_max_files (const Config* config);
int32_t     config_baidu_sign_cache_seconds (const Config* config);
const char* config_baidu_api_base (const Config* config);
const char* config_baidu_pcs_base (const Config* config);

#endif
