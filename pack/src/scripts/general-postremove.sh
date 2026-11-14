#!/bin/bash
set -e

if command -v systemctl >/dev/null 2>&1; then
    systemctl stop downloader.service >/dev/null 2>&1 || true
    systemctl disable downloader.service >/dev/null 2>&1 || true
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

# Keep /var/lib/downloader and the downloader user so completed downloads survive removal.

exit 0
