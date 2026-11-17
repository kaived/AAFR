"""AAFR expert-model inference worker (V0.7.2).

A persistent local process that loads the existing two-stage LightGBM
algorithm selector once and answers prediction requests over JSON Lines:

    stdin  : one request object per line (UTF-8, at most --max-line-bytes)
    stdout : one response object per line, nothing else
    stderr : diagnostics and library warnings (free text)

The model code is the separately trained project's own ``ml/inference.py``
(``RoutingAlgorithmSelector.predict_with_confidence``) and ``ml/features.py``
(raw-to-engineered feature transformation), imported unchanged. This worker
adds only validation, artifact pinning and the protocol; it never retrains,
never changes a threshold and never invents an output the model did not
produce. See docs/V0.7.2_INFERENCE_PROTOCOL.md for the exact schema.

Security: joblib files can execute code when loaded. Every artifact and the
imported model source files are verified against the SHA-256 values pinned in
the AAFR manifest *before* anything is imported or unpickled. A mismatch is a
fatal startup error. The worker opens no socket and executes nothing that a
request names.

Run with ``python -I`` (isolated mode) so PYTHON* environment variables and
the user site directory cannot inject code.
"""

import argparse
import hashlib
import io
import json
import math
import os
import platform
import sys
import time
import traceback
import warnings

PROTOCOL_VERSION = 1
WORKER_VERSION = "1.0.0"
DEFAULT_MAX_LINE_BYTES = 65536

# ---------------------------------------------------------------- stdout guard
# The protocol owns the original stdout. Everything else that writes to
# stdout -- print() in imported code, C-level writes from native libraries --
# is redirected to stderr so it can never corrupt the response stream.
_protocol_fd = os.dup(1)
os.dup2(2, 1)
_protocol = io.open(_protocol_fd, "w", encoding="utf-8", newline="\n", closefd=True)
sys.stdout = sys.stderr


def log(message):
    sys.stderr.write("[aafr-model-worker] " + message + "\n")
    sys.stderr.flush()


def send(message):
    _protocol.write(json.dumps(message, allow_nan=False, separators=(",", ":")) + "\n")
    _protocol.flush()


def fatal(code, message, **extra):
    payload = {"type": "fatal", "protocol_version": PROTOCOL_VERSION, "error_code": code, "message": message}
    payload.update(extra)
    send(payload)
    log("fatal %s: %s" % (code, message))
    sys.exit(2)


def _reject_constant(name):
    # json accepts NaN/Infinity tokens by default; the protocol does not.
    raise ValueError("non-finite JSON constant %s" % name)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


