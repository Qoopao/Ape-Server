#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set -e
rm -rf /tmp/ape-smoke-pyproto
mkdir -p /tmp/ape-smoke-pyproto
./vcpkg_installed/x64-linux/tools/protobuf/protoc -I=proto --python_out=/tmp/ape-smoke-pyproto proto/sdkws.proto proto/apeauth.proto proto/msg.proto
ls -lh /tmp/ape-smoke-pyproto