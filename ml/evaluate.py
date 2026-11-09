"""Compare baseline routing, rule-based adaptive routing and ML adaptive
routing on the same held-out seeds.

Usage:
    python ml/evaluate.py --runs results/pipeline/eval --out results/pipeline/evaluation

Expects <runs>/<mode>/<traffic>_seed<k>/network_data.csv (+ adaptive_route_log.csv).
"""

import argparse
from pathlib import Path

import pandas as pd

MAIN_UDP = ("10.1.1.2", "10.1.10.2", 5000)
MAIN_TCP = ("10.1.2.2", "10.1.9.2", 5001)
CROSS_PORTS = (6000, 6001)
MODES = ["baseline", "adaptive_rule", "adaptive_ml"]
LEVELS = ["low", "medium", "high"]

METRICS = {
    "udp_throughput_mbps": "UDP H1->H10 throughput (Mbps, higher=better)",
    "udp_loss_pct": "UDP H1->H10 packet loss (%, lower=better)",
    "udp_delay_ms": "UDP H1->H10 mean delay (ms, lower=better)",
    "udp_jitter_ms": "UDP H1->H10 jitter (ms, lower=better)",
    "tcp_throughput_mbps": "TCP H2->H9 throughput (Mbps, higher=better)",
    "cross_loss_pct": "Cross-traffic loss (%, lower=better)",
    "network_delivery_pct": "All flows: packets delivered (%, higher=better)",
}


def flow(df, src, dst, port):
    m = df[(df.source == src) & (df.destination == dst) & (df.dst_port == port)]
    return m.iloc[0] if len(m) else None


def run_metrics(run_dir: Path) -> dict:
    nd = pd.read_csv(run_dir / "network_data.csv")
    udp, tcp = flow(nd, *MAIN_UDP), flow(nd, *MAIN_TCP)
    cross = nd[nd.dst_port.isin(CROSS_PORTS)]
    r = {
        "udp_throughput_mbps": udp.throughput if udp is not None else 0.0,
        "udp_loss_pct": 100 * udp.packets_lost / max(udp.packets_sent, 1) if udp is not None else 100.0,
        "udp_delay_ms": 1000 * udp.delay if udp is not None else float("nan"),
        "udp_jitter_ms": 1000 * udp.jitter if udp is not None else float("nan"),
        "tcp_throughput_mbps": tcp.throughput if tcp is not None else 0.0,
        "cross_loss_pct": 100 * cross.packets_lost.sum() / max(cross.packets_sent.sum(), 1),
        "network_delivery_pct": 100 * nd.packets_received.sum() / max(nd.packets_sent.sum(), 1),
    }
    log = run_dir / "adaptive_route_log.csv"
    if log.exists():
        rl = pd.read_csv(log)
        if len(rl):
            r["decisions"] = len(rl)
            r["route_switches"] = int((rl.selected_path != rl.current_path)[rl.current_path != "None"].sum())
            r["ml_decisions"] = int((rl.decision_source == "ml").sum())
            r["fallback_decisions"] = int((rl.decision_source == "fallback").sum())
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", default="results/pipeline/eval")
    ap.add_argument("--out", default="results/pipeline/evaluation")
    args = ap.parse_args()
    runs, out = Path(args.runs), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    rows = []
    for mode in MODES:
        for run_dir in sorted((runs / mode).glob("*_seed*")):
            if not (run_dir / "network_data.csv").exists():
                print("missing results:", run_dir)
                continue
            level, seed = run_dir.name.split("_seed")
            rows.append({"mode": mode, "traffic_level": level, "seed": int(seed),
                         **run_metrics(run_dir)})
    if not rows:
        raise SystemExit(f"No evaluation runs found under {runs}")

    per_run = pd.DataFrame(rows)
    per_run.to_csv(out / "per_run_metrics.csv", index=False)

    cols = list(METRICS) + [c for c in ["route_switches", "fallback_decisions"] if c in per_run]
    summary = (per_run.groupby(["traffic_level", "mode"])[cols]
               .agg(["mean", "std"]).round(3))
    summary.columns = [f"{a}_{b}" for a, b in summary.columns]
    summary = summary.reset_index()
    summary["traffic_level"] = pd.Categorical(summary.traffic_level, LEVELS, ordered=True)
    summary["mode"] = pd.Categorical(summary["mode"], MODES, ordered=True)
    summary = summary.sort_values(["traffic_level", "mode"])
    summary.to_csv(out / "summary_by_traffic_and_mode.csv", index=False)

    # Paired difference vs baseline on the same seed.
    base = per_run[per_run["mode"] == "baseline"].set_index(["traffic_level", "seed"])
    diffs = []
    for mode in MODES[1:]:
        m = per_run[per_run["mode"] == mode].set_index(["traffic_level", "seed"])
        common = m.index.intersection(base.index)
        d = (m.loc[common, list(METRICS)] - base.loc[common, list(METRICS)]).reset_index()
        d["mode"] = mode
        diffs.append(d)
    paired = pd.concat(diffs)
    paired_summary = paired.groupby(["traffic_level", "mode"])[list(METRICS)].mean().round(3)
    paired_summary.to_csv(out / "paired_difference_vs_baseline.csv")

    # Report.
    lines = ["# Evaluation: baseline vs adaptive routing", "",
             f"Runs per mode: {per_run.groupby('mode').size().to_dict()}", ""]
    for metric, label in METRICS.items():
        pivot = per_run.pivot_table(index="traffic_level", columns="mode",
                                    values=metric, aggfunc="mean")
        pivot = pivot.reindex(index=[l for l in LEVELS if l in pivot.index],
                              columns=[m for m in MODES if m in pivot.columns])
        lines += [f"## {label}", "", "```", pivot.round(3).to_string(), "```", ""]
    if "fallback_decisions" in per_run:
        ml = per_run[per_run["mode"] == "adaptive_ml"]
        lines += [f"ML decisions: {int(ml.ml_decisions.sum())}, "
                  f"fallbacks to rule: {int(ml.fallback_decisions.sum())}", ""]
    (out / "report.md").write_text("\n".join(lines), encoding="utf-8")
    print("\n".join(lines))

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        keys = ["udp_throughput_mbps", "udp_loss_pct", "udp_delay_ms", "tcp_throughput_mbps"]
        fig, axes = plt.subplots(1, len(keys), figsize=(4.2 * len(keys), 3.8))
        for ax, key in zip(axes, keys):
            pv = per_run.pivot_table(index="traffic_level", columns="mode", values=key,
                                     aggfunc="mean").reindex(index=LEVELS, columns=MODES)
            er = per_run.pivot_table(index="traffic_level", columns="mode", values=key,
                                     aggfunc="std").reindex(index=LEVELS, columns=MODES)
            pv.plot.bar(ax=ax, yerr=er, capsize=3, rot=0, legend=False)
            ax.set_title(METRICS[key].split(" (")[0] + "\n(" + METRICS[key].split(" (")[1],
                         fontsize=9)
            ax.set_xlabel("")
        axes[0].legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(out / "comparison.png", dpi=130)
        print(f"\nSaved plot -> {out / 'comparison.png'}")
    except Exception as exc:
        print("Plot skipped:", exc)


if __name__ == "__main__":
    main()
