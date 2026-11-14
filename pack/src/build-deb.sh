#!/bin/sh
set -eu

ARCH="${1:-$(uname -m)}"
VERSION="${2:-0.1.0}"
PACK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DIST_DIR="${PACK_DIR}/dist"
ROOTFS_DIR="${PACK_DIR}/build/rootfs"
POSTINSTALL_SRC="${PACK_DIR}/src/scripts/general-postinstall.sh"
POSTREMOVE_SRC="${PACK_DIR}/src/scripts/general-postremove.sh"
CHANGELOG_SRC="${PACK_DIR}/src/changelog.md"
CHANGELOG_CONVERT="${PACK_DIR}/src/tools/changelog-convert.py"
STAGE_DIR="${PACK_DIR}/build/package/deb"
PACKAGE_NAME="downloader"
PACKAGE_PATH="${DIST_DIR}/${PACKAGE_NAME}-${VERSION}-${ARCH}.deb"

case "${ARCH}" in
    amd64|arm64)
        DEB_ARCH="${ARCH}"
        ;;
    *)
        DEB_ARCH="${ARCH}"
        ;;
esac

if [ ! -d "${ROOTFS_DIR}" ]; then
    echo "error: rootfs not found: ${ROOTFS_DIR}" >&2
    exit 1
fi

if [ ! -x "${ROOTFS_DIR}/opt/downloader/bin/downloader" ]; then
    echo "error: downloader binary not found in rootfs" >&2
    exit 1
fi

if [ ! -f "${ROOTFS_DIR}/opt/downloader/share/downloader/web/index.html" ]; then
    echo "error: web assets not found in rootfs" >&2
    exit 1
fi

if [ ! -f "${POSTINSTALL_SRC}" ]; then
    echo "error: postinstall script not found: ${POSTINSTALL_SRC}" >&2
    exit 1
fi

if [ ! -f "${POSTREMOVE_SRC}" ]; then
    echo "error: postremove script not found: ${POSTREMOVE_SRC}" >&2
    exit 1
fi

if [ ! -f "${CHANGELOG_SRC}" ]; then
    echo "error: changelog not found: ${CHANGELOG_SRC}" >&2
    exit 1
fi

if [ ! -f "${CHANGELOG_CONVERT}" ]; then
    echo "error: changelog converter not found: ${CHANGELOG_CONVERT}" >&2
    exit 1
fi

rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}/DEBIAN" "${DIST_DIR}"
cp -a "${ROOTFS_DIR}/." "${STAGE_DIR}/"

cat > "${STAGE_DIR}/DEBIAN/control" <<EOF_CONTROL
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: DingJing <dingjing@live.cn>
Depends: libc6, libcurl4, libsqlite3-0, libssl3, libkrb5-3, libk5crypto3, libcom-err2, libgssapi-krb5-2, libexpat1, zlib1g, libxml2, libsmbclient, libstdc++6, libatomic1, libgcc-s1
Recommends: systemd
Description: Background download service with local web interface
 Downloader is a Unix socket based download daemon supporting HTTP/HTTPS,
 FTP/SFTP, BitTorrent, Metalink, HLS/DASH and SMB. It stores tasks in
 SQLite and serves a local web interface on 127.0.0.1:8000.
EOF_CONTROL

printf '%s\n' '/etc/downloader/download.ini' > "${STAGE_DIR}/DEBIAN/conffiles"
chmod 0644 "${STAGE_DIR}/DEBIAN/conffiles"

install -m 0755 "${POSTINSTALL_SRC}" "${STAGE_DIR}/DEBIAN/postinst"

{
    printf '%s\n' '#!/bin/bash'
    printf '%s\n' 'case "$1" in'
    printf '%s\n' '    remove|purge) ;;'
    printf '%s\n' '    *) exit 0 ;;'
    printf '%s\n' 'esac'
    sed '1{/^#!/d;}' "${POSTREMOVE_SRC}"
} > "${STAGE_DIR}/DEBIAN/postrm"
chmod 0755 "${STAGE_DIR}/DEBIAN/postrm"

mkdir -p "${STAGE_DIR}/usr/share/doc/${PACKAGE_NAME}"
python3 "${CHANGELOG_CONVERT}" deb "${CHANGELOG_SRC}" "${STAGE_DIR}/usr/share/doc/${PACKAGE_NAME}/changelog"
gzip -9 -n "${STAGE_DIR}/usr/share/doc/${PACKAGE_NAME}/changelog"

dpkg-deb --root-owner-group --build "${STAGE_DIR}" "${PACKAGE_PATH}"

echo "deb package: ${PACKAGE_PATH}"
