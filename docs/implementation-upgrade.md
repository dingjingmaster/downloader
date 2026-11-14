# 下载能力升级实施方案

> 执行方式：使用 writing-plans / executing-plans 和 dispatching-parallel-agents，按检查项推进。用户已授权实施；全部验收完成后删除本临时文档，未完成时保留实际进度。不自动提交、推送或重启用户正在运行的服务。

## 目标与约束

在现有 C99、CMake、clib、ProtocolOps、SQLite、Web 架构上完成并发、可靠恢复、认证代理、流媒体录制及 SMB 兼容升级。头文件与源文件同目录，在当前仓库工作，协议自动注册。协议不得直接访问 SQLite。所有新行为用本地 fixture 验证；更新数据库必须兼容旧任务。第三方源码固定版本放在 3thrd，运行不调用外部下载命令。

范围：阶段 1～6；DRM 只做识别和提供器接口，商业平台 SDK/许可证与解密输出不在本轮范围。Cookie 导入和标准 HTTP 认证属于本轮；任意网站验证码、OAuth/网页登录提供器属于后续独立适配。

## 设计决策

1. aria2 使用一个专用线程和单一长期 Session，任务命令排队，多 GID 归属一个任务。拒绝删除全局锁后同时创建 Session 的做法；独立进程方案仅作为未来隔离选择。
2. HTTP 持久化最终有效 URL、校验器、可靠落盘字节和请求上下文标识。恢复须确认同一资源；不同 URL 的同值 ETag 不证明资源相同，不删除签名查询参数猜测身份。跨源请求不转发敏感认证。
3. 媒体使用持久化分片后封装；复用 FFmpeg，不写编解码器。恢复只补下载缺失分片，本地封装中断可重新封装。记录轨道、初始化段、字节范围、序号/时间线、加密引用。直播在其上增加刷新、结束并保存、限时/限量、分卷和缺口报告。
4. Kerberos 使用 libsmb2 + GSSAPI/Kerberos，保留服务器原名与 SPN；票据上下文隔离。SMB1 使用 Samba libsmbclient 兼容后端，先验证可构建性和取消/线程边界，认证失败不触发协议降级。
5. 请求策略集中在 RequestContext：任务覆盖站点覆盖全局。CredentialStore 保存受保护凭据，任务只有引用；代理能力按实际引擎验证，不支持强制代理时拒绝直连。远程 Web 凭据管理须有认证、CSRF 和 HTTPS 部署支持。

## 公共接口与数据

- ProtocolOps 保持统一函数指针接口；新增可选 capability/finish 等接口时更新已有初始化器和测试。
- ProtocolCallbacks 新增版本化 checkpoint/event 回调，核心负责数据库事务；异步引擎事件回到任务所属执行线程，避免回调生命周期悬空和锁反转。
- DownloadTask 增加恢复检查点运行视图；数据库迁移使用 user_version 和事务，保留既有字段、状态及任务数据。
- task_checkpoints 保存任务恢复信息；task_segments 保存频繁更新的媒体分片；credential_profiles/proxy_profiles 保存配置和凭据引用，秘密不出现在普通 JSON/日志。
- 数据先 fsync，再标记检查点完成；恢复核验文件；输出 fsync 后原子提交。任务删除与后台完成事件必须协调。
- 状态与阶段分离，表达获取元数据/下载/录制/封装/校验，以及等待认证和缺片完成结果。

## 实施任务

### 1. HTTP 检查点与重定向恢复
- [x] 测试：相同最终地址暂停/重启续传；目标变化同 ETag 重下；旧库迁移；崩溃窗口。
- [x] 测试：跨域凭据隔离（第 3 项补充覆盖：跨主机重定向不转发 Authorization/Cookie）。
- [x] 修改 daemon/database.[ch]、task.h、task-manager.c、protocol.h：版本化检查点回调与持久化。
- [x] 修改 protocols/http.c：资源身份验证、安全续传、持久化顺序。
- [x] 验证数据库、核心、HTTP 原有及新增测试。

