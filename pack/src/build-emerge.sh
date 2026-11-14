#!/bin/sh
set -eu

ARCH="${1:-$(uname -m)}"
VERSION="${2:-0.1.0}"
PACK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DIST_DIR="${PACK_DIR}/dist"
ROOTFS_DIR="${PACK_DIR}/build/rootfs"
GENTOO_SRC_DIR="${PACK_DIR}/src/gentoo"
POSTINSTALL_SRC="${PACK_DIR}/src/scripts/general-postinstall.sh"
POSTREMOVE_SRC="${PACK_DIR}/src/scripts/general-postremove.sh"
CHANGELOG_SRC="${PACK_DIR}/src/changelog.md"
CHANGELOG_CONVERT="${PACK_DIR}/src/tools/changelog-convert.py"
CATEGORY="net-misc"
PN="downloader"
PACKAGE_NAME="${PN}-gentoo-overlay-${ARCH}-${VERSION}"
STAGE_DIR="${DIST_DIR}/.${PACKAGE_NAME}.root.$$"
OVERLAY_DIR="${STAGE_DIR}/${PACKAGE_NAME}"
PACKAGE_PATH="${DIST_DIR}/${PACKAGE_NAME}.tar.gz"
EBUILD_PATH="${OVERLAY_DIR}/${CATEGORY}/${PN}/${PN}-${VERSION}.ebuild"

cleanup()
{
    if [ -n "${STAGE_DIR:-}" ] && [ -d "${STAGE_DIR}" ]; then
        rm -rf "${STAGE_DIR}"
    fi
}
trap cleanup EXIT INT TERM

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

mkdir -p "${DIST_DIR}"
mkdir -p "${OVERLAY_DIR}"

cp -R "${GENTOO_SRC_DIR}/." "${OVERLAY_DIR}/"
mkdir -p "${OVERLAY_DIR}/${CATEGORY}/${PN}/files"
python3 "${CHANGELOG_CONVERT}" gentoo "${CHANGELOG_SRC}" "${OVERLAY_DIR}/${CATEGORY}/${PN}/ChangeLog"

cat > "${EBUILD_PATH}" <<'EOF_EBUILD'
EAPI=8

inherit systemd

DESCRIPTION="Background download service with local web interface"
HOMEPAGE="https://local/downloader"
SRC_URI=""

LICENSE="all-rights-reserved"
SLOT="0"
KEYWORDS="~__GENTOO_ARCH__"
RESTRICT="bindist mirror strip test"
QA_PREBUILT="/opt/downloader/bin/downloader"

RDEPEND="
	>=net-misc/curl-7.85.0
	>=dev-db/sqlite-3.35
	>=dev-libs/openssl-1.1.1
	>=app-crypt/mit-krb5-1.19
	>=dev-libs/expat-2.4
	>=sys-libs/zlib-1.2.11
	>=dev-libs/libxml2-2.9.12
	>=net-fs/samba-4.15
	sys-devel/gcc:*
"

S="${WORKDIR}"

src_install() {
	exeinto /opt/downloader/bin
	doexe "${FILESDIR}/downloader"

	insinto /opt/downloader/share/downloader
	doins -r "${FILESDIR}/web"

	insinto /opt/downloader/share/licenses/downloader
	doins "${FILESDIR}"/licenses/*

	insinto /etc/downloader
	doins "${FILESDIR}/download.ini"

	systemd_dounit "${FILESDIR}/downloader.service"
	dosym /opt/downloader/bin/downloader /usr/bin/downloader

	keepdir /var/lib/downloader
	keepdir /var/lib/downloader/downloads
	keepdir /var/cache/downloader
	fperms 0750 /var/lib/downloader
	fperms 0750 /var/lib/downloader/downloads
	fperms 0750 /var/cache/downloader
}

pkg_postinst() {
__GENERAL_POSTINSTALL__
}

pkg_postrm() {
	if [ -n "${REPLACED_BY_VERSION:-}" ]; then
		return 0
	fi
__GENERAL_POSTREMOVE__
}
EOF_EBUILD

python3 - "${EBUILD_PATH}" "${POSTINSTALL_SRC}" "${POSTREMOVE_SRC}" "${ARCH}" <<'PY'
import pathlib
import sys

ebuild_path = pathlib.Path(sys.argv[1])
postinstall_path = pathlib.Path(sys.argv[2])
postremove_path = pathlib.Path(sys.argv[3])
gentoo_arch = sys.argv[4]

def script_body(path):
    lines = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#!"):
            continue
        # pkg_postinst/pkg_postrm must return to Portage, not exit the phase shell.
        if line.strip() in ("set -e", "exit 0"):
            continue
        lines.append(line)
    return "\n".join(lines)

ebuild = ebuild_path.read_text(encoding="utf-8")
ebuild = ebuild.replace("__GENERAL_POSTINSTALL__", script_body(postinstall_path))
ebuild = ebuild.replace("__GENERAL_POSTREMOVE__", script_body(postremove_path))
ebuild = ebuild.replace("__GENTOO_ARCH__", gentoo_arch)
ebuild_path.write_text(ebuild, encoding="utf-8")
PY
bash -n "${EBUILD_PATH}"

install -m 0755 "${ROOTFS_DIR}/opt/downloader/bin/downloader" "${OVERLAY_DIR}/${CATEGORY}/${PN}/files/downloader"
install -m 0644 "${ROOTFS_DIR}/usr/lib/systemd/system/downloader.service" "${OVERLAY_DIR}/${CATEGORY}/${PN}/files/downloader.service"
install -m 0644 "${ROOTFS_DIR}/etc/downloader/download.ini" "${OVERLAY_DIR}/${CATEGORY}/${PN}/files/download.ini"
cp -a "${ROOTFS_DIR}/opt/downloader/share/downloader/web" "${OVERLAY_DIR}/${CATEGORY}/${PN}/files/web"
mkdir -p "${OVERLAY_DIR}/${CATEGORY}/${PN}/files/licenses"
cp -a "${ROOTFS_DIR}/opt/downloader/share/licenses/downloader/." "${OVERLAY_DIR}/${CATEGORY}/${PN}/files/licenses/"
find "${OVERLAY_DIR}/${CATEGORY}/${PN}/files/web" -type d -exec chmod 0755 {} +
find "${OVERLAY_DIR}/${CATEGORY}/${PN}/files/web" -type f -exec chmod 0644 {} +
find "${OVERLAY_DIR}/${CATEGORY}/${PN}/files/licenses" -type f -exec chmod 0644 {} +

tar -C "${STAGE_DIR}" -czf "${PACKAGE_PATH}" "${PACKAGE_NAME}"

echo "gentoo overlay package: ${PACKAGE_PATH}"
