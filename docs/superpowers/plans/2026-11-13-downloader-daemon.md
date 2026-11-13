# Downloader 后台服务 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 构建使用 clib 的后台下载服务主干，支持配置加载、Unix Socket IPC、SQLite 任务持久化、任务状态机、调度和可扩展协议适配器接口。

**Architecture:** 进程核心由配置、IPC、任务管理、调度器、数据库和文件提交模块组成；协议实现通过函数指针操作表注册，核心只依赖统一协议接口。第一阶段先建立接口和可测试的 HTTP/HTTPS 适配器，其他协议按同一接口接入。

**Tech Stack:** ISO C99、CMake、项目内 clib、SQLite3、libcurl（HTTP/HTTPS/FTP/FTPS，若构建环境提供则扩展 SFTP）。

**Spec:** `docs/superpowers/specs/2026-11-13-downloader-daemon-design.md`

## Global Constraints

- 优先使用项目内引入的 `clib`，不重复实现 clib 已提供的通用能力。
- 下载器自己的结构体不使用 `C` 前缀，自己的函数不使用 `c_` 前缀；直接调用 clib 时保留 clib 原名。
- 协议能力自动注册，不设置协议启用/禁用配置项。
- 配置搜索顺序为命令行指定路径、程序目录 `config/download.ini`、`/etc/downloader/download.ini`。
- SQLite 数据库默认位于运行路径下的 `meta.db`。
- 协议模块不得直接操作 SQLite、IPC 或核心任务状态。
- 下载完成必须经过校验、`fsync` 和原子重命名。
- 不执行未经授权的 push、历史改写或破坏性清理。

## Review Focus

- 配置缺失、配置路径过长或配置值非法时，进程必须给出明确错误并退出。
- 守护进程重启时，`downloading` 任务必须恢复为 `queued`，不能丢失进度。
- IPC 请求重复、非法 JSON、未知任务和未知协议必须返回稳定错误，不得导致进程退出。
- 暂停、取消和完成并发发生时，临时文件、SQLite 状态和工作线程必须保持一致。
- 目标路径包含 `..`、符号链接或同名正式文件时，必须遵守明确的安全和覆盖策略。

### Task 1: 项目规则、构建骨架和 clib 接入

**Files:**
- Create: `AGENTS.md`
- Modify: `CMakeLists.txt`
- Modify: `config/download.ini`
- Modify: `README.md`
- Create: `daemon/*.h`
- Create: `daemon/*.c`

**Interfaces:**
- Consumes: clib CMake target/include directory and existing clib public headers.
- Produces: downloader executable target, public include layout, test target layout.

- [ ] 将 clib 作为本项目构建依赖接入，并确认 `clib.h`、IPC、线程、文件、JSON 等头文件可用。
- [ ] 建立 downloader 的 include、daemon、protocols 和 tests 目录。
- [ ] 将配置样例统一为 `config/download.ini`，删除协议启用开关。
- [ ] 编写最小启动程序并通过 CMake 编译。
- [ ] 运行 CMake configure、build 和现有测试。

### Task 2: 配置加载和路径解析

**Files:**
- Create: `daemon/config.h`
- Create: `daemon/config.c`
- Test: `tests/test-config.c`

**Interfaces:**
- Produces: `Config* config_load (int argc, char* argv[])`, `void config_free (Config*)`, `const char* config_path (const Config*)`。

- [ ] 测试命令行配置路径优先级。
- [ ] 测试程序目录配置和 `/etc/downloader/download.ini` 回退路径。
- [ ] 测试三处均不存在时返回明确错误。
- [ ] 使用 clib 的 INI/key-file、字符串和路径能力解析通用配置。
- [ ] 实现数值范围、路径长度和必填项校验。
- [ ] 运行配置单元测试和内存检查。

### Task 3: 任务模型、状态机和 SQLite

**Files:**
- Create: `daemon/task.h`
- Create: `daemon/database.h`
- Create: `daemon/task.c`
- Create: `daemon/database.c`
- Test: `tests/test-database.c`

**Interfaces:**
- Produces: `DownloadTask`, `DownloadTaskState`, `int database_open (...)`, `int task_insert (...)`, `int task_update (...)`, `int task_load (...)`, `int task_clear_completed (...)`。

