# 百度网盘下载 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 downloader 中新增原生 `baidu` 协议，支持用 Cookie 登录后下载自己网盘的文件/目录，以及通过转存下载百度分享链接中的文件/目录。

**Architecture:** 协议层负责百度网页 API、签名、转存和单文件下载；核心新增通用 `ProtocolOps.expand` 钩子，把一个目录/分享展开成 N 条单文件任务，并用 `group_uuid`/`group_name` 字段分组。Web/IPC 共用展开路径，任务池继续按普通单文件任务调度。

**Tech Stack:** ISO C99、CMake、clib（JSON/URI/线程/校验和/路径）、libcurl、SQLite、Python 3 本地 mock 测试。

**Spec:** `docs/superpowers/specs/2026-11-14-baidu-netdisk-design.md`

## Global Constraints

- C99，4 空格缩进，函数名与左括号之间保留空格，例如 `task_start (task)`。
- 优先使用 `clib` 已有能力，不重复实现；下载器自己的类型/函数不加 `c_` 前缀。
- 协议实现不得直接操作 SQLite、IPC 或全局任务状态。
- 任务先写临时文件，完成校验并 `fsync` 后再由核心原子提交。
- 百度 Cookie 只存加密凭据库，不写入日志、任务 JSON、`extra_json` 或列表 API。
- 测试不得访问真实百度：Python 测试通过 `[baidu] api_base`/`pcs_base` 指向本地 mock。
- 固定默认值：`transfer_directory = /downloader`、`max_parallel_tasks = 2`、`max_files = 2000`、`sign_cache_seconds = 3600`、创建响应最多返回 50 条任务、数据库版本 4。
- 每完成一个任务执行编译、相关测试和 `git status`；只暂存本任务涉及的文件。

## Review Focus

1. **Cookie 泄漏**：普通 HTTP/SMB 任务误选 `cookie` 凭据时，期望明确失败且不发送 Cookie。测试放在 Task 3（`tests/test-http-auth.py`）。
2. **百度页面/签名结构变化**：`/disk/home` 或分享页缺少预期字段时，期望可诊断的 `baidu_sign_unavailable`/`baidu_share_*` 错误，而不是静默重试或空任务。测试放在 Task 4 与 Task 7。
3. **dlink 过期/403**：下载中途 dlink 失效时，期望重新解析一次后继续，连续失败则终止而非无限循环。测试放在 Task 5 与 Task 7。
4. **超大分享与响应上限**：超过 `max_files` 或任务数超过 50 时，期望返回 `baidu_share_too_large` / `truncated=true`，数据库保持一致，Web 响应不超过 1 MB。测试放在 Task 2、Task 8、Task 9。
5. **本地已有文件与部分转存**：期望跳过已存在目标、补齐缺失转存项、同名不同内容报冲突，绝不覆盖或静默使用错误文件。测试放在 Task 2、Task 6、Task 8。

---

### Task 1: 数据库分组字段与批量接口

**Files:**
- Modify: `daemon/task.h`
- Modify: `daemon/database.h`
- Modify: `daemon/database.c`
- Test: `tests/test-database.c`

**Interfaces:**
- Consumes: 现有 `DownloadTask`、`database_open/insert/load/list/update`。
- Produces:
  - `DOWNLOAD_TASK_GROUP_UUID_SIZE`（64）、`DOWNLOAD_TASK_GROUP_NAME_SIZE`（256）
  - `DownloadTask.groupUuid`、`DownloadTask.groupName`
  - `DownloadTaskGroupSummary`
  - `int database_insert_many (Database* database, const DownloadTask* tasks, size_t count);`
  - `int database_list_group (Database* database, const char* groupUuid, DownloadTask** tasks, size_t* count);`
  - `int database_group_summary (Database* database, const char* groupUuid, DownloadTaskGroupSummary* summary);`

- [ ] **Step 1: 写失败测试**

在 `tests/test-database.c` 增加 `test_group_fields`，并在 `main` 中调用：

```c
static void test_group_fields (const char* path)
{
    Database* database;
    DownloadTask tasks[3];
    DownloadTask* listed = NULL;
    size_t count = 0;
    DownloadTaskGroupSummary summary;

    assert (database_open (path, &database) == 0);
    for (size_t index = 0; index < 3; index++) {
        tasks[index] = make_task (index == 0 ? "group-a" : index == 1 ? "group-b" : "group-c",
                                  index == 0 ? DOWNLOAD_TASK_STATE_COMPLETED
                                             : index == 1 ? DOWNLOAD_TASK_STATE_FAILED
                                                          : DOWNLOAD_TASK_STATE_QUEUED);
        snprintf (tasks[index].groupUuid, sizeof (tasks[index].groupUuid), "group-1");
        snprintf (tasks[index].groupName, sizeof (tasks[index].groupName), "资料");
        tasks[index].totalBytes = 100;
        tasks[index].downloadedBytes = index == 0 ? 100 : (index == 1 ? 25 : 10);
    }
    assert (database_insert_many (database, tasks, 3) == 0);
    assert (database_list_group (database, "group-1", &listed, &count) == 0);
    assert (count == 3);
    assert (strcmp (listed[0].groupUuid, "group-1") == 0);
    assert (strcmp (listed[0].groupName, "资料") == 0);
    c_free (listed);
    listed = NULL;
    assert (database_group_summary (database, "group-1", &summary) == 0);
    assert (summary.taskCount == 3);
    assert (summary.completedCount == 1);
    assert (summary.failedCount == 1);
    assert (summary.totalBytes == 300);
    assert (summary.downloadedBytes == 135);
    /* 批量插入必须原子：重复 uuid 时整批回滚。 */
    tasks[2].uuid[0] = '\0';
    snprintf (tasks[2].uuid, sizeof (tasks[2].uuid), "%s", tasks[0].uuid);
    assert (database_insert_many (database, tasks, 3) != 0);
    assert (database_list_group (database, "group-1", &listed, &count) == 0);
    assert (count == 3);
    c_free (listed);
    database_close (database);
}
```

同时把 `test_checkpoint_persistence` 里的 `assert (version == 3);` 改为 `assert (version == 4);`。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build --target test_database && ./build/test_database`
Expected: 编译失败，`database_insert_many` / `DownloadTaskGroupSummary` 未定义。

- [ ] **Step 3: 实现结构与迁移**

`daemon/task.h`：

```c
#define DOWNLOAD_TASK_GROUP_UUID_SIZE  64U
#define DOWNLOAD_TASK_GROUP_NAME_SIZE  256U

