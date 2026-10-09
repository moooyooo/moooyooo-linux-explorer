#!/bin/sh
set -eu
mlex_source=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mlex_build=${MLEX_BUILD_DIR:-"$mlex_source/../.build/moooyooo-linux-explorer/dev"}
if [ ! -f "$mlex_build/CMakeCache.txt" ]; then
    cmake -S "$mlex_source" -B "$mlex_build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
fi
cmake --build "$mlex_build" --parallel 4
exec "$mlex_build/moooyooo-linux-explorer" "$@"
