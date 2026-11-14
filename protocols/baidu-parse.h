#ifndef DOWNLOADER_BAIDU_PARSE_H
#define DOWNLOADER_BAIDU_PARSE_H

#include "task.h"

#include <clib/macro.h>

#include <stdbool.h>
#include <stddef.h>

#define BAIDU_SURL_SIZE 64U

C_BEGIN_EXTERN_C

typedef struct
{
    char pwd[16];
    char path[DOWNLOAD_TASK_PATH_SIZE];
} BaiduTaskOptions;

typedef struct
{
    char bdstoken[128];
    char uk[32];
    char shareUk[32];
    char shareid[32];
} BaiduShareTokens;

/* Extracts the surl from a Baidu share URL; false when the URL is not a share. */
bool baidu_share_url (const char* url, char* surl, size_t capacity);
/* Extracts ?pwd= from the URL; returns -1/ENOENT when absent. */
int  baidu_share_url_pwd (const char* url, char* pwd, size_t capacity);
/* Decodes baidu:///absolute/path; false for a directory root or parent refs. */
bool baidu_own_url (const char* url, char* path, size_t capacity);
/* Parses extra.baidu.pwd and extra.baidu.path; absent values stay empty. */
int  baidu_task_options (const char* extraJson, BaiduTaskOptions* options);
/* Returns 0 when the Cookie header contains non-empty BDUSS and STOKEN. */
int  baidu_cookie_login (const char* cookie);
/* Extracts share page tokens; missing page flags set ENOENT/ESTALE/EPROTO. */
int  baidu_share_page_tokens (const char* html, BaiduShareTokens* tokens);

C_END_EXTERN_C

#endif
