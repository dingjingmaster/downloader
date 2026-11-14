# Downloader 打包安装实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 在 `pack/` 下实现与 spider 同构的 rootfs、DEB、RPM、Gentoo overlay 打包与 Gentoo 安装脚本。

**Architecture:** 一次 CMake Release 构建产出 `/opt/downloader` 前缀的二进制；rootfs 脚本组装程序、Web 资源、配置、systemd 服务和许可证；三个格式脚本共用同一份 rootfs 与安装/卸载脚本；Gentoo 安装器把 overlay 解到临时目录后直接 `emerge --oneshot`。

**Tech Stack:** POSIX shell、GNU Make、CMake、dpkg-deb、rpmbuild、Gentoo Portage（ebuild/emerge）、Python 3（changelog 转换）。

**Spec:** `docs/superpowers/specs/2026-11-14-packaging-design.md`

## Global Constraints

- 版本来自根 `CMakeLists.txt` 的 `project(downloader VERSION ...)`，当前 `0.1.0`。
- 架构映射：`x86_64 → amd64`、`aarch64 → arm64`。
- 打包构建必须使用 `-DCMAKE_INSTALL_PREFIX=/opt/downloader`，否则 Web 根目录错误。
- 安装布局：程序 `/opt/downloader/bin/downloader`，Web `/opt/downloader/share/downloader/web`，配置 `/etc/downloader/download.ini`，服务 `/usr/lib/systemd/system/downloader.service`，数据 `/var/lib/downloader`，缓存 `/var/cache/downloader`，socket `/run/downloader/downloader.sock`。
- 服务用户/组 `downloader`；默认监听 `127.0.0.1:8000`。
- 包名 `downloader`；Gentoo atom `net-misc/downloader`。
- 配置升级不得覆盖：DEB conffile、RPM `%config(noreplace)`、Gentoo CONFIG_PROTECT。
- 卸载保留 `/var/lib/downloader` 与 `downloader` 用户。
- 不修改 downloader 的 C/C++ 源码。

## Review Focus

- 升级后用户改过的 `/etc/downloader/download.ini` 仍保留（三种格式各自的保护机制）。
- 服务以 `downloader` 用户启动时，数据库、密钥、下载目录、aria2 缓存和 socket 目录都可写。
- 二进制编译期 Web 根目录与安装路径一致，页面不 404。
- 卸载/重装不会删除下载数据。
- 打包构建从干净 clone + 已初始化子模块可完成（不依赖现有 `build/`）。

---

### Task 1: pack 骨架、rootfs、配置与服务

**Files:**
- Create: `pack/Makefile`
- Create: `pack/src/build-rootfs.sh`
- Create: `pack/src/config/download.ini`
- Create: `pack/src/scripts/downloader.service`
- Create: `pack/src/scripts/general-postinstall.sh`
- Create: `pack/src/scripts/general-postremove.sh`
- Modify: `.gitignore`

**Interfaces:**
- Produces: `pack/build/rootfs/` 完整安装树；`pack/build/rootfs.version`、`pack/build/rootfs.arch`。
- Produces: Makefile 变量 `PROJECT_DIR`、`BUILD_DIR`、`VERSION`、`osArch`，供后续任务复用。

- [x] **Step 1: 写 Makefile 的 build/rootfs 目标**

`build`：`cmake -S $(PROJECT_DIR) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/downloader -DCMAKE_C_COMPILER=$(CC) -DCMAKE_CXX_COMPILER=$(CXX) -DDOWNLOADER_BUILD_TESTS=OFF`，然后 `cmake --build $(BUILD_DIR) --parallel $(JOBS)`。
`rootfs: build`：调用 `sh src/build-rootfs.sh "$(osArch)" "$(VERSION)"`。
`test: build`：以 `-DDOWNLOADER_BUILD_TESTS=ON` 重新配置、构建并运行 `ctest`；
CMakeLists 为测试目标追加 `-UNDEBUG`，避免 Release 的 `-DNDEBUG` 编译掉测试断言。
`clean` 删除 `pack/build`，`distclean` 再删除 `pack/dist`。