typedef struct
{
    char     groupUuid[DOWNLOAD_TASK_GROUP_UUID_SIZE];
    char     groupName[DOWNLOAD_TASK_GROUP_NAME_SIZE];
    size_t   taskCount;
    size_t   completedCount;
    size_t   failedCount;
    uint64_t totalBytes;
    uint64_t downloadedBytes;
} DownloadTaskGroupSummary;
```

`DownloadTask` 增加 `groupUuid`、`groupName` 两个字段。

`daemon/database.c`：

- `DOWNLOADER_DATABASE_VERSION` 改为 4。
- `gcDatabaseSchema` 的 `CREATE TABLE` 增加 `group_uuid TEXT NOT NULL DEFAULT ''`、`group_name TEXT NOT NULL DEFAULT ''`。
- 迁移 `version < 4`：用现有 `database_column_exists()` 检查并 `ALTER TABLE` 增加两列，然后执行
  `CREATE INDEX IF NOT EXISTS idx_download_tasks_group ON download_tasks(group_uuid);`。
- `database_read_task` 在 `gap_count` 后追加索引 28/29 读取两个字段；所有 `SELECT` 列表追加
  `group_uuid,group_name`。
- `database_bind_task` 在 `gap_count` 后绑定两个字段；`INSERT`/`UPDATE` 列与占位符同步增加。
- `database_insert_many`：`BEGIN IMMEDIATE`，复用一条 prepared statement 逐条绑定/step，全部成功后
  `COMMIT`；任一步失败 `ROLLBACK` 并返回 -1。`count == 0` 时直接返回 0。
- `database_list_group`：`WHERE group_uuid=? ORDER BY id ASC`，完整读取 `DownloadTask`，调用方用
  `c_free()` 释放。
- `database_group_summary`：一条聚合 SQL：

```sql
SELECT group_name, COUNT(*),
       SUM(state='completed'), SUM(state='failed'),
       SUM(total_bytes), SUM(downloaded_bytes)
FROM download_tasks WHERE group_uuid=?;
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build build --target test_database && ./build/test_database`
Expected: 退出码 0，无 assert 失败。

- [ ] **Step 5: 提交**

```bash
git add daemon/task.h daemon/database.h daemon/database.c tests/test-database.c
git commit -m "feat: add task group fields and batch database writes"
```

---

### Task 2: 核心展开接口与批量创建

**Files:**
- Modify: `daemon/protocol.h`
- Modify: `daemon/task-manager.h`
- Modify: `daemon/task-manager.c`
- Create: `tests/test-task-create.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 的 `database_insert_many`、分组字段。
- Produces:
  - `ProtocolItem`、`ProtocolError`、`ProtocolOps.expand`
  - `TaskCreateResult`
  - `int task_manager_create_expanded (TaskManager* manager, const char* resourceUrl, const char* fileName, const char* targetDirectory, const char* extraJson, TaskCreateResult* result);`

- [ ] **Step 1: 写失败测试**

创建 `tests/test-task-create.c`。测试夹具注册两个仅实现 `canHandle`/`expand` 的协议：
`https://expand.test/...` 走展开，`https://plain.test/...` 无展开。

```c
#define _GNU_SOURCE
#include "credential-store.h"
#include "database.h"
#include "protocol.h"
#include "task-manager.h"
#include <clib/memory.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t gExpandCount = 60;
static bool   gExpandError = false;
static char   gLastExtra[DOWNLOAD_TASK_EXTRA_JSON_SIZE];

static bool expand_can_handle (const char* url, const char* protocol)
{
    return strcmp (protocol, "https") == 0 && strstr (url, "expand.test") != NULL;
}

static bool plain_can_handle (const char* url, const char* protocol)
{
    return strcmp (protocol, "https") == 0 && strstr (url, "plain.test") != NULL;
}

static int fixture_expand (const DownloadTask* request, ProtocolItem** items, size_t* count,
                           char* groupName, size_t groupNameCapacity, ProtocolError* error)
{
    (void)request;
    if (gExpandError) {
        snprintf (error->code, sizeof (error->code), "baidu_password_invalid");
        snprintf (error->message, sizeof (error->message), "提取码错误");
        return -1;
    }
    *items = c_malloc0 (gExpandCount * sizeof (**items));
    assert (*items != NULL);
    *count = gExpandCount;
    snprintf (groupName, groupNameCapacity, "资料");
    for (size_t index = 0; index < gExpandCount; index++) {
        snprintf ((*items)[index].resourceUrl, sizeof ((*items)[index].resourceUrl),
                  "https://expand.test/file-%zu", index);
        snprintf ((*items)[index].fileName, sizeof ((*items)[index].fileName), "file-%zu.bin", index);
        snprintf ((*items)[index].targetSubdirectory, sizeof ((*items)[index].targetSubdirectory),
                  "资料/2024");
        snprintf ((*items)[index].extraJson, sizeof ((*items)[index].extraJson),
                  "{\"expandtest\":{\"index\":%zu}}", index);
        (*items)[index].totalBytes = 100 + index;
        snprintf ((*items)[index].checksumType, sizeof ((*items)[index].checksumType), "md5");
        snprintf ((*items)[index].checksumValue, sizeof ((*items)[index].checksumValue), "abc");
    }
    return 0;
}
```

测试函数：
- `test_plain_single`：`https://plain.test/a.bin` → `taskCount == 1`、`returnedCount == 1`、`groupUuid[0] == '\0'`。
- `test_expand_group_truncation`：60 条 → `taskCount == 60`、`returnedCount == 50`、`truncated == true`；
  数据库 `database_list_group` 返回 60 条，`targetDirectory` 以 `/资料/2024` 结尾，`totalBytes`
  与 `checksumValue` 已写入。
- `test_auth_profile_copied`：请求 `extra` 为 `{"auth_profile":"acct"}`，展开条目为
  `{"expandtest":{}}`；加载创建后的任务，`extraJson` 同时包含 `"auth_profile":"acct"` 和
  `"expandtest"`。
- `test_skip_existing`：预先创建 `<dir>/资料/2024/file-0.bin`，再创建；`skippedCount == 1`，
  `taskCount == 59`，数据库无 `file-0.bin` 任务。
- `test_all_skipped`：`gExpandCount = 1` 且目标文件已存在 → 返回 -1、
  `errorCode == "target_files_exist"`，数据库任务数不增加。
- `test_expand_error`：`gExpandError = true` → 返回 -1，`errorCode == "baidu_password_invalid"`，
  `message == "提取码错误"`，数据库任务数不增加。

测试环境：`mkdtemp` 临时目录、`database_open`、`credential_store_open`（保存一个 basic 配置
`acct/u/s`）、`task_manager_new`、`task_manager_set_request_policy`、`task_manager_set_download_directory`。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build --target test_task_create && ./build/test_task_create`
Expected: 编译失败（`task_manager_create_expanded` 等未定义）。

- [ ] **Step 3: 实现协议展开类型**

`daemon/protocol.h` 在 `ProtocolOps` 末尾（`retryable` 之后）增加：

```c
typedef struct
{
    char     resourceUrl[DOWNLOAD_TASK_URL_SIZE];
    char     fileName[DOWNLOAD_TASK_FILE_NAME_SIZE];
    char     targetSubdirectory[DOWNLOAD_TASK_PATH_SIZE];
    char     extraJson[DOWNLOAD_TASK_EXTRA_JSON_SIZE];
    uint64_t totalBytes;
    char     checksumType[DOWNLOAD_TASK_PROTOCOL_SIZE];
    char     checksumValue[DOWNLOAD_TASK_CHECKSUM_SIZE];
} ProtocolItem;

