#!/bin/sh
# Install development packages on the Ubuntu development host.
# Run this in a regular terminal with sudo support; it never collects passwords.
set -eu
if ! command -v apt-get >/dev/null 2>&1; then
    echo 'This installer requires an Ubuntu/Debian host with apt-get.' >&2
    exit 1
fi
if [ "$(id -u)" -eq 0 ]; then
    apt-get update
    apt-get install --no-remove --no-install-recommends -y \
        build-essential cmake ninja-build pkg-config \
        qt6-base-dev qt6-base-dev-tools qt6-wayland libglib2.0-dev xvfb
else
    sudo apt-get update
    sudo apt-get install --no-remove --no-install-recommends -y \
        build-essential cmake ninja-build pkg-config \
        qt6-base-dev qt6-base-dev-tools qt6-wayland libglib2.0-dev xvfb
fi
echo 'Development packages installed. Run python3 tools/doctor.py to verify.'
