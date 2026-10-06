#!/bin/bash

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "Usage: $(basename "$0") <scope> <device_ip> [device_password]"
    exit 1
fi

export CLEAN_SCOPE=$1

source "$(dirname "$0")/steamos_common_remote.sh" "$2" "${3:-}"

if [[ "$CLEAN_SCOPE" == "tree" ]]; then
    export STEAMOS_CLEAN_PATH="$STEAMOS_DEVICE_PATH"
elif [[ "$CLEAN_SCOPE" == "all" ]]; then
    export STEAMOS_CLEAN_PATH="$STEAMOS_DEVICE_ROOT"
else
    echo "Usage: $(basename "$0") <tree|all> <device_ip> [device_password]"
    exit 1
fi

echo "Removing $STEAMOS_CLEAN_PATH from $STEAMOS_DEVICE_IP..."
envsshpass ssh -t "steamos@$STEAMOS_DEVICE_IP" "rm -rf $(envquote "$STEAMOS_CLEAN_PATH")"