typedef struct
{
    char code[64];
    char message[DOWNLOAD_TASK_ERROR_SIZE];
} ProtocolError;

/* struct ProtocolOps 末尾新增 */
int (*expand) (const DownloadTask* request, ProtocolItem** items, size_t* count,
               char* groupName, size_t groupNameCapacity, ProtocolError* error);
```

放在末尾可保持 `aria.cpp` 的位置初始化不变。

- [ ] **Step 4: 实现 `task_manager_create_expanded`**

`daemon/task-manager.h` 增加 `TaskCreateResult` 与函数声明（字段见 spec）。`daemon/task-manager.c`：

1. 提取现有 URL/fileName 推断逻辑为 `static int task_manager_infer_name (...)`，供普通创建与展开创建共用。
2. 构造临时 `DownloadTask request`（uuid 为空、extra 为请求值），调用
   `request_context_resolve()` 解析凭据；失败时按 errno 写
   `auth_profile_unavailable`/`invalid_url`/`unsupported_protocol`。
3. `ops = protocol_find(url, detected)`；若 `ops == NULL` 或 `ops->expand` 为 NULL，则构造一条
   `ProtocolItem`（resourceUrl/url、fileName、空子目录、请求 extra），保持“未注册协议也能建任务、
   运行时再失败”的现有行为。
4. `expand` 失败：把 `ProtocolError` 复制到结果并返回 -1。
5. 核心把请求 `extraJson` 顶层的 `auth_profile`、`proxy_profile` 合并进每条条目：
   用 clib JSON 解析对象，缺哪个补哪个，再序列化回条目的 `extraJson`。
6. 决定分组：`count > 1 || groupName[0] != '\0'` 时生成
   `group-<now>-<pid>-<sequence>`；否则分组字段为空。
7. 逐条构造 `DownloadTask`：拼接 `targetDirectory = 下载根 + "/" + targetSubdirectory`，
   `filesystem_prepare()`，`lstat(targetPath) == 0` 时 `skippedCount++` 并跳过。
8. 若所有条目都被跳过，返回 -1 并把 `errorCode` 设为 `"target_files_exist"`；
   否则 `database_insert_many()` 一次事务写入全部未跳过任务，失败返回 -1。
9. 只把前 `DOWNLOADER_CREATE_RESULT_TASKS`（50）条复制到 `result->tasks`，
   `returnedCount`/`truncated` 正确设置。

- [ ] **Step 5: 注册测试目标**

在 `CMakeLists.txt` 的测试区添加 `test_task_create`，源文件与 `test_task_manager` 相同，
额外加入 `tests/test-task-create.c`；链接 `clib::static Threads::Threads SQLite3::SQLite3 OpenSSL::Crypto`；
`add_test(NAME downloader_task_create COMMAND test_task_create)`。

- [ ] **Step 6: 运行测试确认通过**

Run: `cmake -S . -B build && cmake --build build --target test_task_create && ./build/test_task_create`
Expected: 退出码 0。

- [ ] **Step 7: 提交**

```bash
git add daemon/protocol.h daemon/task-manager.h daemon/task-manager.c tests/test-task-create.c CMakeLists.txt
git commit -m "feat: add protocol expansion and grouped task creation"
```

---

### Task 3: COOKIE 凭据与适配器隔离

**Files:**
- Modify: `daemon/credential-store.h`
- Modify: `daemon/credential-store.c`
- Modify: `daemon/request-context.c`
- Modify: `protocols/http.c`
- Test: `tests/test-credential-store.c`
- Test: `tests/test-request-context.c`
- Test: `tests/test-http-auth.py`

**Interfaces:**
- Produces: `DOWNLOADER_CREDENTIAL_COOKIE`；`credential_kind_name()` 返回 `"cookie"`；
  `credential_kind_parse()` 接受 `"cookie"`；`baidu://` 在请求上下文里匹配
  `pan.baidu.com` 站点配置；HTTP 适配器拒绝 COOKIE 凭据。

- [ ] **Step 1: 写失败测试**

`tests/test-credential-store.c` 增加：

```c
assert (strcmp (credential_kind_name (DOWNLOADER_CREDENTIAL_COOKIE), "cookie") == 0);
DOWNLOADER_CREDENTIAL_KIND kind;
assert (credential_kind_parse ("cookie", &kind) == 0 && kind == DOWNLOADER_CREDENTIAL_COOKIE);
/* 保存/加载一个 username 为空、secret 为 "BDUSS=x; STOKEN=y" 的 cookie 配置并断言往返一致 */
```

`tests/test-request-context.c` 增加：
- 保存 cookie 配置（空 username），任务 `baidu:///%E8%B5%84%E6%96%99/a.bin` 带
  `extra.auth_profile`，`request_context_resolve` 成功且 kind 为 COOKIE。
- 设置站点配置 `pan.baidu.com -> baidu-acct`，同一任务不写 `extra.auth_profile` 也能解析到该配置。

`tests/test-http-auth.py` 增加场景：创建 cookie 类型认证配置，创建普通 HTTP 任务并引用它，
启动后任务应为 `failed`，错误信息小写后包含 `not supported`，本地 mock 服务器记录的请求头中
不得出现 `Cookie: BDUSS=`。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build --target test_credential_store test_request_context && ./build/test_credential_store && ./build/test_request_context`
Expected: 编译失败（枚举值不存在）。
Run: `python3 -B tests/test-http-auth.py ./build/downloader`
Expected: 新场景失败或无法创建 cookie 配置。

- [ ] **Step 3: 实现**

- `credential-store.h` 枚举增加 `DOWNLOADER_CREDENTIAL_COOKIE`。
- `credential_kind_name` 增加 `"cookie"`；`credential_kind_parse` 增加分支。
- `request_auth_valid`：`COOKIE` 只要求 `secret[0] != '\0'`，不要求 username。
- `request_host`：解析出 scheme 后，若 scheme 大小写不敏感等于 `baidu`，直接写
  `pan.baidu.com` 并返回成功（不依赖空 host）。
- `protocols/http.c` 的认证分支最前面增加：

```c
if (kind == DOWNLOADER_CREDENTIAL_COOKIE) {
    errno = ENOTSUP;
    return -1;
}
```

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build build --target test_credential_store test_request_context && ./build/test_credential_store && ./build/test_request_context`
Run: `python3 -B tests/test-http-auth.py ./build/downloader`
Expected: 均通过。

- [ ] **Step 5: 提交**

```bash
git add daemon/credential-store.h daemon/credential-store.c daemon/request-context.c protocols/http.c tests/test-credential-store.c tests/test-request-context.c tests/test-http-auth.py
git commit -m "feat: add cookie credentials and keep them out of http adapters"
```

---

### Task 4: 百度纯函数模块（签名、页面、URL、参数）

**Files:**
- Create: `protocols/baidu-sign.h`
- Create: `protocols/baidu-sign.c`
- Create: `protocols/baidu-parse.h`
- Create: `protocols/baidu-parse.c`
- Create: `tests/test-baidu.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:

```c
/* baidu-sign.h */
int baidu_sign2 (const char* sign3, const char* sign1, unsigned char* output, size_t capacity,
                 size_t* outputSize);
