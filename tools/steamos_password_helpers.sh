#!/bin/bash

set -euo pipefail

envquote() {
    printf '%q' "$1"
}

envsudo() {
    if [[ -n "${STEAMOS_USER_PASSWORD:-}" ]]; then
        echo "$STEAMOS_USER_PASSWORD" | sudo -S 2>/dev/null "$@"
    else
        sudo "$@"
    fi
}

envsshpass() {
    if [[ -n "${STEAMOS_USER_PASSWORD:-}" ]]; then
        sshpass -p "$STEAMOS_USER_PASSWORD" -- "$@"
    else
        sudo "$@"
    fi
}
