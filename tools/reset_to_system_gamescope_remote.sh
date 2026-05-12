#!/bin/bash

set -euo pipefail

source "$(dirname "$0")/steamos_common_remote.sh" "$@"

./copy_to_steamos_device_rsync.sh "$STEAMOS_DEVICE_IP"
envsshpass ssh -t "steamos@$STEAMOS_DEVICE_IP" "/home/steamos/gamescope_local/tools/reset_to_system_gamescope_local.sh $STEAMOS_USER_PASSWORD"