int baidu_compute_sign (const char* sign3, const char* sign1, char* output, size_t capacity);
int baidu_page_extract_sign (const char* html, char* sign1, size_t sign1Capacity,
                             char* sign3, size_t sign3Capacity,
                             char* timestamp, size_t timestampCapacity);

/* baidu-parse.h */
#define BAIDU_SURL_SIZE 64U
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

bool baidu_share_url (const char* url, char* surl, size_t capacity);
int  baidu_share_url_pwd (const char* url, char* pwd, size_t capacity);
bool baidu_own_url (const char* url, char* path, size_t capacity);
int  baidu_task_options (const char* extraJson, BaiduTaskOptions* options);
int  baidu_cookie_login (const char* cookie);
int  baidu_share_page_tokens (const char* html, BaiduShareTokens* tokens);
```

- [ ] **Step 1: 写失败测试**

`tests/test-baidu.c`：

```c
/* 来自 BaiduPCS-Go netdisksign/sign2_test.go 的固定向量 */
char sign[128];
assert (baidu_compute_sign ("e8c7d729eea7b54551aa594f942decbe",
                            "37dbe07ade9359c1aa70807e847f768c13360ad2",
                            sign, sizeof (sign)) == 0);
assert (strcmp (sign, "8RxCbsVeSzn2UjxJAAiV9QQs/WetOj2FJUGwjsMG6SgxFMWlLS/U1Q==") == 0);
```

其余断言：
- `baidu_page_extract_sign` 从
  `xx"sign1":"AAA"yy"sign3":"BBB","timestamp":1690000000,zz` 提取三个值；缺字段返回 -1 且 errno 为 `EPROTO`。
- `baidu_share_url("https://pan.baidu.com/s/1AbCdEf", ...)` 返回 true、surl 为 `1AbCdEf`；
  `yun.baidu.com` 同理；`https://pan.baidu.com/share/init?surl=1AbCdEf` 返回 `1AbCdEf`。
- `baidu_share_url_pwd("https://pan.baidu.com/s/1AbCdEf?pwd=abcd", ...)` 返回 `abcd`。
- `baidu_own_url("baidu:///%E8%B5%84%E6%96%99/a.bin", path, ...)` → `/资料/a.bin`；
  `baidu:///../etc/passwd` 返回 false。
- `baidu_task_options("{\"baidu\":{\"pwd\":\"abcd\",\"path\":\"/资料\"}}", ...)` 得到两值；
  非法 JSON 返回 -1。
- `baidu_cookie_login("BDUSS=x; STOKEN=y")` 返回 0；缺 STOKEN 返回 -1。
- `baidu_share_page_tokens` 从
  `{"loginstate":1,"bdstoken":"tok","uk":"111","share_uk":"222","shareid":"333"}` 提取；
  页面含 `platform-non-found` 时返回 -1 且 errno == `ESTALE`；含 `error-404` 时 errno == `ENOENT`。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build --target test_baidu`
Expected: 目标不存在或编译失败。

- [ ] **Step 3: 实现**

- `baidu_sign2`：按 BaiduPCS-Go `netdisksign/sign2.go` 移植 256 字节 RC4 变体；
  输入 `sign3` 是密钥串，`sign1` 是待变换串，输出长度等于 `strlen(sign1)`。
- `baidu_compute_sign`：`baidu_sign2` 后调用 clib `c_base64_encode()`。
- `baidu_page_extract_sign`：查找 `"sign1":"`、`"sign3":"`、`"timestamp":`，读到下一个 `"`/逗号；
  任一缺失 `errno = EPROTO`。
- `baidu_share_url`：用 `c_uri_parse`，主机大小写不敏感匹配
  `pan.baidu.com`/`yun.baidu.com`/`eyun.baidu.com`，路径 `/s/<surl>` 或 `/share/init`（surl 查询参数），
  surl 长度 1..63 且只含字母数字 `_-`。
- `baidu_own_url`：scheme 为 `baidu`，用 `c_uri_unescape_string` 解码路径；必须以 `/` 开头，
  拒绝 `..` 组件和空路径。
- `baidu_task_options`：用 clib JSON 解析 `extra.baidu.pwd`/`extra.baidu.path`，做长度校验。
- `baidu_cookie_login`：按 `;` 分段，查找 `BDUSS=`、`STOKEN=` 且值非空。
- `baidu_share_page_tokens`：先检查页面标记，再用字符串/JSON 扫描提取四个字段。

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake -S . -B build && cmake --build build --target test_baidu && ./build/test_baidu`
Expected: 退出码 0。

- [ ] **Step 5: 提交**

```bash
git add protocols/baidu-sign.h protocols/baidu-sign.c protocols/baidu-parse.h protocols/baidu-parse.c tests/test-baidu.c CMakeLists.txt
git commit -m "feat: add baidu sign, url and share parsing helpers"
```

---

### Task 5: 配置、协议识别与百度单文件下载

**Files:**
- Modify: `daemon/config.h`
- Modify: `daemon/config.c`
- Modify: `config/download.ini`
- Modify: `daemon/protocol.c`
- Modify: `daemon/main.c`
- Modify: `daemon/task-manager.c`
- Create: `protocols/baidu.h`
- Create: `protocols/baidu.c`
- Test: `tests/test-config.c`
- Test: `tests/test-protocol.c`
- Create: `tests/test-baidu.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3 的 cookie 凭据；Task 4 的签名/解析函数。
- Produces:
  - `config_baidu_transfer_directory()`、`config_baidu_max_parallel_tasks()`、
    `config_baidu_max_files()`、`config_baidu_sign_cache_seconds()`、
    `config_baidu_api_base()`、`config_baidu_pcs_base()`
  - `const ProtocolOps* baidu_protocol_ops (void);`
  - `int baidu_protocol_configure (const char* transferDirectory, int maxParallelTasks, int maxFiles, int signCacheSeconds, const char* apiBase, const char* pcsBase);`
  - `protocol_detect()` 对 `baidu://` 和百度分享 URL 返回 `"baidu"`。

- [ ] **Step 1: 写失败测试**

`tests/test-protocol.c` 增加：

```c
assert (protocol_detect ("baidu:///a/b.bin", detected, sizeof (detected)) == 0);
assert (strcmp (detected, "baidu") == 0);
assert (protocol_detect ("https://pan.baidu.com/s/1AbCdEf", detected, sizeof (detected)) == 0);
assert (strcmp (detected, "baidu") == 0);
assert (protocol_detect ("https://yun.baidu.com/s/1AbCdEf", detected, sizeof (detected)) == 0);
assert (strcmp (detected, "baidu") == 0);
assert (protocol_detect ("https://pan.baidu.com/disk/home", detected, sizeof (detected)) == 0);
assert (strcmp (detected, "https") == 0);
```

`tests/test-config.c`：最小配置下断言六个默认值；再写一个显式 `[baidu]` 配置断言解析值；
非法 `max_parallel_tasks = 0` / `max_files = 0` 使 `config_load` 失败。

