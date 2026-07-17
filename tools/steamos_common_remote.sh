#!/bin/bash

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $(basename "$0") <target_device> [device_password]"
    exit 1
fi

export STEAMOS_DEVICE_IP="$1"
export STEAMOS_USER_PASSWORD="${2:-${STEAMOS_USER_PASSWORD:-}}"

# Put us in the tools folder...
export script=$(readlink -f -- "$0")

# Get to the script
pushd "$(dirname -- "$script")" > /dev/null

source ./steamos_password_helpers.sh
