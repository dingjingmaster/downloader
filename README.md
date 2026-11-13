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
