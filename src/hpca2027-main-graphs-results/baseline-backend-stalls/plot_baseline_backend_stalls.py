#!/usr/bin/env python3
"""Baseline backend-bound slots by full structure (PRF, ROB, LQ, SQ, Other).

Reads the Baseline rows of a backend-bound-breakdown_summary.csv written by
plot_backend_bound_breakdown.py, so it works after the raw sims are gone. Each
stack is the top-down Backend Bound, as a percentage of the app's issue slots.

Example:

/users/deepmish/miniconda3/envs/scarabinfra/bin/python \
  /users/deepmish/scarab-infra/hpca2027-main-graphs/plot_baseline_backend_stalls.py \
  --summary /users/deepmish/scarab/src/hpca2027-main-graphs-results/backend-bound-breakdown/backend-bound-breakdown_summary.csv \
  --output-dir /users/deepmish/scarab/src/hpca2027-main-graphs-results/baseline-backend-stalls
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402

# plot_ipc lives with the other hpca2027-main graph scripts in scarab-infra.
GRAPH_DIR = Path("/users/deepmish/scarab-infra/hpca2027-main-graphs")
if str(GRAPH_DIR) not in sys.path:
    sys.path.insert(0, str(GRAPH_DIR))

import plot_ipc  # noqa: E402
from plot_ipc import (  # noqa: E402
    AVERAGE_SEPARATOR_WIDTH,
    BAR_EDGE_WIDTH,
    IPC_TICK_FONT,
    register_noto_serif,
)

P = "TOPDOWN_BE_FULL_"
# Colors of plot_backend_stall_resources.py (Helios, ColorBrewer Spectral).
RESOURCE = (
    ("PRF", "#5E4FA1", P + "PRF_SLOTS"),
    ("ROB", "#328795", P + "ROB_SLOTS"),
    ("LQ", "#8EE08A", P + "LQ_SLOTS"),
    ("SQ", "#E5F497", P + "SQ_SLOTS"),
    ("Other", "#C8C8C6", P + "OTHER_SLOTS"),
)

APP_STEP = 10.0
BAR_WIDTH = 5.0
AVERAGE_GAP = 2.4
FIGSIZE = (72.0, 22.0)
AXIS_FONT = IPC_TICK_FONT
LEGEND_FONT = 90
OUTPUT_STEM = "baseline-backend-stalls"


def load_rows(summary: Path):
    with summary.open() as fh:
        return [(r["workload"], [float(r[k]) for _, _, k in RESOURCE])
                for r in csv.DictReader(fh) if r["config"] == "Baseline"]


def plot(rows, output_dir: Path) -> None:
    x_apps = np.arange(len(rows) - 1, dtype=float) * APP_STEP
    x = np.append(x_apps, x_apps[-1] + APP_STEP / 2 + BAR_WIDTH / 2 + AVERAGE_GAP)
    separator_x = float((x_apps[-1] + x[-1]) / 2)
    stacks = np.array([v for _, v in rows])

    plt.rcParams.update({"font.family": plot_ipc.FONT_FAMILY})
    fig, ax = plt.subplots(figsize=FIGSIZE)
    ax.grid(True, axis="y", alpha=0.8, linestyle=":", color="black", linewidth=2.0, zorder=0)
    bottom = np.zeros(len(rows))
    for i, (_, color, _) in enumerate(RESOURCE):
        ax.bar(x, stacks[:, i], BAR_WIDTH, bottom=bottom, color=color,
               edgecolor="black", linewidth=BAR_EDGE_WIDTH, zorder=3)
        bottom += stacks[:, i]
    ax.set_ylim(0, bottom.max() * 1.08)
    ax.axvline(x=separator_x, color="black", linestyle="--",
               linewidth=3 * AVERAGE_SEPARATOR_WIDTH, zorder=2)
    ax.set_ylabel("Backend stalls (%)", fontsize=AXIS_FONT)
    ax.tick_params(axis="y", labelsize=AXIS_FONT, colors="black")
    for spine in ax.spines.values():
        spine.set_color("black")
        spine.set_linewidth(BAR_EDGE_WIDTH)

    ax.set_xticks(x)
    ax.set_xticklabels([name for name, _ in rows], rotation=45, ha="right", color="black")
    ax.tick_params(axis="x", labelsize=AXIS_FONT, length=0, pad=14)
    for tick in ax.get_xticklabels():
        if tick.get_text() == "Average":
            tick.set_weight("bold")
    ax.set_xlim(x[0] - BAR_WIDTH / 2 - 2.0, x[-1] + BAR_WIDTH / 2 + 2.0)

    handles = [Patch(facecolor=c, edgecolor="black", linewidth=BAR_EDGE_WIDTH, label=name)
               for name, c, _ in RESOURCE]
    legend = fig.legend(handles=handles, loc="lower center", bbox_to_anchor=(0.5, 0.86),
                        ncol=len(handles), fontsize=LEGEND_FONT, frameon=True, fancybox=False,
                        edgecolor="black", handlelength=1.6, columnspacing=1.4)
    legend.get_frame().set_linewidth(BAR_EDGE_WIDTH)
    plt.subplots_adjust(top=0.85)

    output_dir.mkdir(parents=True, exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(output_dir / f"{OUTPUT_STEM}.{ext}", dpi=300, bbox_inches="tight",
                    pad_inches=0.08)
    plt.close(fig)


def main() -> None:
    register_noto_serif()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    rows = load_rows(args.summary)
    if not rows:
        raise SystemExit(f"No Baseline rows in {args.summary}.")
    plot(rows, args.output_dir)
    avg = dict(rows)["Average"]
    print("Average", "  ".join(f"{n}={v:.2f}" for (n, _, _), v in zip(RESOURCE, avg)))
    print(f"Outputs in {args.output_dir}")


if __name__ == "__main__":
    main()
