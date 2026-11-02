#!/usr/bin/env python3
"""Run the upstream workload on a fresh local database and record container resources.

Requires the million-postgres container, million-bench network and million-bun:1.4.2
image described in README.md. Only feed_bench is dropped; feed_seed is never served.
"""

import argparse
import contextlib
import hashlib
import json
import pathlib
import subprocess
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent.parent
ROOT = HERE.parent.parent
BIN = HERE / "tools/bin"


def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def resources(container):
    lines = command(
        "docker",
        "exec",
        container,
        "sh",
        "-c",
        "cat /sys/fs/cgroup/cpu.stat; echo memory_peak $(cat /sys/fs/cgroup/memory.peak); echo memory_current $(cat /sys/fs/cgroup/memory.current)",
    )
    return {line.split()[0]: int(line.split()[1]) for line in lines.splitlines()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--pool", type=int, default=256)
    parser.add_argument("--gzip-level", type=int, default=1)
    parser.add_argument("--users", type=int, default=2000)
    parser.add_argument("--think-scale", type=float, default=0.05)
    parser.add_argument("--ramp", default="10s")
    parser.add_argument("--hold", default="30s")
    parser.add_argument("--app-cpus", default="4")
    parser.add_argument("--generator", choices=("host", "linux"), default="host")
    parser.add_argument("--generators", type=int, choices=range(1, 9), default=1)
    args = parser.parse_args()
    if args.generator == "host" and args.generators != 1:
        parser.error("Multiple generators require --generator linux")
    if not args.name.replace("-", "").replace("_", "").isalnum():
        parser.error("name must contain letters, numbers, hyphens or underscores")
    out = HERE / "results"
    out.mkdir(exist_ok=True)
    subprocess.run(
        ["docker", "rm", "-f", "million-app"],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    command(
        "docker",
        "exec",
        "million-postgres",
        "psql",
        "-U",
        "postgres",
        "-d",
        "postgres",
        "-v",
        "ON_ERROR_STOP=1",
        "-c",
        "DROP DATABASE IF EXISTS feed_bench WITH (FORCE)",
        "-c",
        "CREATE DATABASE feed_bench TEMPLATE feed_seed",
    )
    command(
        "docker",
        "run",
        "-d",
        "--name",
        "million-app",
        "--network",
        "million-bench",
        "--cpus",
        args.app_cpus,
        "--memory",
        "3g",
        "--ulimit",
        "nofile=1048576:1048576",
        "-v",
        f"{ROOT}:/challenge",
        "-w",
        "/challenge/submissions/bun-elysia-youssefsz",
        "-e",
        "DATABASE_URL=postgres://postgres:challenge-local@million-postgres:5432/feed_bench",
        "-e",
        "JWT_SECRET=million-users-challenge",
        "-e",
        "HOST=0.0.0.0",
        "-e",
        "PORT=3000",
        "-e",
        "EXTRA_PORT=3001",
        "-p",
        "127.0.0.1:18082:3000",
        "-p",
        "127.0.0.1:18083:3001",
        "million-bun:1.4.2",
        "bun",
        "dist/main.js",
        "--workers",
        str(args.workers),
        "--pool",
        str(args.pool),
        "--gzip-level",
        str(args.gzip_level),
    )
    # Both listeners must be ready before any measured request is sent.
    for port in (18082, 18083):
        for attempt in range(120):
            try:
                with urllib.request.urlopen(
                    f"http://127.0.0.1:{port}/health", timeout=1
                ) as response:
                    if response.status == 200:
                        break
            except OSError:
                time.sleep(0.25)
        else:
            raise RuntimeError(f"Server did not become ready on {port}")
    with (out / f"{args.name}.warmup.log").open("w") as log:
        subprocess.run(
            [
                str(BIN / "mload"),
                "-target",
                "127.0.0.1",
                "-ports",
                "18082,18083",
                "-users",
                "200",
                "-ramp",
                "2s",
                "-hold",
                "5s",
                "-gzip",
                "-think-scale",
                "0.05",
                "-tokens",
                str(ROOT / "seed/tokens.json"),
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    before = {name: resources(name) for name in ("million-app", "million-postgres")}
    started = time.monotonic()
    commands = []
    outputs = []
    synchronized_start = int((time.time() + 8) * 1000) if args.generators > 1 else 0
    offset = 0
    for index in range(args.generators):
        users = args.users // args.generators + (index < args.users % args.generators)
        name = args.name if args.generators == 1 else f"{args.name}-g{index}"
        result_path = out / f"{name}.json"
        outputs.append(result_path)
        common = [
            "-users",
            str(users),
            "-offset",
            str(offset),
            "-start",
            str(synchronized_start),
            "-ramp",
            args.ramp,
            "-hold",
            args.hold,
            "-gzip",
            "-think-scale",
            str(args.think_scale),
        ]
        offset += users
        if args.generator == "host":
            workload = [
                str(BIN / "mload"),
                "-target",
                "127.0.0.1",
                "-ports",
                "18082,18083",
                *common,
                "-tokens",
                str(ROOT / "seed/tokens.json"),
                "-out",
                str(result_path),
            ]
        else:
            workload = [
                "docker",
                "run",
                "--rm",
                "--name",
                f"million-load-{index}",
                "--network",
                "million-bench",
                "--cpus",
                "2",
                "--memory",
                "3500m",
                "--ulimit",
                "nofile=1048576:1048576",
                "--sysctl",
                "net.ipv4.ip_local_port_range=1024 65535",
                "-v",
                f"{ROOT}:/challenge",
                "-v",
                f"{BIN / 'mload-linux'}:/usr/local/bin/mload:ro",
                "million-bun:1.4.2",
                "mload",
                "-target",
                "million-app",
                "-ports",
                "3000,3001",
                *common,
                "-tokens",
                "/challenge/seed/tokens.json",
                "-out",
                f"/challenge/submissions/bun-elysia-youssefsz/results/{name}.json",
            ]
        commands.append(workload)
    with contextlib.ExitStack() as stack:
        processes = []
        for index, workload in enumerate(commands):
            name = args.name if args.generators == 1 else f"{args.name}-g{index}"
            log = stack.enter_context((out / f"{name}.log").open("w"))
            processes.append(
                subprocess.Popen(workload, stdout=log, stderr=subprocess.STDOUT)
            )
        for process in processes:
            if process.wait() != 0:
                raise RuntimeError("Load generator failed; inspect its log")
    if args.generators > 1:
        with (out / f"{args.name}.log").open("w") as log:
            subprocess.run(
                [
                    str(BIN / "mload"),
                    "-merge",
                    "-out",
                    str(out / f"{args.name}.json"),
                    *map(str, outputs),
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
            )
    elapsed = time.monotonic() - started
    after = {name: resources(name) for name in before}
    metadata = {
        "parameters": vars(args),
        "elapsed_s": elapsed,
        "workload_commands": commands,
        "bundle_sha256": hashlib.sha256(
            (HERE / "dist/main.js").read_bytes()
        ).hexdigest(),
        "resources": {
            name: {
                "cpu_cores_average": (
                    after[name]["usage_usec"] - before[name]["usage_usec"]
                )
                / elapsed
                / 1e6,
                "throttled_s": (
                    after[name]["throttled_usec"] - before[name]["throttled_usec"]
                )
                / 1e6,
                "memory_peak_bytes": after[name]["memory_peak"],
                "memory_end_bytes": after[name]["memory_current"],
            }
            for name in before
        },
    }
    (out / f"{args.name}.environment.json").write_text(
        json.dumps(metadata, indent=2) + "\n"
    )
    (out / f"{args.name}.server.log").write_text(
        command("docker", "logs", "million-app") + "\n"
    )
    result = json.loads((out / f"{args.name}.json").read_text())
    print(
        json.dumps(
            {
                "name": args.name,
                **{
                    key: result[key]
                    for key in (
                        "users",
                        "req_per_s",
                        "p95_ms",
                        "p99_ms",
                        "err_pct",
                        "pass",
                    )
                },
                "resources": metadata["resources"],
            }
        ),
        flush=True,
    )


if __name__ == "__main__":
    main()
