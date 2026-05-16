#!/usr/bin/env python3
"""
手动构造一条 OTLP Trace 并发送到 OTEL Collector (localhost:4317)

这条 Trace 模拟一次 IM 消息发送的完整链路:
  WebServer (Root) → MsgService/SendMessages → MongoDB insert

运行:
  python3 scripts/send_trace_demo.py
"""

import time
import uuid
from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from opentelemetry.sdk.resources import SERVICE_NAME, Resource

# ── 1. 初始化: 连接 Collector ──────────────────────────
# 创建 resource,标识这个 trace 来自哪个服务
resource = Resource(attributes={
    SERVICE_NAME: "ape-webserver",
    "service.namespace": "ape-im",
    "service.version": "1.0.0",
})

# 创建 TracerProvider,绑定 resource
provider = TracerProvider(resource=resource)

# 创建 OTLP gRPC exporter,指向 Collector 的 4317 端口
otlp_exporter = OTLPSpanExporter(
    endpoint="localhost:4317",
    insecure=True,  # 不用 TLS
)

# 用 BatchSpanProcessor 包装 exporter (与 Collector 的 batch processor 呼应)
processor = BatchSpanProcessor(otlp_exporter)
provider.add_span_processor(processor)
trace.set_tracer_provider(provider)

# 获取一个 tracer 实例
tracer = trace.get_tracer("demo.tracer")


def simulate_work(name: str, duration_ms: int, parent_ctx=None):
    """
    模拟一个工作步骤,创建 Span 并 sleep 模拟耗时
    """
    if parent_ctx:
        # 有父 Span → 设置 parent context
        ctx = trace.set_span_in_context(parent_ctx)
        span = tracer.start_span(name, context=ctx)
    else:
        # 无父 Span → Root Span
        span = tracer.start_span(name)

    span.set_attribute("demo.step", name)
    span.set_attribute("demo.duration_ms", duration_ms)
    time.sleep(duration_ms / 1000.0)  # 模拟真实耗时

    span.end()
    return span


# ── 2. 构造一条 Trace: WebServer → MsgService → MongoDB ──

print("=" * 60)
print("  OTel Trace Demo: IM 消息发送链路")
print("=" * 60)

# Span 1: WebServer 收到 WebSocket 消息 (Root Span)
print("\n[1/3] WebServer 收到 /msg/send 请求 ...")
ws_span = tracer.start_span("WS /msg/send")
ws_span.set_attribute("ws.message_type", "send_msg")
ws_span.set_attribute("ws.user_id", "123456")
ws_span.set_attribute("ws.operation", "SendMessages")
ws_span.set_attribute("ws.message_size", 1024)
time.sleep(0.05)  # 模拟 50ms 处理

# 获取 Root Span 的 context,准备传给下游
ws_ctx = trace.set_span_in_context(ws_span)

print(f"  ├─ TraceID:  {ws_span.get_span_context().trace_id:032x}")
print(f"  ├─ SpanID:   {ws_span.get_span_context().span_id:016x}")
print(f"  └─ ParentID: (none, 我是 Root)")

# Span 2: 调用 MsgService.SendMessages (Child of ws_span)
print("\n[2/3] gRPC 调用 MsgService/SendMessages ...")
rpc_span = tracer.start_span(
    "MsgService/SendMessages",
    context=ws_ctx,  # 设置父 context → 自动继承 trace_id + 设置 parent_span_id
    attributes={
        "rpc.system": "grpc",
        "rpc.service": "MsgService",
        "rpc.method": "SendMessages",
        "rpc.grpc.status_code": 0,
        "net.peer.ip": "10.0.0.5",
    },
)
time.sleep(0.12)  # 模拟 120ms RPC 调用

rpc_ctx = trace.set_span_in_context(rpc_span)

print(f"  ├─ TraceID:  {rpc_span.get_span_context().trace_id:032x}")
print(f"  ├─ SpanID:   {rpc_span.get_span_context().span_id:016x}")
print(f"  └─ ParentID: {ws_span.get_span_context().span_id:016x}")

# Span 3: MongoDB 写入 (Child of rpc_span)
print("\n[3/3] MongoDB 写入消息 ...")
db_span = tracer.start_span(
    "MongoDB insert conversations",
    context=rpc_ctx,
    attributes={
        "db.system": "mongodb",
        "db.collection.name": "conversations",
        "db.operation": "insert",
        "db.mongodb.document_size": 2048,
    },
)
time.sleep(0.08)  # 模拟 80ms 数据库写入

db_span.end()
rpc_span.end()
ws_span.end()

print(f"  ├─ TraceID:  {db_span.get_span_context().trace_id:032x}")
print(f"  ├─ SpanID:   {db_span.get_span_context().span_id:016x}")
print(f"  └─ ParentID: {rpc_span.get_span_context().span_id:016x}")

# ── 3. 导出 ──
print("\n" + "=" * 60)
print("  导出数据到 OTEL Collector (localhost:4317) ...")

# BatchSpanProcessor 需要手动 flush,否则可能没发完就退出了
processor.force_flush(timeout_millis=5000)

print("  完成! 打开 Jaeger 查看:")
print("  → http://localhost:16686")
print("  → Service 选 'ape-webserver'")
print("  → 点击 'Find Traces'")
print("=" * 60)
