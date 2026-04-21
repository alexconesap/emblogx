#!/usr/bin/env bash

echo "Use -V for verbosity (enables console output)"
echo "Use -R for filtering tests (e.g. -R CRC32)"

set -euo pipefail

CDIR=$(pwd)

if [ ! -d build ]; then
    mkdir -p build
    cd build
    cmake ..
    cd "$CDIR"
fi

cd build
cmake --build .

CTEST_ARGS=(--output-on-failure)

if [ "$#" -gt 0 ]; then
    CTEST_ARGS+=("$@")
fi

ctest "${CTEST_ARGS[@]}"

cd "$CDIR"
