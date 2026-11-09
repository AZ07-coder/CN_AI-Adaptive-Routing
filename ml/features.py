"""Feature definitions shared by training (train_route_selector.py) and
inference (predict_route.py), so the simulator and the model always agree."""

import pandas as pd

RAW_COLUMNS = ["queue_A", "queue_B", "util_A", "util_B", "current_path"]

FEATURES = [
    "queue_A",
    "queue_B",
    "util_A",
    "util_B",
    "queue_diff",
    "util_diff",
    "on_path_A",
    "on_path_B",
]

TARGET = "selected_path"
CLASSES = ["Path_A", "Path_B"]


def build_features(df: pd.DataFrame) -> pd.DataFrame:
    """Turn raw route-log columns into the model's feature matrix."""
    out = pd.DataFrame(index=df.index)
    out["queue_A"] = pd.to_numeric(df["queue_A"], errors="coerce")
    out["queue_B"] = pd.to_numeric(df["queue_B"], errors="coerce")
    out["util_A"] = pd.to_numeric(df["util_A"], errors="coerce")
    out["util_B"] = pd.to_numeric(df["util_B"], errors="coerce")
    out["queue_diff"] = out["queue_A"] - out["queue_B"]
    out["util_diff"] = out["util_A"] - out["util_B"]
    current = df["current_path"].astype(str)
    out["on_path_A"] = (current == "Path_A").astype(int)
    out["on_path_B"] = (current == "Path_B").astype(int)
    return out[FEATURES]
