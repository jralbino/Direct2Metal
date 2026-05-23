"""Bench A/B parser for FP32 vs INT8 UART logs.

Usage:
    python3 tools/bench_int8_ab.py fp32.log int8.log
    python3 tools/bench_int8_ab.py fp32.log int8.log --skip 2

Parses the V165+ `[P] cap=… l0=… bb=… neck=… head=… render=…` and
`[T] …ms` lines; reports per-stage mean/median and a side-by-side
delta. `--skip N` drops the first N frames (warm-up / first-frame
weight CRC).
"""

import argparse
import re
import statistics as st
from pathlib import Path


P_LINE = re.compile(
    r"\[P\]\s+cap=(\d+)\s+l0=(\d+)\s+bb=(\d+)\s+neck=(\d+)\s+head=(\d+)\s+render=(\d+)"
)
T_LINE = re.compile(r"\[T\]\s+(\d+)\s*ms")
DET_LINE = re.compile(r"\[DET\]\s+c=(\d+)\s+%=(\d+)")
DET_NONE = re.compile(r"\[DET\]\s+none")


def parse_log(path: Path, skip: int):
    p_rows = []   # [(cap, l0, bb, neck, head, render), ...]
    t_rows = []   # [total_ms, ...]
    dets_per_frame = []  # list of list of (cls, pct); aligned to T_rows
    cur_dets = []
    for line in path.read_text(errors="replace").splitlines():
        m = DET_LINE.search(line)
        if m:
            cur_dets.append((int(m.group(1)), int(m.group(2))))
            continue
        if DET_NONE.search(line):
            cur_dets = []  # explicit "none" — clears any stale
            continue
        m = P_LINE.search(line)
        if m:
            p_rows.append(tuple(int(g) for g in m.groups()))
            continue
        m = T_LINE.search(line)
        if m:
            t_rows.append(int(m.group(1)))
            dets_per_frame.append(cur_dets)
            cur_dets = []

    # Drop warm-up.
    p_rows = p_rows[skip:]
    t_rows = t_rows[skip:]
    dets_per_frame = dets_per_frame[skip:]
    return p_rows, t_rows, dets_per_frame


def summarize(p_rows, t_rows):
    if not t_rows:
        return None
    stages = ["cap", "l0", "bb", "neck", "head", "render"]
    cols = list(zip(*p_rows)) if p_rows else [[] for _ in stages]
    out = {}
    for name, vals in zip(stages, cols):
        if not vals:
            out[name] = None
            continue
        out[name] = {
            "mean": st.mean(vals),
            "median": st.median(vals),
            "min": min(vals),
            "max": max(vals),
        }
    out["total"] = {
        "mean": st.mean(t_rows),
        "median": st.median(t_rows),
        "min": min(t_rows),
        "max": max(t_rows),
    }
    out["n_frames"] = len(t_rows)
    out["fps_mean"] = 1000.0 / out["total"]["mean"]
    return out


def fmt_delta(a, b):
    """b vs a; positive means b is slower."""
    if a == 0:
        return "n/a"
    pct = (b - a) / a * 100.0
    sign = "+" if pct >= 0 else ""
    return f"{sign}{pct:.1f}%"


def render(a_path, a, b_path, b):
    print(f"\nA: {a_path}  ({a['n_frames']} frames)")
    print(f"B: {b_path}  ({b['n_frames']} frames)\n")
    print(f"{'stage':<8}  {'A_mean':>9}  {'A_med':>8}  {'B_mean':>9}  {'B_med':>8}  {'Δmean':>8}")
    print("-" * 60)
    for name in ["cap", "l0", "bb", "neck", "head", "render", "total"]:
        sa = a.get(name)
        sb = b.get(name)
        if sa is None or sb is None:
            continue
        delta = fmt_delta(sa["mean"], sb["mean"])
        print(
            f"{name:<8}  "
            f"{sa['mean']:>7.1f}ms  {sa['median']:>6}ms  "
            f"{sb['mean']:>7.1f}ms  {sb['median']:>6}ms  "
            f"{delta:>8}"
        )
    print()
    print(f"A fps = {a['fps_mean']:.2f}    B fps = {b['fps_mean']:.2f}    "
          f"Δfps = {fmt_delta(b['fps_mean'], a['fps_mean'])}")


def detection_summary(name, dets_per_frame):
    if not dets_per_frame:
        return
    n_with = sum(1 for f in dets_per_frame if f)
    n_total = len(dets_per_frame)
    cls_counts = {}
    confs = []
    for f in dets_per_frame:
        for cls, pct in f:
            cls_counts[cls] = cls_counts.get(cls, 0) + 1
            confs.append(pct)
    top = sorted(cls_counts.items(), key=lambda kv: -kv[1])[:5]
    print(f"  {name}: {n_with}/{n_total} frames with detections, "
          f"{sum(cls_counts.values())} total dets, "
          f"top classes: {top}")
    if confs:
        print(f"        conf: mean={st.mean(confs):.1f}%  "
              f"min={min(confs)}%  max={max(confs)}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fp32_log", type=Path)
    ap.add_argument("int8_log", type=Path)
    ap.add_argument("--skip", type=int, default=2,
                    help="Drop the first N frames as warm-up (default 2).")
    args = ap.parse_args()

    a_p, a_t, a_d = parse_log(args.fp32_log, args.skip)
    b_p, b_t, b_d = parse_log(args.int8_log, args.skip)
    a = summarize(a_p, a_t)
    b = summarize(b_p, b_t)
    if a is None or b is None:
        raise SystemExit("No [P]/[T] lines found in one of the logs.")
    render(args.fp32_log.name, a, args.int8_log.name, b)
    print("\nDetections (raw, post-NMS):")
    detection_summary("FP32", a_d)
    detection_summary("INT8", b_d)


if __name__ == "__main__":
    main()
