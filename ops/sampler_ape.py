#!/usr/bin/env python3
"""Sample ape-server process + dependency container resources to CSV."""
import csv
import os
import signal
import subprocess
import sys
import time

OUT = sys.argv[1]
HZ = os.sysconf("SC_CLK_TCK")
INTERVAL = 2.0
CONTAINER_EVERY = 5
stop = False


def _stop(*_args):
    global stop
    stop = True


signal.signal(signal.SIGTERM, _stop)
signal.signal(signal.SIGINT, _stop)


def snapshot():
    procs = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            cmd = open(f"/proc/{pid}/cmdline", "rb").read().replace(b"\x00", b" ").decode(errors="ignore")
            if "/root/ape-server/build/main" not in cmd:
                continue
            stat = open(f"/proc/{pid}/stat").read()
            rest = stat[stat.rfind(")") + 2 :].split()
            ticks = int(rest[11]) + int(rest[12])
            rss_kb = 0
            with open(f"/proc/{pid}/status") as fh:
                for line in fh:
                    if line.startswith("VmRSS:"):
                        rss_kb = int(line.split()[1])
                        break
            procs[pid] = (ticks, rss_kb)
        except (FileNotFoundError, ProcessLookupError, IndexError, ValueError):
            continue
    return procs


def container_rows():
    try:
        out = subprocess.run(
            ["docker", "stats", "--no-stream", "--format", "{{.Name}}|{{.CPUPerc}}|{{.MemUsage}}"],
            capture_output=True,
            text=True,
            timeout=20,
        ).stdout
    except Exception:
        return []
    rows = []
    for line in out.strip().splitlines():
        parts = line.split("|")
        if len(parts) != 3:
            continue
        name, cpu, mem = parts
        if name not in ("mysql", "ape-mongo", "kafka-standalone", "ape-redis"):
            continue
        cpu = cpu.replace("%", "").strip()
        token = mem.split("/")[0].strip()
        mb = 0.0
        try:
            if token.endswith("MiB"):
                mb = float(token[:-3])
            elif token.endswith("GiB"):
                mb = float(token[:-3]) * 1024
            elif token.endswith("KiB"):
                mb = float(token[:-3]) / 1024
        except ValueError:
            pass
        rows.append((name, cpu, round(mb, 1)))
    return rows


def main():
    fh = open(OUT, "w", newline="")
    w = csv.writer(fh)
    w.writerow(["timestamp", "kind", "name", "cpu_pct", "rss_mb"])
    prev = snapshot()
    prev_t = time.time()
    n = 0
    while not stop:
        time.sleep(INTERVAL)
        now = time.time()
        cur = snapshot()
        dt = max(now - prev_t, 0.001)
        for pid, (ticks, rss_kb) in cur.items():
            old = prev.get(pid)
            cpu = 0.0
            if old is not None:
                cpu = (ticks - old[0]) / HZ / dt * 100.0
            w.writerow([time.strftime("%Y-%m-%dT%H:%M:%S"), "service", "ape-server", round(cpu, 2), round(rss_kb / 1024, 1)])
        if n % CONTAINER_EVERY == 0:
            for name, cpu, mem in container_rows():
                w.writerow([time.strftime("%Y-%m-%dT%H:%M:%S"), "container", name, cpu, mem])
        fh.flush()
        prev, prev_t = cur, now
        n += 1
    fh.close()


if __name__ == "__main__":
    main()
