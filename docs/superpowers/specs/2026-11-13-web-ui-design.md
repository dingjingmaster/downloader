# Downloader Web 界面设计

## 1. 目标

在 downloader 守护进程内嵌一个轻量 HTTP 服务，提供任务管理 Web 页面和 REST API。Web 服务与现有 IPC 共用任务管理器、SQLite 数据库、调度器和协议适配器，不重复实现任务逻辑。

Web 默认开启，默认监听 `0.0.0.0:8000`。

## 2. 配置

配置文件增加：

```ini
[web]
enabled = true
listen = 0.0.0.0
port = 8000
```

- `enabled = false` 时不创建 HTTP 监听线程，也不打开 Web 端口。
- `listen` 默认监听所有 IPv4 地址，允许局域网访问；部署时应结合防火墙和网络边界控制访问范围。
- `port` 必须是 `1` 到 `65535` 的有效端口。
- HTTP 服务启动失败时，守护进程应明确记录错误并退出，不静默降级。

## 3. Web 页面

静态页面放在 `web/` 目录，由 downloader 内置提供：

```text
web/
    index.html
    app.js
    style.css
```

页面功能：

- 分页显示全部任务；
- 显示文件名、协议、资源链接、状态；
- 显示文件总大小、已下载大小、百分比、下载速度；
- 新建下载任务；
- 开始任务；
- 暂停任务；
- 删除任务；
- 清空全部已完成任务；
- 定时刷新当前页任务状态。

页面不直接访问 SQLite 或 Unix Socket，只通过 HTTP REST API 操作。

## 4. REST API

### 查询任务

```text
GET /api/tasks?page=1&page_size=20
```

响应包括：

- 当前页码和页面大小；
- 总任务数；
- 总页数；
- 当前页任务数组。

每个任务至少返回：

```json
{
  "id": "task-uuid",
  "file_name": "example.iso",
  "target_directory": "/backup/download",
  "resource_url": "https://example.test/example.iso",
  "protocol": "https",
  "state": "downloading",
  "total_bytes": 1000000,
  "downloaded_bytes": 250000,
  "progress": 25.0,
  "speed_bytes_sec": 102400,
  "created_at": 0,
  "completed_at": 0
}
```

### 查询单个任务

```text
GET /api/tasks/{id}
```

### 新建任务

```text
POST /api/tasks
Content-Type: application/json
```

请求至少包含：

```json
{
  "url": "https://example.test/example.iso",
  "file_name": "example.iso",
  "target_directory": "/backup/download",
  "protocol": "https",
  "extra": {}
}
```

服务端负责校验 URL、协议、文件名和目标目录，创建任务后默认为 `queued`。

### 开始任务

```text
POST /api/tasks/{id}/start
```

### 暂停任务

```text
POST /api/tasks/{id}/pause
```

### 删除任务

```text
DELETE /api/tasks/{id}
```

删除任务默认删除任务记录和未完成临时文件，不删除已完成正式文件。

### 清空已完成任务

```text
POST /api/tasks/clear-completed
```

## 5. HTTP 服务边界

HTTP 服务只负责：

- 监听 TCP 地址和端口；
- 解析有限范围内的 HTTP 请求；
- 解析 JSON 请求体；
- 返回 JSON 或静态资源；
- 将操作转交给任务管理器。

HTTP 服务不直接操作：

- SQLite；
- 协议对象；
- 下载文件；
- 调度线程内部状态。

第一阶段只支持：

- `GET`；
- `POST`；
- `DELETE`；
- `Content-Type: application/json`；
- 固定大小请求体上限；
- HTTP/1.1 短连接。

不实现 WebSocket。页面使用定时轮询刷新任务进度。

## 6. 任务管理器

新增任务管理器作为 IPC 和 Web API 的共同后端，负责：

- 创建任务并写入 SQLite；
- 查询任务和分页数据；
- 删除任务；
- 开始、暂停、恢复任务；
- 清空已完成任务；
- 将协议回调转换为任务状态和进度更新；
- 保护任务对象、调度器和数据库之间的并发访问。

IPC 和 HTTP API 不再各自实现任务语义。

## 7. 安全边界

- 默认监听 `0.0.0.0`，允许局域网访问。
- 限制 HTTP 请求头和请求体大小。
- 拒绝路径穿越和非法文件名。
- 不允许通过 Web API 修改配置或执行进程级命令。
- 所有错误返回统一 JSON 格式，不泄露内部路径以外的敏感信息。
- 远程访问、认证和 TLS 不属于第一阶段范围。

## 8. 目录结构

```text
daemon/
    task-manager.c
    task-manager.h
    web.c
    web.h
web/
    index.html
    app.js
    style.css
tests/
    test-task-manager.c
    test-web.c
```

