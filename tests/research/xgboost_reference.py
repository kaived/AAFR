"""Verification and reference values for the imported XGBoost v3 link-weight model.

Uses the Python XGBoost package (the reference implementation) on the
artifacts in research/models/xgboost_v3/, which are never modified.

  python -I tests/research/xgboost_reference.py                  # write the C++ parity header
  python -I tests/research/xgboost_reference.py --check          # exit 1 if the header is stale
  python -I tests/research/xgboost_reference.py --dataset CSV    # also evaluate on the original split

Without --dataset nothing here depends on the training data: the parity
inputs are a fixed grid plus seeded random points. With --dataset (the
original network_dataset_raw.csv or routing_expert_dataset.csv) the script
rebuilds the original split exactly as the training script did and reports
the imported model's metrics on it. No model is trained.
"""

import argparse
import hashlib
import json
import math
import pathlib
import sys

import numpy as np
import xgboost as xgb

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODEL_DIR = ROOT / "research" / "models" / "xgboost_v3"
OUTPUT = ROOT / "tests" / "fixtures" / "research" / "xgboost_reference_values.h"
EPS = 1e-6
FEATURES = ["delay_ms", "loss_ratio", "inv_bottleneck_bw"]

# One-way delays (ms) and capacities (Mb/s) of the six-switch testbed links.
TESTBED = [("s1", "s2", 10.0, 2.0), ("s2", "s6", 10.0, 2.0), ("s1", "s3", 100.0, 5.0), ("s3", "s6", 100.0, 5.0),
           ("s1", "s4", 100.0, 7.0), ("s4", "s5", 100.0, 7.0), ("s5", "s6", 100.0, 7.0), ("s2", "s3", 50.0, 3.0),
           ("s3", "s5", 50.0, 4.0)]


def load_artifacts():
    prov = json.loads((MODEL_DIR / "provenance.json").read_text(encoding="utf-8"))
    problems = []
    for a in prov["artifacts"]:
        data = (MODEL_DIR / a["file"]).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != a["sha256"] or len(data) != a["bytes"]:
            problems.append(f"{a['file']}: sha256 {digest} / {len(data)} bytes, manifest {a['sha256']} / {a['bytes']}")
    scaler = json.loads((MODEL_DIR / "scaler_regressor.json").read_text(encoding="utf-8"))
    metrics = json.loads((MODEL_DIR / "regressor_metrics.json").read_text(encoding="utf-8"))
    features = (MODEL_DIR / "features_regressor.txt").read_text(encoding="utf-8").split()
    norm = json.loads((MODEL_DIR / "normalization.json").read_text(encoding="utf-8"))
    booster = xgb.Booster()
    booster.load_model(str(MODEL_DIR / "regressor.json"))
    if features != FEATURES or scaler["features"] != FEATURES or metrics["inputs"] != FEATURES:
        problems.append(f"feature order differs: txt {features}, scaler {scaler['features']}, metrics {metrics['inputs']}")
    if booster.num_features() != 3 or booster.num_boosted_rounds() != 400:
        problems.append(f"model has {booster.num_features()} features, {booster.num_boosted_rounds()} rounds")
    if len(scaler["mean"]) != 3 or len(scaler["scale"]) != 3 or min(scaler["scale"]) <= 0:
        problems.append("scaler is not 3 positive scales")
    for key in ("delay_ms", "latency_ms", "loss", "inv_bw", "hops", "bandwidth_mbps", "throughput_mbps"):
        if key not in norm or not (norm[key]["std"] > 0):
            problems.append(f"normalization.json lacks a positive std for {key}")
    return prov, scaler, metrics, norm, booster, problems


def raw_predict(booster, scaler, rows):
    x = np.asarray(rows, dtype=np.float64)
    z = ((x - np.asarray(scaler["mean"])) / np.asarray(scaler["scale"])).astype(np.float32)
    return booster.predict(xgb.DMatrix(z)), z


def training_features(latency, loss, throughput):
    return np.column_stack([latency, loss, 1.0 / np.maximum(throughput, EPS)])