`tests/test-baidu.py`（第一版）：
- 启动本地 mock HTTP 服务，提供 `/disk/home`、`/api/list`、`/api/download`、`/dlink/<id>`。
- 写临时配置：

```ini
[daemon]
ipc_socket = <tmp>/ipc.sock
[download]
database = <tmp>/meta.db
download_dir = <tmp>/downloads
max_retries = 0
[web]
listen = 127.0.0.1
port = <port>
[baidu]
api_base = http://127.0.0.1:<mock>
pcs_base = http://127.0.0.1:<mock>
```

- `scenario_own_single_file`：
  1. 通过 `/api/auth-profiles` 创建 `kind=cookie`、`secret="BDUSS=x; STOKEN=y"` 的配置 `baidu-acct`。
  2. 创建任务：`url=baidu:///a.bin`（Task 6 前的单任务路径直接落在下载根目录），
     `extra={"auth_profile":"baidu-acct"}`；mock `/api/list?dir=/` 返回 `a.bin`。
  3. 启动并等待完成。
  4. 断言 `<tmp>/downloads/a.bin` 内容等于 mock 数据、大小正确。
- `scenario_resume`：mock dlink 第一次在发送一半后断开，第二次按 `Range` 返回剩余部分；
  断言任务最终完成且文件内容完整。
- `scenario_dlink_forbidden`：`/api/download` 第一次返回 `dlink`，dlink 返回 403；第二次重新解析后成功。
- `scenario_auth_missing`：不创建认证配置直接创建 `baidu:///...` 任务并启动；任务失败，
  `error_message` 包含 `baidu_auth_missing`，且 mock 未收到任何百度 API 请求。
- `scenario_sign_refresh`：`/api/download` 第一次返回 `{"errno":113}`，第二次成功；断言
  `/disk/home` 被调用两次（签名缓存被清空）。
- `scenario_parallel_limit`：配置 `max_parallel_tasks = 1`，同时启动两个单文件任务；
  mock dlink 记录并发请求数，断言最大并发为 1，两个任务最终都完成。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build --target test_config test_protocol && ./build/test_config && ./build/test_protocol`
Expected: `protocol_detect` 对百度 URL 返回失败。
Run: `python3 -B tests/test-baidu.py ./build/downloader`
Expected: 创建/下载失败。

- [ ] **Step 3: 实现配置与识别**

- `daemon/config.h/.c` 增加六个 getter；`config_load_values` 读取 `[baidu]` 六个键，
  校验 `max_parallel_tasks > 0`、`max_files > 0`、`sign_cache_seconds >= 0`、
  `api_base`/`pcs_base` 非空；`config_free` 释放新增字符串。
- `config/download.ini` 增加 spec 中的 `[baidu]` 段。
- `daemon/protocol.c` 的 `protocol_detect`：
  - scheme `baidu` → `baidu`；
  - http/https 且 host 为三个百度域名、路径匹配 `/s/` 或 `/share/init` → `baidu`；
  - 其他百度路径继续按原逻辑。
- `daemon/main.c` 在 `http` 之前注册 `baidu_protocol_ops()`，并用
  `baidu_protocol_configure()` 传入六个配置值。
- `daemon/task-manager.c` 的 `task_manager_curl_protocol` 增加 `"baidu"`，使带凭据的百度任务
  不被通用策略检查拒绝。

- [ ] **Step 4: 实现会话、签名与任务打开**

结构：

```c
typedef struct BaiduTask
{
    DownloadTask          source;
    ProtocolCallbacks     callbacks;
    void*                 userData;
    bool                  pauseRequested;
    bool                  cancelRequested;
    CURLcode              lastResult;
    long                  status;
    char                  apiPath[DOWNLOAD_TASK_PATH_SIZE];
    char                  fsid[64];
} BaiduTask;
```

实现：
- `canHandle`：`strcmp(protocol,"baidu")==0` 且 `baidu_own_url()` 或 `baidu_share_url()` 成功。
  分享 URL 在 Task 7 前由 `open` 返回 ENOTSUP，避免落到 HTTP 适配器把分享页当文件下载。
- `open`：复制任务；解析 `baidu:///` 路径；检查 `request_context_auth_kind == COOKIE` 与
  `baidu_cookie_login(secret)`；保存。分享 URL 暂调用 `onFailed(..., ENOTSUP, "baidu share support pending")` 并返回 -1。
- 内部 `baidu_http_session`：一个 easy handle，设置 `CURLOPT_COOKIEFILE ""`、
  `CURLOPT_COOKIE` 为凭据 secret、UA 为 `pan.baidu.com`、超时与代理（来自 requestContext）。
- `baidu_sign_get`：按 `c_checksum_sha256(secret)` 十六进制值查进程级缓存（`CMutex` 保护）；
  未命中则 `GET {api_base}/disk/home`，禁止跟随跳转，检查 `Location`，调用
  `baidu_page_extract_sign` + `baidu_compute_sign`；缓存 `sign_cache_seconds`。

- [ ] **Step 5: 实现 fs_id 与 dlink 解析**

- `baidu_resolve_fsid`：`GET {api_base}/api/list?dir=<父目录>&num=1000&page=<n>`，匹配
  `server_filename`，取 `fs_id`/`size`/`md5`。
- `baidu_resolve_dlink`：`POST {api_base}/api/download` 表单 `sign`、`timestamp`、`fidlist=[fsid]`；
  解析 `dlink` 数组。errno `112`/`113` 时清缓存并重试一次；`31066`/`31297` → `baidu_file_not_found`。

- [ ] **Step 6: 实现下载循环、并发限制与错误映射**

- `run`：获取并发许可（`CMutex`+`CCond` 计数器，上限 `max_parallel_tasks`），解析 dlink，
  用 `Range: bytes=<downloadedBytes>-` 写临时文件；进度回调节流；`pauseRequested` 返回 1，
  `cancelRequested` 返回 -1；403/401 或 JSON 错误时重解析一次；完成后校验大小（md5 非空才校验），
  失败时截断到 0、`onCheckpoint(durableBytes=0)`、重下一次；第二次失败调用
  `onFailed(userData, EIO, "baidu_checksum_mismatch: ...")`。
- `retryable`：curl 网络类错误/408/429/5xx/ETIMEDOUT/ECONNRESET 等返回 true；认证、签名、
  密码、文件不存在等返回 false。
- 所有失败路径先调用 `callbacks.onFailed()` 写具体中文消息，再返回 -1。

- [ ] **Step 7: 注册构建与测试**

- `CMakeLists.txt` 的 `downloader` 可执行文件加入 `protocols/baidu.c`、`protocols/baidu-sign.c`、
  `protocols/baidu-parse.c`。
- 增加 ctest：

```cmake
add_test(NAME downloader_baidu COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-baidu.py" $<TARGET_FILE:downloader>)
set_tests_properties(downloader_baidu PROPERTIES TIMEOUT 180)
```

