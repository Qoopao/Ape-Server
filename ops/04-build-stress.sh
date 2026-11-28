#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set -e
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug > /tmp/ape-cmake.log 2>&1
cmake --build build --target stress_test -j8 2>&1 | tee /tmp/ape-stress-build.log
ls -lh build/stress_test