def evaluate(dataset, scaler, metrics, norm, booster):
    import pandas as pd
    from sklearn.metrics import mean_absolute_error, r2_score
    from sklearn.model_selection import train_test_split
    from sklearn.preprocessing import StandardScaler

    df = pd.read_csv(dataset)
    X = training_features(df["latency"].to_numpy(float), df["loss"].to_numpy(float), df["throughput"].to_numpy(float))
    y = 100.0 - df["score"].to_numpy(float)
    X_tr, X_te, y_tr, y_te = train_test_split(X, y, test_size=0.2, random_state=42)
    refit = StandardScaler().fit(X_tr)
    mean_diff = float(np.max(np.abs(refit.mean_ - np.asarray(scaler["mean"]))))
    scale_diff = float(np.max(np.abs(refit.scale_ - np.asarray(scaler["scale"]))))
    reconstructed = mean_diff == 0.0 and scale_diff == 0.0
    print(f"[split] rows {len(y)}: train {len(y_tr)}, test {len(y_te)}; scaler refit on train vs shipped: "
          f"max |mean diff| {mean_diff:.3e}, max |scale diff| {scale_diff:.3e} -> "
          f"{'EXACT reconstruction' if reconstructed else 'NOT reconstructed'}")
    pred, _ = raw_predict(booster, scaler, X_te)
    r2, mae = float(r2_score(y_te, pred)), float(mean_absolute_error(y_te, pred))
    print(f"[test] imported model on rebuilt test split: R2 {r2:.10f}  MAE {mae:.10f}")
    print(f"[test] recorded in regressor_metrics.json:   R2 {metrics['r2']:.10f}  MAE {metrics['mae']:.10f}")
    print(f"[test] |difference|: R2 {abs(r2 - metrics['r2']):.3e}  MAE {abs(mae - metrics['mae']):.3e}")
    floored = np.maximum(pred, 0.0)
    print(f"[test] with floor at 0: R2 {float(r2_score(y_te, floored)):.6f}  MAE {float(mean_absolute_error(y_te, floored)):.6f}")
    pred_tr, _ = raw_predict(booster, scaler, X_tr)
    print(f"[train] R2 {float(r2_score(y_tr, pred_tr)):.6f}  MAE {float(mean_absolute_error(y_tr, pred_tr)):.6f}")
    for name, p in (("test", pred), ("train", pred_tr)):
        q = np.percentile(p, [0, 1, 10, 50, 90, 99, 100])
        print(f"[dist {name}] raw min/p1/p10/p50/p90/p99/max {np.round(q, 3).tolist()}; negative {int((p < 0).sum())}; "
              f"in [0, 0.5) {int(((p >= 0) & (p < 0.5)).sum())}; non-finite {int((~np.isfinite(p)).sum())}")
    low = X_te[:, 0] < 10.0
    print(f"[low-delay test rows, latency < 10 ms] n {int(low.sum())}; target 0: {int((y_te[low] == 0).sum())}; "
          f"raw pred mean {float(pred[low].mean()):.4f}, max {float(pred[low].max()):.4f}, negative {int((pred[low] < 0).sum())}")
    # Path-objective statistics: recomputed over all rows (ddof 0) as the training script did.
    def zs(a):
        a = np.asarray(a, float)
        return float(a.mean()), float(a.std())
    recompute = {
        "delay_ms": zs(df["latency"]), "latency_ms": zs(df["latency"]), "loss": zs(df["loss"]),
        "inv_bw": zs(1.0 / np.maximum(df["throughput"], EPS)), "hops": zs(df["avg_hop_count"]),
        "bandwidth_mbps": zs(df["offered_load_mbps"]), "throughput_mbps": zs(df["throughput"]),
    }
    worst = max(max(abs(m - norm[k]["mean"]), abs(s - norm[k]["std"])) for k, (m, s) in recompute.items())
    print(f"[normalization.json] recomputed over all {len(df)} rows, ddof 0: max |difference| {worst:.3e}")
    print(f"[normalization.json] delay mean over all rows {norm['delay_ms']['mean']:.6f} vs train-only "
          f"{float(X_tr[:, 0].mean()):.6f} (the file includes the test partition)")
    return reconstructed


