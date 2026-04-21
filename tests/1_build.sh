#!/bin/bash
set -euo pipefail
CDIR=$(pwd)
mkdir -p build
cd build
cmake ..
cd "$CDIR"
echo "Build configured. Run ./2_run.sh to build and execute tests."
