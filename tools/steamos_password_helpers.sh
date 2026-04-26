#!/bin/bash

set -euo pipefail

envsudo() {
    if [[ -n "${STEAMOS_USER_PASSWORD:-}" ]]; then
        echo "$STEAMOS_USER_PASSWORD" | sudo -S "$@"
    else
        sudo "$@"
    fi
}

envsshpass() {
    if [[ -n "${STEAMOS_USER_PASSWORD:-}" ]]; then
        sshpass -p "$STEAMOS_USER_PASSWORD" "$@"
    else
        sudo "$@"
    fi
}