def parity_inputs():
    rows, labels = [], []

    def add(label, delay, loss, thr):
        rows.append([delay, loss, 1.0 / max(thr, EPS)])
        labels.append(label)

    for d in (5.0, 20.0, 40.0, 80.0):
        for l in (0.0, 0.01, 0.1):
            add("normal", d, l, 40.0)
    for d in (0.1, 0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 9.9):
        add("low_delay", d, 0.0, 100.0)
        add("low_delay", d, 0.001, 10.0)
    for l in (0.3, 0.5, 0.7, 0.95, 1.0):
        add("high_loss", 30.0, l, 30.0)
    for thr in (0.1, 1.0, 3.0, 5.0):
        add("low_throughput", 30.0, 0.05, thr)
    add("throughput_floor", 30.0, 0.05, 0.0)  # 1 / max(0, 1e-6) = 1e6: far outside training.
    add("extreme", 500.0, 0.0, 40.0)
    add("extreme", 0.0, 0.0, 1e6)
    rng = np.random.default_rng(20261119)
    for _ in range(64):
        add("random_in_range", float(rng.uniform(2.7, 135.0)), float(rng.uniform(0.0, 0.72)), float(rng.uniform(3.9, 118.7)))
    for a, b, cap, delay in TESTBED:
        # Link-level runtime input: 1 / available bandwidth, no utilization, no loss.
        rows.append([delay, 0.0, 1.0 / cap])
        labels.append(f"testbed_{a}_{b}")
    return rows, labels


def lit(x):
    x = float(x)
    return repr(x)


def build_header(scaler, booster):
    rows, labels = parity_inputs()
    pred, z = raw_predict(booster, scaler, rows)
    lines = ["#pragma once", "",
             "// GENERATED by tests/research/xgboost_reference.py -- do not edit by hand.",
             f"// xgboost {xgb.__version__}, numpy {np.__version__}. Raw (unfloored) float32 predictions of",
             "// research/models/xgboost_v3/regressor.json for the inputs below.", "",
             "#include <vector>", "", "namespace aafr::xgboost_reference {", "",
             "struct Case {", "  const char* label;", "  double delay_ms;", "  double loss_ratio;",
             "  double inv_bottleneck_bw;", "  float z[3];  // Normalized float32 features as passed to XGBoost.",
             "  float raw;   // Python XGBoost prediction before the floor at 0.", "};", "",
             "inline const std::vector<Case> kCases = {"]
    for label, r, zz, p in zip(labels, rows, z, pred):
        zs = ", ".join(repr(float(v)) + "f" for v in zz)
        lines.append(f'    {{"{label}", {lit(r[0])}, {lit(r[1])}, {lit(r[2])}, {{{zs}}}, {repr(float(p))}f}},')
    lines += ["};", "", "}  // namespace aafr::xgboost_reference", ""]
    return "\n".join(lines), rows, labels, pred


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--dataset")
    args = ap.parse_args()
    prov, scaler, metrics, norm, booster, problems = load_artifacts()
    print(f"[artifacts] {len(prov['artifacts'])} files, xgboost {xgb.__version__}, "
          f"model features {booster.num_features()}, rounds {booster.num_boosted_rounds()}: "
          f"{'consistent' if not problems else 'PROBLEMS'}")
    for p in problems:
        print("  -", p)
    if problems:
        return 1
    text, rows, labels, pred = build_header(scaler, booster)
    if args.dataset:
        evaluate(args.dataset, scaler, metrics, norm, booster)
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print("xgboost_reference_values.h is stale; rerun tests/research/xgboost_reference.py")
            return 1
        print("xgboost_reference_values.h is up to date")
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    tb = [(l, r, p) for l, r, p in zip(labels, rows, pred) if l.startswith("testbed_")]
    for l, r, p in tb:
        print(f"[testbed] {l}: delay {r[0]} ms, inv_bw {r[2]:.4f} -> raw {float(p):.6f}, W_link {max(0.0, float(p)):.6f}")
    print(f"wrote {OUTPUT.relative_to(ROOT)} ({len(rows)} cases); raw negative {int((pred < 0).sum())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