- [ ] **Step 8: 运行测试确认通过**

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build -R 'downloader_(config|protocol|baidu)' --output-on-failure`
Expected: 全部通过，包括单文件、续传、dlink 403、签名刷新、并发上限和未配置凭据场景。

- [ ] **Step 9: 提交**

```bash
git add daemon/config.h daemon/config.c config/download.ini daemon/protocol.c daemon/main.c daemon/task-manager.c protocols/baidu.h protocols/baidu.c tests/test-config.c tests/test-protocol.c tests/test-baidu.py CMakeLists.txt
git commit -m "feat: add baidu config, detection and single-file download"
```

---

### Task 6: 自己网盘目录展开与 Web 创建接入

**Files:**
- Modify: `protocols/baidu.c`
- Modify: `daemon/web-api.c`
- Test: `tests/test-baidu.py`

**Interfaces:**
- Consumes: Task 2 的 `ProtocolOps.expand`、`task_manager_create_expanded`；Task 5 的百度 HTTP 会话。
- Produces: `baidu:///目录` 展开成 N 条任务；`POST /api/tasks` 走展开创建。

- [ ] **Step 1: 写失败测试**

`tests/test-baidu.py` 增加 `scenario_own_directory`：
- mock `/api/list?dir=/资料` 返回 `a.bin`（文件）和 `2024`（目录，`isdir=1`）；
  `/api/list?dir=/资料/2024` 返回 `b.bin`。
- 创建 `baidu:///资料`，断言响应含 `group` 且 `task_count == 2`。
- 通过 `/api/tasks?page=1&page_size=50` 找到两条任务，逐条 `start`。
- 断言本地存在 `<downloads>/资料/a.bin` 和 `<downloads>/资料/2024/b.bin`。
- 逐条任务 `pause`/`resume`，断言最终完成。

增加 `scenario_target_exists`：预先创建 `<downloads>/资料/a.bin`，再次创建 `baidu:///资料`；
断言 `response["group"]["skipped"] == 1`、`task_count == 1`。

同时更新 Task 5 的 `scenario_auth_missing`：Web 创建改为展开路径后，未配置凭据时应在创建阶段
失败，断言非 2xx 且 `error == "baidu_auth_missing"`。

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -B tests/test-baidu.py ./build/downloader`
Expected: `baidu:///资料` 创建后只有一条任务或创建失败。

- [ ] **Step 3: 实现百度目录展开**

在 `baidu.c` 增加 `static int baidu_expand (...)` 并填入 ops：
- `baidu_own_url` 成功时：
  - 列父目录找到目标条目；文件 → 一条；目录 → 递归 `baidu_list_directory()`（分页 `num=1000`，
    `fs_id` 去重，深度/总数受 `max_files` 限制）。
  - 每条 `ProtocolItem`：`fileName = server_filename`；
    `targetSubdirectory = 网盘绝对路径去掉开头 '/'`（目录内容包含根目录名）；
    `extraJson = {"baidu":{"kind":"own","fs_id":"...","size":N,"md5":"...","api_path":"..."}}`；
    `totalBytes`/`checksumType=md5`/`checksumValue` 填写。
  - 文件：`groupName` 留空；目录：`groupName = 根目录名`。
- 分享 URL 暂返回 `errno = ENOTSUP`，`ProtocolError.code = "unsupported_protocol"`（Task 7 实现）。
- 列表条目名做安全清理；含 `/`、`\`、`..`、控制字符的条目跳过并记录。

- [ ] **Step 4: Web 创建接入展开**

`daemon/web-api.c` 的 `web_api_create`：
- 改调 `task_manager_create_expanded()`。
- 实现 spec 的完整错误码 → HTTP 状态映射（400/401/403/404/409/413/502/500），
  并新增 `web_api_set_error_message()` 输出 `{"ok":false,"error":"<code>","message":"<转义文本>"}`。
- 成功时响应包含 `group`（id/name/task_count/skipped/truncated）、`tasks`（返回的 ≤50 条），
  并保留 `"task"` 为第一条；`group` 不存在时不输出。
- 释放 `TaskCreateResult.tasks`。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && python3 -B tests/test-baidu.py ./build/downloader`
Expected: 自己网盘目录展开、跳过已存在目标、暂停/恢复全部通过。

- [ ] **Step 6: 提交**

```bash
git add protocols/baidu.c daemon/web-api.c tests/test-baidu.py
git commit -m "feat: expand baidu own directories into per-file tasks"
```

---

### Task 7: 分享链接解析、校验、转存与单文件展开

**Files:**
- Modify: `protocols/baidu.c`
- Test: `tests/test-baidu.py`

**Interfaces:**
- Consumes: Task 4 的 `baidu_share_url`/`baidu_share_page_tokens`；Task 5/6 的 HTTP 会话与目录展开。
- Produces: `https://pan.baidu.com/s/<surl>` + 提取码 → 转存 → 单文件/分享展开；`ProtocolOps.expand` 支持分享。

- [ ] **Step 1: 写失败测试**

`tests/test-baidu.py` 增加 mock 路由：
- `/s/<surl>` 返回包含 `{"loginstate":1,"bdstoken":"tok","uk":"111","share_uk":"222","shareid":"333"}`
  的 HTML；
- `POST /share/verify`：`pwd` 正确返回 `{"errno":0,"randsk":"r"}` + `Set-Cookie: BDCLND=...`；
  错误返回 `{"errno":-9}`；
- `GET /share/list?...root=1` 返回根条目；
- `POST /share/transfer` 记录 `fs_id`，返回 `info:[{"path":"/downloader/<surl>/<name>"}]`；
- `POST {pcs_base}/rest/2.0/pcs/file?method=mkdir` 返回 `{"errno":0}`；
- 转存后 `/api/list` 返回该路径下的文件。

场景 `scenario_share_single_file`：
- 分享根只有一个文件 `a.bin`；创建 `https://pan.baidu.com/s/1AbCdEf`，
  `extra={"auth_profile":"baidu-acct","baidu":{"pwd":"abcd"}}`。
- 断言转存请求包含正确的 `shareid`/`from`/`fs_id`；响应 `group` 非空（单文件也建组）。
- 启动任务，断言 `<downloads>/a.bin` 内容正确。

`scenario_share_password_errors`：
- 缺少 pwd → HTTP 400，`error == "baidu_password_required"`；
- 错误 pwd（mock 返回 -9）→ HTTP 400，`error == "baidu_password_invalid"`。

`scenario_share_expired`：`/s/<surl>` 含 `platform-non-found` → HTTP 404，
`error == "baidu_share_expired"`；含 `error-404` → `baidu_share_not_found`。

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -B tests/test-baidu.py ./build/downloader`
Expected: 分享任务创建失败或返回 `unsupported_protocol`。

- [ ] **Step 3: 实现分享展开**

在 `baidu.c` 的 `baidu_expand` 中增加分享分支，按 spec 顺序实现：
1. `GET {api_base}/s/<surl>`（UA `pan.baidu.com`，Referer `{api_base}/disk/home`），
   `baidu_share_page_tokens` 检测错误并提取 tokens。
2. 需要 pwd 时 `POST /share/verify?...` 表单 `pwd/vcode/vcode_str/bdstoken`；保留 Cookie jar；
   `errno=-9` → `baidu_password_invalid`；`8001` → `baidu_challenge_required`。
3. 重新 GET 分享页刷新 token。
4. `GET /share/list?...root=1&shorturl=<surl>` 取根条目；为空 → `baidu_share_empty`。
5. 计算转存目录 `transfer_directory + "/" + surl`；`POST {pcs_base}/rest/2.0/pcs/file?method=mkdir&path=...&app_id=250528`。
6. 对根条目 `POST /share/transfer`；解析 `info[].path`，缺失时回退列出转存目录。
7. 从转存路径递归 `/api/list`，生成条目；`resourceUrl = baidu:///<转存路径>`，
   `extra.baidu.source_share = surl`；本地 `targetSubdirectory` 相对分享根，不含 `<surl>`。
