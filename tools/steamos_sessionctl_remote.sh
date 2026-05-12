#!/bin/bash

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "Usage: $(basename "$0") <verb> <device_ip> [device_password]"
    exit 1
fi

export SESSION_VERB=$1

source "$(dirname "$0")/steamos_common_remote.sh" "$2" "$3"

./copy_to_steamos_device_rsync.sh "$STEAMOS_DEVICE_IP"
envsshpass ssh -t "steamos@$STEAMOS_DEVICE_IP" "/home/steamos/gamescope_local/tools/steamos_sessionctl_local.sh $SESSION_VERB $STEAMOS_USER_PASSWORD"
