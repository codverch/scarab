#!/usr/bin/env python3
"""Run the hpca2027 revision experiments for the LD2 bank-port change.

Each run reuses the exact PARAMS.out of a committed run (its "--" section,
which holds every parameter including the experiment flags), applies a few
overrides, and runs one simpoint in the allbench_traces Docker image with the
traces from /dev/shm/ifuse mounted at /simpoint_traces.

Usage:
  run_campaign.py --set validate|bankport|interleave8 --out DIR [-j N]
"""

import argparse
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SCARAB = Path("/users/deepmish/scarab")
TRACES = Path("/dev/shm/ifuse")
IMAGE = "allbench_traces:054bbde"

MAIN = "hpca2027-revision-main"
MAIN_DIR = "src/hpca2027-revision-main-results/datacenter"
REV_IFUSE = "hpca2027-revision-ifuse"
D1_DIR = "src/simulations/1-cycle-delayed-ifuse"

SIMPOINTS = {
    "bfs": ["103", "17", "94", "96"], "dfs": ["28", "8", "94", "96"],
    "pagerank": ["107", "91", "94", "97", "99"], "corebench": ["4", "8", "9"],
    "appworld": ["4", "5", "9"], "terminal_bench": ["12", "5"],
    "cachebench": ["0"], "clickhouse": ["17"], "duckdb": ["5", "6"],
    "leveldb": ["5", "9"], "memcached": ["149", "156", "162", "169", "216"],
}

# name -> (binary build dir, source branch, source run dir, overrides)
IFUSE_SRC = (REV_IFUSE, D1_DIR)
CONFIGS = {
    "ifuse-bankport": ("build-rev-ifuse", *IFUSE_SRC,
                       {"ifuse_ld2_wake_delay": "0"}),
    "1-cycle-delayed-ifuse-bankport": ("build-rev-ifuse", *IFUSE_SRC,
                                       {"ifuse_ld2_wake_delay": "1"}),
    "ifuse-bankport-8B": ("build-rev-ifuse", *IFUSE_SRC,
                          {"ifuse_ld2_wake_delay": "0",
                           "dcache_interleave_factor": "8"}),
    "1-cycle-delayed-ifuse-bankport-8B": ("build-rev-ifuse", *IFUSE_SRC,
                                          {"ifuse_ld2_wake_delay": "1",
                                           "dcache_interleave_factor": "8"}),
    "baseline-8B": ("build-rev-baseline", MAIN, f"{MAIN_DIR}/baseline",
                    {"dcache_interleave_factor": "8"}),
    "helios-8B": ("build-rev-helios", MAIN, f"{MAIN_DIR}/helios",
                  {"dcache_interleave_factor": "8"}),
    "rfp-8B": ("build-rev-rfp", MAIN, f"{MAIN_DIR}/rfp",
               {"dcache_interleave_factor": "8"}),
    # Validation: the committed configs unchanged, to check reproduction.
    "validate-baseline": ("build-rev-baseline", MAIN, f"{MAIN_DIR}/baseline", {}),
    "validate-helios": ("build-rev-helios", MAIN, f"{MAIN_DIR}/helios", {}),
    "validate-rfp": ("build-rev-rfp", MAIN, f"{MAIN_DIR}/rfp", {}),
}
SETS = {
    "validate": ["validate-baseline", "validate-helios", "validate-rfp"],
    "bankport": ["ifuse-bankport", "1-cycle-delayed-ifuse-bankport"],
    "interleave8": ["ifuse-bankport-8B", "1-cycle-delayed-ifuse-bankport-8B",
                    "baseline-8B", "helios-8B", "rfp-8B"],
}


def committed_params(branch, run_dir):
    text = subprocess.run(
        ["git", "-C", str(SCARAB), "show", f"{branch}:{run_dir}/PARAMS.out"],
        capture_output=True, text=True, check=True).stdout
    return text.split("--- Cut out everything below")[0].splitlines()


def params_in(branch, run_dir, overrides):
    """The '--' section of a committed PARAMS.out, with overrides applied.

    revision-main has no RFP run for CacheBench. It uses CacheBench's baseline
    params plus the RFP flags that every RFP app except bfs shares (taken
    from clickhouse/17)."""
    if run_dir == f"{MAIN_DIR}/rfp/cachebench/0":
        lines = committed_params(branch, f"{MAIN_DIR}/baseline/cachebench/0")
        lines += [l for l in committed_params(branch, f"{MAIN_DIR}/rfp/clickhouse/17")
                  if l.startswith("--rfp")]
    else:
        lines = committed_params(branch, run_dir)
    for flag, value in overrides.items():
        lines = [l for l in lines if not re.match(rf"--{flag}\s", l)]
        lines.append(f"--{flag} {value}")
    return "\n".join(lines) + "\n"


def run_one(job, out):
    cfg, app, sp, apps_filter = job
    build, branch, src, overrides = CONFIGS[cfg]
    d = out / cfg / app / sp
    if (d / "exit.code").exists() and (d / "exit.code").read_text().strip() == "0":
        return cfg, app, sp, "skip"
    d.mkdir(parents=True, exist_ok=True)
    (d / "PARAMS.in").write_text(params_in(branch, f"{src}/{app}/{sp}", overrides))
    rel = d.relative_to(out)
    cmd = (f"cd /runs/{rel} && /scarab/src/{build}/opt/scarab "
           f"--cbp_trace_r0=/simpoint_traces/{app}/traces_simp/trace/{sp}.zip "
           f"--power_intf_on 0 > sim.log 2>&1; echo $? > exit.code")
    uid = subprocess.run(["id", "-u"], capture_output=True, text=True).stdout.strip()
    gid = subprocess.run(["id", "-g"], capture_output=True, text=True).stdout.strip()
    subprocess.run(["docker", "run", "--rm", "--user", f"{uid}:{gid}",
                    "-e", "HOME=/tmp",
                    "-v", f"{SCARAB}:/scarab:ro",
                    "-v", f"{TRACES}:/simpoint_traces:ro",
                    "-v", f"{out}:/runs", IMAGE, "bash", "-c", cmd])
    code = (d / "exit.code").read_text().strip() if (d / "exit.code").exists() else "missing"
    return cfg, app, sp, code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", required=True, choices=SETS)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--apps", default="", help="comma list, e.g. bfs:94")
    ap.add_argument("-j", type=int, default=28)
    args = ap.parse_args()
    args.out = args.out.resolve()

    only = {tuple(x.split(":")) for x in args.apps.split(",") if x}
    jobs = [(c, a, s, None) for c in SETS[args.set] for a, sps in SIMPOINTS.items()
            for s in sps if not only or (a, s) in only]
    # Longest apps first so the tail is short.
    jobs.sort(key=lambda j: j[1] not in ("clickhouse", "memcached", "cachebench"))
    with ThreadPoolExecutor(args.j) as ex:
        def safe(j):
            try:
                return run_one(j, args.out)
            except Exception as e:  # keep the campaign going
                return j[0], j[1], j[2], f"error:{e}"
        for cfg, app, sp, code in ex.map(safe, jobs):
            print(f"{cfg} {app}/{sp} exit={code}", flush=True)


if __name__ == "__main__":
    main()
