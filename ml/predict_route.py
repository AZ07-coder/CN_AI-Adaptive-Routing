"""Called by the NS-3 simulation at every adaptive-routing decision.

Usage:
    python predict_route.py [--model PATH] queue_A queue_B util_A util_B current_path

Prints one JSON line, e.g. {"selected_path": "Path_A"}, and exits 0.
On any error prints {"error": "..."} and exits 1 so the simulator falls back
to its queue rule.
"""

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default=str(HERE / "route_selector.pkl"))
    parser.add_argument("queue_A", type=float)
    parser.add_argument("queue_B", type=float)
    parser.add_argument("util_A", type=float)
    parser.add_argument("util_B", type=float)
    parser.add_argument("current_path", choices=["Path_A", "Path_B", "None"])
    args = parser.parse_args()

    import joblib
    import pandas as pd
    from features import build_features

    bundle = joblib.load(args.model)
    raw = pd.DataFrame([{
        "queue_A": args.queue_A,
        "queue_B": args.queue_B,
        "util_A": args.util_A,
        "util_B": args.util_B,
        "current_path": args.current_path,
    }])
    X = build_features(raw)[bundle["features"]]
    prediction = str(bundle["model"].predict(X)[0])
    print(json.dumps({"selected_path": prediction}), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception as exc:  # report, let the simulator fall back
        print(json.dumps({"error": f"{type(exc).__name__}: {exc}"}), flush=True)
        sys.exit(1)