8. 分享始终返回 `groupName`：单根条目使用其名称，多条目使用分享页标题或
   `百度分享-<surl>`。
9. `canHandle` 增加分享 URL。

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build build && python3 -B tests/test-baidu.py ./build/downloader`
Expected: 单文件分享、提取码错误/缺失、分享失效场景通过。

- [ ] **Step 5: 提交**

```bash
git add protocols/baidu.c tests/test-baidu.py
git commit -m "feat: transfer baidu share links and expand single-file shares"
```

---

### Task 8: 分享目录展开、路径过滤与转存补齐

**Files:**
- Modify: `protocols/baidu.c`
- Test: `tests/test-baidu.py`

**Interfaces:**
- Consumes: Task 7 的分享解析/转存。
- Produces: 分享目录递归展开、`extra.baidu.path` 过滤、部分转存补齐、同名冲突、`max_files` 限制。

- [ ] **Step 1: 写失败测试**

`scenario_share_directory`：
- 分享根含目录 `资料`，其中 `a.bin` 与 `2024/b.bin`。
- 创建后 `task_count == 2`，本地路径为 `<downloads>/资料/...`，不含 `downloader/<surl>`。

`scenario_share_path_filter`：
- `extra.baidu.path="/资料/2024"` → 只创建 `b.bin`，本地路径仍为
  `<downloads>/资料/2024/b.bin`。
- 不存在的路径 → `baidu_share_path_not_found`。

`scenario_share_partial_transfer`：
- 转存目录已存在 `资料`，mock `/api/list` 返回它；再次创建时：
  - 已存在且 size/md5 一致 → 不调用 `/share/transfer`；
  - 缺失一个根条目 → 只为缺失 `fs_id` 调用 transfer；
  - 同名但 md5 不同 → `baidu_transfer_conflict`（HTTP 409）。

`scenario_share_limits`：
- 超过 `max_files = 3` → HTTP 413、`baidu_share_too_large`；
- 分享根为空 → `baidu_share_empty`。

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -B tests/test-baidu.py ./build/downloader`
Expected: 目录分享只创建一条任务或路径错误。

- [ ] **Step 3: 实现**

- 转存前先 `GET /api/list?dir=<转存目录>`（非递归），按 `server_filename` 对比根条目：
  - 文件：存在且 `size`/`md5` 一致 → 视为已转存；目录：名称存在即视为已转存；
  - 同名但文件内容不同 → `baidu_transfer_conflict`；
  - 缺失 `fs_id` 才 transfer；解析 `info[]` 的 `-30`/`errno=4` 为已存在。
- 递归列出转存后的条目，按 `extra.baidu.path` 过滤：
  - 文件 → 单条；
  - 目录 → 该目录子树；
  - 路径不存在 → `baidu_share_path_not_found`。
- 总文件数在展开过程中累计，超过 `max_files` 立即返回 `baidu_share_too_large`。
- 本地 `targetSubdirectory` 始终使用分享根开始的路径，过滤不改变层级。
- 已知限制：转存复用只对比转存目录根层；已存在目录内部的缺失文件不自动补齐，
  需要完整重建时由用户删除云端转存目录后重新创建。
- 错误通过 `ProtocolError` 返回，消息包含百度 errno（不含 Cookie）。

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build build && python3 -B tests/test-baidu.py ./build/downloader`
Expected: 所有分享场景通过。

- [ ] **Step 5: 提交**

```bash
git add protocols/baidu.c tests/test-baidu.py
git commit -m "feat: expand baidu share directories with transfer reuse"
```

---

### Task 9: Web/API 分组展示、整组操作与错误映射

**Files:**
- Modify: `daemon/task-manager.h`
- Modify: `daemon/task-manager.c`
- Modify: `daemon/web-api.c`
- Test: `tests/test-web-api.c`
- Test: `tests/test-baidu.py`

**Interfaces:**
- Consumes: Task 1 的 `database_list_group`/`database_group_summary`；Task 2 的 `TaskCreateResult`。
- Produces:
  - `DownloadGroupAction`（START/PAUSE/RESUME/DELETE）
  - `int task_manager_group_action (TaskManager* manager, const char* groupUuid, DownloadGroupAction action, size_t* affected);`
  - `int task_manager_group_summary (TaskManager* manager, const char* groupUuid, DownloadTaskGroupSummary* summary);`
  - API：创建响应 `group/tasks/task/skipped/truncated`；列表任务含 `group_id`/`group_name`；
    列表响应含 `groups`；`POST/DELETE /api/groups/<uuid>/<action>`。

- [ ] **Step 1: 写失败测试**

`tests/test-web-api.c` 增加一个仅实现 `expand` 的 fixture（`canHandle` 只匹配
`https://expand.test/...`，返回 3 条任务，组名 `资料`），断言：
- `POST /api/tasks` → 201；body 含 `"group"`、`"task_count":3`、`"tasks"`、`"task"`。
- `GET /api/tasks?page=1&page_size=20` → body 含 `"group_id"`、`"group_name"`、`"groups"`、
  `"completed"`。
- `POST /api/groups/<uuid>/pause` → 200；任务状态变为 paused（对 queued 任务直接生效）。
- `POST /api/groups/<uuid>/resume` → 200；任务回到 queued。
- `DELETE /api/groups/<uuid>` → 200；列表里该组任务消失。
- fixture 返回 `baidu_password_invalid` → 400 且 body 含该错误码；
  `baidu_share_too_large` → 413；`baidu_transfer_conflict` → 409。

