# downloader 打包

`pack/` 以同一份 rootfs 产出 DEB、RPM 和 Gentoo overlay，并提供 Gentoo 一键安装脚本。
三种格式安装相同的内容与目录布局。

## 依赖

构建机需要：

- CMake、make、C/C++ 编译器、strip、pkg-config
- libcurl、SQLite、OpenSSL、expat、zlib、libxml2 的开发文件
- MIT krb5 与 Samba libsmbclient 的开发文件（`pkg-config krb5`、`pkg-config smbclient`）
- Debian 包：`dpkg-deb`
- RPM 包：`rpmbuild`
- Gentoo overlay：`tar`、`gzip`
- Python 3（changelog 转换）

aria2、FFmpeg、libsmb2 已随 `3thrd/` 提供并静态链接，构建前需初始化子模块：

```sh
git submodule update --init --recursive
```

## 构建

```sh
make -C pack              # 三种格式全部构建
make -C pack deb          # 仅 DEB
make -C pack rpm          # 仅 RPM
make -C pack emerge       # 仅 Gentoo overlay
make -C pack rootfs       # 只生成 build/rootfs
make -C pack test         # 以测试配置构建并运行 ctest
make -C pack clean
make -C pack distclean
```

版本号取自根目录 `CMakeLists.txt` 的 `project(downloader VERSION ...)`。
架构自动映射：`x86_64 → amd64`，`aarch64 → arm64`。

`make test` 在 Release 下构建测试；CMakeLists 为测试目标追加 `-UNDEBUG`，因为测试用
`assert()` 表达前置条件和副作用（例如 `assert(fclose(config) == 0)`），`-DNDEBUG`
会把这些调用整体编译掉。

打包构建使用 `-DCMAKE_INSTALL_PREFIX=/opt/downloader`，因为 Web 静态资源目录是
编译期常量；不要用仓库现有的 `build/` 二进制出包。

产物位于 `pack/dist/`：

- `downloader-<version>-<arch>.deb`
- `downloader-<version>-<arch>.rpm`
- `downloader-gentoo-overlay-<arch>-<version>.tar.gz`

## 安装布局

| 路径 | 说明 |
|---|---|
| `/opt/downloader/bin/downloader` | 主程序 |
| `/opt/downloader/share/downloader/web/` | Web 静态资源 |
| `/opt/downloader/share/licenses/downloader/` | 第三方许可证 |
| `/usr/bin/downloader` | 指向主程序的符号链接 |
| `/etc/downloader/download.ini` | 默认配置 |
| `/usr/lib/systemd/system/downloader.service` | systemd 服务 |
| `/var/lib/downloader/` | 数据库、密钥与默认下载目录 |
| `/var/lib/downloader/downloads/` | 默认下载目录 |
| `/var/cache/downloader/` | aria2 引擎/DHT 缓存 |
| `/run/downloader/downloader.sock` | IPC Unix Socket |

服务默认参数（`pack/src/scripts/downloader.service`）：

```
--config /etc/downloader/download.ini
```

配置默认监听 `127.0.0.1:8000`。远程访问需要部署者自行配置反向代理和 HTTPS；
Web 登录可在 `/etc/downloader/download.ini` 的 `[web] username/password` 中启用。

安装时创建 `downloader` 系统用户/组、数据目录和缓存目录，执行
`systemctl daemon-reload` 并 `enable` 服务，但不会自动启动。
卸载时停止并禁用服务，保留 `/var/lib/downloader` 和用户，避免删除下载数据。

服务以 `downloader` 用户运行，且不启用 `ProtectSystem`/`ProtectHome` 等文件系统
sandbox，因此可以把任务目标目录配置到 `/var/lib/downloader` 之外。使用这些目录前
需要自行授予 `downloader` 用户写权限。

## 配置保留

- DEB 将 `/etc/downloader/download.ini` 登记为 conffile。
- RPM 使用 `%config(noreplace)`。
- Gentoo 的 `/etc` 文件由 CONFIG_PROTECT 自动保护。

升级后需要重启服务才会使用新版本二进制。

## Debian

```sh
sudo dpkg -i pack/dist/downloader-<version>-<arch>.deb
sudo systemctl start downloader.service
```

运行时依赖：`libc6, libcurl4, libsqlite3-0, libssl3, libkrb5-3, libk5crypto3,
libcom-err2, libgssapi-krb5-2, libexpat1, zlib1g, libxml2, libsmbclient,
libstdc++6, libatomic1, libgcc-s1`。

## RPM

```sh
sudo rpm -Uvh pack/dist/downloader-<version>-<arch>.rpm
sudo systemctl start downloader.service
```

RPM 的 soname 依赖由 rpmbuild 自动生成；安装脚本需要 `shadow-utils`。

## Gentoo overlay

先以普通用户构建 overlay，再以 root 安装（脚本自动生成 Manifest 并调用 emerge）：

```sh
make -C pack emerge
sudo pack/tools/install-gentoo.sh
# 或指定已有的 overlay 包
sudo pack/tools/install-gentoo.sh pack/dist/downloader-gentoo-overlay-<arch>-<version>.tar.gz
# 也可以使用 make 目标（不会重新构建）
sudo make -C pack install-gentoo
```

脚本会解压到临时目录、校验 ebuild、生成 Manifest，然后以 `PORTDIR_OVERLAY`
临时仓库方式执行 `emerge --oneshot net-misc/downloader`，不会把临时 overlay
留在系统里。

手动安装：

```sh
tar -xzf pack/dist/downloader-gentoo-overlay-<arch>-<version>.tar.gz
cd downloader-gentoo-overlay-<arch>-<version>
ebuild net-misc/downloader/downloader-<version>.ebuild manifest
PORTDIR_OVERLAY="$PWD" emerge --oneshot net-misc/downloader
```

ebuild 使用 `RESTRICT="bindist mirror strip test"`，二进制标记为 `QA_PREBUILT`，
不会在安装时重新编译或 strip。`keepdir` 保留数据目录，`pkg_postinst`
（与 DEB/RPM 共用同一脚本）创建 `downloader` 用户并修正目录属主。

## 许可证

仓库暂无 LICENSE 文件，包元数据暂时使用 `all-rights-reserved` 占位。
安装包附带 clib、aria2、FFmpeg 和 libsmb2 的许可证文本。
程序动态链接 Samba 的 `libsmbclient`（GPLv3），正式分发前需要复核
RPM/Gentoo 的许可证与依赖元数据。