class Worker:
    def __init__(self, model_dir, manifest_path, max_line_bytes):
        self.model_dir = os.path.abspath(model_dir)
        self.manifest_path = os.path.abspath(manifest_path)
        self.max_line_bytes = max_line_bytes
        self.manifest = None
        self.selector = None
        self.raw_names = []
        self.ranges = {}
        self.requests = 0

    # ------------------------------------------------------------ startup
    def load(self):
        started = time.perf_counter()
        try:
            with open(self.manifest_path, "r", encoding="utf-8") as handle:
                manifest = json.load(handle, parse_constant=_reject_constant)
        except FileNotFoundError:
            fatal("E_MANIFEST_MISSING", "manifest not found: %s" % self.manifest_path)
        except (ValueError, OSError) as error:
            fatal("E_MANIFEST_INVALID", "manifest unreadable: %s" % error)
        required = ["schema", "model_version", "feature_contract_version", "artifacts", "raw_features",
                    "model_feature_columns", "stage1", "stage2", "label_map"]
        missing = [k for k in required if k not in manifest]
        if missing or manifest.get("schema") != "aafr.expert_model_manifest.v1":
            fatal("E_MANIFEST_INVALID", "manifest schema or fields invalid (missing: %s)" % ",".join(missing))
        if manifest.get("protocol_version") != PROTOCOL_VERSION:
            fatal("E_PROTOCOL_VERSION", "manifest protocol_version %r != worker %d"
                  % (manifest.get("protocol_version"), PROTOCOL_VERSION))
        self.manifest = manifest

        # 1. Verify every pinned file before importing or unpickling anything.
        hashes = {}
        for relative, expected in sorted(manifest["artifacts"].items()):
            path = os.path.join(self.model_dir, relative)
            if not os.path.isfile(path):
                fatal("E_ARTIFACT_MISSING", "artifact missing: %s" % relative, artifact=relative)
            actual = sha256_file(path)
            if actual != expected:
                fatal("E_ARTIFACT_HASH_MISMATCH", "artifact %s does not match the pinned SHA-256" % relative,
                      artifact=relative, expected=expected, actual=actual)
            hashes[relative] = actual
        verified_ms = (time.perf_counter() - started) * 1000.0

        # 2. Dependencies.
        deps_started = time.perf_counter()
        versions = {}
        for module in ("numpy", "pandas", "sklearn", "lightgbm", "joblib"):
            try:
                versions[module] = __import__(module).__version__
            except Exception as error:  # ImportError or a broken install.
                fatal("E_DEPENDENCY_MISSING", "python module %s unavailable: %s" % (module, error), module=module)
        dependency_ms = (time.perf_counter() - deps_started) * 1000.0

        # 3. Import the model project's own code (now verified) and load.
        sys.path.insert(0, self.model_dir)
        load_warnings = []
        import_started = time.perf_counter()
        try:
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter("always")
                from ml import features as model_features  # noqa: E402
                from ml.inference import RoutingAlgorithmSelector  # noqa: E402
                selector = RoutingAlgorithmSelector.load(os.path.join(self.model_dir, "models"))
            for w in caught:
                text = "%s: %s" % (w.category.__name__, str(w.message).splitlines()[0])
                if text not in load_warnings:
                    load_warnings.append(text)
        except Exception as error:
            log(traceback.format_exc())
            fatal("E_MODEL_LOAD_FAILED", "model load failed: %s" % error)
        load_ms = (time.perf_counter() - import_started) * 1000.0

        # 4. Metadata and contract validation.
        problems = []
        raw_manifest = [f["name"] for f in manifest["raw_features"]]
        if list(model_features.RAW_INPUT_FEATURES) != raw_manifest:
            problems.append("RAW_INPUT_FEATURES differ from the manifest feature contract")
        if list(selector.feature_cols) != list(manifest["model_feature_columns"]):
            problems.append("model feature_columns differ from the manifest")
        if list(model_features.get_feature_columns()) != list(selector.feature_cols):
            problems.append("features.get_feature_columns() differs from model_metadata feature_columns")
        if selector.stage2_model is None or selector.label_encoder is None:
            problems.append("stage 2 selector or label encoder missing")
        else:
            classes = [str(c) for c in selector.label_encoder.classes_]
            if classes != list(manifest["stage2"]["classes"]):
                problems.append("stage 2 classes %s differ from the manifest" % classes)
        if not math.isclose(float(selector.threshold), float(manifest["stage1"]["threshold"]), rel_tol=0, abs_tol=1e-12):
            problems.append("stage 1 threshold %r differs from the manifest %r"
                            % (selector.threshold, manifest["stage1"]["threshold"]))
        n_in = getattr(selector.stage1_model, "n_features_in_", None)
        if n_in is not None and n_in != len(selector.feature_cols):
            problems.append("stage 1 expects %s features, metadata lists %d" % (n_in, len(selector.feature_cols)))
        if problems:
            fatal("E_METADATA_UNSUPPORTED", "; ".join(problems), problems=problems)

        self.selector = selector
        self.raw_names = raw_manifest
        self.ranges = {f["name"]: (f.get("training_min"), f.get("training_max")) for f in manifest["raw_features"]}
        labels = ["Dijkstra"] + [str(c) for c in selector.label_encoder.classes_]
        send({
            "type": "ready",
            "protocol_version": PROTOCOL_VERSION,
            "worker_version": WORKER_VERSION,
            "model_name": manifest.get("model_name", ""),
            "model_version": manifest["model_version"],
            "feature_contract_version": manifest["feature_contract_version"],
            "raw_features": raw_manifest,
            "model_feature_count": len(selector.feature_cols),
            "labels": labels,
            "stage2_classes": [str(c) for c in selector.label_encoder.classes_],
            "gate_threshold": float(selector.threshold),
            "metadata_timestamp": selector.metadata.get("timestamp", ""),
            "artifact_sha256": hashes,
            "library_versions": versions,
            "python_version": platform.python_version(),
            "load_warnings": load_warnings,
            "verify_ms": round(verified_ms, 3),
            "dependency_import_ms": round(dependency_ms, 3),
            "load_ms": round(load_ms, 3),
            "pid": os.getpid(),
        })
        log("ready: %s (%d raw features, %d model columns)" % (manifest["model_version"], len(raw_manifest),
                                                              len(selector.feature_cols)))

    # ------------------------------------------------------------ requests
    def error(self, request_id, code, message, **extra):
        payload = {"type": "prediction", "protocol_version": PROTOCOL_VERSION, "request_id": request_id,
                   "status": "error", "error_code": code, "message": message}
        payload.update(extra)
        send(payload)

    def handle_predict(self, msg):
        request_id = msg.get("request_id")
        if not isinstance(request_id, str) or not request_id or len(request_id) > 128:
            self.error(None, "E_MISSING_FIELD", "request_id must be a non-empty string of at most 128 characters")
            return
        if msg.get("protocol_version") != PROTOCOL_VERSION:
            self.error(request_id, "E_PROTOCOL_VERSION", "protocol_version must be %d" % PROTOCOL_VERSION)
            return
        for field in ("model_version", "feature_contract_version", "features"):
            if field not in msg:
                self.error(request_id, "E_MISSING_FIELD", "missing field %s" % field)
                return
        if msg["model_version"] != self.manifest["model_version"]:
            self.error(request_id, "E_MODEL_VERSION_MISMATCH", "requested model %r, loaded %r"
                       % (msg["model_version"], self.manifest["model_version"]))
            return
        if msg["feature_contract_version"] != self.manifest["feature_contract_version"]:
            self.error(request_id, "E_FEATURE_CONTRACT_MISMATCH", "requested contract %r, loaded %r"
                       % (msg["feature_contract_version"], self.manifest["feature_contract_version"]))
            return
        features = msg["features"]
        if not isinstance(features, dict):
            self.error(request_id, "E_INVALID_FEATURE_VALUE", "features must be an object")
            return
        missing = [n for n in self.raw_names if n not in features]
        if missing:
            self.error(request_id, "E_MISSING_FEATURES", "%d required feature(s) missing" % len(missing),
                       features=missing)
            return
        unexpected = sorted(n for n in features if n not in self.ranges)
        if unexpected:
            self.error(request_id, "E_UNEXPECTED_FEATURES", "%d unknown feature(s)" % len(unexpected),
                       features=unexpected)
            return
        values = {}
        bad = []
        for name in self.raw_names:
            v = features[name]
            if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(float(v)):
                bad.append(name)
            else:
                values[name] = float(v)
        if bad:
            self.error(request_id, "E_INVALID_FEATURE_VALUE", "non-numeric or non-finite value(s)", features=bad)
            return
        out_of_range = []
        for name in self.raw_names:
            lo, hi = self.ranges[name]
            if lo is not None and hi is not None and not (lo <= values[name] <= hi):
                out_of_range.append(name)

        started = time.perf_counter()
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                result = self.selector.predict_with_confidence(values)
        except Exception as error:
            log(traceback.format_exc())
            self.error(request_id, "E_INFERENCE_FAILED", "inference failed: %s" % error)
            return
        inference_ms = (time.perf_counter() - started) * 1000.0
        self.requests += 1
        gate = float(result["gate_confidence"])
        threshold = float(result["threshold"])
        stage2_ran = gate >= threshold
        probabilities = {str(k): float(v) for k, v in result.get("algo_probabilities", {}).items()}
        response = {
            "type": "prediction",
            "protocol_version": PROTOCOL_VERSION,
            "request_id": request_id,
            "status": "ok",
            "model_version": self.manifest["model_version"],
            "feature_contract_version": self.manifest["feature_contract_version"],
            "model_label": str(result["algorithm"]),
            "label_source": "stage2_argmax" if stage2_ran else "stage1_below_threshold",
            "gate_probability": gate,
            "gate_threshold": threshold,
            "stage2_ran": stage2_ran,
            "stage2_probabilities": probabilities if stage2_ran else None,
            "probabilities_calibrated": False,
            "out_of_training_range": out_of_range,
            # engineer_features() sets every scenario one-hot to 0 without a
            # scenario_category; no training row looked like that.
            "distribution_warnings": ["scenario_category one-hot columns are all 0 (unseen in training)"],
            "inference_ms": round(inference_ms, 3),
            "echo": {k: msg.get(k) for k in ("network_state_id", "graph_version", "evaluation_id", "state_version")},
            "reasoning": str(result.get("reasoning", "")),
        }
        send(response)

    def run(self):
        stdin = sys.stdin.buffer
        while True:
            line = stdin.readline(self.max_line_bytes + 1)
            if not line:
                log("stdin closed; exiting")
                return 0
            if len(line) > self.max_line_bytes and not line.endswith(b"\n"):
                # Drain the rest of the oversized line, then report it.
                while True:
                    rest = stdin.readline(self.max_line_bytes + 1)
                    if not rest or rest.endswith(b"\n"):
                        break
                self.error(None, "E_MESSAGE_TOO_LARGE", "request exceeds %d bytes" % self.max_line_bytes)
                continue
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line.decode("utf-8"), parse_constant=_reject_constant)
            except (ValueError, UnicodeDecodeError) as error:
                self.error(None, "E_MALFORMED_JSON", "request is not valid JSON: %s" % error)
                continue
            if not isinstance(msg, dict):
                self.error(None, "E_MALFORMED_JSON", "request must be a JSON object")
                continue
            kind = msg.get("type")
            if kind == "predict":
                self.handle_predict(msg)
            elif kind == "ping":
                send({"type": "pong", "protocol_version": PROTOCOL_VERSION, "request_id": msg.get("request_id"),
                      "requests_served": self.requests, "pid": os.getpid()})
            elif kind == "shutdown":
                send({"type": "bye", "protocol_version": PROTOCOL_VERSION, "requests_served": self.requests})
                return 0
            else:
                self.error(msg.get("request_id") if isinstance(msg.get("request_id"), str) else None,
                           "E_UNKNOWN_TYPE", "unknown message type %r" % (kind,))


def main():
    parser = argparse.ArgumentParser(description="AAFR expert-model inference worker")
    parser.add_argument("--model-dir", required=True, help="model release root containing models/ and ml/")
    parser.add_argument("--manifest", required=True, help="AAFR model manifest with pinned SHA-256 values")
    parser.add_argument("--max-line-bytes", type=int, default=DEFAULT_MAX_LINE_BYTES)
    args = parser.parse_args()
    send({"type": "hello", "protocol_version": PROTOCOL_VERSION, "worker_version": WORKER_VERSION,
          "python_version": platform.python_version(), "pid": os.getpid()})
    worker = Worker(args.model_dir, args.manifest, max(1024, args.max_line_bytes))
    worker.load()
    return worker.run()


if __name__ == "__main__":
    sys.exit(main())