- [x] **Step 2: 写打包配置**

`pack/src/config/download.ini` 内容：`ipc_socket=/run/downloader/downloader.sock`、
`download_dir=/var/lib/downloader/downloads`、`database=/var/lib/downloader/meta.db`、
`[web] listen=127.0.0.1 port=8000`，其余键沿用仓库 `config/download.ini` 的默认值。

- [x] **Step 3: 写 systemd 服务与安装/卸载脚本**

服务使用 `User=downloader`、`Group=downloader`、`StateDirectory=downloader`、
`CacheDirectory=downloader`、`RuntimeDirectory=downloader`、`WorkingDirectory=/var/lib/downloader`、
`Environment=XDG_CACHE_HOME=/var/cache/downloader`、`Environment=HOME=/var/lib/downloader`、
`ExecStart=/opt/downloader/bin/downloader --config /etc/downloader/download.ini`，
`Restart=on-failure`；不启用文件系统 sandbox（下载目标可配置到状态目录之外）。

`general-postinstall.sh` 创建 `downloader` 用户/组、`/var/lib/downloader`、
`/var/lib/downloader/downloads`、`/var/cache/downloader`（0750），执行 `systemctl daemon-reload`
和 `systemctl enable downloader.service`，不启动服务。
`general-postremove.sh` 停止并禁用服务，保留数据与用户。

- [x] **Step 4: 写 rootfs 脚本**

校验二进制存在、`readelf` 机器类型与 arch 匹配；清空并创建
`/opt/downloader/bin`、`/opt/downloader/share/downloader/web`、
`/opt/downloader/share/licenses/downloader`、`/etc/downloader`、`/usr/lib/systemd/system`、`/usr/bin`。
安装并 `strip --strip-unneeded` 主程序；复制 `web/`；安装配置、服务和
`3thrd/{aria2c,ffmpeg,libsmb2}` 的许可证文件；创建 `/usr/bin/downloader` 符号链接；
写入 version/arch 文件。

- [x] **Step 5: 运行构建与 rootfs 验证**

Run: `make -C pack build`（首次会构建 aria2/FFmpeg/libsmb2，耗时较长）。
Expected: 退出码 0，`pack/build/downloader/downloader` 存在。

Run: `make -C pack rootfs`
Expected: 退出码 0。

Run: `find pack/build/rootfs -type f -o -type l | sort` 和
`pack/build/rootfs/opt/downloader/bin/downloader --version`
Expected: 包含上表所有路径；`--version` 输出引擎版本。

- [x] **Step 6: Commit**

```bash
git add pack/Makefile pack/src .gitignore
git commit -m "build: add pack rootfs and systemd service"
```

---

### Task 2: changelog 转换与 DEB

**Files:**
- Create: `pack/src/changelog.md`
- Create: `pack/src/tools/changelog-convert.py`
- Create: `pack/src/build-deb.sh`
- Modify: `pack/Makefile`

**Interfaces:**
- Consumes: `pack/build/rootfs/`（Task 1）。
- Produces: `pack/dist/downloader-<version>-<arch>.deb`。

- [x] **Step 1: 写 changelog 与转换器**

`changelog.md` 使用 `## <version> -- <date>` + 编号条目；转换器支持 `deb`、`rpm`、`gentoo`
三种输出，包名固定为 `downloader`，维护者 `DingJing <dingjing@live.cn>`。

- [x] **Step 2: 先验证 deb 目标失败**

Run: `make -C pack deb`
Expected: 失败，`pack/src/build-deb.sh` 不存在。

- [x] **Step 3: 写 build-deb.sh**

检查 rootfs、二进制、postinst/postrm、changelog、转换器存在；复制 rootfs 到
`pack/build/package/deb`；生成 `DEBIAN/control`（Package `downloader`，Depends:
`libc6, libcurl4, libsqlite3-0, libssl3, libkrb5-3, libk5crypto3, libcom-err2,
libgssapi-krb5-2, libexpat1, zlib1g, libxml2, libsmbclient, libstdc++6, libatomic1,
libgcc-s1`，Recommends `systemd`）；写入 `DEBIAN/conffiles` 包含
`/etc/downloader/download.ini`；安装 postinst/postrm；生成 gzip changelog；
`dpkg-deb --root-owner-group --build`。

