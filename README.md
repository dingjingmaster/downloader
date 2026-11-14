# 下载器

当前项目正在构建一个基于 Unix Domain Socket 的后台下载服务。项目基础能力优先使用仓库内的 `clib`，下载协议通过统一的函数指针接口接入。

## 构建

```sh
git submodule update --init --recursive
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

aria2、FFmpeg、libsmb2 源码位于 `3thrd/`，由 CMake 构建静态库并链接到程序。
构建依赖、固定版本、许可证和协议接入边界见 [第三方引擎说明](3thrd/README.md)。
`./build/downloader --version` 可查看内置引擎版本。

## 打包安装

`pack/` 提供 DEB、RPM 和 Gentoo overlay 三种安装包，以及 Gentoo 一键安装脚本：

```sh
make -C pack                  # 在 pack/dist/ 生成三种包
sudo pack/tools/install-gentoo.sh
```

安装到 `/opt/downloader`，配置在 `/etc/downloader/download.ini`，数据和缓存分别位于
`/var/lib/downloader` 和 `/var/cache/downloader`。服务默认只监听 `127.0.0.1:8000`。
详细依赖、布局、安装与卸载行为见 [pack/README.md](pack/README.md)。

## 配置

启动时按以下顺序查找配置文件：

1. 命令行指定的配置文件；
2. 程序目录下的 `config/download.ini`；
3. 安装后的 `/etc/downloader/download.ini`。

## Web 界面

默认启动 Web 界面，监听 `0.0.0.0:8000`，浏览器访问 `http://主机地址:8000/`。
配置文件中的 `[web] enabled = false` 可以关闭 Web 监听，但不会关闭 Unix Socket IPC。
页面支持分页查看任务，以及新建、开始、暂停、删除和清空已完成任务。

Web 静态资源安装到 `${CMAKE_INSTALL_PREFIX}/share/downloader/web`。默认监听所有 IPv4 地址时，应通过主机防火墙或网络边界限制访问范围。

## 协议列表

|协议|是否支持|
|:---|:-------|
|HTTP|libcurl 适配器|
|HTTPS|libcurl 适配器|
|FTP|libcurl 适配器|
|SFTP|libcurl 适配器，取决于 libcurl 构建能力|
|BitTorrent|内置 libaria2，种子、单文件和多文件、校验与续传|
|Magnet|内置 libaria2，获取元数据后下载实际文件|
|Metalink|内置 libaria2，Metalink 3/4，多文件与校验|
|WebDAV||
|SMB|内置 libsmb2，SMB2/3、NTLMSSP、断点续传|
|HLS|内置 FFmpeg，HTTP(S) 点播、主播放列表、AES-128，封装为 MKV|
|MPEG-DASH|内置 FFmpeg，HTTP(S) 静态点播，封装为 MKV|
|S3/对象存储||
|云盘 API|百度网盘：Cookie 登录；自己网盘文件/目录；分享链接转存后下载|
|eD2K/Kad||
|RTSP/RTMP||

新任务只需粘贴链接。程序识别 `magnet:`、`smb:`，以及 HTTP(S) 路径的
`.torrent`、`.meta4`、`.metalink`、`.m3u8`、`.mpd` 后缀（忽略查询参数）。
没有后缀的流媒体地址可使用 `hls+https://…` / `dash+https://…`，也支持对应的 HTTP 形式。
本机种子/Metalink 文件支持 `file:///绝对路径/文件.torrent` 等元数据链接。
SMB 地址格式为 `smb://用户:密码@主机:端口/共享/文件`，特殊字符需 URL 编码；
没有凭据时使用服务器允许的匿名/访客访问。Kerberos 使用
`extra = {"smb": {"auth": "kerberos", "domain": "REALM"}}` 和 URL 中的用户/密码获取
隔离的内存票据缓存（SPN 为 `cifs/<主机名>`，需正确配置 `KRB5_CONFIG`）；认证失败不会
降级到 NTLM/匿名。SMB1-only 服务器可用 `smb1://` 兼容后端（系统 Samba libsmbclient，
强制 NT1、禁止自动匿名回退）；SMB1 的暂停后恢复会从头下载，且 libsmbclient 采用
GPLv3，链接它会影响分发的许可证义务，打包前需确认。

BT/Magnet/Metalink 每个任务生成一个结果目录，保留资源中的文件层级。
完成前写入任务独立的隐藏临时目录；全部下载、校验并同步后原子提交，已有结果不会被覆盖。
删除未完成任务会清理其临时内容，删除已完成任务只删除记录，保留下载结果。
aria2 使用独立引擎线程和单一长期 Session 并发运行这些任务，核心任务池仍限制总并发；
单个任务的暂停或删除不会阻塞其他任务，DHT 状态集中在引擎级缓存目录。其他协议继续由任务池调度。