- [ ] 先写建表和状态转换测试。
- [ ] 建立包含任务 UUID、路径、URL、协议、状态、时间、字节数、进度、重试、校验、HTTP 缓存字段和 JSON 扩展字段的表。
- [ ] 实现数据库初始化和 schema 版本迁移入口。
- [ ] 实现启动恢复：`downloading` 转为 `queued`。
- [ ] 验证参数绑定、事务和 SQLite 错误转换。
- [ ] 运行数据库测试、并发访问测试和内存检查。

### Task 4: 协议接口、注册表和 HTTP 适配器

**Files:**
- Create: `daemon/protocol.h`
- Create: `daemon/protocol.c`
- Create: `protocols/http.c`
- Test: `tests/test-protocol.c`

**Interfaces:**
- Produces: `ProtocolOps`, `ProtocolTask`, `protocol_register (...)`, `protocol_find (...)`, `protocol_open (...)`, `protocol_run (...)`, `protocol_pause (...)`, `protocol_resume (...)`, `protocol_cancel (...)`, `protocol_close (...)`。

- [ ] 测试协议注册、重复注册、按 URL/协议查找和不支持协议错误。
- [ ] 定义统一的 `open/run/pause/resume/cancel/close` 函数指针接口和进度、完成、失败回调。
- [ ] 定义协议统一错误码和能力描述。
- [ ] 使用 libcurl 实现 HTTP/HTTPS 的响应、断点续传、进度回调和错误回调。
- [ ] 将临时文件写入和完成提交交给核心文件模块。
- [ ] 使用本地 HTTP fixture 验证下载、暂停、取消、断点续传和失败恢复。

### Task 5: 文件安全、原子提交和调度器

**Files:**
- Create: `daemon/filesystem.h`
- Create: `daemon/scheduler.h`
- Create: `daemon/filesystem.c`
- Create: `daemon/scheduler.c`
- Test: `tests/test-filesystem.c`
- Test: `tests/test-scheduler.c`

**Interfaces:**
- Produces: `int filesystem_prepare (...)`, `int filesystem_commit (...)`, `int scheduler_start (...)`, `int scheduler_submit (...)`, `int scheduler_pause (...)`, `int scheduler_cancel (...)`, `void scheduler_stop (...)`。

- [ ] 测试路径穿越、非法文件名、已有目标文件和临时文件命名。
- [ ] 实现临时文件、校验、`fsync` 和原子 rename。
- [ ] 使用 clib 线程、线程池、互斥锁、条件变量或队列建立有限并发调度。
- [ ] 实现任务状态与协议回调的一致更新。
- [ ] 验证任务完成、失败、暂停、取消和进程停止时的资源回收。

### Task 6: Unix Socket IPC 和守护进程入口

**Files:**
- Create: `daemon/ipc.h`
- Create: `daemon/ipc.c`
- Modify: `daemon/main.c`
- Test: `tests/test-ipc.c`

**Interfaces:**
- Produces: `int ipc_start (...)`, `void ipc_stop (...)`, `int ipc_handle_request (...)`。

- [ ] 基于 clib IPC 实现 Socket 权限、请求分发和响应编码。
- [ ] 实现 `create/delete/start/pause/resume/cancel/query/list/clear_completed/shutdown`。
- [ ] 测试非法请求、未知操作、未知任务、重复请求和超大消息。
- [ ] 将 IPC 请求交给任务管理器和调度器，不在 IPC 层直接操作协议对象。
- [ ] 实现信号处理、优雅退出和资源释放。
- [ ] 运行端到端测试：启动守护进程、创建任务、查询状态、暂停、恢复和完成。

### Task 7: 后续协议适配器和发布安装

**Files:**
- Create: `protocols/ftp.c`
- Create: `protocols/sftp.c`
- Create: `protocols/bittorrent.c`
- Create: `protocols/metalink.c`
- Modify: `clib/cmake/install.cmake` or root install rules
- Modify: `config/download.ini`

**Interfaces:**
- Consumes: Task 4 的 `ProtocolOps` 和 Task 5 的文件提交接口。
- Produces: 第一阶段协议自动注册和安装后的 `/etc/downloader/download.ini`。

- [ ] 按相同协议接口逐个增加 FTP/FTPS、SFTP、BitTorrent/Magnet 和 Metalink。
- [ ] 对构建环境缺少可选依赖的协议给出编译期或启动期明确结果。
- [ ] 添加本地 fixture 或 mock 测试，不依赖公网。
- [ ] 安装可执行文件、配置文件、Socket 运行目录和必要的 SQLite 初始化路径。
- [ ] 验证安装后配置搜索顺序和 `meta.db` 位置。

