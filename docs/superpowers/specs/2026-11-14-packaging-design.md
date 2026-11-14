# 下载器打包安装设计

## 意图

为 downloader 增加与 `../spider/pack/` 同构的打包能力：一份 rootfs 产出 DEB、RPM 和
Gentoo overlay，并提供 Gentoo 一键安装脚本。安装后以 systemd 服务运行，Web 只监听
本机回环地址。

## 目标

- `make -C pack` 一次产出三种安装包；也支持 `deb`、`rpm`、`emerge`、`rootfs`、`test` 子目标。
- 安装布局仿照 spider：程序在 `/opt/downloader`，配置在 `/etc/downloader`，数据在
  `/var/lib/downloader`。
- 安装时创建 `downloader` 系统用户/组和 systemd 服务；启用但不自动启动。
- 卸载时保留数据和用户。
- 升级不覆盖用户修改过的配置。
- 附带第三方许可证文件。

## 非目标

- 不修改 downloader 的下载、Web 或协议实现。
- 不为第三方静态库提供独立运行时依赖（aria2、FFmpeg、libsmb2 已静态链接）。
- 不生成源码包或在安装时重新编译。
- 不配置 HTTPS、反向代理或 Web 登录凭据；这些由部署者决定。

## 已确认决策

1. 安装前缀 `/opt/downloader`（与 spider 一致），`/usr/bin/downloader` 为符号链接。
2. systemd 服务默认监听 `127.0.0.1:8000`；远程访问由部署者配置。
3. 产出 DEB、RPM、Gentoo overlay 三种格式。

## 安装布局

| 路径 | 内容 |
|---|---|
| `/opt/downloader/bin/downloader` | 主程序（strip 后） |
| `/opt/downloader/share/downloader/web/` | Web 静态资源 |
| `/opt/downloader/share/licenses/downloader/` | 第三方许可证 |
| `/usr/bin/downloader` | 指向主程序的符号链接 |
| `/etc/downloader/download.ini` | 默认配置（受保护，不随升级覆盖） |
| `/usr/lib/systemd/system/downloader.service` | systemd 服务 |
| `/var/lib/downloader` | 数据库、密钥、下载目录 |
| `/var/lib/downloader/downloads` | 默认下载目录 |
| `/var/cache/downloader` | aria2 DHT/引擎缓存 |
| `/run/downloader/downloader.sock` | IPC Unix Socket |

## 关键约束

- Web 根目录是编译期常量 `$CMAKE_INSTALL_PREFIX/share/downloader/web`，因此打包构建必须
  使用 `-DCMAKE_INSTALL_PREFIX=/opt/downloader`。运行时不搜索其他路径。
- 程序按命令行 → 程序目录下 `config/download.ini` → `/etc/downloader/download.ini`
  的顺序查找配置。打包不把配置放到程序目录，避免遮蔽 `/etc`。
- 数据库、密钥和下载目录使用绝对路径，服务 `WorkingDirectory=/var/lib/downloader`。
- IPC socket 位于 `/run/downloader`，由 systemd `RuntimeDirectory=downloader` 提供。
- aria2 使用 `XDG_CACHE_HOME` 或 `HOME/.cache`；服务设置
  `Environment=XDG_CACHE_HOME=/var/cache/downloader`。
- 运行时动态依赖：libcurl、sqlite3、OpenSSL、expat、zlib、libxml2、MIT krb5/gssapi、
  Samba libsmbclient、libstdc++、libatomic、glibc。

## 打包结构

```
pack/
├── Makefile
├── README.md
├── src/
│   ├── build-rootfs.sh
│   ├── build-deb.sh
│   ├── build-rpm.sh
│   ├── build-emerge.sh
│   ├── changelog.md
│   ├── config/download.ini
│   ├── scripts/{downloader.service,general-postinstall.sh,general-postremove.sh}
│   ├── tools/changelog-convert.py
│   └── gentoo/{metadata,profiles,net-misc/downloader}
└── tools/install-gentoo.sh
```

- 版本取自根 `CMakeLists.txt` 的 `project(downloader VERSION ...)`。
- 架构映射：`x86_64 → amd64`、`aarch64 → arm64`。
- 产物：`downloader-<version>-<arch>.deb`、`.rpm`、
  `downloader-gentoo-overlay-<arch>-<version>.tar.gz`。
- Gentoo 包名 `net-misc/downloader`。

## 配置保留

- DEB：`DEBIAN/conffiles` 列出 `/etc/downloader/download.ini`。
- RPM：`%config(noreplace) /etc/downloader/download.ini`。
- Gentoo：`/etc` 文件由 CONFIG_PROTECT 自动保护。

## 服务

- 专用 `downloader` 系统用户/组，shell 为 nologin。
- `StateDirectory`、`CacheDirectory`、`RuntimeDirectory` 均由 systemd 创建。
- 不启用文件系统 sandbox：下载目标可以配置到 `StateDirectory` 之外的目录。
- `Restart=on-failure`。
- 安装脚本执行 `daemon-reload` 和 `enable`，不自动 `start`。
- 卸载脚本 `stop` + `disable`，保留 `/var/lib/downloader` 和用户。

## 许可证

仓库暂无 LICENSE。包元数据沿用 spider 的 `all-rights-reserved` 占位，并安装
aria2、FFmpeg、libsmb2 的许可证文本。动态链接的 Samba/libsmbclient 为 GPLv3，
RPM/Gentoo 的依赖与许可证元数据需在正式分发前复核。README 会明确标注这一点。

## 验证

- `cmake --build` 打包构建通过。
- `make -C pack rootfs` 生成正确布局。
- `dpkg-deb --info/--contents`、`rpm -qpl`、`tar -tzf` 检查三种产物内容。
- `pack/tools/install-gentoo.sh --print-default-package` 能解析默认包。
- 至少验证生成的 ebuild 通过 `bash -n` 和 `ebuild ... manifest`（若环境可用）。
- 现有 `ctest` 不受影响；打包流程本身不改源码。
