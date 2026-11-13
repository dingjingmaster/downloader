# Downloader 下载执行层设计

## 1. 目标

把当前 `scheduler` worker 从 `ENOTSUP` 替换为真实下载执行链。第一阶段完成 HTTP/HTTPS 的本地可控下载，支持任务开始、暂停、恢复、取消、进度持久化、失败记录以及 `.part` 文件完成后的 `fsync + rename` 原子提交。

## 2. 已确定的架构

采用统一协议接口和任务管理器集中编排：

```text
Web / IPC
    -> TaskManager
        -> SQLite 状态与进度
        -> Scheduler
            -> DownloadExecution
                -> ProtocolOps
                    -> progress/completed/failed 回调
```

- scheduler 不理解协议，只执行任务 worker。
- 协议模块不直接访问 SQLite、IPC 或全局任务状态。
- TaskManager 持有每个运行任务的协议上下文和控制状态。
- 协议回调只报告事件，TaskManager 负责更新 `DownloadTask` 并写入数据库。
- 下载内容始终写入任务的 `tempPath`，完成校验和 `fsync` 后调用 `filesystem_commit()`。

## 3. 第一阶段范围

包含：

- `http://` 和 `https://` 使用现有 libcurl 协议适配器；
- 从 `.part` 文件大小恢复下载偏移；
- 任务状态：`created -> queued -> downloading -> completed/failed`；
- `pause` 让当前协议执行返回可恢复的暂停结果；
- `resume` 重新提交任务并继续 `.part` 文件；
- `delete/cancel` 停止执行并删除未完成临时文件；
- 进度、总字节、已下载字节、速度和时间写入 SQLite；
- 本地 HTTP fixture 测试真实下载和失败场景。

不包含：

- BitTorrent、Magnet、Metalink 的执行引擎；
- 多连接分片下载；
- TLS 证书校验策略以外的认证 UI；
- 完成文件的哈希校验扩展；
- 真实公网下载测试。

## 4. 运行上下文

每个活动任务拥有一个 `DownloadExecution`：

- `DownloadTask task`：worker 使用的任务快照；
- `ProtocolContext* protocolContext`：协议适配器私有上下文；
- `volatile bool pauseRequested`；
- `volatile bool cancelRequested`；
- 回调指向 TaskManager；
- 由 TaskManager 互斥锁保护上下文表。

worker 只调用统一的 `open/run/close` 生命周期。`pause/cancel` 设置控制标志并调用协议操作表中对应的 cooperative 操作；协议回调必须周期性检查控制状态。

## 5. 状态和错误语义

- 创建任务写入 `created`。
- 开始或恢复成功提交 scheduler 后写入 `queued`。
- worker 真正开始协议传输后写入 `downloading`。
- 用户暂停后写入 `paused`，保留 `.part` 文件和已下载字节。
- 下载完成且原子提交成功后写入 `completed` 和 `completedAt`。
- 网络、协议、文件或提交错误写入 `failed`、`errorCode`、`errorMessage`。
- 删除任务先请求取消，worker 结束后删除数据库记录和 `.part` 文件；已完成正式文件不删除。

## 6. 并发边界

- TaskManager 的互斥锁保护活动执行表和数据库更新。
- 同一 UUID 同时只能有一个 execution。
- progress 回调不能在协议线程中直接执行 SQLite SQL；必须调用 TaskManager 回调。
- 删除/暂停与 worker 完成并发时，以取消/暂停控制状态和最终 worker 结果为依据，禁止恢复已删除任务记录。

## 7. 测试策略

- 使用本地 loopback HTTP server 或静态 fixture，不访问公网。
- 测试完整下载后正式文件内容正确、`.part` 不残留、数据库为 `completed`。
- 测试服务中断后任务为 `failed`，错误信息持久化。
- 测试预置 `.part` 后恢复下载偏移。
- 测试暂停后不再推进字节，恢复后继续下载。
- 测试删除任务不会删除已完成正式文件。

