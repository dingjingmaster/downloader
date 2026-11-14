# 百度网盘下载设计

## 意图

在 downloader 中新增原生 `baidu` 协议，支持百度网盘的两种来源：

1. 自己账号内的文件和目录：`baidu:///绝对/路径`。
2. 他人分享链接：`https://pan.baidu.com/s/<surl>` + 提取码。

分享链接按百度网盘客户端 `BaiduPCS-Go` 已验证的路径实现：先转存到自己的网盘，
再从自己网盘逐文件下载。第一版不做上传、分享创建、OAuth、文件浏览器和网页直链绕过。

## 已确认决策

1. 下载器只围绕百度网盘下载，不实现上传、分享管理、转存之外的云盘操作。
2. 一个文件一个下载任务：目录/分享展开成 N 条任务（方案 B）。
3. 分享链接采用“转存到自己网盘，再下载”（转存步骤）。
4. 允许登录：使用 BDUSS + STOKEN Cookie；公开分享同样先登录，不做匿名直链。
5. 原生 C 实现，不引入 BaiduPCS-Go 子进程；接口和签名算法根据其源码移植。

## 目标

- `baidu` 协议接管 `baidu://` 和 `pan.baidu.com` / `yun.baidu.com` 的分享链接。
- 用现有 AES-256-GCM 凭据库保存百度 Cookie；任务通过 `extra.auth_profile` 引用，
  也支持 `[auth] default_profile` 和站点配置。
- 目录与分享在创建任务时同步展开成多条单文件任务，保留网盘目录层级到本地。
- 单文件下载支持 HTTP Range 续传、暂停/取消、无限网络重试、MD5 校验。
- dlink 过期或签名过期时自动重新解析；认证失效给出明确错误，不静默降级。
- Web 表单支持提取码、分享内路径、认证配置；任务列表按分组展示。
- 所有测试使用本地 mock，不访问真实百度服务。

## 非目标

- 匿名直链下载分享内容（`/api/sharedownload` 网页接口）；百度风控下不可靠。
- 上传、创建分享、取消分享、文件删除、回收站、离线下载。
- 官方开放平台 OAuth 和 AccessToken（需要应用审核，当前不具备）。
- 二维码登录、验证码识别、风控绕过、会员限速绕过。
- 自动删除已转存到网盘的云端副本。
- 网盘文件浏览器；第一版只接受路径字符串。

## 反目标

- 把协议专用逻辑写进核心或 Web API。
- 在协议线程里直接访问 SQLite。
- 用多个布尔字段表达任务/分组状态。
- 为了“能下”而伪造 User-Agent 绕过验证码，或隐藏失败。

## 约束

- `protocol_detect()` 会覆盖调用方传入的协议名，百度识别必须加在这里。
- 协议实现只能通过 `ProtocolOps` 和 `RequestContext` 访问凭据/代理，不能直接建任务或写库。
- 任务临时文件 → fsync → 原子提交的现有流程不能绕过。
- 百度按账号限速；并发过高容易触发风控。第一版不做多连接分片。
- 百度网页 API 是非公开接口，字段可能变化；所有解析集中在 `protocols/baidu.c`。

## 方案选择

### 文件建模：目录任务 vs 每文件任务

- **A：一个目录/分享 = 一个任务，结果目录**（类似 BT）。核心改动小，但任务内要自建
  每文件续传清单，界面只有总进度，暂停/重试粒度粗。
- **B：一个文件 = 一个任务**。复用现有单文件续传、暂停、重试、原子提交；需要核心
  支持“展开 + 分组”。用户明确选择 B。

选择 B。核心新增通用展开钩子，协议层不直接建任务。

### 分享路径：匿名直链 vs 转存后下载

- **匿名直链**：打开分享页、`/share/verify`、`/share/list`、`/api/sharedownload` 取
  dlink。少一次云端转存，但百度近年对匿名 dlink 风控严格，常见 `31362 sign error`；
  LinkSwift 已在 2025-07 移除分享页下载代码。
- **转存后下载**：`BaiduPCS-Go` 的实际做法，先 `/share/transfer` 到自己的网盘，再用
  账号下载接口取 dlink。需要登录，占用网盘空间并留下副本，但路径成熟、错误可诊断。

选择转存后下载。转存目录可配置，程序不自动删除云端副本。

### 实现方式：原生 C vs 子进程 vs 开放平台

- **原生 C**：在 `protocols/baidu.c` 实现，复用 libcurl、clib JSON/URI/线程/校验和。
  无新运行时依赖，进度和错误都在现有任务体系内。
- **BaiduPCS-Go 子进程**：集成快，但引入 Go 构建/运行依赖，打包、错误映射和暂停语义
  都要额外适配，且与“优先 C99 + clib”的项目约束冲突。
