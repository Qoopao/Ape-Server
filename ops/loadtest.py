#!/usr/bin/env python3
"""Scaled load test for ape-server.

Uses the real WSS protocol (type 90/91/101/104/106) with N paired clients.
Each pair is made mutual friends via direct MySQL inserts before the run, then
the clients send 1:1 messages at a stepped aggregate rate. Delivery latency is
measured from the send frame to the recipient's 104 push.

Outputs JSON + per-second CSV.
"""
import argparse
import asyncio
import base64
import csv
import json
import os
import random
import ssl
import struct
import subprocess
import sys
import time
from collections import defaultdict

sys.path.insert(0, "/tmp/ape-smoke-pyproto")

SKIP_ACK = False
import apeauth_pb2  # noqa: E402
import sdkws_pb2  # noqa: E402


class WS:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer

    @classmethod
    async def connect(cls, host="127.0.0.1", port=6666):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        reader, writer = await asyncio.open_connection(host, port, ssl=ctx, limit=1 << 20)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        writer.write(req.encode())
        await writer.drain()
        head = await reader.readuntil(b"\r\n\r\n")
        if b"101" not in head.split(b"\r\n")[0]:
            raise RuntimeError(f"ws handshake failed: {head[:120]!r}")
        return cls(reader, writer)

    async def send_frame(self, opcode, data):
        header = bytearray([0x80 | opcode])
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", n)
        header += mask
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.writer.write(bytes(header) + payload)
        await self.writer.drain()

    async def recv_frame(self, timeout=10):
        async def _recv():
            hdr = await self.reader.readexactly(2)
            opcode = hdr[0] & 0x0F
            length = hdr[1] & 0x7F
            if length == 126:
                length = struct.unpack("!H", await self.reader.readexactly(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", await self.reader.readexactly(8))[0]
            mask = None
            if hdr[1] & 0x80:
                mask = await self.reader.readexactly(4)
            payload = await self.reader.readexactly(length)
            if mask:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            if opcode == 0x9:
                await self.send_frame(0xA, payload)
                return await _recv()
            if opcode == 0x8:
                raise ConnectionError("closed")
            return opcode, payload

        return await asyncio.wait_for(_recv(), timeout=timeout)

    async def send_req(self, req):
        await self.send_frame(0x2, req.SerializeToString())

    async def recv_resp(self, timeout=10):
        _, payload = await self.recv_frame(timeout=timeout)
        resp = sdkws_pb2.SdkWSResp()
        if not resp.ParseFromString(payload):
            raise RuntimeError("bad SdkWSResp")
        return resp

    async def close(self):
        try:
            await self.send_frame(0x8, b"")
        except Exception:
            pass
        self.writer.close()
        try:
            await self.writer.wait_closed()
        except Exception:
            pass


class Client:
    def __init__(self, index, account, token, ws):
        self.index = index
        self.account = account
        self.token = token
        self.ws = ws
        self.target = 0
        self.lock = asyncio.Lock()
        self.task = None
        self.acked_seq = 0

    async def send_msg(self, phase, seq, run_id):
        cid = f"{run_id}_p{phase}_{seq:07d}"
        content = f"ape loadtest seq={seq} ts={time.time():.6f}".encode()
        msg = sdkws_pb2.MsgData(
            sendID=self.account,
            recvID=self.target,
            clientMsgID=cid,
            sessionType=1,
            contentType=101,
            content=content,
            sendTime=time.time(),
        )
        req = sdkws_pb2.SendMessageReq()
        req.msgs.append(msg)
        outer = sdkws_pb2.SdkWSReq(
            type=101,
            token=self.token,
            account=self.account,
            requestId=cid,
            data=req.SerializeToString(),
        )
        async with self.lock:
            await self.ws.send_req(outer)
        return cid

    async def send_ack(self, md):
        if SKIP_ACK:
            return
        ack = sdkws_pb2.AckReq(userID=self.account, ackType=1)
        ca = ack.conversations.add()
        ca.conversationID = md.convID
        ca.upToSeq = md.seq
        outer = sdkws_pb2.SdkWSReq(
            type=106,
            token=self.token,
            account=self.account,
            requestId="ack",
            data=ack.SerializeToString(),
        )
        async with self.lock:
            try:
                await self.ws.send_req(outer)
            except Exception:
                pass


class Stats:
    def __init__(self):
        self.attempted = 0
        self.delivered = 0
        self.accepted = 0
        self.send_errors = 0
        self.recv_errors = 0
        self.error_codes = defaultdict(int)
        self.pending = {}
        self.accepted_cids = set()
        self.samples = []  # (phase, second, latency_ms)
        self.phase_attempted = defaultdict(int)
        self.phase_accepted = defaultdict(int)
        self.phase_delivered = defaultdict(int)
        self.phase_errors = defaultdict(int)
        self.phase_codes = defaultdict(lambda: defaultdict(int))
        self.attempted_by_sec = defaultdict(int)


async def register(phone, nickname, password):
    ws = await WS.connect()
    try:
        req = sdkws_pb2.SdkWSReq(type=90, requestId="reg-" + phone)
        reg = apeauth_pb2.RegisterRequest(phone=phone, nickname=nickname, password=password)
        req.data = reg.SerializeToString()
        await ws.send_req(req)
        resp = await ws.recv_resp(timeout=10)
        if resp.account == 0:
            raise RuntimeError(f"register failed: {resp.errorCode} {resp.errorMsg}")
        return resp.account
    finally:
        await ws.close()


async def login(account, password, device):
    ws = await WS.connect()
    req = sdkws_pb2.SdkWSReq(type=91, account=account, deviceID=device, requestId="login")
    req.data = password.encode()
    await ws.send_req(req)
    resp = await ws.recv_resp(timeout=10)
    if resp.errorCode not in ("", "0") or not resp.token:
        raise RuntimeError(f"login failed: {resp.errorCode} {resp.errorMsg}")
    # Drain 209 / 210 login sync notifications.
    while True:
        extra = await ws.recv_resp(timeout=5)
        if extra.type == 210:
            break
        if extra.type in (104, 200):
            # Shouldn't happen for a fresh user, but don't block login.
            continue
    return ws, resp.token


def sql(statement):
    subprocess.run(
        ["docker", "exec", "mysql", "mysql", "-uape_user", "-pape_password", "ape_auth", "-e", statement],
        check=True,
        capture_output=True,
        text=True,
    )


def make_friends(clients):
    pairs = []
    for i in range(0, len(clients), 2):
        a = clients[i].account
        b = clients[i + 1].account
        pairs.append((a, b))
    rows = ",".join(f"({a},{b},UNIX_TIMESTAMP()),({b},{a},UNIX_TIMESTAMP())" for a, b in pairs)
    pairs_rows = ",".join(f"({min(a,b)},{max(a,b)})" for a, b in pairs)
    sql(f"INSERT IGNORE INTO friends(owner_account,friend_account,created_time) VALUES {rows};")
    sql(f"INSERT IGNORE INTO friend_pairs(low_account,high_account) VALUES {pairs_rows};")


def pct(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    return values[int((len(values) - 1) * p)]


def lat_stats(values):
    if not values:
        return {"count": 0, "avgMs": 0, "p50Ms": 0, "p90Ms": 0, "p95Ms": 0, "p99Ms": 0, "maxMs": 0}
    values = sorted(values)
    return {
        "count": len(values),
        "avgMs": round(sum(values) / len(values), 2),
        "p50Ms": round(pct(values, 0.50), 2),
        "p90Ms": round(pct(values, 0.90), 2),
        "p95Ms": round(pct(values, 0.95), 2),
        "p99Ms": round(pct(values, 0.99), 2),
        "maxMs": round(values[-1], 2),
    }


def recv_loop(client, stats, run_id):
    async def loop():
        while True:
            try:
                resp = await client.ws.recv_resp(timeout=60)
            except asyncio.CancelledError:
                raise
            except Exception as e:
                print(f"recv_loop client={client.index} exit: {type(e).__name__}: {e}", flush=True)
                return
            now = time.perf_counter()
            if resp.type in (104, 200):
                md = sdkws_pb2.MsgData()
                if not md.ParseFromString(resp.data):
                    stats.recv_errors += 1
                    continue
                stats.delivered += 1
                item = stats.pending.pop(md.clientMsgID, None)
                if item is not None:
                    phase, second, sent_at = item
                    stats.phase_delivered[phase] += 1
                    stats.samples.append((phase, second, (now - sent_at) * 1000.0))
                await client.send_ack(md)
            elif resp.type == 101:
                if resp.errorCode not in ("", "0"):
                    item = stats.pending.get(resp.requestId)
                    phase = item[0] if item else 0
                    stats.send_errors += 1
                    stats.phase_errors[phase] += 1
                    stats.error_codes[resp.errorCode] += 1
                    stats.phase_codes[phase][resp.errorCode] += 1
                    continue
                sr = sdkws_pb2.SendMessageResp()
                if not sr.ParseFromString(resp.data):
                    stats.send_errors += 1
                    continue
                for info in sr.infos:
                    cid = info.msg.clientMsgID
                    item = stats.pending.get(cid)
                    phase = item[0] if item else 0
                    if info.errorCode in ("0", "", "PENDING"):
                        if cid not in stats.accepted_cids:
                            stats.accepted_cids.add(cid)
                            stats.accepted += 1
                            stats.phase_accepted[phase] += 1
                    else:
                        stats.send_errors += 1
                        stats.phase_errors[phase] += 1
                        stats.error_codes[info.errorCode] += 1
                        stats.phase_codes[phase][info.errorCode] += 1
            elif resp.type == 0:
                stats.recv_errors += 1

    return asyncio.create_task(loop())


async def run_phase(clients, stats, phase, rate, duration, run_id):
    loop = asyncio.get_event_loop()
    start = loop.time()
    end = start + duration
    interval = 1.0 / rate
    next_at = start
    seq = 0
    sem = asyncio.Semaphore(256)
    tasks = []

    async def send_one(client, second, seq_no):
        cid = f"{run_id}_p{phase}_{seq_no:07d}"
        stats.pending[cid] = (phase, second, time.perf_counter())
        try:
            await client.send_msg(phase, seq_no, run_id)
        except Exception:
            stats.pending.pop(cid, None)
            stats.send_errors += 1
            stats.phase_errors[phase] += 1
        finally:
            sem.release()

    while True:
        now = loop.time()
        if now >= end:
            break
        if next_at > now:
            await asyncio.sleep(next_at - now)
        next_at += interval
        if next_at < loop.time():
            next_at = loop.time()
        await sem.acquire()
        second = int(loop.time() - start)
        stats.attempted += 1
        stats.phase_attempted[phase] += 1
        stats.attempted_by_sec[(phase, second)] += 1
        seq += 1
        client = clients[seq % len(clients)]
        tasks.append(asyncio.create_task(send_one(client, second, seq)))
    await asyncio.gather(*tasks)
    return loop.time() - start


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clients", type=int, default=100)
    ap.add_argument("--rates", default="50,100,200,300,500")
    ap.add_argument("--phase", type=float, default=20.0)
    ap.add_argument("--warmup", type=float, default=5.0)
    ap.add_argument("--out", default="/root/ape-server/ops/loadtest/results")
    ap.add_argument("--prefix", default="")
    ap.add_argument("--no-ack", action="store_true")
    ap.add_argument("--warmup-drain", type=float, default=10.0)
    args = ap.parse_args()

    if args.clients % 2 != 0 or args.clients < 2:
        raise SystemExit("clients must be even and >= 2")
    rates = [int(x) for x in args.rates.split(",") if x.strip()]
    run_id = args.prefix or ("ape_" + time.strftime("%m%d_%H%M%S"))
    os.makedirs(args.out, exist_ok=True)

    global SKIP_ACK
    SKIP_ACK = args.no_ack
    stats = Stats()
    clients = []
    started = time.time()
    print(f"run_id={run_id} clients={args.clients} rates={rates} phase={args.phase}s")

    t0 = time.perf_counter()
    base = random.randrange(10_000_000, 90_000_000)
    for i in range(args.clients):
        phone = f"137{(base + i) % 100000000:08d}"
        account = await register(phone, f"lt_{run_id}_{i}", "pw123456")
        ws, token = await login(account, "pw123456", f"ltdev-{i}")
        clients.append(Client(i, account, token, ws))
        if (i + 1) % 10 == 0 or i == args.clients - 1:
            print(f"  logged in {i+1}/{args.clients}")
    setup_seconds = time.perf_counter() - t0
    print(f"setup: {setup_seconds:.2f}s")

    for i, c in enumerate(clients):
        c.target = clients[i ^ 1].account
    make_friends(clients)
    print("friendships inserted")

    recv_tasks = [recv_loop(c, stats, run_id) for c in clients]

    await run_phase(clients, stats, 0, 20, args.warmup, run_id)
    warmup_attempted = stats.attempted
    await asyncio.sleep(args.warmup_drain)
    stats.__init__()
    print("warmup done, measurement starts")

    results = []
    for idx, rate in enumerate(rates, start=1):
        print(f"phase {idx}: target {rate} msg/s for {args.phase}s")
        duration = await run_phase(clients, stats, idx, rate, args.phase, run_id)
        await asyncio.sleep(2)
        lats = [s[2] for s in stats.samples if s[0] == idx]
        attempted = stats.phase_attempted[idx]
        delivered = stats.phase_delivered[idx]
        accepted = stats.phase_accepted[idx]
        errors = stats.phase_errors[idx]
        res = {
            "targetRps": rate,
            "durationSec": round(duration, 2),
            "attempted": attempted,
            "accepted": accepted,
            "delivered": delivered,
            "sendErrors": errors,
            "sentRps": round(attempted / duration, 1) if duration else 0,
            "achievedRps": round(delivered / duration, 1) if duration else 0,
            "errorRatePct": round(100.0 * errors / attempted, 2) if attempted else 0,
            "latency": lat_stats(lats),
            "errorCodes": dict(stats.phase_codes[idx]),
        }
        results.append(res)
        print(
            f"  attempted={attempted} delivered={delivered} accepted={accepted} "
            f"errors={errors} achieved={res['achievedRps']}/s "
            f"p50={res['latency']['p50Ms']}ms p95={res['latency']['p95Ms']}ms p99={res['latency']['p99Ms']}ms"
        )

    total_attempted = sum(r["attempted"] for r in results)
    total_delivered = sum(r["delivered"] for r in results)
    total_errors = sum(r["sendErrors"] for r in results)

    second_keys = sorted(stats.attempted_by_sec.keys())
    series = []
    for phase_idx, second in second_keys:
        lats = [s[2] for s in stats.samples if s[0] == phase_idx and s[1] == second]
        series.append({
            "phase": phase_idx,
            "second": second,
            "attempted": stats.attempted_by_sec[(phase_idx, second)],
            "delivered": len(lats),
            "p95Ms": round(pct(lats, 0.95), 2) if lats else 0,
        })

    finished = time.time()
    report = {
        "meta": {
            "runId": run_id,
            "startedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(started)),
            "finishedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(finished)),
            "durationSec": round(finished - started, 1),
            "clients": args.clients,
            "rates": rates,
            "phaseDurationSec": args.phase,
            "setupSeconds": round(setup_seconds, 2),
            "gateway": "wss://127.0.0.1:6666",
            "protocol": "type 90/91/101/104/106",
            "warmupAttempted": warmup_attempted,
        },
        "setup": {
            "clients": args.clients,
            "registerAndLoginSeconds": round(setup_seconds, 2),
            "friendships": args.clients // 2,
        },
        "totals": {
            "attempted": total_attempted,
            "delivered": total_delivered,
            "sendErrors": total_errors,
            "deliveryRatePct": round(100.0 * total_delivered / total_attempted, 2) if total_attempted else 0,
        },
        "phases": results,
        "series": series,
    }

    json_path = os.path.join(args.out, run_id + ".json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)

    csv_path = os.path.join(args.out, run_id + ".csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["phase", "second", "attempted", "delivered", "p95_ms"])
        for row in series:
            w.writerow([row["phase"], row["second"], row["attempted"], row["delivered"], row["p95Ms"]])

    print(f"report: {json_path}")
    print(f"csv:    {csv_path}")

    for t in recv_tasks:
        t.cancel()
    await asyncio.gather(*recv_tasks, return_exceptions=True)
    for c in clients:
        await c.ws.close()


if __name__ == "__main__":
    asyncio.run(main())