"""End-to-end pipeline for AI-based adaptive routing.

    python run_pipeline.py                 # everything
    python run_pipeline.py --stages eval   # only some stages
    python run_pipeline.py --quick         # small smoke test

Stages
  build    compile ns3/adaptive_topology.cc inside ns-3
  collect  run rule-based adaptive simulations on TRAINING seeds -> labelled decisions
  train    train/compare models, save ml/route_selector.pkl
  eval     run baseline / adaptive_rule / adaptive_ml on held-out TEST seeds
  report   aggregate metrics, write results/pipeline/evaluation/

Where it runs
  * If ns-3 is found (NS3_DIR env var, default /opt/ns3) it runs directly
    (Linux / WSL with ns-3 installed).
  * Otherwise it re-launches itself inside the Docker image built from
    docker/Dockerfile (ns-3.47 + Python + scikit-learn), mounting this project.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent
NS3_DIR = Path(os.environ.get("NS3_DIR", "/opt/ns3"))
IMAGE = "ai-routing"
LEVELS = ["low", "medium", "high"]
OUT = ROOT / "results" / "pipeline"
MODEL = ROOT / "ml" / "route_selector.pkl"
ALL_STAGES = ["build", "collect", "train", "eval", "report"]


def sh(cmd, **kw):
    print("$", " ".join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), check=True, **kw)


# ---------------------------------------------------------------- docker
def run_in_docker(argv):
    if shutil.which("docker") is None:
        sys.exit("ns-3 not found and Docker is not installed. Install Docker Desktop, "
                 "or set NS3_DIR to an ns-3 source tree.")
    have = subprocess.run(["docker", "image", "inspect", IMAGE],
                          capture_output=True).returncode == 0
    if not have:
        print("Building Docker image (ns-3 compile takes ~10-20 min the first time)...")
        sh(["docker", "build", "-t", IMAGE, "-f", ROOT / "docker" / "Dockerfile",
            ROOT / "docker"])
    sh(["docker", "run", "--rm", "-v", f"{ROOT}:/project", "-w", "/project",
        IMAGE, "python3", "run_pipeline.py", *argv])


# ---------------------------------------------------------------- stages
def find_binary():
    hits = sorted(glob.glob(str(NS3_DIR / "build" / "scratch" / "*adaptive_topology*")))
    hits = [h for h in hits if os.access(h, os.X_OK) and not h.endswith((".o", ".d"))]
    if not hits:
        sys.exit("adaptive_topology binary not found - run the build stage.")
    return hits[0]


def stage_build():
    shutil.copy(ROOT / "ns3" / "adaptive_topology.cc", NS3_DIR / "scratch" / "adaptive_topology.cc")
    sh(["./ns3", "build", "adaptive_topology"], cwd=NS3_DIR)


def simulate(binary, out_dir: Path, level, seed, routing, decision="rule"):
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [binary, f"--trafficLevel={level}", f"--seed={seed}",
           f"--routingMode={routing}", f"--decisionMode={decision}",
           f"--outputPrefix={out_dir}/", f"--pythonCmd={sys.executable}",
           f"--predictScript={ROOT / 'ml' / 'predict_route.py'}",
           f"--modelPath={MODEL}"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    (out_dir / "simulation.log").write_text(res.stdout + res.stderr)
    if res.returncode != 0 or not (out_dir / "network_data.csv").exists():
        raise RuntimeError(f"simulation failed ({out_dir}):\n{res.stderr[-2000:]}")
    return out_dir


def run_batch(jobs, workers):
    binary = find_binary()
    failures = 0
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(simulate, binary, *job) for job in jobs]
        for i, fut in enumerate(as_completed(futures), 1):
            try:
                d = fut.result()
                print(f"  [{i}/{len(jobs)}] done {d.relative_to(ROOT)}", flush=True)
            except Exception as exc:
                failures += 1
                print(f"  [{i}/{len(jobs)}] FAILED: {exc}", flush=True)
    if failures:
        sys.exit(f"{failures} simulation(s) failed - see simulation.log files.")


def clean(path: Path):
    if path.exists():
        shutil.rmtree(path)


def stage_collect(seeds, workers):
    clean(OUT / "train")
    jobs = [(OUT / "train" / f"{lvl}_seed{s}", lvl, s, "adaptive", "rule")
            for lvl in LEVELS for s in seeds]
    print(f"Collecting training data: {len(jobs)} rule-based adaptive runs")
    run_batch(jobs, workers)


def stage_train():
    if MODEL.exists():
        legacy = ROOT / "ml" / "legacy"
        legacy.mkdir(exist_ok=True)
        backup = legacy / "route_selector_previous.pkl"
        shutil.copy(MODEL, backup)
    sh([sys.executable, ROOT / "ml" / "train_route_selector.py",
        "--runs", OUT / "train", "--out", MODEL,
        "--report-dir", OUT / "training"])


def stage_eval(seeds, workers):
    if not MODEL.exists():
        sys.exit("No trained model - run the train stage first.")
    # Smoke-test the model exactly the way the simulator calls it.
    sh([sys.executable, ROOT / "ml" / "predict_route.py", "--model", MODEL,
        "40", "0", "95.0", "10.0", "Path_A"])
    clean(OUT / "eval")
    jobs = []
    for lvl in LEVELS:
        for s in seeds:
            jobs.append((OUT / "eval" / "baseline" / f"{lvl}_seed{s}", lvl, s, "baseline"))
            jobs.append((OUT / "eval" / "adaptive_rule" / f"{lvl}_seed{s}", lvl, s,
                         "adaptive", "rule"))
            jobs.append((OUT / "eval" / "adaptive_ml" / f"{lvl}_seed{s}", lvl, s,
                         "adaptive", "ml"))
    print(f"Evaluation: {len(jobs)} runs on held-out seeds {list(seeds)}")
    run_batch(jobs, workers)


def stage_report():
    sh([sys.executable, ROOT / "ml" / "evaluate.py", "--runs", OUT / "eval",
        "--out", OUT / "evaluation"])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stages", nargs="+", default=ALL_STAGES, choices=ALL_STAGES)
    ap.add_argument("--train-seeds", type=int, default=20, help="seeds per traffic level")
    ap.add_argument("--test-seeds", type=int, default=8, help="seeds per traffic level")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--quick", action="store_true", help="tiny run for a smoke test")
    args = ap.parse_args()

    if not (NS3_DIR / "ns3").exists():
        return run_in_docker(sys.argv[1:])

    if args.quick:
        args.train_seeds, args.test_seeds = 4, 2
    train_seeds = range(1, args.train_seeds + 1)
    test_seeds = range(1001, 1001 + args.test_seeds)  # never overlap with training

    OUT.mkdir(parents=True, exist_ok=True)
    stages = list(args.stages)
    needs_sim = {"collect", "eval"} & set(stages)
    have_bin = glob.glob(str(NS3_DIR / "build" / "scratch" / "*adaptive_topology*"))
    if needs_sim and "build" not in stages and not have_bin:
        stages.insert(0, "build")  # fresh container: compile first
    for stage in [s for s in ALL_STAGES if s in stages]:
        print(f"\n===== {stage.upper()} =====", flush=True)
        if stage == "build":
            stage_build()
        elif stage == "collect":
            stage_collect(train_seeds, args.jobs)
        elif stage == "train":
            stage_train()
        elif stage == "eval":
            stage_eval(test_seeds, args.jobs)
        elif stage == "report":
            stage_report()
    print("\nPipeline finished. Results in", OUT.relative_to(ROOT))


if __name__ == "__main__":
    main()