- **开放平台 OAuth**：需要应用审核和文件分享服务权限，当前不具备。

选择原生 C。签名、Cookie、接口字段按 `qjfoidnh/BaiduPCS-Go` 源码移植。

## 架构

### 协议识别与注册

- 新增 `protocols/baidu.c` / `protocols/baidu.h`，实现 `ProtocolOps`：
  `name = "baidu"`、`canHandle`、`expand`、`open`、`run`、`pause`、`resume`、`cancel`、
  `close`、`retryable`。
- `daemon/protocol.c` 的 `protocol_detect()`：
  - scheme 为 `baidu` → `baidu`。
  - scheme 为 `http`/`https`，主机为 `pan.baidu.com`、`yun.baidu.com`、`eyun.baidu.com`，
    路径以 `/s/` 开头或为 `/share/init` → `baidu`。
- `daemon/main.c` 在 `http` 之前注册 `baidu`。
- `canHandle` 做同样的主机/路径判断；`http` 适配器仍处理其他百度域名，不抢占普通下载。

### 认证与凭据

- `DownloaderCredentialKind` 增加 `DOWNLOADER_CREDENTIAL_COOKIE`；`credential_kind_name`
  返回 `cookie`，`credential_kind_parse` 接受 `cookie`。
- `secret` 保存完整 Cookie 头，例如
  `BDUSS=...; STOKEN=...; PANWEB=1`；至少要求包含 `BDUSS` 和 `STOKEN`。
- 使用现有 AES-256-GCM 凭据库加密；`scope` 固定为 `pan.baidu.com`；`username` 仅作账号标签。
- `request_auth_valid()` 允许 COOKIE 类型使用空用户名，但仍要求非空 secret。
- Web 认证配置表单的类型下拉增加 `Cookie`，密码/令牌输入框用于粘贴 Cookie 头。
- 协议通过 `request_context_resolve()` 取得凭据，不直接读凭据库。
- Cookie 不写入日志、任务 JSON、`extra_json` 或列表 API。
- **防止 Cookie 泄漏**：`http`、`aria`、`media`、`smb`、`smb1` 适配器在选择
  `COOKIE` 类型凭据时返回 `auth_kind_unsupported`（不可重试），不得把它当作
  Basic/Digest 发送。`baidu` 适配器只接受 `COOKIE` 类型；未配置凭据时返回
  `baidu_auth_missing`。
- `baidu:///` 没有主机名：`request_context_resolve()` 对 `baidu` scheme 使用
  `pan.baidu.com` 作为站点配置匹配主机，使站点配置、默认配置和显式
  `extra.auth_profile` 都能生效。
- 签名缓存按 **Cookie secret 的 SHA-256 十六进制值** 作为键（`c_checksum_sha256`），
  不依赖认证配置名；同一账号的不同配置共享缓存，不同账号互不影响。
- 认证错误：
  - 缺少 Cookie、缺少 BDUSS/STOKEN，或凭据引用无法加载 → `baidu_auth_missing`
    （不可重试）。
  - `/disk/home` 跳转 passport 或返回登录页 → `baidu_auth_expired`（不可重试）。
  - 页面能打开但没有 sign 字段 → `baidu_sign_unavailable`（不可重试，提示页面可能改版）。
- 风控/验证码（如 `errno = 8001`）→ `baidu_challenge_required`，提示稍后重试或更新 Cookie，
  程序不尝试绕过。

### 任务展开接口

核心新增通用展开钩子，协议只负责解析和返回条目，不建任务、不写库。

`daemon/protocol.h`：

```c
#define DOWNLOADER_PROTOCOL_GROUP_NAME_SIZE 256U

typedef struct
{
    char resourceUrl[DOWNLOAD_TASK_URL_SIZE];
    char fileName[DOWNLOAD_TASK_FILE_NAME_SIZE];
    /* 相对任务创建时下载根目录的子目录；空串表示根目录。 */
    char targetSubdirectory[DOWNLOAD_TASK_PATH_SIZE];
    char extraJson[DOWNLOAD_TASK_EXTRA_JSON_SIZE];
    /* 创建时即可展示的大小与校验值；0/空表示未知。 */
    uint64_t totalBytes;
    char     checksumType[DOWNLOAD_TASK_PROTOCOL_SIZE];
    char     checksumValue[DOWNLOAD_TASK_CHECKSUM_SIZE];
} ProtocolItem;

typedef struct
{
    char code[64];
    char message[DOWNLOAD_TASK_ERROR_SIZE];
} ProtocolError;

struct ProtocolOps
{
    /* ...现有字段... */
    /* 可选：把一次提交展开成具体文件。在 API 线程同步执行；
     * request->requestContext 已由核心解析，协议可读取凭据/代理。
     * items 由协议分配，核心用 c_free() 释放；返回 0 表示成功。
     * 失败时 error 填 code/message（两者都必须以 NUL 结尾）。 */
    int (*expand) (const DownloadTask* request, ProtocolItem** items, size_t* count,
                   char* groupName, size_t groupNameCapacity, ProtocolError* error);
};
```

