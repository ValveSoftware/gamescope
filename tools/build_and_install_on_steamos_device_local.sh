#!/bin/bash

set -euo pipefail

source "$(dirname "$0")/steamos_common_local.sh" "$@"

./build_on_steamos_device_local.sh

# Get to code root.
pushd ..

echo "Installing built gamescope..."
envsudo steamos-readonly disable

steamvr stop

# One option is to do this, but this also installs a bunch of headers and rubbish
# that we don't want.
#envsudo meson install --skip-subprojects -C build.local

# So just install the main gamescope executable for now...
envsudo cp -ra build.local/src/gamescope /usr/bin/gamescope

steamvr start
