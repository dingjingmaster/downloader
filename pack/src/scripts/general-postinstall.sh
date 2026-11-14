#!/bin/bash
set -e

DOWNLOADER_USER="downloader"
DOWNLOADER_STATE="/var/lib/downloader"
DOWNLOADER_DOWNLOADS="/var/lib/downloader/downloads"
DOWNLOADER_CACHE="/var/cache/downloader"

if [ -x /usr/sbin/nologin ]; then
    DOWNLOADER_SHELL="/usr/sbin/nologin"
elif [ -x /sbin/nologin ]; then
    DOWNLOADER_SHELL="/sbin/nologin"
else
    DOWNLOADER_SHELL="/bin/false"
fi

if ! getent group "${DOWNLOADER_USER}" >/dev/null 2>&1; then
    groupadd -r "${DOWNLOADER_USER}" 2>/dev/null || true
fi

if ! getent passwd "${DOWNLOADER_USER}" >/dev/null 2>&1; then
    useradd -r -g "${DOWNLOADER_USER}" -d "${DOWNLOADER_STATE}" -s "${DOWNLOADER_SHELL}" \
        -c "Downloader background service" "${DOWNLOADER_USER}" 2>/dev/null || true
fi

install -d -o "${DOWNLOADER_USER}" -g "${DOWNLOADER_USER}" -m 0750 "${DOWNLOADER_STATE}" 2>/dev/null || \
    mkdir -p "${DOWNLOADER_STATE}" 2>/dev/null || true
install -d -o "${DOWNLOADER_USER}" -g "${DOWNLOADER_USER}" -m 0750 "${DOWNLOADER_DOWNLOADS}" 2>/dev/null || \
    mkdir -p "${DOWNLOADER_DOWNLOADS}" 2>/dev/null || true
install -d -o "${DOWNLOADER_USER}" -g "${DOWNLOADER_USER}" -m 0750 "${DOWNLOADER_CACHE}" 2>/dev/null || \
    mkdir -p "${DOWNLOADER_CACHE}" 2>/dev/null || true

chown "${DOWNLOADER_USER}:${DOWNLOADER_USER}" "${DOWNLOADER_STATE}" 2>/dev/null || true
chown "${DOWNLOADER_USER}:${DOWNLOADER_USER}" "${DOWNLOADER_DOWNLOADS}" 2>/dev/null || true
chown "${DOWNLOADER_USER}:${DOWNLOADER_USER}" "${DOWNLOADER_CACHE}" 2>/dev/null || true
chmod 0750 "${DOWNLOADER_STATE}" 2>/dev/null || true
chmod 0750 "${DOWNLOADER_DOWNLOADS}" 2>/dev/null || true
chmod 0750 "${DOWNLOADER_CACHE}" 2>/dev/null || true

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable downloader.service >/dev/null 2>&1 || true
fi

exit 0