`daemon/task-manager.h`：

```c
#define DOWNLOADER_CREATE_RESULT_TASKS 50U

typedef struct
{
    char          groupUuid[DOWNLOAD_TASK_GROUP_UUID_SIZE];
    char          groupName[DOWNLOAD_TASK_GROUP_NAME_SIZE];
    size_t        taskCount;   /* 实际创建的任务数 */
    size_t        skippedCount;/* 本地目标已存在而跳过的条目 */
    bool          truncated;   /* tasks 只包含前 50 条 */
    DownloadTask* tasks;       /* 最多 50 条；调用方使用 c_free() 释放 */
    size_t        returnedCount;
    char          errorCode[64];
    char          errorMessage[DOWNLOAD_TASK_ERROR_SIZE];
} TaskCreateResult;

int task_manager_create_expanded (TaskManager* manager, const char* resourceUrl,
                                  const char* fileName, const char* targetDirectory,
                                  const char* extraJson, TaskCreateResult* result);
```

创建流程：

1. `protocol_detect()` 识别协议。
2. 构造临时 `DownloadTask`，`request_context_resolve()` 解析凭据/代理。
3. 调用 `ops->expand`（没有该钩子时生成单条条目）。
4. 核心把原请求 `extraJson` 顶层的 `auth_profile`、`proxy_profile` 复制到每条条目
   （条目里已有的同名字段以条目为准），保证子任务解析到同一凭据；协议自己的
   `baidu` 子对象不受影响。
5. 核心逐条检查本地 `target_path`：已存在普通文件/目录的条目跳过并计入
   `skippedCount`，不创建任务。全部跳过时返回错误 `target_files_exist`，不建分组。
6. 决定分组：
   - 分享来源始终建分组，即使只有一个文件；
   - 自己网盘的目录建分组；单个文件不建分组。
7. 每条条目：校验文件名/子目录，拼接 `target_directory`，`filesystem_prepare()` 计算
   `target_path`/`temp_path`，写入相同的分组字段和 `totalBytes`/`checksum*`。
8. 用一个 SQLite 事务插入；每 200 条一批，任一失败则全部回滚，不留下半组任务。
9. 只把前 50 条任务复制到结果里（`truncated = true`），其余由调用方重新查询列表；
   这样 1 MB 的 Web 响应上限和峰值内存都可控。
10. 返回创建的任务、分组信息、跳过数；API 线程负责释放。

约束：

- 子目录只接受相对路径；拒绝 `..`、空组件、绝对路径、NUL。核心用
  `filesystem_path_is_inside()` 再次确认结果在下载根目录内。
