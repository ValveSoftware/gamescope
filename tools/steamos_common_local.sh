#!/bin/bash

set -euo pipefail

if [[ $# -gt 1 ]]; then
    echo "Usage: $(basename "$0") [device_password]"
    exit 1
fi

if [[ -f "/usr/bin/steamvr" ]]; then
    export STEAMOS_VR_SESSION=1
else
    export STEAMOS_VR_SESSION=0
fi

export STEAMOS_USER_PASSWORD="${1:-${STEAMOS_USER_PASSWORD:-}}"

# Put us in the tools folder...
export script=$(readlink -f -- "$0")

# Get to the script
pushd "$(dirname -- "$script")" > /dev/null

# Setup envsudo.
source ./steamos_password_helpers.sh