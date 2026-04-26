#!/bin/bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $(basename "$0") <verb> [device_password]"
    exit 1
fi

source "$(dirname "$0")/steamos_common_local.sh" "$2"

export SESSION_VERB=$1

if [[ "$STEAMOS_VR_SESSION" == "1" ]]; then
    steamvr "$SESSION_VERB"
else
    envsudo systemctl "$SESSION_VERB" sddm
fi