- [x] **Step 4: 验证 DEB 内容**

Run: `make -C pack deb`
Expected: `pack/dist/downloader-0.1.0-amd64.deb` 存在。

Run: `dpkg-deb --info pack/dist/downloader-0.1.0-amd64.deb` 和
`dpkg-deb --contents pack/dist/downloader-0.1.0-amd64.deb`
Expected: control 含依赖与描述；内容含 `/opt/downloader/bin/downloader`、
`/opt/downloader/share/downloader/web/index.html`、`/etc/downloader/download.ini`、
`/usr/lib/systemd/system/downloader.service`、`/usr/bin/downloader`。
`dpkg-deb --ctrl-tarfile ... | tar -xO ./conffiles` 输出配置文件路径。

- [x] **Step 5: Commit**

```bash
git add pack/src/changelog.md pack/src/tools/changelog-convert.py pack/src/build-deb.sh pack/Makefile
git commit -m "build: add Debian package target"
```

---

### Task 3: RPM

**Files:**
- Create: `pack/src/build-rpm.sh`
- Modify: `pack/Makefile`

**Interfaces:**
- Consumes: rootfs、共享安装脚本、changelog 转换器。
- Produces: `pack/dist/downloader-<version>-<arch>.rpm`。

- [x] **Step 1: 写 build-rpm.sh**

从 rootfs 生成 source tar；spec 使用 `License: all-rights-reserved`、`BuildArch`
（amd64→x86_64，arm64→aarch64）、`Requires(post): shadow-utils`；`%install` 复制
rootfs；`%post`/`%postun` 嵌入共享脚本；`%files` 列出 `/opt/downloader` 树、
`%config(noreplace) /etc/downloader/download.ini`、systemd 服务、`/usr/bin/downloader`
和许可证；`%check` 校验主程序、Web index、服务文件；changelog 从转换器追加；
`rpmbuild --define "_topdir ..." -bb` 后复制产物到 `pack/dist/`。

- [x] **Step 2: 验证 RPM**

Run: `make -C pack rpm`
Expected: `pack/dist/downloader-0.1.0-amd64.rpm` 存在。

Run: `rpm -qpl pack/dist/downloader-0.1.0-amd64.rpm`
Expected: 包含上表路径。

Run: `rpm -qp --qf '%{NAME} %{VERSION} %{ARCH}\n' ...` 和
`rpm -qp --scripts ...`
Expected: `downloader 0.1.0 x86_64`；脚本含 `systemctl enable downloader.service`。
`rpm -qp --dump ... | grep download.ini` 显示 `c`（config）标志。

- [x] **Step 3: Commit**

```bash
git add pack/src/build-rpm.sh pack/Makefile
git commit -m "build: add RPM package target"
```

---

### Task 4: Gentoo overlay 与安装脚本

**Files:**
- Create: `pack/src/gentoo/metadata/layout.conf`
- Create: `pack/src/gentoo/profiles/repo_name`
- Create: `pack/src/gentoo/net-misc/downloader/metadata.xml`
- Create: `pack/src/build-emerge.sh`
- Create: `pack/tools/install-gentoo.sh`
- Modify: `pack/Makefile`

**Interfaces:**
- Consumes: rootfs、共享安装脚本、转换器。
- Produces: `pack/dist/downloader-gentoo-overlay-<arch>-<version>.tar.gz`。

- [x] **Step 1: 写 overlay 元数据**

`profiles/repo_name` 为 `downloader-crawler` 之外的独立名字 `downloader`；
`metadata/layout.conf` 为 `masters = gentoo`、`thin-manifests = true`；
`net-misc/downloader/metadata.xml` 填写维护者和描述。

- [x] **Step 2: 写 build-emerge.sh**

