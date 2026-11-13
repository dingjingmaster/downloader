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
int32_t     config_max_concurrent_tasks (const Config* config);
int32_t     config_max_retries (const Config* config);
int32_t     config_connect_timeout (const Config* config);
int32_t     config_transfer_timeout (const Config* config);

#endif