- 文件名沿用 `task_manager_validate()`：不含 `/`、`\`、`..`。
- 展开失败不创建任何任务；`ProtocolError.code` 原样交给 Web/IPC。
- 展开是同步网络操作，大目录可能使创建请求耗时较长；第一版接受，后续可改为准备阶段任务。
- `ProtocolItem` 约 10 KB，`max_files` 上限同时限制展开内存。

### 数据库与分组

数据库版本从 3 升到 4，事务内迁移：

```sql
ALTER TABLE download_tasks ADD COLUMN group_uuid TEXT NOT NULL DEFAULT '';
ALTER TABLE download_tasks ADD COLUMN group_name TEXT NOT NULL DEFAULT '';
CREATE INDEX IF NOT EXISTS idx_download_tasks_group ON download_tasks(group_uuid);
```

`DownloadTask` 增加：

```c
#define DOWNLOAD_TASK_GROUP_UUID_SIZE 64U
#define DOWNLOAD_TASK_GROUP_NAME_SIZE 256U
char groupUuid[DOWNLOAD_TASK_GROUP_UUID_SIZE];
char groupName[DOWNLOAD_TASK_GROUP_NAME_SIZE];
```

- 不新增“父任务”行。分组是任务字段，调度器只看到普通单文件任务，状态机不变。
- 单文件下载 `group_uuid` 为空，保持旧行为。
- `database_insert_many()` 在单个事务里插入整组。
- `database_list_group()` 返回某分组全部任务（按创建顺序）。
- `database_group_summary()` 返回分组汇总：任务数、完成/失败数、总字节、已下载字节；
  列表 API 对当前页出现过的分组逐个查询。
- 删除/清空已完成任务只删除记录和本地临时文件；云端转存副本保留。

### 下载

每个文件任务独立执行：

1. 通过 `request_context` 取 Cookie，解析出 BDUSS/STOKEN。
2. 从内存签名缓存取 `sign`/`timestamp`（按认证配置名缓存 1 小时，互斥保护）。
   过期则 `GET {api_base}/disk/home`：
   - 带 Cookie，UA 可跟随 BaiduPCS-Go 使用网盘客户端 UA。
   - 不自动跟随跳转：`Location` 为 `/` 或 passport → 认证失效。
   - 解析页面中的 `"sign1"`、`"sign3"`、`"timestamp"`。
   - `sign = base64(Sign2(sign3, sign1))`；`Sign2` 为源码中的 RC4 变体（输入按 ASCII 字节）。
3. `POST {api_base}/api/download`，表单：
   - `sign`、`timestamp`
   - `fidlist=[<fs_id>]`（支持多条，本设计每次一个文件）
   返回 `dlink` 列表。
4. `GET dlink`：
   - Header：`Cookie`（复制账号 Cookie）、`User-Agent: pan.baidu.com`，不发送 `Referer`。
   - `Range: bytes=<durableBytes>-`；206 续传，200 从头写。
   - 网络错误交给任务管理器的无限重试；每次重试重新解析 dlink。
5. 写临时文件，定期 `fsync` 后调用 `onCheckpoint`；完成后校验大小和 MD5，再
   `onCompleted`，由核心原子提交。
6. `onMetadata` 写入 `etag = md5`、`lastModified = server_mtime`；恢复时重新解析元数据，
   大小或 MD5 变化则丢弃检查点从头下载。

校验规则：

- **大小**：始终校验；不一致按校验失败处理。
- **MD5**：`/api/list` 返回非空 md5 时校验；md5 为空时只校验大小并继续，不把文件判失败。
- **校验失败重试在协议内完成**：把临时文件 `ftruncate(0)` 并 `fsync`，调用
  `onCheckpoint` 写入 `durableBytes = 0`，在同一 `run` 内从头再下一次；第二次仍失败
  返回不可重试的 `baidu_checksum_mismatch`。不新增管理器计数器，也不会无限循环。

错误处理：

- dlink 返回 401/403 或 JSON 错误：重新解析一次；仍失败 → `baidu_dlink_failed`（不可重试）。
- `/api/download` 返回 `112`/`113`（签名过期）：清空签名缓存，刷新后重试一次。
- `31066`/`31297`（文件不存在）→ `baidu_file_not_found`（不可重试）。
- 风控 `8001` → `baidu_challenge_required`（不可重试）。
- curl 网络错误、超时、HTTP 5xx → `retryable()` 返回 true，交给现有无限重试。
- 第二次 MD5/大小不一致 → `baidu_checksum_mismatch`（不可重试）。

`/api/download` 一次可返回多个 `dlink`，但第一版仍按单文件任务请求，避免一次解析结果
过期影响整组；后续可在展开时预取。

### 自己网盘

URL 形式：`baidu:///绝对/路径`（路径按 URL 转义）。

展开流程：

1. 去掉 `baidu://`，URL 解码，得到以 `/` 开头的网盘绝对路径；拒绝 `..` 和空路径。
2. 列父目录：`GET {api_base}/api/list?dir=<父目录>&num=1000&page=<n>&order=name&desc=0`。
   响应条目字段：`fs_id`、`isdir`、`server_filename`、`size`、`md5`、`path`、
   `server_mtime`。
3. 路径是文件 → 单条任务；路径是目录 → 递归分页列出全部文件。
4. 本地布局：`targetSubdirectory` = 网盘绝对路径去掉开头的 `/`，保留完整层级。
   例如 `/资料/2024` → `<下载目录>/资料/2024/...`；单个文件 `/资料/a.zip` →
   `<下载目录>/资料/a.zip`。
5. 分组名 = 根目录名；单个文件不建分组。

展开条目额外字段写入 `extraJson`：

```json
{"baidu":{"kind":"own","fs_id":"123","size":456,"md5":"...","api_path":"/资料/2024/a.zip"}}
```

`resourceUrl` 保持 `baidu:///...`，便于恢复和排查。

### 分享链接

URL 形式：`https://pan.baidu.com/s/<surl>`，提取码来自：

1. URL 的 `?pwd=xxxx`；
2. 任务 `extra.baidu.pwd`（Web 表单/API 均可）。

可选 `extra.baidu.path` 只下载分享内指定文件或子目录；省略时展开整个分享根。

展开流程（参数和顺序按 BaiduPCS-Go 源码）：

