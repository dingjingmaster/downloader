#!/bin/sh
set -eu

ARCH="${1:-$(uname -m)}"
VERSION="${2:-0.1.0}"
PACK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
PROJECT_DIR="$(CDPATH= cd -- "${PACK_DIR}/.." && pwd)"
BUILD_DIR="${PACK_DIR}/build/downloader"
ROOTFS_DIR="${PACK_DIR}/build/rootfs"
WEB_SRC_DIR="${PROJECT_DIR}/web"
CONFIG_SRC="${PACK_DIR}/src/config/download.ini"
SERVICE_SRC="${PACK_DIR}/src/scripts/downloader.service"
DOWNLOADER_BIN="${BUILD_DIR}/downloader"
STRIP_TOOL="${STRIP:-strip}"
INSTALL_PREFIX="/opt/downloader"
SYSTEMD_DIR="/usr/lib/systemd/system"

if [ ! -x "${DOWNLOADER_BIN}" ]; then
    echo "error: downloader binary not found: ${DOWNLOADER_BIN}" >&2
    exit 1
fi

if command -v readelf >/dev/null 2>&1; then
    MACHINE="$(LC_ALL=C readelf -h "${DOWNLOADER_BIN}" | sed -n 's/.*Machine:[[:space:]]*//p')"
    case "${ARCH}" in
        amd64)
            case "${MACHINE}" in
                *X86-64*) ;;
                *)
                    echo "error: binary machine '${MACHINE}' does not match arch '${ARCH}'" >&2
                    exit 1
                    ;;
            esac
            ;;
        arm64)
            case "${MACHINE}" in
                AArch64) ;;
                *)
                    echo "error: binary machine '${MACHINE}' does not match arch '${ARCH}'" >&2
                    exit 1
                    ;;
            esac
            ;;
    esac
fi

if [ ! -d "${WEB_SRC_DIR}" ]; then
    echo "error: web directory not found: ${WEB_SRC_DIR}" >&2
    exit 1
fi

if [ ! -f "${CONFIG_SRC}" ]; then
    echo "error: config not found: ${CONFIG_SRC}" >&2
    exit 1
fi

if [ ! -f "${SERVICE_SRC}" ]; then
    echo "error: service unit not found: ${SERVICE_SRC}" >&2
    exit 1
fi

rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}${INSTALL_PREFIX}/bin"
mkdir -p "${ROOTFS_DIR}${INSTALL_PREFIX}/share/downloader/web"
mkdir -p "${ROOTFS_DIR}${INSTALL_PREFIX}/share/licenses/downloader"
mkdir -p "${ROOTFS_DIR}/etc/downloader"
mkdir -p "${ROOTFS_DIR}${SYSTEMD_DIR}"
mkdir -p "${ROOTFS_DIR}/usr/bin"

install -m 0755 "${DOWNLOADER_BIN}" "${ROOTFS_DIR}${INSTALL_PREFIX}/bin/downloader"
"${STRIP_TOOL}" --strip-unneeded "${ROOTFS_DIR}${INSTALL_PREFIX}/bin/downloader"

cp -a "${WEB_SRC_DIR}/." "${ROOTFS_DIR}${INSTALL_PREFIX}/share/downloader/web/"
find "${ROOTFS_DIR}${INSTALL_PREFIX}/share/downloader/web" -type d -exec chmod 0755 {} +
find "${ROOTFS_DIR}${INSTALL_PREFIX}/share/downloader/web" -type f -exec chmod 0644 {} +

install -m 0644 "${CONFIG_SRC}" "${ROOTFS_DIR}/etc/downloader/download.ini"
install -m 0644 "${SERVICE_SRC}" "${ROOTFS_DIR}${SYSTEMD_DIR}/downloader.service"

copy_license()
{
    source_path="$1"
    target_name="$2"

    if [ -f "${source_path}" ]; then
        install -m 0644 "${source_path}" \
            "${ROOTFS_DIR}${INSTALL_PREFIX}/share/licenses/downloader/${target_name}"
    fi
}

copy_license "${PROJECT_DIR}/clib/LICENSE" "clib-LICENSE"
copy_license "${PROJECT_DIR}/3thrd/aria2c/COPYING" "aria2-COPYING"
copy_license "${PROJECT_DIR}/3thrd/aria2c/LICENSE.OpenSSL" "aria2-LICENSE.OpenSSL"
copy_license "${PROJECT_DIR}/3thrd/ffmpeg/COPYING.LGPLv2.1" "ffmpeg-COPYING.LGPLv2.1"
copy_license "${PROJECT_DIR}/3thrd/ffmpeg/LICENSE.md" "ffmpeg-LICENSE.md"
copy_license "${PROJECT_DIR}/3thrd/libsmb2/COPYING" "libsmb2-COPYING"
copy_license "${PROJECT_DIR}/3thrd/libsmb2/LICENCE-LGPL-2.1.txt" "libsmb2-LICENCE-LGPL-2.1.txt"

ln -s "${INSTALL_PREFIX}/bin/downloader" "${ROOTFS_DIR}/usr/bin/downloader"

printf '%s\n' "${VERSION}" > "${PACK_DIR}/build/rootfs.version"
printf '%s\n' "${ARCH}" > "${PACK_DIR}/build/rootfs.arch"

echo "rootfs: ${ROOTFS_DIR}"
