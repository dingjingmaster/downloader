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
|云盘 API||
|eD2K/Kad||
|RTSP/RTMP||

新任务只需粘贴链接。程序识别 `magnet:`、`smb:`，以及 HTTP(S) 路径的
`.torrent`、`.meta4`、`.metalink`、`.m3u8`、`.mpd` 后缀（忽略查询参数）。
没有后缀的流媒体地址可使用 `hls+https://…` / `dash+https://…`，也支持对应的 HTTP 形式。
本机种子/Metalink 文件支持 `file:///绝对路径/文件.torrent` 等元数据链接。
SMB 地址格式为 `smb://用户:密码@主机:端口/共享/文件`，特殊字符需 URL 编码；
没有凭据时使用服务器允许的匿名/访客访问。

BT/Magnet/Metalink 每个任务生成一个结果目录，保留资源中的文件层级。
完成前写入任务独立的隐藏临时目录；全部下载、校验并同步后原子提交，已有结果不会被覆盖。
删除未完成任务会清理其临时内容，删除已完成任务只删除记录，保留下载结果。
aria2 当前串行运行这些任务，等待期间也能暂停或删除；其他协议继续由任务池调度。

流媒体保留最佳音频和视频，不转码；暂停后继续会从头重新封装，避免拼接损坏的容器。
总大小无法预先确定时界面显示“未知”，已下载字节持续更新。
暂不支持直播录制、DRM/SAMPLE-AES、字幕及其他备用轨道；SMB1 和 Kerberos 也未接入。
普通 AES-128 HLS 密钥通过 HTTP(S) 获取，不属于上述 DRM 限制。

本地协议测试通过 `ctest --test-dir build --output-on-failure` 运行。流媒体测试使用系统
`ffmpeg/ffprobe` 生成和检查 fixture，SMB 测试使用临时目录和高端口启动 `smbd`；
这些命令只用于测试，downloader 运行不依赖它们。BT 测试自带回环 tracker/peer。
