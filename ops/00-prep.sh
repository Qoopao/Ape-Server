#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
for n in mysql kafka-standalone kafka-ui; do
  if docker ps -a --format '{{.Names}}' | grep -qx "$n"; then
    new="legacy-${n}-$(date +%H%M%S)"
    docker rename "$n" "$new" && echo "renamed $n -> $new"
  fi
done
docker ps -a --format '{{.Names}}' | grep -E '^(mysql|kafka-standalone|kafka-ui|ape-mongo|ape-redis)$' || true