EAPI 8，`inherit systemd`；`RESTRICT="bindist mirror strip test"`；
`QA_PREBUILT="/opt/downloader/bin/downloader"`；`KEYWORDS="~<arch>"`；
`RDEPEND` 包含 `net-misc/curl`、`dev-db/sqlite`、`dev-libs/openssl`、
`app-crypt/mit-krb5`、`dev-libs/expat`、`sys-libs/zlib`、`dev-libs/libxml2`、
`net-fs/samba`、`sys-devel/gcc`；
`src_install` 安装程序、Web、配置、服务、符号链接、许可证，`keepdir`
数据/缓存目录并 `fperms 0750`；`pkg_postinst`/`pkg_postrm` 嵌入共享脚本
（去掉 shebang、`set -e`、`exit 0`）；用 Python 替换占位符后 `bash -n` 校验。

- [x] **Step 3: 写 install-gentoo.sh**

仿照 spider：解析参数或 `--print-default-package`；默认匹配
`dist/downloader-gentoo-overlay-<arch>-*.tar.gz`；要求 root 和 `emerge`；
解压到 `mktemp -d`；定位含 `profiles/repo_name` 的 overlay；
修正目录/文件权限和 `files/downloader` 可执行位；用临时
`PORTDIR_OVERLAY`、`PORTAGE_TMPDIR`、`DISTDIR`、`PKGDIR` 执行
`emerge --verbose --oneshot --reinstall-atoms=net-misc/downloader net-misc/downloader`；退出时清理临时目录。

- [x] **Step 4: 验证 overlay 与安装脚本**

Run: `make -C pack emerge`
Expected: tar.gz 存在。

Run: `tar -tzf pack/dist/downloader-gentoo-overlay-amd64-0.1.0.tar.gz`
Expected: 含 `profiles/repo_name`、`net-misc/downloader/downloader-0.1.0.ebuild`、
`files/downloader`、`files/download.ini`、`files/downloader.service`、`files/web/index.html`。

Run: `sh pack/tools/install-gentoo.sh --print-default-package`
Expected: 输出 tar.gz 路径。

Run: 解包后 `bash -n net-misc/downloader/downloader-0.1.0.ebuild` 和
`ebuild net-misc/downloader/downloader-0.1.0.ebuild manifest`
Expected: 语法检查通过；Manifest 生成。

- [x] **Step 5: Commit**

```bash
git add pack/src/gentoo pack/src/build-emerge.sh pack/tools/install-gentoo.sh pack/Makefile
git commit -m "build: add Gentoo overlay and installer"
```

---

### Task 5: README、总验证与提交

**Files:**
- Create: `pack/README.md`
- Modify: `README.md`（增加打包入口说明）
- Modify: `pack/Makefile`（如需修正 `all/packages` 依赖）

**Interfaces:**
- Consumes: 前三项打包产物。
- Produces: 可复现的打包说明与最终产物。

- [x] **Step 1: 写 pack/README.md**

说明依赖（cmake/make/编译器/strip/dpkg-deb/rpmbuild/tar/gzip/python3、krb5 与
smbclient 开发库）、五个 make 目标、产物名、安装布局、服务默认值、配置保留策略、
卸载保留数据、许可证与 `net-fs/samba` GPLv3 注意事项、Gentoo 安装命令。

- [x] **Step 2: 从干净产物状态重建三种包**

Run: `make -C pack distclean && make -C pack`
Expected: 退出码 0；`pack/dist/` 含 `.deb`、`.rpm`、`-gentoo-overlay-*.tar.gz`。

- [x] **Step 3: 检查产物与工作区**

Run: `ls -lh pack/dist/`、`git status --short`、`git diff --check`
Expected: 三个产物；只包含本次新增的 `pack/`、设计/计划文档、`.gitignore` 和 README 变更。

- [x] **Step 4: Commit**

```bash
git add pack/README.md README.md docs/superpowers/specs/2026-11-14-packaging-design.md docs/superpowers/plans/2026-11-14-packaging.md
git commit -m "docs: document packaging and installation"
```
