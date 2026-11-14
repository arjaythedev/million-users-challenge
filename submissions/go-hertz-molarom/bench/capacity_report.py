#!/usr/bin/env python3
"""Summarise a bench/capacity.sh run and extrapolate to the scoring box.

Usage: capacity_report.py <capN dir>
Env:   KERNEL_BYTES_PER_CONN (4096): Linux kernel memory per idle established socket (sock, tcp_sock,
       file, epoll item); not visible on macOS, so it is an assumption added to the projection.
"""
import csv
import os
import re
import statistics
import sys

BOX_RAM = 32 * 2**30
BOX_USABLE = BOX_RAM * 0.95          # OS, page cache, systemd
REQ_PER_USER = 42000 / 1_000_000     # think-scale 2.5: ~1 request per 24 s per user
TARGET_CPU = 0.85                    # the search aims runs at ~85% CPU
KERNEL_B = int(os.environ.get("KERNEL_BYTES_PER_CONN", "4096"))
TICK = re.compile(r"t=\s*(\d+)s users=(\d+) conns=(\d+) req/s=([\d.]+) fails=(\d+) "
                  r"p95=([\d.]+)ms p99=([\d.]+)ms rss=(\d+)MB")
MB = 2**20


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def num(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def interp(rows, key, t):
    """Linear interpolation of a cumulative column at time t."""
    pts = [(num(r["t"]), num(r.get(key))) for r in rows if num(r.get(key)) is not None]
    if not pts:
        return None
    if t <= pts[0][0]:
        return pts[0][1]
    for (ta, va), (tb, vb) in zip(pts, pts[1:]):
        if ta <= t <= tb:
            return va if tb == ta else va + (vb - va) * (t - ta) / (tb - ta)
    return pts[-1][1]


def nearest(rows, key, t):
    pts = [r for r in rows if num(r.get(key)) is not None]
    if not pts:
        return None
    return num(min(pts, key=lambda r: abs(num(r["t"]) - t))[key])


def fit(xs, ys):
    """Least squares y = a + b x; returns (a, b)."""
    if len(xs) < 2 or len(set(xs)) < 2:
        return None
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return my - b * mx, b


def load_ticks(phase):
    out = []
    for m in read_csv(os.path.join(phase, "mloads.csv")):
        start = int(m["start_ms"]) / 1000
        ticks = []
        log = os.path.join(phase, m["id"] + ".log")
        if os.path.exists(log):
            for line in open(log):
                g = TICK.search(line)
                if g:
                    end = start + int(g[1])
                    ticks.append(dict(end=end, conns=int(g[3]), rps=float(g[4]), fails=int(g[5]),
                                      p95=float(g[6]), p99=float(g[7]), rss=int(g[8])))
        out.append((m["id"], ticks))
    return out


def window_stats(phase, w, samples, pg, mloads, ncpu):
    t0, t1 = num(w["t0"]), num(w["t1"])
    dur = t1 - t0
    in_win = [r for r in samples if t0 <= num(r["t"]) <= t1]
    if not in_win:
        return None  # sampler got no answer during the window (server or machine stalled)
    rps = fails = 0.0
    p95 = p99 = 0.0
    conns = 0
    for _, ticks in mloads:
        last = [k for k in ticks if k["end"] <= t1]
        if last:
            conns += last[-1]["conns"]
        inside = [k for k in ticks if k["end"] - 5 >= t0 and k["end"] <= t1]
        if inside:
            rps += statistics.fmean(k["rps"] for k in inside)
            fails += sum(k["fails"] for k in inside)
            p95 = max(p95, max(k["p95"] for k in inside))
            p99 = max(p99, max(k["p99"] for k in inside))

    def rate(key):
        a, b = interp(samples, key, t0), interp(samples, key, t1)
        if a is None or b is None or b < a:  # missing, or the counter reset (e.g. a service restart)
            return None
        return (b - a) / dur

    srv_cores = rate("srv_cpu_s")
    ml_cores = rate("mload_cpu_s") or 0.0
    pg_cores = rate("pg_cpu_s")  # Linux: the Postgres cgroup
    if pg_cores is None and pg:  # macOS: docker stats
        pg_vals = [num(r["pg_cpu_pct"]) for r in pg if t0 <= num(r["t"]) <= t1 and num(r["pg_cpu_pct"]) is not None]
        pg_cores = statistics.fmean(pg_vals) / 100 if pg_vals else 0.0
    pg_cores = pg_cores or 0.0
    box_cores = rate("box_srv_s")        # Linux: busy time of the server's cpus, kernel network work included
    sirq_cores = rate("box_srv_sirq_s")
    rss = statistics.median(num(r["srv_rss_kb"]) for r in in_win) * 1024
    total, released = nearest(samples, "total", t1), nearest(samples, "released", t1)
    slab, tcpm = nearest(samples, "slab_kb", t1), nearest(samples, "tcpmem_pages", t1)
    return dict(
        name=w["name"], conns=conns, rps=rps, fails=fails, p95=p95, p99=p99,
        srv_cores=srv_cores, core_ms=(srv_cores * 1000 / rps) if rps else None,
        box_cores=box_cores, sirq_cores=sirq_cores,
        box_ms=(box_cores * 1000 / rps) if rps and box_cores is not None else None,
        rss=rss, live=nearest(samples, "live", t1), stacks=nearest(samples, "stacks", t1),
        go_res=(total - released) if total is not None and released is not None else None,
        kern=(slab * 1024 + tcpm * 4096) if slab is not None and tcpm is not None else None,
        mload_rss=(nearest(samples, "mload_rss_kb", t1) or 0) * 1024,
        goroutines=nearest(samples, "goroutines", t1),
        ml_cores=ml_cores, pg_cores=pg_cores,
        busy=((box_cores if box_cores is not None else srv_cores) + ml_cores + pg_cores) / ncpu, dur=dur,
    )


def read_env(out):
    env = {}
    path = os.path.join(out, "env.txt")
    if os.path.exists(path):
        for line in open(path):
            for tok in line.split():
                k, _, v = tok.partition("=")
                env[k] = v
    return env


def cpu_count(spec):
    n = 0
    for part in filter(None, spec.split(",")):
        a, _, b = part.partition("-")
        n += int(b or a) - int(a) + 1
    return n


def users_for(cores, core_ms):
    return cores * TARGET_CPU * 1000 / core_ms / REQ_PER_USER


def fmt_users(n):
    return f"{n / 1e6:.2f}M" if n >= 1e6 else f"{n / 1e3:.0f}k"


def main():
    out = sys.argv[1]
    env = read_env(out)
    ncpu = int(env.get("ncpu") or os.cpu_count() or 1)
    linux = env.get("os") == "Linux"
    srv_n = cpu_count(env.get("server_cpus", "")) if linux else 0
    phases = sorted((d for d in os.listdir(out) if os.path.exists(os.path.join(out, d, "windows.csv"))),
                    key=lambda d: (d != "stairs", d.startswith("mixed"), d))
    windows = []
    for ph in phases:
        d = os.path.join(out, ph)
        samples, pg, mloads = read_csv(f"{d}/samples.csv"), read_csv(f"{d}/pg.csv"), load_ticks(d)
        for w in read_csv(f"{d}/windows.csv"):
            s = window_stats(d, w, samples, pg, mloads, ncpu)
            if s is None:
                print(f"  ! {ph}/{w['name']}: no samples in the window (stalled); skipped")
                continue
            s["phase"] = ph
            s["label"] = ph if ph.startswith("ladder") else w["name"] + (ph[ph.index("_"):] if "_" in ph else "")
            windows.append(s)
    if not windows:
        sys.exit(f"no measurement windows under {out}")

    where = f"Linux VM, server pinned to {srv_n} cpus ({env.get('server_cpus')})" if linux else "macOS"
    print(f"\n== measurement windows ({where}; {ncpu} cpus total) ==")
    print(f"{'window':<30}{'conns':>8}{'req/s':>8}{'p95ms':>7}{'p99ms':>7}{'fails':>6}{'srv':>6}"
          + (f"{'box':>6}{'sirq':>6}{'util':>6}" if linux else "")
          + f"{'cms/req':>8}{'RSS MB':>8}{'live':>6}{'stk':>6}{'KB/c':>6}{'mload':>6}{'pg':>5}{'busy':>6}")
    for s in windows:
        kb = s["rss"] / s["conns"] / 1024 if s["conns"] else 0
        ms = s["box_ms"] if s["box_ms"] is not None else s["core_ms"]
        cm = f"{ms:.3f}" if ms else "-"
        lx = (f"{s['box_cores']:>6.2f}{s['sirq_cores']:>6.2f}{s['box_cores'] / srv_n:>6.0%}" if linux else "")
        print(f"{s['label']:<30}{s['conns']:>8}{s['rps']:>8.0f}{s['p95']:>7.0f}{s['p99']:>7.0f}{s['fails']:>6.0f}"
              f"{s['srv_cores']:>6.2f}{lx}{cm:>8}{s['rss'] / MB:>8.0f}{(s['live'] or 0) / MB:>6.0f}"
              f"{(s['stacks'] or 0) / MB:>6.0f}{kb:>6.1f}{s['ml_cores']:>6.1f}{s['pg_cores']:>5.1f}{s['busy']:>6.0%}")
    print("  srv = server process cores" + ("; box = busy time of the server's cpus incl. kernel (what the search"
          " sees); sirq = softirq part; util = box / pinned cpus; cms/req = box core-ms per request" if linux else
          "; cms/req = server core-ms per request"))
    # The scoring pass criteria, applied per window (mload's worst 5 s tick, so slightly stricter than a run)
    for s in windows:
        why = []
        if s["rps"] and s["fails"] / (s["rps"] * (s["dur"] or 1)) >= 0.01:
            why.append(f"errors {s['fails'] / (s['rps'] * s['dur']):.1%}")
        if s["p95"] >= 500:
            why.append(f"p95 {s['p95']:.0f}ms")
        if s["p99"] >= 1000:
            why.append(f"p99 {s['p99']:.0f}ms")
        s["bad"] = bool(why)
        if why:
            who = []
            if linux and s["box_cores"] / srv_n > 0.9:
                who.append("server cpus saturated")
            if linux and s["pg_cores"] > 3.5:
                who.append("postgres cpus saturated")
            if linux and s["ml_cores"] > 3.5:
                who.append("mload cpus saturated")
            print(f"  ! FAIL {s['label']}: {', '.join(why)} (pass: p95<500ms, p99<1s, errors<1%)"
                  + (f" [{'; '.join(who)}]" if who else "") + "; left out of the fits")
    if not any(s["bad"] for s in windows):
        print("  all windows pass p95<500ms, p99<1s, errors<1%")

    # --- memory: slope over the staircase ---------------------------------------------------------
    stairs = [s for s in windows if s["phase"].startswith("stairs") and s["conns"] and not s["bad"]]
    print("\n== memory per connection (stairs, fit vs open conns) ==")
    fits = {}
    for key, label in (("rss", "RSS"), ("live", "live heap"), ("stacks", "goroutine stacks"),
                       ("go_res", "Go-mapped resident"), ("kern", "kernel (both ends)"), ("mload_rss", "mload RSS")):
        pts = [(s["conns"], s[key]) for s in stairs if s[key]]
        f = fit([p[0] for p in pts], [p[1] for p in pts])
        if f:
            fits[key] = f
            print(f"  {label:<20} {f[1]:>8.0f} B/conn   base {f[0] / MB:>6.0f} MB   at 1M: {(f[0] + f[1] * 1e6) / 2**30:>5.1f} GiB")
    g = fit([s["conns"] for s in stairs], [s["goroutines"] or 0 for s in stairs])
    if g:
        print(f"  goroutines           {g[1]:>8.2f} /conn")
    kernel_b, kernel_src = KERNEL_B, f"assumed {KERNEL_B} B/conn"
    if "kern" in fits and fits["kern"][1] > 0:
        # loopback: both the server's and mload's socket are in this kernel; the box only has the server's
        kernel_b, kernel_src = fits["kern"][1] / 2, f"measured {fits['kern'][1] / 2:.0f} B/conn (slab + TCP buffers, half of both ends)"
    mem_users = mem_users_opt = None
    if "rss" in fits:
        kern = kernel_b * 1e6
        rss1m = fits["rss"][0] + fits["rss"][1] * 1e6
        print(f"\n  1M users on 32 GiB: RSS {rss1m / 2**30:.1f} GiB + kernel ~{kern / 2**30:.1f} GiB "
              f"({kernel_src}) = {(rss1m + kern) / 2**30:.1f} GiB of {BOX_USABLE / 2**30:.1f} usable")
        mem_users = (BOX_USABLE - fits["rss"][0]) / (fits["rss"][1] + kernel_b)
        print(f"  memory ceiling, RSS as it behaves here:         {fmt_users(mem_users)} users")
        if "live" in fits and "stacks" in fits:
            # With GOMEMLIMIT (75% of RAM in start.sh) the GC holds the heap near live + a little headroom.
            per = (fits["live"][1] + fits["stacks"][1]) * 1.25 + kernel_b
            base = fits["live"][0] + fits["stacks"][0]
            mem_users_opt = (BOX_USABLE - base) / per
            print(f"  memory ceiling, live+stacks x1.25 (GOMEMLIMIT): {fmt_users(mem_users_opt)} users")

    # --- CPU: core-ms per request -> users at 85% ------------------------------------------------
    metric = "box core-ms/req (server cpus incl. kernel)" if linux else "server core-ms/req"
    print(f"\n== CPU per request -> users at 85% CPU (scoring pace ~0.042 req/s/user; {metric}) ==")
    print(f"  {'window':<30}{'cms/req':>9}{'4 cores':>10}{'8 cores':>10}{'16 cores':>10}")
    for s in windows:
        s["ms"] = s["box_ms"] if s["box_ms"] is not None else s["core_ms"]
        if s["ms"]:
            u = [users_for(c, s["ms"]) for c in (4, 8, 16)]
            flag = "  (failed)" if s["bad"] else ""
            print(f"  {s['label']:<30}{s['ms']:>9.3f}" + "".join(f"{fmt_users(x):>10}" for x in u) + flag)
    print("  needed for 1M users: 16 cores <= 0.324 core-ms/req, 8 cores <= 0.162, 4 cores <= 0.081")

    # --- verdict ---------------------------------------------------------------------------------
    heavy = [s for s in windows if s["rps"] >= 10000 and s["ms"] and not s["bad"]]
    if not heavy:
        print("\n  ! no passing high-rate window (hot/ladder/mixed): the cpu limit below is not measured")
    if not heavy and not mem_users:
        return
    print("\n" + "=" * 100)
    print("  ESTIMATE")
    print("=" * 100)
    rep = None
    if heavy:
        # representative CPU cost: the passing high-rate window with the most open connections (mixed if it passed)
        rep = max(heavy, key=lambda s: (s["conns"], s["rps"]))
        worst = max(heavy, key=lambda s: s["ms"])
        print(f"  CPU cost used: {rep['ms']:.3f} core-ms/req from '{rep['label']}' ({rep['conns']} conns, {rep['rps']:.0f} req/s);"
              f" worst passing high-rate window {worst['ms']:.3f} ('{worst['label']}')")
    ladder = sorted((s for s in windows if s["phase"].startswith("ladder")), key=lambda s: s["rps"])
    if ladder and linux:
        ok = [s for s in ladder if not s["bad"]]
        top = ok[-1] if ok else None
        first_fail = next((s for s in ladder if s["bad"]), None)
        if top:
            print(f"  Direct: the {srv_n}-cpu server passed {top['rps']:.0f} req/s (~{fmt_users(top['rps'] / REQ_PER_USER)} users"
                  f" of request load) at {top['box_cores'] / srv_n:.0%} cpu utilization")
        if first_fail:
            print(f"          and failed at {first_fail['rps']:.0f} req/s ({first_fail['box_cores'] / srv_n:.0%} utilization)")
    # Connection scale actually demonstrated at the scoring pace (stairs + mixed base windows), per server cpu.
    # This catches costs that grow with open connections (GC over per-connection heap and stacks), which the
    # few-connection hot/ladder windows can't see.
    pace = [s for s in windows if s["phase"].startswith("stairs") or s["label"].startswith("base_") and "+hot" not in s["label"]]
    p_ok = max((s["conns"] for s in pace if not s["bad"]), default=None)
    p_fail = min((s["conns"] for s in pace if s["bad"]), default=None)
    per_cpu = srv_n if linux and srv_n else None
    if per_cpu and (p_ok or p_fail):
        print(f"  Scale at the scoring pace on {per_cpu} cpus: passed up to {p_ok or 0:,} conns"
              + (f", failed from {p_fail:,}" if p_fail else "") + " (users per cpu: "
              + f"{(p_ok or 0) / per_cpu:,.0f} ok" + (f", {p_fail / per_cpu:,.0f} failed)" if p_fail else ")"))
    if mem_users:
        print(f"  Memory ceiling (any core count, 32 GiB): {fmt_users(mem_users)} users"
              + (f" (optimistic {fmt_users(mem_users_opt)})" if mem_users_opt else ""))
    print()
    print(f"  {'box':<10}{'cpu limit':>12}{'memory limit':>14}{'scale limit':>13}{'estimate':>12}   verdict")
    for cores, target, cap in ((16, 1_000_000, 1_000_000), (8, None, 3_000_000), (4, None, 3_000_000)):
        cpu_u = users_for(cores, rep["ms"]) if rep else None
        # demonstrated users-per-cpu scaled to this box (linear; the measured box is the 4-cpu configuration)
        scale_ok = cores * p_ok / per_cpu if per_cpu and p_ok else None
        scale_fail = cores * p_fail / per_cpu if per_cpu and p_fail else None
        lims = {k: v for k, v in (("cpu", cpu_u), ("memory", mem_users), ("connection-scale", scale_fail)) if v}
        if not lims:
            continue
        binding = min(lims, key=lims.get)
        est = min(min(lims.values()), cap)
        untested = False
        if not (p_ok or p_fail):
            # no scoring-pace windows at all: request cost alone says nothing about holding the connections
            verdict = ">>> UNPROVEN: no staircase/mixed run at the scoring pace"
            print(f"  {str(cores) + ' cores':<10}{fmt_users(cpu_u) if cpu_u else '-':>12}"
                  f"{fmt_users(mem_users) if mem_users else '-':>14}{'-':>13}{'-':>12}   {verdict}")
            continue
        if scale_fail:  # between what passed and what failed
            est = min(est, scale_ok or est)
            rng = f"~{fmt_users(scale_ok or 0)}-{fmt_users(min(scale_fail, cap))}"
        elif scale_ok and scale_ok < est:
            # nothing failed, but nothing past scale_ok was tried either: only that much is demonstrated
            untested = True
            rng = f">={fmt_users(scale_ok)} (untested up to {fmt_users(est)})"
            est = scale_ok
        else:
            rng = f"~{fmt_users(est)}"
        if target:
            if est >= target:
                verdict = ">>> PASS: holds 1,000,000 (max)"
            elif untested:
                verdict = f">>> UNPROVEN at 1M: demonstrated {rng}; run the staircase further"
            elif scale_fail and scale_fail >= target:
                verdict = f">>> BORDERLINE at 1M: {rng} ({binding}-bound)"
            else:
                verdict = f">>> FAIL at 1M: {rng} ({binding}-bound)"
        else:
            verdict = (f">>> score {rng}" if untested else f">>> score {rng} ({binding}-bound)") \
                + (" (max)" if est >= cap else "")
        print(f"  {str(cores) + ' cores':<10}{fmt_users(cpu_u) if cpu_u else '-':>12}"
              f"{fmt_users(mem_users) if mem_users else '-':>14}{fmt_users(scale_fail) if scale_fail else '-':>13}"
              f"{fmt_users(est):>12}   {verdict}")
    print("=" * 100)
    print("  Caveats: one VM stands in for two boxes (loopback, not a NIC; arm64 Apple cores, not x86 vCPUs);"
          if linux else "  Caveats: macOS process CPU only (no kernel network time); Apple cores, not x86 vCPUs;")
    print("  cpu limit = few-connection request cost only; scale limit = connections per cpu that failed at the scoring")
    print("  pace, scaled linearly to the box; memory from the staircase slope. Treat as an estimate.")


if __name__ == "__main__":
    main()