流媒体保留最佳音频和视频，不转码。清单、初始化段和分片按 URL 与字节范围持久化在
任务旁的私有目录：暂停/崩溃后只补下载缺失分片，再在本地重新封装，从不在损坏的容器上
追加；成功提交后清理分片缓存，失败或暂停时保留以便重试。总大小无法预先确定时界面
显示“未知”，已下载字节持续更新。
DRM（Widevine、PlayReady、FairPlay、SAMPLE-AES）只做识别并明确拒绝，不会伪装成可下载；
普通 AES-128 HLS（含密钥轮换）通过 HTTP(S) 获取密钥，不属于上述 DRM 限制。

HLS 直播可以录制：清单按目标时长刷新，滑动窗口和片段去重由 demuxer 处理；清单不缓存，
已完成分片继续复用。任务 `extra.record.duration`（秒）或 `extra.record.size`（字节）达到
后优雅结束并提交；也可通过 API/IPC 的 `finish` 或 Web 的“结束录制”保存。界面显示录制
时长、大小和缺口数，未知总长不显示虚假百分比。缺片会累计为缺口并明确失败，不发布伪
完整录制。分卷录制、动态 DASH 的本地验证、低延迟 HLS 和复杂编码切换尚未支持，不会隐式
宣称可用。字幕及其他备用轨道尚未接入；SMB1/Kerberos 的能力与限制见上文。

## 百度网盘

新增任务时，认证配置选择 `Cookie` 类型，粘贴浏览器网盘页面的完整 Cookie 头（至少包含
`BDUSS` 和 `STOKEN`；`STOKEN` 是网盘页面里的 Cookie，不是 `bdstoken`）。`baidu://`
任务没有主机名，按 `pan.baidu.com` 匹配站点配置，也可以直接选择显式认证配置或设置
`[auth] default_profile`。

- 自己网盘：`baidu:///绝对/路径`。目录会递归展开成每个文件一条任务，本地保留网盘层级。
- 分享链接：`https://pan.baidu.com/s/xxxx`，提取码写入 `extra.baidu.pwd`
  （Web 表单的“百度提取码”）。可选 `extra.baidu.path` 只展开分享内的文件或子目录；
  百度转存接口只能按分享根条目转存，因此选择子目录中的文件时仍会转存包含它的根条目。
- 分享下载会先把根条目**转存到自己的网盘**再下载，目录由 `[baidu] transfer_directory`
  决定，实际目录为 `<transfer_directory>/<surl>`（默认 `/downloader/<surl>`）；会占用
  网盘空间，程序不自动删除。
- 同一分享重复创建会复用已转存内容，只补缺失的根条目；同名但大小/MD5 不同会报
  `baidu_transfer_conflict`，不会覆盖。转存复用只比较根层，已存在目录内部的缺失文件
  不会自动补齐；需要完整重建时先删除云端转存目录。
- `[baidu] max_parallel_tasks` 限制同时下载数（默认 2），`max_files` 限制单次展开数量
  （默认 2000，超出返回 `baidu_share_too_large`）。
- 所有百度请求依赖非公开网页 API；Cookie 失效、页面改版或触发验证码时任务会给出明确错误，
  不会绕过验证或限速。建议使用小号，并避免把 Cookie 写入日志或分享。
- 测试使用本地 mock，通过 `[baidu] api_base`/`pcs_base` 指向回环地址；生产环境不要修改
  这两个值。

本地协议测试通过 `ctest --test-dir build --output-on-failure` 运行。流媒体测试使用系统
`ffmpeg/ffprobe` 生成和检查 fixture，SMB 测试使用临时目录和高端口启动 `smbd`；
这些命令只用于测试，downloader 运行不依赖它们。BT 测试自带回环 tracker/peer。

## HTTP 下载可靠性

HTTP/HTTPS 文件下载默认发送下载器的 User-Agent。新建任务中的“HTTP 下载选项”默认折叠，
可指定 User-Agent 和资源页面 Referer；也可以通过 API 提交：

```json
{
  "url": "https://example.org/file.bin",
  "extra": {
    "http": {
      "user_agent": "downloader/0.1.0",
      "referer": "https://example.org/downloads"
    }
  }
}
```

