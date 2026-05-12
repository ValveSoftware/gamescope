#!/bin/bash

set -euo pipefail

source "$(dirname "$0")/steamos_common_local.sh" "$@"

steamvr stop

envsudo steamos-readonly disable
envsudo pacman -Sy --noconfirm gamescope

steamvr start
