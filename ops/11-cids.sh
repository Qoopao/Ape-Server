#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
docker exec mysql mysql -uape_user -pape_password -N -B -e "
SELECT 'outbox_cids', COUNT(*) FROM im_outbox WHERE dedupe_key LIKE '%ape_full_185220%';
SELECT 'outbox_distinct', COUNT(DISTINCT dedupe_key) FROM im_outbox WHERE dedupe_key LIKE '%ape_full_185220%';
SELECT 'outbox_done', COUNT(*) FROM im_outbox WHERE dedupe_key LIKE '%ape_full_185220%' AND state=2;
SELECT 'outbox_pending', COUNT(*) FROM im_outbox WHERE dedupe_key LIKE '%ape_full_185220%' AND state IN (0,1);
SELECT 'outbox_failed', COUNT(*) FROM im_outbox WHERE dedupe_key LIKE '%ape_full_185220%' AND state=3;
" ape_auth 2>/dev/null