#!/bin/bash

set -euo pipefail

source "$(dirname "$0")/steamos_common_remote.sh" "$@"

pushd ..
echo "Copying $(pwd) to $STEAMOS_DEVICE_IP:$STEAMOS_DEVICE_PATH..."
envsshpass rsync -rzahs --delete --info=progress2 --exclude '/build*' --exclude '.git' \
    --rsync-path="mkdir -p $(envquote "$STEAMOS_DEVICE_PATH") && rsync" \
    . "steamos@$STEAMOS_DEVICE_IP:$STEAMOS_DEVICE_PATH"