1. `GET {api_base}/s/<surl>`，带 Cookie 和网盘客户端 UA，`Referer: {api_base}/disk/home`。
   - 页面包含 `error-404` → 分享不存在；
   - 包含 `platform-non-found` → 分享已失效；
   - 从页面中形如 `{...loginstate...}` 的 JSON 提取 `bdstoken`、`uk`、`share_uk`、`shareid`。
2. 有提取码时 `POST {api_base}/share/verify?shareid=...&time=<毫秒>&clienttype=1&uk=<share_uk>`：
   - 表单：`pwd`、`vcode=null`、`vcode_str=null`、`bdstoken`。
   - `errno = -9` → 提取码错误；`errno = 8001` → 风控验证。
   - 成功后保存响应 Set-Cookie（`BDCLND`），后续请求复用。
3. 重新 `GET {api_base}/s/<surl>`（`Referer` 为 `/share/init?surl=...`），刷新 Cookie 和 token。
4. `GET {api_base}/share/list?bdstoken=...&root=1&web=5&app_id=250528&shorturl=<surl>&channel=chunlei`：
   - 取根条目：`server_filename`、`fs_id`、`isdir`、`size`、`md5`、`path`。
   - 根条目为空 → `baidu_share_empty`。
   - 超过 `[baidu] max_files` 上限 → `baidu_share_too_large`（先展开后统计）。
5. 计算转存目录 `<transfer_directory>/<surl>`；确保目录存在：
   `POST {pcs_base}/rest/2.0/pcs/file?method=mkdir&path=<转存目录>&app_id=250528`
   （已存在视为成功）。
6. 先列出转存目录根（`/api/list?dir=<转存目录>`，非递归），按 `server_filename`
   与分享根条目匹配：
   - 名称存在且 `size`/`md5` 与分享条目一致 → 视为已转存；
   - 名称相同但大小/MD5 不同 → `baidu_transfer_conflict`，不覆盖；
   - 只对缺失的 `fs_id` 调用 `POST {api_base}/share/transfer?app_id=250528&channel=chunlei&clienttype=0&web=1&shareid=...&from=<share_uk>&bdstoken=...&filename=<首条名称>&item_num=<条数>&fs_id=[...]&path=<转存目录>`。
     - 表单同时带 `fsidlist` 和 `path`，与源码保持一致。
     - `errno = 12` 且 `info[].errno = -30`、`errno = 4` → 对应条目已在目标目录，按已转存处理。
     - 其他失败只对失败的 `fs_id` 重试一次；仍失败 → `baidu_transfer_failed`，不创建任何任务。
   - 成功响应 `info[].path` 是转存后的网盘路径；缺失时回退为列出转存目录。
7. 从转存后的网盘路径递归 `/api/list`（同“自己网盘”），按 `extra.baidu.path` 过滤后
   生成单文件条目：
   - `targetSubdirectory` = **相对分享根**的路径（例如 `资料/2024`），不包含
     `<surl>` 转存容器；本地结果为 `<下载目录>/资料/2024/...`；
   - `resourceUrl` 写成 `baidu:///<转存路径>`，`extra.baidu.source_share` 记录原分享
     `surl`，避免重试时再次转存；
   - 单个根文件分享 → `<下载目录>/<文件名>`。
8. 分享**始终创建分组**，即使只有一个文件；分组名：分享根只有一个条目时用其名称，
   多个条目时用分享页标题，取不到则用 `百度分享-<surl>`。名称做文件名安全清理。

转存复用算法（支持失败后重建）：

- 同一分享的转存目录固定为 `<surl>`，不因重试创建新目录。
- 每次创建先对比分享根条目与转存目录根条目：存在且大小/MD5 一致就复用，
  缺失项才转存；因此部分转存的分享会被“补齐”，不会重复转存。
- 对比只到转存目录根层；已存在目录内部的缺失文件不会自动补齐（百度转存以根条目为单位）。
  需要完整重建时，用户可删除云端转存目录后重新创建任务。
- 同名但内容不一致时明确报冲突，不静默使用错误文件。
- 转存占用网盘空间，程序不自动删除；删除本地任务不会删除云端副本。

`extra.baidu.path` 的处理：

- 只做过滤，不改变本地布局；本地路径始终保留分享根开始的层级。
- 指向文件 → 只转存并展开该 `fs_id`，生成一条任务。
- 指向子目录 → 转存包含该目录的根条目，只展开该子目录下的文件。
- 路径不存在 → `baidu_share_path_not_found`。

### Web 与 API

任务创建：

- 现有 `POST /api/tasks` 和 IPC `create` 都改为调用 `task_manager_create_expanded()`。
- 响应保持兼容并新增分组信息（`tasks` 最多 50 条）：

