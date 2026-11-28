#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
RUN="ape_full_$(date +%H%M%S)"
OUT=ops/loadtest/results
mkdir -p "$OUT" logs
echo "RUN=$RUN"
bash ops/counts.sh "$OUT/$RUN-before.txt"
python3 ops/sampler_ape.py "$OUT/$RUN-resources.csv" &
SPID=$!
trap 'kill $SPID 2>/dev/null || true' EXIT
python3 ops/loadtest.py \
  --clients 20 \
  --rates 5,10,20,30,40 \
  --phase 20 --warmup 5 --warmup-drain 10 \
  --prefix "$RUN" --out "$OUT" 2>&1 | tee "logs/$RUN.log"
RC=${PIPESTATUS[0]}
sleep 8
bash ops/counts.sh "$OUT/$RUN-after.txt"
kill $SPID 2>/dev/null || true
echo "LOADTEST_EXIT=$RC"
echo "RUN=$RUN"