`tests/test-baidu.py` 增加 `scenario_group_actions`：对分享目录组调用
`/api/groups/<id>/start|pause|resume|delete`，断言所有子任务状态同步变化；
删除整组后本地临时文件被清理、云端转存请求不再发生。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build --target test_web_api && ./build/test_web_api`
Expected: 新断言失败（无 `group` 字段/路由）。

- [ ] **Step 3: 实现任务管理器分组动作**

- `task_manager_group_summary`：加锁后调用 `database_group_summary`。
- `task_manager_group_action`：
  1. 加锁、`database_list_group` 读取 uuid 列表并复制，解锁。
  2. 逐条调用 `task_manager_start/pause/resume/delete`；统计成功数。
  3. `affected` 返回成功数；删除会等待活动任务结束。

- [ ] **Step 4: 实现 Web API**

- `web_api_append_task` 增加 `group_id`、`group_name`。
- `web_api_create` 增加分组字段与 `tasks` 数组（错误状态映射已在 Task 6 完成）；
  失败响应继续使用 `web_api_set_error_message`。
- `web_api_list`：收集当前页任务的 `group_id`，逐个 `task_manager_group_summary`，
  输出 `groups` 数组。
- `web_api_handle` 在 `/api/tasks` 之前处理 `/api/groups/`：
  - `POST /api/groups/<uuid>/start|pause|resume`
  - `DELETE /api/groups/<uuid>`
  - 未知 action → 404；方法不符 → 405。

- [ ] **Step 5: 运行测试确认通过**

Run: `cmake --build build && ctest --test-dir build -R 'downloader_(web_api|baidu)' --output-on-failure`
Expected: 全部通过。

- [ ] **Step 6: 提交**

```bash
git add daemon/task-manager.h daemon/task-manager.c daemon/web-api.c tests/test-web-api.c tests/test-baidu.py
git commit -m "feat: expose task groups and group actions in the API"
```

---

### Task 10: Web 表单与分组 UI

**Files:**
- Modify: `web/index.html`
- Modify: `web/app.js`
- Modify: `web/style.css`
- Test: `tests/test-web-static.c`

**Interfaces:**
- Consumes: Task 9 的 API 字段与端点。
- Produces: 新建任务表单的百度字段、Cookie 凭据选项、分组标题行和整组按钮。

- [ ] **Step 1: 写失败测试**

`tests/test-web-static.c` 增加断言：`index.html` 含 `name="baidu_pwd"`、`name="baidu_path"`、
`<option value="cookie">`；`app.js` 含 `extra.baidu`、`/api/groups/`、`group_id`。

- [ ] **Step 2: 运行并确认失败**

Run: `cmake --build build --target test_web_static && ./build/test_web_static`
Expected: 断言失败。

- [ ] **Step 3: 实现**

- `index.html` 新建任务表单增加：
  - `百度提取码` 输入 `name="baidu_pwd"`，`maxlength="16"`；
  - `分享内路径/网盘内路径` 输入 `name="baidu_path"`；
  - 用 `<details id="baidu-options">` 包住，默认折叠。
- 凭据表单类型下拉增加 `<option value="cookie">Cookie（BDUSS/STOKEN）</option>`，
  并在密码/令牌标签旁提示粘贴完整 Cookie 头。
- `app.js`：
  - URL 输入变化时，若是 `baidu://` 或百度分享域名，显示 `#baidu-options`；
  - 提交时若 `baidu_pwd`/`baidu_path` 非空，写入 `extra.baidu`；
  - 渲染任务表时按 `group_id` 插入分组标题行，显示 `body.groups` 里的汇总与
    `开始/暂停/删除整组` 按钮；按钮调用 `/api/groups/<id>/<action>`；
  - 创建成功且 `group.truncated` 时提示“已创建 N 条任务，列表分页查看”。
- `style.css` 增加 `.group-row` 样式（背景、加粗、按钮间距）。

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build build --target test_web_static test_web_e2e && ./build/test_web_static && ctest --test-dir build -R downloader_web_e2e --output-on-failure`
Expected: 通过。

- [ ] **Step 5: 提交**

```bash
git add web/index.html web/app.js web/style.css tests/test-web-static.c
git commit -m "feat: add baidu fields and group controls to the web ui"
```

---

### Task 11: IPC 展开、文档与最终验证

**Files:**
- Modify: `daemon/main.c`
- Modify: `README.md`
- Test: `tests/test-baidu.py`

**Interfaces:**
- Consumes: 全部前序任务。
- Produces: IPC `create` 与 Web 共用展开；README 记录协议、登录、转存目录和限制。

- [ ] **Step 1: 写失败测试**

`tests/test-baidu.py` 增加 `scenario_ipc_create`：
- 通过 Unix socket 发送
  `{"op":"create","url":"baidu:///%E8%B5%84%E6%96%99","extra":{"auth_profile":"baidu-acct"}}`。
- 断言响应 `ok=true`、`group.task_count == 2`、`skipped == 0`、`truncated == false`、
  `id` 非空。
- 再发一个无 `file_name`/`protocol` 的普通 URL 创建请求，断言仍能成功（向后兼容）。

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -B tests/test-baidu.py ./build/downloader`
Expected: IPC `create` 因缺少 `file_name`/`protocol` 返回 `missing_task_field`。

- [ ] **Step 3: 实现 IPC 展开**

`daemon/main.c` 的 `create` 分支：
- 仅要求 `url`；`file_name`、`target_directory`、`protocol` 改为可选
  （新增 `daemon_json_get_optional_string`）。
- 调用 `task_manager_create_expanded()`。
- 响应：

```json
{"ok":true,"id":"<第一条任务 uuid 或空>","group":{"id":"...","name":"...","task_count":2},
 "skipped":0,"truncated":false}
```

- 失败时输出 `{"ok":false,"error":"<code>","message":"<转义后的消息>"}`。
- 释放 `TaskCreateResult.tasks`。

- [ ] **Step 4: 更新 README**

- 协议表增加“百度网盘：Cookie 登录，自己网盘文件/目录，分享链接转存后下载”。
- 增加“百度网盘”章节：BDUSS/STOKEN 获取方式、`baidu:///路径` 用法、
  分享 URL + 提取码、`[baidu]` 配置、转存占空间且不自动删除、风控与限速说明、非公开 API 风险。
- 说明 Web 凭据类型选 `Cookie`，`baidu://` 任务通过显式认证配置、默认配置或
  `pan.baidu.com` 站点配置取得登录信息。

- [ ] **Step 5: 运行完整验证**

Run:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: 全部测试通过，无失败/超时。

- [ ] **Step 6: 提交**

```bash
git add daemon/main.c README.md tests/test-baidu.py
git commit -m "feat: expand baidu tasks over ipc and document the protocol"
```

---

## 计划自检记录

- **Spec 覆盖**：分组/迁移（Task 1）、展开与 50 条响应（Task 2）、Cookie 凭据与泄漏隔离（Task 3）、
  签名/URL/页面解析（Task 4）、配置/识别/单文件下载（Task 5）、自己网盘目录（Task 6）、
  分享转存与单文件（Task 7）、分享目录/路径过滤/补齐/上限（Task 8）、API/整组操作/错误映射（Task 9）、
  Web 表单与 UI（Task 10）、IPC/文档/全量验证（Task 11）。
- **接口一致性**：`ProtocolItem`/`ProtocolError`/`TaskCreateResult` 在 Task 2 定义，后续任务只引用；
  `task_manager_create_expanded` 使用 Task 1 的 `database_insert_many`；
  Web/IPC 使用 Task 2 的返回类型；分组动作使用 Task 1 的查询接口。
- **Review Focus 对应测试**：Cookie 泄漏 → Task 3；页面/签名变化 → Task 4、Task 7；
  dlink 过期 → Task 5、Task 7；超大分享/响应上限 → Task 2、Task 8、Task 9；
  本地已有文件/部分转存 → Task 2、Task 6、Task 8。