```json
{
  "ok": true,
  "group": {"id": "group-...", "name": "资料", "task_count": 2000,
            "skipped": 3, "truncated": true},
  "tasks": [ ... 最多 50 条 ... ],
  "task": { ... 第一条任务或 null ... }
}
```

`task` 为第一条任务；`tasks` 最多前 50 条，客户端通过列表 API 获取其余任务。
自己网盘单文件且不建分组时 `group` 为 null；分享即使只有一个文件也有分组。
`skipped` 是本地目标已存在而未创建任务的条目数。

- 创建失败时返回具体错误码，不再统一为 `create_failed`：

```json
{"ok": false, "error": "baidu_password_invalid", "message": "提取码错误"}
```

HTTP 状态按错误码映射；IPC 响应同样带 `error` 和 `message`。

任务列表：

- `GET /api/tasks` 的每条任务增加 `group_id`、`group_name`。
- 增加 `groups` 数组，包含当前页出现过分组的总汇总：

```json
{"id":"group-...","name":"资料","task_count":12,"completed":3,"failed":0,
 "total_bytes":123,"downloaded_bytes":45}
```

`total_bytes` 来自创建时 `ProtocolItem.totalBytes`，因此排队中的任务也有正确大小。

任务列表：

- `GET /api/tasks` 的每条任务增加 `group_id`、`group_name`。
- 增加 `groups` 数组，包含当前页出现过分组的总汇总：

```json
{"id":"group-...","name":"资料","task_count":12,"completed":3,"failed":0,
 "total_bytes":123,"downloaded_bytes":45}
```

分组操作：

- `POST /api/groups/<uuid>/start`
- `POST /api/groups/<uuid>/pause`
- `POST /api/groups/<uuid>/resume`
- `DELETE /api/groups/<uuid>`（删除整组任务和本地临时文件，不删云端副本）

核心新增 `task_manager_group_action()`，遍历 `database_list_group()` 后调用现有单任务
动作，返回影响条数。删除整组时逐条等待活动任务结束。

Web 页面：

- 新建任务表单增加：
  - 百度提取码 `baidu_pwd`（可选，写入 `extra.baidu.pwd`）；
  - 分享内路径/网盘内路径 `baidu_path`（可选，写入 `extra.baidu.path`）；
  - 当 URL 是 `baidu://` 或百度分享域名时展开这两个字段，其他 URL 不显示。
  - 认证配置下拉沿用现有字段，用户选择 Cookie 类型的百度配置。
- 任务表格按 `group_id` 插入分组标题行：显示分组名、汇总进度、完成/失败数，
  以及“开始/暂停/删除整组”按钮。
- 单文件任务保持现有行样式。
- 凭据表单的类型下拉增加 `Cookie`，提示粘贴完整 Cookie 头。

### 配置

`config/download.ini` 增加：

```ini
[baidu]
# 分享转存到网盘的目标根目录；实际目录为 <transfer_directory>/<surl>。
transfer_directory = /downloader
# 同时下载的百度任务上限，降低账号风控概率。
max_parallel_tasks = 2
# 单次展开允许的最大文件数，防止超大分享撑爆任务表和创建响应。
# 展开条目约 10 KB，默认值对应约 20 MB 峰值内存。
max_files = 2000
# 签名缓存秒数，过期后重新读取 /disk/home。
sign_cache_seconds = 3600
# 排障/测试用：覆盖 API 根地址；默认官方地址。
api_base = https://pan.baidu.com
pcs_base = https://pcs.baidu.com
```

`daemon/config.c` / `config.h` 增加对应读取函数；未配置时使用上述默认值。
`api_base`/`pcs_base` 仅用于测试和域名迁移，文档标注不要随意修改。

### 错误、重试与状态

任务失败时 `errorMessage` 写用户可读的中文说明，`errorCode` 使用下表映射的 errno：

