#!/bin/bash

set -euo pipefail

source "$(dirname "$0")/steamos_common_local.sh" "$@"

pushd ..

echo "Setting up build..."
meson setup build.local --prefix=/usr

echo "Building gamescope..."
meson compile -C build.local