`[download] max_retries` 是单次执行内的额外尝试次数；耗尽后任务不会被标记为失败，
任务管理器会把临时网络故障重新排队并一直重试，退避从 1 秒递增、封顶 60 秒。
HTTP 408、429、500、502、503、504 及连接超时/重置、传输中断等按此策略持续重试，
支持 `Retry-After`；401、403、404、证书错误、磁盘写入错误等永久错误仍然直接失败。
暂停、删除会立即中断等待中的重试；守护进程重启后会自动恢复排队中的任务。
界面显示累计重试次数和最近一次失败原因。

HTTP(S) 文件在服务器支持 `Range` 且大小至少 1 MiB 时，默认使用 3 个连接并行下载：
启动前用 HEAD 探测大小、有效 URL 和校验器，三个分片分别写入
`<临时文件>.segments/` 并周期性 `fsync`；暂停、崩溃或网络中断后只补齐缺失分片，
全部完成后按顺序拼接成临时文件，再由核心校验并原子重命名。服务器不支持 HEAD、
忽略 Range、资源小于 1 MiB，或分片校验失败时自动回落到原有的单连接下载路径，
不会发布拼接不完整的结果。Web 任务列表持续显示每个任务的实时下载速度。

`connect_timeout` 控制 curl 下载的连接阶段；`transfer_timeout` 控制本次执行的总时长，
包含各次尝试和退避等待，0 表示不设置总时限。这两个选项目前用于 curl 适配器，
并不改变 BT、SMB 或流媒体引擎各自的超时策略。

HTTP 续传保存并使用强 ETag 或 Last-Modified，通过 `If-Range` 和 `Content-Range`
确认文件及偏移。资源改变、服务器忽略 Range 或偏移失效时会安全重新下载。
旧任务没有保存这些校验信息时，也会从头下载，避免把两个不同版本拼成一个文件。
错误的部分响应不会被当成完整文件发布；重定向仅允许 HTTP(S)，最多 10 次。
416 偏移失效允许一次从头恢复，不占用临时故障重试次数。

续传身份绑定“最终有效 URL + 请求上下文（User-Agent/Referer 选项）”，因此经过
重定向的下载在暂停后仍可继续；目标地址或请求选项变化时，即使 ETag 相同也会
从头下载。临时文件按检查点先 `fsync` 再持久化已确认字节，进程崩溃后只续传检查点
确认过的部分，未同步的尾部会被丢弃。旧数据库在启动时事务化迁移到新版本；旧任务
没有检查点时按原文件大小和已保存的校验信息安全降级恢复。

登录权限、已过期的临时签名链接和服务器访问策略仍需对应的有效凭据或新链接。
新增的通用重试不会绕过这些限制，也不会关闭 TLS 证书验证。

## 认证、代理与 Web 安全

命名认证配置（Basic/Digest/Bearer）和代理配置保存在 SQLite 中，秘密使用
AES-256-GCM 加密，密钥文件默认是 `<database>.key`，首次创建时为 0600 且必须是
普通单链接文件；权限过宽时程序拒绝启动。秘密不会出现在任务 JSON、日志或 Web 列表中。
Web API 的管理入口：

- `GET/POST /api/auth-profiles`、`DELETE /api/auth-profiles/<名称>`
- `GET/POST /api/proxy-profiles`、`DELETE /api/proxy-profiles/<名称>`
- `GET/POST /api/sites`（按域名后缀选择认证/代理配置）

任务通过 `extra.auth_profile` / `extra.proxy_profile` 引用配置名；优先级为任务 >
最长域名后缀的站点配置 > `[auth] default_profile` / `[proxy] default_profile`。
配置了代理时不会再回退到直连（即使环境变量设置了 `no_proxy`）；libcurl 之外的
引擎（BT/Magnet/Metalink、HLS/DASH、SMB）尚不能执行该策略，此时任务明确失败而不是
静默直连。HTTP Cookie 使用 libcurl 的 Cookie 引擎按任务隔离，导入 Cookie 通过
`extra.http.cookies` 数组（每个元素是 `name=value; Path=/ ...` 形式的 Set-Cookie 值）。

`[web] username` 和 `password` 都非空时启用登录会话与 CSRF 校验：`POST /api/login`
返回会话 Cookie 和 CSRF 令牌，所有 `/api/*`（登录除外）要求会话，修改类请求还要求
`X-CSRF-Token`。内置 Web 监听只有 HTTP；远程部署必须在前置反向代理终止 HTTPS，并
限制监听地址。未配置用户名/密码时保持本地无认证兼容模式，不应直接暴露到公网。
