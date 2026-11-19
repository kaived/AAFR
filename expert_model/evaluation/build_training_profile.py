"""Build the training-distribution profile of the AAFR expert model (V0.7.4).

Reads the model project's training dataset (routing_expert_dataset.csv, the
file ml/train.py trained the pinned artifacts on) and writes
expert_model/models/training_profile.json:

  - per raw feature: min, max, mean, std and the 1st/5th/50th/95th/99th
    percentiles over the 3500 training rows;
  - per scenario category: row count, mean vector and a ridge-regularised
    inverse covariance of the standardised 35 raw features, plus the empirical
    distribution (p50/p95/p99/max) of the training rows' own Mahalanobis
    distance to their category. AAFR's compatibility check uses these
    empirical quantiles as its threshold (not a chi-square assumption: the
    features are skewed, integer-valued and partly constant);
  - label distributions (stage-1 target, eps1 algorithm target);
  - the sha256 of the dataset, so a different dataset is detected.

Read-only with respect to the model: it loads no model artifact and changes
nothing in the model project. Run with the model worker's interpreter:

  .venv\\Scripts\\python.exe -I expert_model/evaluation/build_training_profile.py \\
      --model-dir <adaptive-routing-release> --out expert_model/models/training_profile.json
"""

import argparse
import hashlib
import json
import os
import sys

import numpy as np
import pandas as pd

RIDGE = 1e-3  # added to the diagonal of each standardised covariance


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--manifest", default=os.path.join(os.path.dirname(__file__), "..", "models", "model_manifest.json"))
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with open(args.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    raw = [f["name"] for f in manifest["raw_features"]]
    data_path = os.path.join(args.model_dir, "data", "routing_expert_dataset.csv")
    df = pd.read_csv(data_path)
    missing = [c for c in raw if c not in df.columns]
    if missing:
        print("dataset lacks raw features: %s" % missing, file=sys.stderr)
        return 2

    X = df[raw].astype(float).to_numpy()
    mean = X.mean(axis=0)
    std = X.std(axis=0)
    scale = np.where(std > 0, std, 1.0)
    Z = (X - mean) / scale

    features = []
    for i, name in enumerate(raw):
        col = X[:, i]
        q = np.percentile(col, [1, 5, 50, 95, 99])
        features.append({
            "name": name,
            "min": float(col.min()), "max": float(col.max()),
            "mean": float(mean[i]), "std": float(std[i]),
            "p01": float(q[0]), "p05": float(q[1]), "p50": float(q[2]), "p95": float(q[3]), "p99": float(q[4]),
            "distinct_values": int(len(np.unique(col))),
        })

    categories = []
    for cat, idx in df.groupby("scenario_category").indices.items():
        Zc = Z[idx]
        mu = Zc.mean(axis=0)
        cov = np.cov(Zc, rowvar=False) + RIDGE * np.eye(len(raw))
        inv = np.linalg.inv(cov)
        d = Zc - mu
        dist = np.sqrt(np.einsum("ij,jk,ik->i", d, inv, d))
        categories.append({
            "category": cat,
            "rows": int(len(idx)),
            "mean_z": [float(v) for v in mu],
            "inv_cov_z": [[float(v) for v in row] for row in inv],
            "distance_p50": float(np.percentile(dist, 50)),
            "distance_p95": float(np.percentile(dist, 95)),
            "distance_p99": float(np.percentile(dist, 99)),
            "distance_max": float(dist.max()),
            "num_nodes_min": float(df.loc[df.index[idx], "num_nodes"].min()),
            "num_nodes_max": float(df.loc[df.index[idx], "num_nodes"].max()),
        })

    profile = {
        "schema": "aafr.training_profile.v1",
        "model_version": manifest["model_version"],
        "feature_contract_version": manifest["feature_contract_version"],
        "dataset": "data/routing_expert_dataset.csv",
        "dataset_sha256": sha256(data_path),
        "rows": int(len(df)),
        "standardisation": {"mean": [float(v) for v in mean], "scale": [float(v) for v in scale]},
        "ridge": RIDGE,
        "raw_features": features,
        "categories": categories,
        "labels": {
            "invoke_metaheuristic": {str(k): int(v) for k, v in df["invoke_metaheuristic"].value_counts().items()},
            "target_algo_eps1": {str(k): int(v) for k, v in df["target_algo_eps1"].value_counts().items()},
        },
        "notes": [
            "Rows come from the model project's discrete-event simulator (generate_dataset.py), 1.5 s of simulated traffic per row.",
            "Telemetry features aggregate every simulated packet of every flow in the whole network, not one path.",
            "Training rows carry real scenario_category one-hots; inference without a category sets all seven to 0, a combination absent from training.",
        ],
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(profile, f, indent=1)
    print("wrote %s (%d rows, %d categories)" % (args.out, len(df), len(categories)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