### 2. aria2 并发
- [x] 测试：两个本地下载同时取得进度；单任务暂停删除不影响其他任务；Magnet/Metalink 子 GID 隔离。
- [x] 修改 protocols/aria.cpp、aria.h、engines.cpp：专用线程、共享 Session、任务/GID 映射、受控关闭。
- [x] 保持核心全局并发预算，独立临时目录，迁移 DHT 状态到引擎级。
- [x] 验证 BT、Magnet、Metalink、取消与退出测试。

### 3. 认证、代理与 Web 配套
- [x] 测试：Cookie 域/路径/有效期与账户隔离、Basic/Digest/Bearer、代理认证与 DNS、无意外直连、Web 未认证访问/CSRF。
- [x] 新增 daemon/request-context.[ch]、credential-store.[ch] 等公共模块，使用成熟密码库保护秘密，密钥独立权限管理。
- [x] 修改配置、任务核心和各适配器：统一解析配置和能力校验；Cookie 更新集中合并；跨域不泄露秘密。
- [x] 修改 daemon/web*、web/*：登录会话、凭据/代理配置及任务选择、HTTPS 部署说明。
- [x] 验证 API、Web、HTTP、引擎适配和存储权限测试。

### 4. 媒体分片恢复与 DRM 识别
- [x] 测试：HLS/DASH 中断重启不重取完整分片、BYTERANGE/初始化段、轨道固定、AES 密钥轮换、DRM 拒绝且明确原因。
- [x] 修改 protocols/media.[ch]，按职责增加分片/清单模块；持久化、校验和核心回调管理。
- [x] 完成最后封装后再提交，磁盘满/封装失败保留分片，成功后清理。
- [x] 增加 DRM 提供器接口及已识别未支持方案的清晰错误，不伪装商业 DRM 可下载。
- [x] 验证本地点播与故障恢复测试。

### 5. 直播
- [x] 测试：动态 HLS、窗口滑动、缺片、结束并保存（显式 finish）、限时/限量。
- [ ] 测试：直播暂停后重启（重建输入时无法稳定恢复编码参数）；播放列表自然 ENDLIST 结束（当前 fixture 报 corrupt packet）；分卷录制；动态 DASH 的本地 MPD 验证（代码已允许 dynamic 类型）。
- [x] 新增直播刷新/片段去重，区分暂停、取消、结束保存。
- [x] API/IPC/Web 增加结束录制，显示时长、大小及缺口，不展示未知总长百分比。
- [x] 普通 HLS 完成；低延迟 HLS、复杂编码切换和动态 DASH 验证尚未宣称支持（README 明确边界）。

### 6. SMB 认证与旧协议兼容
- [x] 验证并固定 Samba/Kerberos 依赖，更新 3thrd 构建与许可证。
- [x] 测试：本地 Kerberos KDC/SMB、主机名 SPN、并发身份隔离；SMB1 下载/暂停/删除/错误密码不降级。
- [ ] 测试：Kerberos 票据过期（需要短生命周期票据与等待窗口；本轮未覆盖）。
- [x] 修改 protocols/smb.c，按需要新增 SMB1 后端；超时和取消可中断，旧 NTLM/SMB2/3 不回归。
- [x] 更新配置/UI 凭据选择和说明。

## Review Focus

- 跨域跳转和多账户并发不得泄漏 Cookie、Authorization、代理秘密。
- 崩溃和磁盘满不得发布拼接错误、缺片却声称完整的结果。
- 引擎线程退出、暂停/删除与回调竞态不得使用释放后的任务。
- 代理策略不得仅覆盖清单或 tracker 而泄漏分片、密钥、peer/DHT 直连。
- 旧数据库迁移失败应回滚，不能丢用户任务；旧任务无检查点须安全降级恢复。

## 总体验收及文档生命周期

- [x] cmake --build build -j4 成功。
- [x] ctest --test-dir build --output-on-failure -j4 全部通过（25/25）。
- [x] 修改代码的静态检查、node --check web/app.js、git diff --check 通过。
- [x] 独立代码审查；所有影响正确性的发现均修复并补回归（aria 失败路径的修复有 ruling，见 ledger）。
- [x] README 与 3thrd/README 记录实际能力、配置、边界，不能把未验证能力标为完成。
- [ ] 全部实现并验收后删除本文件；当前保留，剩余事项见第 5、6 项与执行记录。

## 执行记录

- 起始工作区干净；已确认现有 19 项测试记录通过。
- 2026-11-13 完成第 1 项：
  - 数据库迁移到 `user_version=1`，新增 `task_checkpoints`，迁移在事务内完成，失败回滚且不丢任务；已测旧库、冲突对象回滚、更高版本拒绝。
  - `ProtocolCallbacks` 新增版本化 `onCheckpoint`；任务管理器先校验文件长度，恢复时截断未 fsync 的尾部，缺失数据时丢弃检查点并安全重下。
  - HTTP 持久化最终有效 URL、校验器和请求上下文标识；续传要求最终 URL 与请求上下文一致，同 ETag 不同地址仍从头下载；暂停和崩溃后按检查点确认的字节续传。
  - 独立代码审查发现 1 个重要问题（未收到 2xx 的运行会用空 URL 覆盖检查点）和 1 个重要回归（过长最终 URL 使本来有效的下载失败），均已按 TDD 补回归并修复；另有 4 项次要健壮性问题记录在 `.superpowers/sdd/implementation-upgrade/progress.md`，本轮未处理。
  - 验证：`cmake --build build -j4` 成功；`ctest --test-dir build --output-on-failure -j4` 19/19 通过；HTTP resilience 39/39；`-Wall -Wextra -Werror` 语法检查通过；`node --check web/app.js`、`git diff --check` 通过。
  - 跨域凭据隔离测试由第 3 项的 HTTP 认证集成测试覆盖（跨主机重定向不转发 Authorization/Cookie）。
  - 按项目约定未自动提交，改动保留在工作区。
- 2026-11-13 完成第 6 项（除票据过期测试）：
  - 启用 libsmb2 的 LibKrb5/GSSAPI 构建；新增 `smb.auth=kerberos` + `smb.domain` 选项，使用隔离的 MEMORY 票据缓存（用户名/密码获取票据），SPN 使用 URL 主机名且不含端口。
  - 新增 `smb1://` 兼容后端（Samba libsmbclient，强制 NT1、禁止自动匿名回退、30s 超时、按 64KiB 检查暂停/取消）；认证失败不会降级到 SMB2/3 或 guest。
  - 本地 KDC + Samba keytab 验证：Kerberos 下载、错误密码不降级、并发隔离会话；SMB1 下载、错误密码、暂停/重启、活动删除。旧 NTLM/SMB2/3 测试不回归。
  - 第三方说明记录 libsmb2 两处补丁、krb5/GSSAPI 系统依赖和 libsmbclient（GPL）链接边界。
  - 未完成：票据过期测试；不同系统身份的并发隔离（环境只有一个系统用户）；SMB1 暂停后从头重启（libsmbclient 无范围续传）。
  - 按项目约定未自动提交，改动保留在工作区。
- 2026-11-13 完成第 5 项（除分卷与动态 DASH 本地验证）：
  - HLS 直播由 FFmpeg demuxer 刷新清单并滑动窗口/去重；清单不缓存，分片继续按任务持久化。
  - `extra.record.duration`（媒体时间线秒数）与 `extra.record.size`（落盘字节）达到后优雅结束并提交；`ProtocolOps.stop` 支持“结束录制”，经 API/IPC/Web 调用。
  - 数据库迁移到 user_version=3，新增录制时长与缺口字段；Web 显示录制时长/缺口，未知总长不显示虚假百分比。
  - 缺片会累计缺口并让任务明确失败，不发布伪完整录制；失败/暂停保留分片。
  - 验证：媒体测试 18 项通过（动态 HLS 窗口、限时、限量、显式结束录制、缺片缺口、VOD/DRM/分片复用；VOD 上的 finish 不再发布截断文件）。
  - 未完成：直播暂停后重启（FFmpeg 直播 demuxer 在重建输入时无法稳定恢复编码参数）、自然 ENDLIST 结束（fixture 下报 corrupt packet）、分卷录制、动态 DASH 的本地动态 MPD 验证；低延迟 HLS/复杂编码切换不宣称支持。
  - 按项目约定未自动提交，改动保留在工作区。
- 2026-11-13 完成第 4 项：
  - 清单、初始化段和媒体分片按 URL + 字节范围选项的 SHA-256 键持久化到任务旁的私有 `.segments` 目录；仅在读到 EOF 或达到 `end_offset-offset` 时标记完整，恢复时只补缺失资源。加密密钥（`crypto+`）不缓存，避免密钥轮换读到旧密钥。
  - 成功后清理分片缓存；暂停/失败保留；删除任务时由核心清理 sidecar。封装仍由本地 FFmpeg 完整重建，不在损坏容器上追加。
  - 新增 `media-drm.[ch]`：识别 Widevine、PlayReady、FairPlay、SAMPLE-AES 并以“DRM <名称> 不受支持”明确拒绝，不宣称可下载商业 DRM。
  - 验证：媒体测试 13 项通过（HLS/DASH 暂停恢复不重取已完成分片、DASH 初始化段、HLS BYTERANGE、AES-128 单钥与轮换、DRM 识别、失败保留分片、删除清理）；完整 ctest 随后执行。
  - 按项目约定未自动提交，改动保留在工作区。
- 2026-11-13 完成第 3 项：
  - 新增 `credential-store`（AES-256-GCM，密钥文件 0600/单链接校验，数据库迁移到 user_version=2）和 `request-context`（任务 > 最长站点后缀 > 全局默认；缺失配置明确失败）。
  - HTTP 支持 Basic/Digest/Bearer、按任务隔离的 Cookie 引擎与 `extra.http.cookies` 导入、强制认证代理（清空 no_proxy，不直连）；跨主机重定向不转发 Authorization/Cookie；401/407 允许 libcurl 完成认证协商。
  - 非 curl 引擎（BT/Magnet/Metalink、HLS/DASH、SMB）在解析出认证/代理策略时返回 ENOTSUP，不静默直连。
  - Web 增加登录会话与 CSRF、凭据/代理/站点管理 API、前端登录与配置选择；`[web] username/password` 未配置时保持本地兼容模式，README 说明 HTTPS 反向代理边界。
  - 验证：23 项 ctest 全部通过（含新增 credential/request-context/web-auth/http-auth 测试）；HTTP 认证集成 9 项通过；Web e2e 覆盖 401/登录/CSRF/退出；`node --check web/app.js`、严格 C 编译通过。
  - 已知边界：Cookie 会话不跨守护进程重启持久化；站点覆盖仅提供 API；登录无速率限制；密码配置为明文（内存中哈希、常量时间比较），远程部署必须由反向代理终止 HTTPS。
  - 按项目约定未自动提交，改动保留在工作区。
- 2026-11-13 完成第 2 项：
  - aria2 改为进程级专用引擎线程 + 单一长期 Session；下载 worker 通过命令队列提交任务并等待快照，所有回调仍在任务所属 worker 线程执行。
  - 每个任务维护自己的 GID 集合（含 Magnet/Metalink 跟随子 GID）；暂停/删除只移除该任务的 GID，其他任务继续。已完成 GID 的终止数据缓存在任务内，避免长期 Session 的 `max-download-result` 裁剪破坏活动任务。
  - `ProtocolOps` 增加可选 `finish`，主程序在所有 worker 退出后调用 `protocol_finish_all()` 受控关闭引擎；DHT 状态迁移到引擎级缓存目录（跨进程用文件锁互斥，竞争时回退到进程私有目录）。
  - 验证：`cmake --build build -j4` 成功；`ctest --test-dir build --output-on-failure -j4` 19/19 通过；BT 集成 15 项断言通过（含并发进度、暂停隔离、Magnet/Metalink 子 GID 隔离、活动任务退出）；aria 适配器测试通过；`-Wall -Wextra -Werror` 编译通过。
  - 按项目约定未自动提交，改动保留在工作区。
