# Downloader 后台下载服务设计草案

## 1. 目标

构建一个后台下载进程，启动时读取配置文件，通过 Unix Domain Socket 接收任务操作，使用 SQLite 持久化任务状态，并通过统一的协议适配器接口支持多种下载协议。

第一阶段支持：HTTP、HTTPS、FTP、FTPS、SFTP、BitTorrent、Magnet、Metalink。

## 2. 非目标

- 第一阶段不实现 GUI。
- 第一阶段不把协议开关放入配置文件。
- 核心任务管理器不直接包含 HTTP、FTP、BT 等协议细节。
- 不在第一阶段同时实现 HLS、MPEG-DASH、云盘 API、eD2K/Kad、RTSP/RTMP 等扩展协议。

## 3. 配置文件

配置文件搜索顺序：

1. 命令行明确指定的配置文件；
2. 程序目录下的 `config/download.ini`；
3. 安装路径 `/etc/downloader/download.ini`；
4. 全部不存在时打印错误并退出。

配置文件只保存通用配置，例如：

- Unix Socket 路径；
- SQLite 数据库路径；
- 默认下载目录；
- 最大并发任务数；
- 重试次数；
- 超时；
- 临时文件策略；
- 覆盖策略；
- 日志级别。

协议支持由程序内建并自动注册，不设置 `[protocol.*].enabled` 配置项。

## 4. 协议适配器接口

核心定义一套 C 接口，协议实现通过函数指针填充操作表。任务管理器、调度器和 IPC 层只操作抽象接口，不判断具体协议。

建议接口形态：

```c
typedef struct downloader_protocol_ops downloader_protocol_ops_t;
typedef struct downloader_protocol downloader_protocol_t;
typedef struct downloader_protocol_task downloader_protocol_task_t;

struct downloader_protocol_ops {
    const char *name;

    bool (*can_handle)(const char *url, const char *protocol);

    int (*open)(downloader_protocol_task_t **task,
                const downloader_task_spec_t *spec,
                const downloader_protocol_callbacks_t *callbacks,
                void *user_data);

    int (*run)(downloader_protocol_task_t *task);
    int (*pause)(downloader_protocol_task_t *task);
    int (*resume)(downloader_protocol_task_t *task);
    int (*cancel)(downloader_protocol_task_t *task);

    void (*close)(downloader_protocol_task_t *task);
};
```

协议实现通过回调向核心报告：

```c
typedef struct downloader_protocol_callbacks {
    int (*on_started)(void *user_data);
    int (*on_progress)(void *user_data,
                       uint64_t total_bytes,
                       uint64_t downloaded_bytes,
                       uint64_t speed_bytes_sec);
    int (*on_paused)(void *user_data);
    int (*on_completed)(void *user_data,
                        const char *checksum_type,
                        const char *checksum_value);
    int (*on_failed)(void *user_data,
                     int error_code,
                     const char *error_message);
};
```

### 接口约束

- `open` 只创建协议上下文，不开始传输。
- `run` 在工作线程中执行实际下载。
- `pause`、`resume`、`cancel` 必须支持并发控制，不能直接销毁工作线程正在使用的上下文。
- `close` 负责释放协议上下文、网络连接和临时资源。
- 协议层不得直接修改 SQLite。
- 协议层不得直接改变核心任务状态，只能通过回调报告事件。
- 所有协议必须使用统一的错误码和进度回调。

## 5. 协议注册

核心维护协议注册表：

```c
int downloader_protocol_register(const downloader_protocol_ops_t *ops);
const downloader_protocol_ops_t *downloader_protocol_find(
    const char *url, const char *protocol);
```

第一阶段在程序启动时自动注册内建协议实现，例如：

```text
http_adapter_register()
ftp_adapter_register()
sftp_adapter_register()
bittorrent_adapter_register()
metalink_adapter_register()
```

不通过配置文件启用或禁用协议。某个协议编译时不可用时，应在启动日志中明确记录，创建该协议任务时返回“不支持的协议”。

## 6. 任务状态和持久化

任务状态包括：

```text
created -> queued -> downloading -> completed
                              ├──> paused
                              ├──> failed
                              └──> cancelled
```

SQLite 主表至少保存：

- 任务 UUID；
- 文件名、目标目录、最终路径、临时路径；
- 资源 URL、协议类型；
- 状态、错误码、错误信息；
- 创建、开始、暂停、完成、更新时间；
- 总字节数、已下载字节数、进度、速度；
- 重试次数和最大重试次数；
- 校验算法和校验值；
- ETag、Last-Modified 等断点续传信息；
- JSON 扩展字段。

协议实现不直接操作数据库，由核心回调统一持久化。

## 7. 文件完成流程

下载数据先写入临时文件，例如 `filename.part`。完成后执行大小/哈希校验、`fsync`，再通过原子重命名提交为正式文件。

## 8. IPC

使用 Unix Domain Socket。消息采用长度前缀加 JSON，提供：

```text
create
delete
start
pause
resume
cancel
query
list
clear_completed
shutdown
```

IPC 层只负责解析请求和返回结果，不直接调用协议函数；请求交给任务管理器和调度器。

## 9. 启动恢复

- `downloading` 启动后恢复为 `queued`；
- `queued` 自动重新调度；
- `paused` 保持暂停；
- `completed` 不重复下载；
- 临时文件和 SQLite 记录共同决定是否可以断点续传。

## 10. 建议目录结构

```text
daemon/
    main.c
    config.h
    config.c
    ipc.h
    ipc.c
    task.h
    task.c
    scheduler.h
    scheduler.c
    database.h
    database.c
    protocol.h
    protocol.c
    filesystem.h
    filesystem.c
    logger.c
protocols/
    http.c
    ftp.c
    sftp.c
    bittorrent.c
    metalink.c
docs/
    superpowers/specs/
```

