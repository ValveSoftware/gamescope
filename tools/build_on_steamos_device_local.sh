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
meson setup build.local --prefix=/usr -Denable_tests=false -Denable_zenity=false

echo "Building gamescope..."
meson compile -C build.local
