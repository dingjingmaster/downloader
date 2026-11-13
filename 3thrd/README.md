# 内置下载引擎

三个引擎通过 Git submodule 固定源码，由顶层 CMake 自动编译静态库并链接进
downloader。构建过程不下载源码，也不安装或调用 aria2c、ffmpeg、smbclient 命令。

|目录|上游版本|固定提交|用途|
|---|---|---|---|
|aria2c|release-1.37.0|02f2d0d8472b3c38c29b4dba8c75ebd5fdd2899a|libaria2，BitTorrent / Magnet / Metalink 引擎|
|ffmpeg|n8.0.3|8ae0b34901ba60a802f183ee75a250a9fc3e09a5|libavformat / libavcodec / libavutil，HLS / DASH 解复用及封装|
|libsmb2|v6.0.0|b9444620ba7f26ed216a663bf2282a9427e36c36|SMB2 / SMB3 客户端；不包含 SMB1|

## 构建

首次获取源码（需要网络）：

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/downloader --version
```

需要 C/C++ 编译器、CMake >= 3.20、make、autoconf、automake、libtool、gettext、
pkg-config 和 NASM，以及系统开发包：OpenSSL、zlib、Expat、libxml2、SQLite3、libcurl。
已有 clib 继续使用仓库内源码。项目代码仍为 C99；libaria2 的桥接文件使用 C++11。
当前构建入口面向本机 Linux，不支持通过这些参数直接交叉编译。

每个外部库默认使用 4 个编译进程，可通过
`-DDOWNLOADER_DEPENDENCY_JOBS=8` 调整。依赖之间也会并行构建，注意总内存占用。
第三方配置、编译、安装日志位于 `build/3thrd/*-prefix/src/*-stamp/`。
库和头文件仅安装到 `build/3thrd/installed/`，不写系统目录。
aria2 的 autoreconf 在构建目录的副本执行，不修改上游子模块。
更新子模块版本后请使用新构建目录，避免 ExternalProject 复用旧版本的完成标记。

FFmpeg 只编译网络读取、HLS/DASH 等解复用、MP4/Matroska/MPEG-TS 封装及所需
码流过滤器，不编译转码器和命令行程序。libsmb2 不编译示例或 Kerberos 支持。
这不是完全静态的可执行文件：上述系统基础库仍是构建及运行依赖。

## 接入边界

本次建立源码、构建、链接和离线引擎检查入口。已有任务执行仍使用 curl 适配器；
新引擎尚未注册为 `ProtocolOps`，不能据此声称 Web 中已能下载 BT、Magnet、
Metalink、SMB、HLS 或 DASH。后续适配需要处理单进程唯一 aria2 Session、
多文件任务的暂存和提交，以及流媒体暂停恢复，继续通过核心回调报告进度。

`downloader_engines` 测试创建和销毁 aria2 Session、SMB 上下文，并检查
HLS/DASH 输入格式和 MP4 输出格式，完全离线。此测试不替代协议端到端下载测试。

## 许可证

上游源码保留各自许可证：aria2 GPL-2.0-or-later（部分源文件带 OpenSSL 链接例外），
本配置的 FFmpeg 为 LGPL-2.1-or-later，libsmb2 库为 LGPL-2.1-or-later。
发行时需要履行实际组合软件的许可义务，不能把静态链接的 aria2 当作独立外部程序。
安装会同时将上游许可文件放入 `share/licenses/downloader/`。
