#!/usr/bin/env python3
"""可观测端到端冒烟：用客户端拿到的 logID 反查 Jaeger，并核对 VictoriaMetrics 指标。

前置：
  - 服务端已启动，且客户端已完成一次单聊发送；
  - OTel collector / Jaeger / VictoriaMetrics 可用；
  - 客户端响应里的 SdkWSResp.logID（= 发送 trace id）与 serverMsgID 已知。

示例：
  python3 scripts/observability/observability_smoke.py \
      --log-id 4bf92f3577b34da6a3ce929d0e0e4736 \
      --server-msg-id 0f1e2d3c-4b5a-6978-8a9b-0c1d2e3f4a5b \
      --delivery-service ape-msg
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def get_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=10) as response:
        return json.loads(response.read().decode("utf-8"))


def fetch_trace(jaeger: str, trace_id: str) -> list[dict]:
    payload = get_json(f"{jaeger.rstrip('/')}/api/traces/{trace_id}")
    return payload.get("data") or []


def find_trace_by_tag(jaeger: str, service: str, tag: str, value: str) -> list[dict]:
    query = urllib.parse.urlencode({"service": service, "tags": json.dumps({tag: value}), "limit": 20})
    payload = get_json(f"{jaeger.rstrip('/')}/api/traces?{query}")
    return payload.get("data") or []


def span_names(trace: dict) -> set[str]:
    return {span.get("operationName", "") for span in trace.get("spans", [])}


def service_names(trace: dict) -> set[str]:
    return {process.get("serviceName", "") for process in trace.get("processes", {}).values()}


def metric_samples(vm: str, expression: str) -> list[dict]:
    query = urllib.parse.urlencode({"query": expression})
    payload = get_json(f"{vm.rstrip('/')}/api/v1/query?{query}")
    return (payload.get("data") or {}).get("result") or []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log-id", required=True, help="SdkWSResp.logID（发送 trace id）")
    parser.add_argument("--server-msg-id", help="服务端 serverMsgID，用于查找投递尝试 span")
    parser.add_argument("--delivery-service", default="ape-msg",
                        help="投递尝试 span 所在 service.name（--role=all 时为 ape-all）")
    parser.add_argument("--jaeger", default="http://localhost:16686")
    parser.add_argument("--vm", default="http://localhost:8428")
    args = parser.parse_args()

    traces = fetch_trace(args.jaeger, args.log_id)
    check(bool(traces), f"Jaeger 中存在 trace {args.log_id}")
    if traces:
        trace = traces[0]
        names = span_names(trace)
        check(any(name.startswith("WS ") for name in names),
              f"发送 trace 含 WS root span（{sorted(names)}）")
        services = service_names(trace)
        check(any(name.startswith("ape-") for name in services),
              f"trace 的 service.name 已按 role 区分（{sorted(services)}）")

    if args.server_msg_id:
        attempts = find_trace_by_tag(args.jaeger, args.delivery_service,
                                     "im.server_msg_id", args.server_msg_id)
        check(bool(attempts), f"按 im.server_msg_id={args.server_msg_id} 找到投递尝试 trace")
        if attempts:
            names = span_names(attempts[0])
            check("outbox.publish.op1" in names,
                  f"投递尝试 trace 含 outbox.publish.op1（{sorted(names)}）")

    check(bool(metric_samples(args.vm, "ape_im_send_total")),
          "VictoriaMetrics 有 ape_im_send_total")
    check(bool(metric_samples(args.vm, "ape_outbox_pending")),
          "VictoriaMetrics 有 ape_outbox_pending")
    check(bool(metric_samples(args.vm, "ape_ws_connections_active")),
          "VictoriaMetrics 有 ape_ws_connections_active")

    if failures:
        print(f"\n{len(failures)} check(s) failed")
        return 1
    print("\nobservability smoke passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (urllib.error.URLError, TimeoutError) as error:
        print(f"cannot reach observability backend: {error}", file=sys.stderr)
        sys.exit(2)
