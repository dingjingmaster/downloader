#!/bin/sh
# Apply the tracked libsmb2 patches when they are not already present.
# A clean submodule checkout needs them for Kerberos support and buildability.
set -e
source_dir=$1
patch_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/patches
[ -d "$patch_dir" ] || exit 0
for p in "$patch_dir"/*.patch; do
    [ -f "$p" ] || continue
    if patch -p1 -d "$source_dir" --dry-run --batch --forward < "$p" >/dev/null 2>&1; then
        patch -p1 -d "$source_dir" --batch --forward < "$p"
    fi
done
