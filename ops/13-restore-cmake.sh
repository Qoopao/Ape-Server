#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug >/tmp/ape-cmake-restore.log 2>&1
echo "cmake_rc=$?"
grep -c 'stress_test' build/build.ninja || true
git diff -- CMakeLists.txt | tail -20