#!/bin/bash

set -euo pipefail

source "$(dirname "$0")/steamos_common_local.sh" "$@"

pushd ..

# Older deploys left .git-only subproject husks behind, meson never refetches those.
for dir in subprojects/*/; do
    if [[ "$(ls -A "$dir")" == ".git" ]]; then
        echo "Removing broken subproject $dir..."
        rm -rf "$dir"
    fi
done

echo "Setting up build..."
MESON_ARGS=(--prefix=/usr -Denable_tests=false -Denable_zenity=false)
if [[ -d build.local ]]; then
    # A plain setup skips an existing directory, which breaks after a meson update on the device.
    meson setup --reconfigure build.local "${MESON_ARGS[@]}" || meson setup --wipe build.local "${MESON_ARGS[@]}"
else
    meson setup build.local "${MESON_ARGS[@]}"
fi

echo "Building gamescope..."
meson compile -C build.local
