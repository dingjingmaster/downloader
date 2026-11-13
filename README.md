# 下载器

当前项目正在构建一个基于 Unix Domain Socket 的后台下载服务。项目基础能力优先使用仓库内的 `clib`，下载协议通过统一的函数指针接口接入。

## 构建

```sh
cmake -S . -B build
cmake --build build
```

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
|HTTP||
|HTTPS||
|FTP|libcurl 适配器|
|SFTP|libcurl 适配器，取决于 libcurl 构建能力|
|BitTorrent|待接入 BT 引擎|
|Magnet|随 BitTorrent 引擎接入|
|Metalink|待接入多源解析器|
|WebDAV||
|SMB||
|HLS||
|MPEG-DASH||
|S3/对象存储||
|云盘 API||
|eD2K/Kad||
|RTSP/RTMP||