| 错误码 | errno | 可重试 | 说明 |
|---|---|---|---|
| `baidu_auth_missing` | EPERM | 否 | 未配置 BDUSS/STOKEN |
| `baidu_auth_expired` | EACCES | 否 | Cookie 失效，需要更新 |
| `baidu_sign_unavailable` | EPROTO | 否 | 首页结构变化/无法取签名 |
| `baidu_challenge_required` | EACCES | 否 | 触发风控/验证码 |
| `baidu_share_not_found` | ENOENT | 否 | 分享不存在 |
| `baidu_share_expired` | ESTALE | 否 | 分享已失效 |
| `baidu_password_required` | EINVAL | 否 | 需要提取码 |
| `baidu_password_invalid` | EINVAL | 否 | 提取码错误 |
| `baidu_share_empty` | ENODATA | 否 | 分享没有文件 |
| `baidu_share_too_large` | EFBIG | 否 | 超过 max_files |
| `baidu_share_path_not_found` | ENOENT | 否 | 指定的分享内路径不存在 |
| `baidu_transfer_failed` | EIO | 否 | 转存失败（含冲突详情） |
| `baidu_transfer_conflict` | EEXIST | 否 | 目标目录同名但内容不一致 |
| `baidu_file_not_found` | ENOENT | 否 | 网盘文件已删除/移动 |
| `baidu_dlink_failed` | EIO | 否 | 两次解析仍拿不到可用 dlink |
| `baidu_checksum_mismatch` | EIO | 否（协议内重下一次） | 第二次大小/MD5 仍不一致 |
| `baidu_network_error` | EAGAIN | 是 | 网络错误/超时/5xx |
| `auth_kind_unsupported` | EINVAL | 否 | 非百度适配器收到 COOKIE 凭据 |
| `auth_profile_unavailable` | ENOENT | 否 | 引用的凭据配置不存在/无法解密 |
| `invalid_url` | EINVAL | 否 | URL/路径/提取码参数非法 |
| `unsupported_protocol` | ENOTSUP | 否 | 无法识别的协议 |
| `target_files_exist` | EEXIST | 否 | 全部目标文件已存在，未创建任务 |

创建任务的 HTTP 状态映射：

- 400：`invalid_url`、`baidu_password_required`、`baidu_password_invalid`、
  `auth_kind_unsupported`；
- 401/403：`baidu_auth_missing`、`baidu_auth_expired`、`baidu_challenge_required`；
- 404：`baidu_share_not_found`、`baidu_share_expired`、`baidu_share_path_not_found`、
  `baidu_file_not_found`、`auth_profile_unavailable`；
- 409：`baidu_transfer_conflict`、`target_files_exist`；
- 413：`baidu_share_too_large`；
- 502：`baidu_network_error`、`baidu_transfer_failed`、`baidu_dlink_failed`；
- 其他：500。

核心在展开前失败时负责填 `TaskCreateResult.errorCode/errorMessage`（例如凭据解析失败）；
展开失败时使用 `ProtocolError`。Web 和 IPC 都原样输出该错误码。

### 并发与限速

- 任务池仍由 `max_concurrent_tasks` 控制（默认 5）。
- `protocols/baidu.c` 内部用进程级信号量限制同时下载的百度任务数为
  `[baidu] max_parallel_tasks`（默认 2）；等待不占用额外连接，但会占用任务池线程。
- 每个文件单连接，不发多线程 Range；不做会员限速绕过。
- 签名缓存和 Cookie 只存在内存，进程退出即消失；Cookie 本体在加密凭据库。

## 测试

### C 单元测试

`tests/test-baidu.c`：

- `protocol_detect`：`baidu:///a/b`、`https://pan.baidu.com/s/1abc`、
  `https://yun.baidu.com/s/1abc`、普通 `https://pan.baidu.com/disk/home` 不应误判。
- `baidu:///` 路径解析：转义、拒绝 `..`、拒绝空路径。
- 分享 URL/提取码解析：`?pwd=`、extra 覆盖、缺失提取码。
- 文件名/子目录安全校验：路径分隔符、`..`、超长路径。
- Cookie 解析：缺少 BDUSS/STOKEN；COOKIE 凭据允许空用户名。
- `request_context_resolve`：`baidu:///` 命中 `pan.baidu.com` 站点配置。
- 非百度适配器拒绝 COOKIE 凭据，返回 `auth_kind_unsupported`。
- 核心展开：复制 `auth_profile`/`proxy_profile` 到每条条目；目标已存在时跳过；
  只返回前 50 条任务并设置 `truncated`。
- `Sign2` + base64 测试向量（来自 BaiduPCS-Go `netdisksign/sign2_test.go`）：
  输入 `sign3 = "e8c7d729eea7b54551aa594f942decbe"`、
  `sign1 = "37dbe07ade9359c1aa70807e847f768c13360ad2"`，
  输出 base64 必须等于 `8RxCbsVeSzn2UjxJAAiV9QQs/WetOj2FJUGwjsMG6SgxFMWlLS/U1Q==`。

### Python 集成测试

`tests/test-baidu.py` 启动本地 mock HTTP 服务，通过临时配置
`[baidu] api_base = http://127.0.0.1:<port>`、`pcs_base = http://127.0.0.1:<port>`，
daemon 使用临时数据库和临时下载目录。mock 覆盖：

- `/disk/home` 返回固定 `sign1`/`sign3`/`timestamp`；
- `/api/download` 返回 mock dlink；
- `/api/list` 分页返回文件/目录；
- `/s/<surl>` 返回带 `loginstate` 的 HTML；
- `/share/verify` 正确/错误提取码；
- `/share/list`、`/share/transfer`；
- `/rest/2.0/pcs/file?method=mkdir`；
- dlink 路径支持 Range、206/200、中断和恢复。

