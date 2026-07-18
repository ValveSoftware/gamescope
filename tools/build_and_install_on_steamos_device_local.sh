#!/bin/bash

set -euo pipefail

source "$(dirname "$0")/steamos_common_local.sh" "$@"

./build_on_steamos_device_local.sh

# Get to code root.
pushd ..

echo "Installing built gamescope..."
envsudo steamos-readonly disable

steamvr stop

envsudo meson install --skip-subprojects --tags runtime -C build.local

steamvr start
