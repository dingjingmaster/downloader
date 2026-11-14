# 实施计划：无限网络重试、Web 速度、HTTP 三线程分片续传

## Task 1: 网络无限重试与自动恢复
- [x] tests: HTTP 临时故障耗尽后管理器继续重试并最终完成；403 仍失败；暂停/删除中断重试；崩溃重启自动恢复。
- [x] ProtocolOps.retryable；任务管理器重试队列线程（退避封顶 60s）；启动时恢复 queued 任务。
- [x] 更新受影响的 resilience/download 测试（exhausted→recovered、超时重试、崩溃自动恢复）。

## Task 2: Web 速度显示
- [x] Web 列表新增速度列，格式化 B/s…；空值显示 —；node --check。

## Task 3: HTTP 三线程分片与续传
- [x] tests: HEAD+Range fixture 断言 3 个并发 Range；暂停/重启只补缺失分片；不支持 Range 回落单流；小文件单流。
- [x] HEAD 探测大小/有效 URL/校验器；`<temp>.segments` 元数据与分片文件（每 1 MiB fsync）；3 个 clib 线程；暂停/取消；完成拼接与清理。
- [x] 保持现有单流路径与测试不变（无 HEAD 的 fixture 自然回落）；分片 401/407 交给 libcurl 协商，412/416 回落单流。
- 未共享 Cookie jar：各分片句柄独立导入任务 Cookie，服务端在分片响应中新建的 Cookie 不跨分片共享。

## 验收
- [x] cmake --build build -j4；ctest --test-dir build --output-on-failure -j4 全绿（26/26）；node --check；git diff --check；提交并 push。
- 验证记录：HTTP resilience 39 项、parallel 4 项、download/media 全部通过；strict 编译见提交前检查。
