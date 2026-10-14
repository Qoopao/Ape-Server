#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_root="$PWD/build/group-test-tools"
mkdir -p "$task_root"
cd "$task_root"
if [[ ! -x root/usr/sbin/mariadbd ]]; then
    apt-get -o Acquire::Retries=3 download mariadb-server-core mariadb-client-core libaio1t64
    for package_file in ./*.deb; do dpkg-deb -x "$package_file" root; done
fi
if [[ ! -e root/usr/lib/x86_64-linux-gnu/liburing.so.2 ]]; then
    apt-get -o Acquire::Retries=3 download liburing2
    for package_file in ./liburing2*.deb; do dpkg-deb -x "$package_file" root; done
fi
if [[ ! -e root/usr/lib/x86_64-linux-gnu/libnuma.so.1 ]]; then
    apt-get -o Acquire::Retries=3 download libnuma1
    for package_file in ./libnuma1*.deb; do dpkg-deb -x "$package_file" root; done
fi
export LD_LIBRARY_PATH="$task_root/root/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
# Each run gets its own data directory. No existing database or service is touched.
data_dir="$(mktemp -d "$task_root/data.XXXXXX")"
server="$task_root/root/usr/sbin/mariadbd"
client="$task_root/root/usr/bin/mariadb"
printf 'CREATE DATABASE mysql;\nCREATE DATABASE im_group_test;\n' | "$server" --no-defaults --bootstrap --skip-grant-tables --user=root --datadir="$data_dir" > "$data_dir/bootstrap.log" 2>&1
"$server" --no-defaults --skip-grant-tables --user=root --datadir="$data_dir" --port=33079 --bind-address=127.0.0.1 --socket="$data_dir/mysql.sock" --pid-file="$data_dir/mysql.pid" --skip-log-bin > "$data_dir/server.log" 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true' EXIT
for attempt in {1..100}; do
    if "$client" --no-defaults --host=127.0.0.1 --port=33079 --user=root -e 'SELECT 1' > /dev/null 2>&1; then break; fi
    if ! kill -0 "$server_pid" 2>/dev/null; then cat "$data_dir/server.log"; exit 1; fi
    sleep 0.1
done
cd ../..
cmake -S . -B build -DAPE_GROUP_TESTS=ON > build/group-test-config.log 2>&1
cmake --build build --target group_integration -j 2 > build/group-test-build.log 2>&1
cmake --build build --target main -j 2 >> build/group-test-build.log 2>&1
timeout 120 build/group_integration | tee build/group-test-results.log
