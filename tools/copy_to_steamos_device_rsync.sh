#!/bin/bash

set -euo pipefail

source "$(dirname "$0")/steamos_common_remote.sh" "$@"

pushd ..
echo "Copying $(pwd) to $STEAMOS_DEVICE_IP..."
envsshpass rsync -rzah --info=progress2  --exclude '/build*' --exclude '.git' . "steamos@$STEAMOS_DEVICE_IP:/home/steamos/gamescope_local"
