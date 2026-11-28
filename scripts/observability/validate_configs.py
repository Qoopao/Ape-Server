#!/usr/bin/env python3
"""可观测配置的静态校验：不启动任何容器，只检查文件结构与引用一致性。

用法：python3 scripts/observability/validate_configs.py
退出码 0 = 全部通过。
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
OTEL_DIR = ROOT / "docker" / "otel"
GRAFANA_DIR = OTEL_DIR / "grafana"

# 与 src/om/obs_metrics.cpp、otel_metrics.cpp、otel_grpc_interceptor.cpp 保持一致。
KNOWN_METRICS = {
    "ape_ws_connections_active",
    "ape_ws_frames_total",
    "ape_ws_request_duration_milliseconds_bucket",
    "ape_ws_auth_total",
    "ape_ws_kicks_total",
    "ape_ws_write_drops_total",
    "ape_ws_write_queue_frames",
    "ape_ws_route_renew_failures_total",
    "ape_im_send_total",
    "ape_im_send_duration_milliseconds_bucket",
    "ape_outbox_attempts_total",
    "ape_outbox_tombstones_total",
    "ape_outbox_pending",
    "ape_outbox_oldest_age",
    "ape_kafka_produce_total",
    "ape_kafka_produce_duration_milliseconds_bucket",
    "ape_kafka_consume_total",
    "ape_kafka_lag",
    "ape_push_deliveries_total",
    "ape_push_acks_total",
    "ape_group_fanout_recipients_total",
    "ape_group_fanout_duration_milliseconds_bucket",
    "rpc_server_requests_total",
    "rpc_server_duration_milliseconds_bucket",
}

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def load_yaml(path: Path):
    with path.open(encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def metric_names(expr: str) -> set[str]:
    # 去掉 by(...) / 函数名后按标识符扫描；只保留 ape_/rpc_ 前缀。
    return {
        token
        for token in re.findall(r"[a-zA-Z_][a-zA-Z0-9_]*", expr)
        if token.startswith(("ape_", "rpc_"))
    }


def validate_datasources() -> None:
    path = GRAFANA_DIR / "provisioning" / "datasources" / "datasources.yaml"
    check(path.exists(), f"missing {path.relative_to(ROOT)}")
    if not path.exists():
        return
    document = load_yaml(path) or {}
    sources = {item.get("uid"): item for item in document.get("datasources", [])}
    for uid in ("vm", "jaeger", "es"):
        check(uid in sources, f"datasource uid missing: {uid}")
    check(sources.get("vm", {}).get("isDefault") is True, "VictoriaMetrics must be default")


def validate_dashboards() -> None:
    provider = GRAFANA_DIR / "provisioning" / "dashboards" / "dashboards.yaml"
    check(provider.exists(), f"missing {provider.relative_to(ROOT)}")
    if provider.exists():
        document = load_yaml(provider) or {}
        providers = document.get("providers", [])
        check(bool(providers), "dashboard provider is empty")

    dashboards = sorted((GRAFANA_DIR / "dashboards").glob("*.json"))
    check(len(dashboards) >= 4, f"expected >=4 dashboards, got {len(dashboards)}")
    for path in dashboards:
        with path.open(encoding="utf-8") as handle:
            dashboard = json.load(handle)
        name = path.name
        check(bool(dashboard.get("uid")), f"{name}: missing uid")
        check(bool(dashboard.get("title")), f"{name}: missing title")
        panels = dashboard.get("panels") or []
        check(bool(panels), f"{name}: no panels")
        for panel in panels:
            check(bool(panel.get("title")), f"{name}: panel without title")
            datasource = (panel.get("datasource") or {}).get("uid")
            check(datasource == "vm", f"{name}: panel {panel.get('title')} not on vm")
            targets = panel.get("targets") or []
            check(bool(targets), f"{name}: panel {panel.get('title')} has no targets")
            for target in targets:
                expr = target.get("expr", "")
                unknown = metric_names(expr) - KNOWN_METRICS
                check(not unknown, f"{name}: unknown metric(s) {sorted(unknown)} in {expr!r}")


def validate_alerts() -> None:
    path = GRAFANA_DIR / "provisioning" / "alerting" / "alert-rules.yaml"
    check(path.exists(), f"missing {path.relative_to(ROOT)}")
    if not path.exists():
        return
    document = load_yaml(path) or {}
    groups = document.get("groups", [])
    check(bool(groups), "alert groups empty")
    rules = [rule for group in groups for rule in group.get("rules", [])]
    check(len(rules) >= 6, f"expected >=6 alert rules, got {len(rules)}")
    for rule in rules:
        uid = rule.get("uid", "<no-uid>")
        check(bool(rule.get("condition")), f"{uid}: missing condition")
        refs = {item.get("refId") for item in rule.get("data", [])}
        check(rule.get("condition") in refs, f"{uid}: condition ref not in data")
        check(bool((rule.get("annotations") or {}).get("summary")),
              f"{uid}: missing annotation summary")


def validate_collector() -> None:
    path = OTEL_DIR / "otel-collector-config.yaml"
    check(path.exists(), f"missing {path.relative_to(ROOT)}")
    if not path.exists():
        return
    config = load_yaml(path) or {}
    processors = config.get("processors", {})
    check("memory_limiter" in processors, "collector: missing memory_limiter")
    check("batch" in processors, "collector: missing batch processor")
    pipelines = (config.get("service") or {}).get("pipelines", {})
    for name, pipeline in pipelines.items():
        check("memory_limiter" in (pipeline.get("processors") or []),
              f"collector: pipeline {name} lacks memory_limiter")
    log_exporters = (pipelines.get("logs") or {}).get("exporters") or []
    check(log_exporters == ["elasticsearch"],
          f"collector: logs exporters should be [elasticsearch], got {log_exporters}")


def validate_compose_mounts() -> None:
    compose = OTEL_DIR / "docker-compose.yml"
    check(compose.exists(), "missing docker/otel/docker-compose.yml")
    if not compose.exists():
        return
    for relative in ("grafana/provisioning", "grafana/dashboards"):
        check((OTEL_DIR / relative).exists(), f"compose mount missing: {relative}")


def main() -> int:
    validate_datasources()
    validate_dashboards()
    validate_alerts()
    validate_collector()
    validate_compose_mounts()
    if failures:
        print("observability config validation FAILED:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("observability config validation passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
