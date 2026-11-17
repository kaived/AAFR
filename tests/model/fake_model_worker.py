"""Fault-injecting stand-in for the AAFR expert-model worker (tests only).

Speaks the same JSON Lines protocol as expert_model/inference/aafr_model_worker.py
but needs no third-party package. Behaviour comes from <model-dir>/fake_mode.json:

  {"startup": "ok" | "fatal" | "exit" | "hang" | "bad_contract" | "wrong_version" | "garbage",
   "responses": [ {"kind": ..., ...}, ... ]   # consumed in order; the last repeats
  }

Response kinds:
  ok           {"label": "PSO", "gate": 0.7}       a valid prediction
  hang         no answer at all
  late         {"delay_ms": 800, "label": ...}    answers after a delay
  crash        the process exits immediately
  malformed    writes a non-JSON line
  unknown_label{"label": "Teleport"}
  wrong_version a prediction claiming another model version
  bad_echo     a prediction echoing another evaluation context
  error        {"code": "E_INFERENCE_FAILED"}
  oversize     a single line larger than --max-line-bytes
  bad_probs    probabilities that do not sum to 1
  slow_model   {"inference_ms": 5000}  reports excessive model time
"""

import argparse
import json
import os
import sys
import time

proto = sys.stdout


def send(obj):
    proto.write(json.dumps(obj) + "\n")
    proto.flush()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--model-dir", required=True)
    p.add_argument("--manifest", required=True)
    p.add_argument("--max-line-bytes", type=int, default=65536)
    a = p.parse_args()
    with open(os.path.join(a.model_dir, "fake_mode.json"), encoding="utf-8") as f:
        mode = json.load(f)
    with open(a.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    version = manifest["model_version"]
    contract = manifest["feature_contract_version"]
    raw = [x["name"] for x in manifest["raw_features"]]
    threshold = manifest["stage1"]["threshold"]
    classes = manifest["stage2"]["classes"]

    send({"type": "hello", "protocol_version": 1, "worker_version": "fake", "python_version": "x", "pid": os.getpid()})
    startup = mode.get("startup", "ok")
    if startup == "fatal":
        send({"type": "fatal", "protocol_version": 1, "error_code": mode.get("fatal_code", "E_ARTIFACT_HASH_MISMATCH"),
              "message": "scripted fatal startup"})
        return 2
    if startup == "exit":
        sys.stderr.write("ModuleNotFoundError: No module named 'lightgbm'\n")
        sys.stderr.flush()
        return 1
    if startup == "hang":
        time.sleep(3600)
    if startup == "garbage":
        proto.write("this is not json\n")
        proto.flush()
    time.sleep(mode.get("startup_delay_ms", 0) / 1000.0)
    ready = {"type": "ready", "protocol_version": 1, "worker_version": "fake", "model_name": "fake",
             "model_version": version, "feature_contract_version": contract, "raw_features": raw,
             "model_feature_count": 55, "labels": ["Dijkstra"] + classes, "stage2_classes": classes,
             "gate_threshold": threshold, "library_versions": {}, "load_warnings": [], "verify_ms": 1.0,
             "load_ms": 2.0, "dependency_import_ms": 0.5, "pid": os.getpid()}
    if startup == "bad_contract":
        ready["raw_features"] = raw[:-1] + ["teleport_index"]
    if startup == "wrong_version":
        ready["model_version"] = "some-other-model"
    send(ready)

    responses = list(mode.get("responses", [{"kind": "ok"}]))
    while True:
        line = sys.stdin.readline()
        if not line:
            return 0
        msg = json.loads(line)
        if msg.get("type") == "shutdown":
            send({"type": "bye", "protocol_version": 1, "requests_served": 0})
            return 0
        if msg.get("type") == "ping":
            send({"type": "pong", "protocol_version": 1, "request_id": msg.get("request_id"), "requests_served": 0})
            continue
        spec = responses[0] if len(responses) == 1 else responses.pop(0)
        kind = spec.get("kind", "ok")
        rid = msg.get("request_id")
        label = spec.get("label", "Dijkstra")
        gate = spec.get("gate", 0.7 if label != "Dijkstra" else 0.2)
        stage2 = gate >= threshold
        probs = None
        if stage2:
            probs = {c: 0.1 for c in classes}
            if label in probs:
                probs[label] = 0.7
            else:
                probs[classes[0]] = 0.7
        resp = {"type": "prediction", "protocol_version": 1, "request_id": rid, "status": "ok",
                "model_version": version, "feature_contract_version": contract, "model_label": label,
                "label_source": "stage2_argmax" if stage2 else "stage1_below_threshold",
                "gate_probability": gate, "gate_threshold": threshold, "stage2_ran": stage2,
                "stage2_probabilities": probs, "probabilities_calibrated": False, "out_of_training_range": [],
                "distribution_warnings": [], "inference_ms": spec.get("inference_ms", 1.5),
                "echo": {k: msg.get(k) for k in ("network_state_id", "graph_version", "evaluation_id", "state_version")}}
        if kind == "hang":
            continue
        if kind == "crash":
            os._exit(3)
        if kind == "malformed":
            proto.write("{not json\n")
            proto.flush()
            continue
        if kind == "late":
            time.sleep(spec.get("delay_ms", 800) / 1000.0)
        if kind == "unknown_label":
            resp["model_label"] = label
            if stage2:
                resp["stage2_probabilities"] = {label: 0.7, classes[0]: 0.1, classes[1]: 0.1, classes[2]: 0.1}
        if kind == "wrong_version":
            resp["model_version"] = "some-other-model"
        if kind == "bad_echo":
            resp["echo"]["graph_version"] = (msg.get("graph_version") or 0) + 99
        if kind == "error":
            resp = {"type": "prediction", "protocol_version": 1, "request_id": rid, "status": "error",
                    "error_code": spec.get("code", "E_INFERENCE_FAILED"), "message": "scripted error"}
        if kind == "oversize":
            proto.write("x" * (a.max_line_bytes + 10) + "\n")
            proto.flush()
            continue
        if kind == "bad_probs" and stage2:
            resp["stage2_probabilities"] = {c: 0.5 for c in classes}
        send(resp)


if __name__ == "__main__":
    sys.exit(main())
