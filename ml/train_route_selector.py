"""Train the route selector from rule-labelled simulation runs.

Usage:
    python ml/train_route_selector.py --runs results/pipeline/train --out ml/route_selector.pkl

Each run directory must contain adaptive_route_log.csv produced by
adaptive_topology with routingMode=adaptive decisionMode=rule.

Train/test are split BY RUN (no decisions from the same simulation in both),
and several models are compared against a majority-class baseline.
"""

import argparse
import json
import sys
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
import sklearn
from sklearn.dummy import DummyClassifier
from sklearn.ensemble import RandomForestClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import (accuracy_score, balanced_accuracy_score,
                             confusion_matrix, f1_score)
from sklearn.model_selection import GroupKFold, GroupShuffleSplit
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.tree import DecisionTreeClassifier

sys.path.insert(0, str(Path(__file__).resolve().parent))
from features import CLASSES, FEATURES, TARGET, build_features  # noqa: E402


def load_runs(runs_dir: Path) -> pd.DataFrame:
    frames = []
    for log in sorted(runs_dir.glob("*/adaptive_route_log.csv")):
        df = pd.read_csv(log)
        if df.empty:
            continue
        df["run_id"] = log.parent.name
        df["traffic_level"] = log.parent.name.split("_")[0]
        frames.append(df)
    if not frames:
        raise SystemExit(f"No adaptive_route_log.csv files under {runs_dir}")
    return pd.concat(frames, ignore_index=True)


def candidate_models():
    return {
        "Majority baseline": DummyClassifier(strategy="most_frequent"),
        "Logistic regression": make_pipeline(
            StandardScaler(), LogisticRegression(max_iter=1000, class_weight="balanced")),
        "Decision tree": DecisionTreeClassifier(
            max_depth=5, class_weight="balanced", random_state=42),
        "Random forest": RandomForestClassifier(
            n_estimators=200, max_depth=8, min_samples_leaf=2,
            class_weight="balanced", random_state=42, n_jobs=-1),
    }


def scores(y_true, y_pred):
    return {
        "accuracy": accuracy_score(y_true, y_pred),
        "balanced_accuracy": balanced_accuracy_score(y_true, y_pred),
        "f1_macro": f1_score(y_true, y_pred, average="macro", zero_division=0),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", default="results/pipeline/train")
    ap.add_argument("--out", default="ml/route_selector.pkl")
    ap.add_argument("--report-dir", default="results/pipeline/training")
    args = ap.parse_args()

    runs_dir, report_dir = Path(args.runs), Path(args.report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)

    raw = load_runs(runs_dir)
    raw = raw[raw[TARGET].isin(CLASSES)].copy()
    X = build_features(raw)
    keep = X.notna().all(axis=1)
    X, raw = X[keep], raw[keep]
    y, groups = raw[TARGET].to_numpy(), raw["run_id"].to_numpy()
    raw.to_csv(report_dir / "routing_training_data.csv", index=False)

    print(f"Decisions: {len(raw)}  runs: {raw['run_id'].nunique()}")
    print("Label distribution:", raw[TARGET].value_counts().to_dict())
    if len(set(y)) < 2:
        raise SystemExit("Need both Path_A and Path_B labels to train.")

    # Hold-out split by run.
    gss = GroupShuffleSplit(n_splits=1, test_size=0.25, random_state=42)
    tr, te = next(gss.split(X, y, groups))

    # Cross-validation by run on the training part (for model selection).
    n_folds = min(5, len(set(groups[tr])))
    gkf = GroupKFold(n_splits=n_folds)

    rows, fitted = [], {}
    for name, model in candidate_models().items():
        cv = []
        for a, b in gkf.split(X.iloc[tr], y[tr], groups[tr]):
            m = sklearn.base.clone(model).fit(X.iloc[tr].iloc[a], y[tr][a])
            cv.append(scores(y[tr][b], m.predict(X.iloc[tr].iloc[b]))["f1_macro"])
        model.fit(X.iloc[tr], y[tr])
        fitted[name] = model
        test = scores(y[te], model.predict(X.iloc[te]))
        rows.append({"model": name, "cv_f1_macro_mean": np.mean(cv),
                     "cv_f1_macro_std": np.std(cv),
                     **{f"test_{k}": v for k, v in test.items()}})

    table = pd.DataFrame(rows)
    table.to_csv(report_dir / "model_comparison.csv", index=False)
    print("\n" + table.round(4).to_string(index=False))

    # Select on CV score (not the test set), excluding the dummy.
    best = (table[table["model"] != "Majority baseline"]
            .sort_values("cv_f1_macro_mean", ascending=False).iloc[0]["model"])
    best_test = scores(y[te], fitted[best].predict(X.iloc[te]))
    cm = confusion_matrix(y[te], fitted[best].predict(X.iloc[te]), labels=CLASSES)
    pd.DataFrame(cm, index=[f"true_{c}" for c in CLASSES],
                 columns=[f"pred_{c}" for c in CLASSES]).to_csv(
        report_dir / "confusion_matrix.csv")

    # Refit the chosen model on all data for deployment.
    final = sklearn.base.clone(candidate_models()[best]).fit(X, y)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    joblib.dump({"model": final, "model_name": best, "features": FEATURES,
                 "target": TARGET, "sklearn_version": sklearn.__version__}, out)

    summary = {
        "decisions": int(len(raw)), "runs": int(raw["run_id"].nunique()),
        "train_runs": int(len(set(groups[tr]))), "test_runs": int(len(set(groups[te]))),
        "label_distribution": raw[TARGET].value_counts().to_dict(),
        "selected_model": best, "holdout_metrics_of_selected": best_test,
        "model_file": str(out), "features": FEATURES,
        "note": ("Labels come from the hysteresis queue rule, so these scores measure "
                 "how well the model imitates that rule, not routing optimality. "
                 "Routing quality is measured separately in the evaluation stage."),
    }
    (report_dir / "training_summary.json").write_text(json.dumps(summary, indent=2))
    print(f"\nSelected: {best}  hold-out: { {k: round(v, 4) for k, v in best_test.items()} }")
    print(f"Saved model -> {out}")


if __name__ == "__main__":
    main()