场景：

1. `baidu:///` 单文件下载；暂停后恢复继续使用 Range；校验 MD5。
2. `baidu:///目录` 展开成 N 条任务，本地路径为 `<下载目录>/资料/2024/...`，
   整组开始/暂停/删除。
3. 分享单文件 + 正确提取码：转存、始终建一个文件的分组、下载到
   `<下载目录>/<文件名>`。
4. 分享目录：转存到 `/downloader/<surl>`，展开 N 条任务，本地路径不包含
   `<surl>` 容器；`extra.baidu.path` 只过滤不改变层级。
5. 错误提取码、缺少提取码、分享失效、分享为空、超过 `max_files = 2000`。
6. dlink 首次 403，重新解析后成功；两次失败任务失败且不无限重试。
7. 网络中断后由管理器重试并完成；重试时重新解析 dlink。
8. 恢复时大小/MD5 变化，丢弃检查点从头下载；md5 为空时只校验大小。
9. 同名重复创建分享：复用转存目录，不重复转存；部分转存时只补缺失 `fs_id`；
   同名但大小/MD5 不一致报 `baidu_transfer_conflict`。
10. 未配置 Cookie、Cookie 失效、页面无 sign 字段的错误映射；
    普通 HTTP 任务误选 COOKIE 凭据时报 `auth_kind_unsupported`，不发送 Cookie。
11. 本地目标文件已存在时跳过并计入 `skipped`；全部存在返回 `target_files_exist`。
12. 超过 50 条任务的创建响应设置 `truncated = true`，列表 API 能取到全部任务。
13. IPC `create` 与 Web 一样触发目录/分享展开，并返回 `group`/`tasks`。

测试不得访问真实百度；mock 数据固定在 `tests/fixtures/baidu/`。

### 构建与静态检查

- CMake 注册 `test-baidu`（C）和 `test-baidu.py`（ctest）。
- 每次修改至少执行：`cmake --build build`、`ctest --test-dir build --output-on-failure`，
  以及项目现有静态检查命令。

## 实施顺序

1. 核心展开接口、任务分组字段、数据库迁移、`task_manager_create_expanded`、
   分组动作与列表汇总、Web/IPC 接入。
2. Cookie 凭据类型、空用户名规则、非百度适配器拒绝 COOKIE、Web 认证表单、任务表单字段。
3. `baidu` 协议骨架：URL 识别、Cookie 解析、签名、`/api/download`、单文件下载与续传。
4. `baidu:///` 文件/目录展开。
5. 分享页解析、verify、list、transfer、展开。
6. Web 分组展示与整组操作。
7. C 单元测试、Python mock 集成测试、README 与配置文档。

## 风险与约束

- 百度网页 API 非公开，字段和签名可能变化；集中解析并保留 mock 回归测试。
- 转存会占用账号空间并留下副本；不自动删除，避免误删用户数据。
- 主账号 Cookie 有风控和封禁风险；建议使用小号或在文档中明确提示。
- 并发、频繁转存可能触发验证码；程序不绕过，只报错。
- `/disk/home` 的 sign 抓取依赖页面结构；失败时给出明确错误而不是静默重试。
- 大分享创建请求耗时长；第一版接受同步展开，后续可改为后台准备任务。

## 我的判断（可在审批时调整）

- 分组用任务字段而不是父任务行：调度器保持简单，分组汇总按需查询。
- 分享转存后的子任务以 `baidu:///<转存路径>` 作为 `resourceUrl`，原分享只存在
  `extra.baidu.source_share`，避免重试时重复转存。
- 转存目录默认 `/downloader/<surl>`，不自动删除。
- 默认同时下载 2 个百度任务，低于全局并发 5。
- 展开同步发生在创建请求内；失败不留下部分任务。
- 分享即使只有一个文件也建分组，便于整组开始/暂停/删除和标记转存来源；
  自己网盘的单个文件不建分组。
- 本地已有目标文件时跳过该条目；部分跳过继续创建其余任务，全部存在则
  `target_files_exist`。不覆盖已有文件，也不静默重下。
- `max_files` 默认 2000；创建响应最多返回 50 条任务，其余通过列表 API 获取。
- MD5 为空时只校验大小，不因此判失败。
- COOKIE 凭据允许空用户名，但只能用于百度适配器；其他适配器明确拒绝，避免 Cookie
  被当作 Basic 凭据泄漏。
- IPC 与 Web 共用展开路径，IPC 响应新增 `group`/`tasks` 字段并保留 `task` 兼容字段。
