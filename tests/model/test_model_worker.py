"""Protocol tests for expert_model/inference/aafr_model_worker.py (V0.7.2).

Drives the real worker process over stdin/stdout with the real, pinned model
artifacts. Exit code 77 (CTest SKIP) when the model directory or the model's
Python packages are not available; never reports unexecuted checks as passed.
"""

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
import unittest

ARGS = None


def skip(reason):
    print("SKIP:", reason)
    sys.exit(77)


class Worker:
    def __init__(self, manifest=None, max_line=65536):
        cmd = [sys.executable, "-I", "-X", "utf8", ARGS.worker, "--model-dir", ARGS.model_dir,
               "--manifest", manifest or ARGS.manifest, "--max-line-bytes", str(max_line)]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, encoding="utf-8", bufsize=1)
        self.hello = self.read()
        self.ready = self.read()

    def read(self):
        line = self.p.stdout.readline()
        if not line:
            return None
        return json.loads(line)  # every stdout line must be JSON

    def send(self, obj_or_text):
        text = obj_or_text if isinstance(obj_or_text, str) else json.dumps(obj_or_text)
        self.p.stdin.write(text + "\n")
        self.p.stdin.flush()
        return self.read()

    def close(self):
        try:
            self.send({"type": "shutdown"})
        except Exception:
            pass
        self.p.stdin.close()
        self.p.wait(timeout=20)
        self.p.stdout.close()
        self.p.stderr.close()


def manifest():
    with open(ARGS.manifest, encoding="utf-8") as f:
        return json.load(f)


def features(**overrides):
    m = manifest()
    f = {x["name"]: (x["training_min"] + x["training_max"]) / 2.0 for x in m["raw_features"]}
    for k in ("num_nodes", "num_edges", "diameter", "req_shortest_hops", "source_in_degree", "source_out_degree",
              "dest_in_degree", "dest_out_degree", "max_queue_depth", "link_failure_events",
              "link_recovery_events", "reroute_count"):
        f[k] = float(round(f[k]))
    f.update(overrides)
    return f


def predict(rid, feats, **extra):
    m = manifest()
    msg = {"type": "predict", "protocol_version": 1, "request_id": rid, "model_version": m["model_version"],
           "feature_contract_version": m["feature_contract_version"], "network_state_id": 3, "graph_version": 9,
           "evaluation_id": "e1", "features": feats}
    msg.update(extra)
    return msg


class WorkerProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.w = Worker()
        if cls.w.ready is None or cls.w.ready.get("type") != "ready":
            raise RuntimeError("worker not ready: %r / stderr: %s" % (cls.w.ready, cls.w.p.stderr.read()[-2000:]))

    @classmethod
    def tearDownClass(cls):
        cls.w.close()

    def test_ready_describes_verified_model(self):
        r = self.w.ready
        self.assertEqual(self.w.hello["type"], "hello")
        self.assertEqual(r["model_version"], manifest()["model_version"])
        self.assertEqual(len(r["raw_features"]), 35)
        self.assertEqual(r["model_feature_count"], 55)
        self.assertEqual(r["labels"], ["Dijkstra", "ACO", "GWO", "PSO", "WOA"])
        self.assertAlmostEqual(r["gate_threshold"], 0.51, places=9)
        self.assertEqual(set(r["artifact_sha256"]), set(manifest()["artifacts"]))

    def test_valid_prediction_and_correlation(self):
        for i in range(5):
            r = self.w.send(predict("ok-%d" % i, features(latency=5.0 + 10 * i)))
            self.assertEqual(r["status"], "ok")
            self.assertEqual(r["request_id"], "ok-%d" % i)
            self.assertIn(r["model_label"], ["Dijkstra", "ACO", "GWO", "PSO", "WOA"])
            self.assertTrue(0.0 <= r["gate_probability"] <= 1.0)
            self.assertEqual(r["stage2_ran"], r["gate_probability"] >= r["gate_threshold"])
            if r["stage2_ran"]:
                self.assertAlmostEqual(sum(r["stage2_probabilities"].values()), 1.0, places=6)
            else:
                self.assertIsNone(r["stage2_probabilities"])
                self.assertEqual(r["model_label"], "Dijkstra")
            self.assertFalse(r["probabilities_calibrated"])
            self.assertEqual(r["echo"]["network_state_id"], 3)
            self.assertEqual(r["echo"]["graph_version"], 9)
            self.assertEqual(r["echo"]["evaluation_id"], "e1")

    def test_matches_direct_model_call(self):
        # Same answer as calling the model project's own code in-process.
        sys.path.insert(0, ARGS.model_dir)
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            from ml.inference import RoutingAlgorithmSelector
            sel = RoutingAlgorithmSelector.load(os.path.join(ARGS.model_dir, "models"))
            f = features(latency=40.0, loss=0.2)
            direct = sel.predict_with_confidence(f)
        r = self.w.send(predict("direct", f))
        self.assertEqual(r["model_label"], direct["algorithm"])
        self.assertAlmostEqual(r["gate_probability"], direct["gate_confidence"], places=12)

    def test_rejections(self):
        f = features()
        missing = dict(f)
        del missing["jitter"]
        self.assertEqual(self.w.send(predict("m", missing))["error_code"], "E_MISSING_FEATURES")
        self.assertEqual(self.w.send(predict("u", dict(f, teleport=1.0)))["error_code"], "E_UNEXPECTED_FEATURES")
        self.assertEqual(self.w.send(predict("s", dict(f, latency="fast")))["error_code"], "E_INVALID_FEATURE_VALUE")
        self.assertEqual(self.w.send(predict("b", dict(f, latency=True)))["error_code"], "E_INVALID_FEATURE_VALUE")
        nan = json.dumps(predict("n", f)).replace('"latency": %s' % json.dumps(f["latency"]), '"latency": NaN')
        self.assertIn('NaN', nan)
        self.assertEqual(self.w.send(nan)["error_code"], "E_MALFORMED_JSON")
        inf = nan.replace("NaN", "Infinity")
        self.assertEqual(self.w.send(inf)["error_code"], "E_MALFORMED_JSON")
        self.assertEqual(self.w.send(predict("v", f, model_version="x"))["error_code"], "E_MODEL_VERSION_MISMATCH")
        self.assertEqual(self.w.send(predict("c", f, feature_contract_version="x"))["error_code"],
                         "E_FEATURE_CONTRACT_MISMATCH")
        self.assertEqual(self.w.send(predict("p", f, protocol_version=2))["error_code"], "E_PROTOCOL_VERSION")
        self.assertEqual(self.w.send("{broken")["error_code"], "E_MALFORMED_JSON")
        self.assertEqual(self.w.send("[1,2]")["error_code"], "E_MALFORMED_JSON")
        self.assertEqual(self.w.send({"type": "teleport", "request_id": "t"})["error_code"], "E_UNKNOWN_TYPE")
        r = self.w.send(predict("range", dict(f, num_nodes=4.0)))
        self.assertEqual(r["status"], "ok")
        self.assertIn("num_nodes", r["out_of_training_range"])
        # Still alive and correct after every rejection.
        self.assertEqual(self.w.send(predict("after", f))["status"], "ok")

    def test_oversized_request(self):
        r = self.w.send(json.dumps({"type": "ping", "pad": "x" * 70000}))
        self.assertEqual(r["error_code"], "E_MESSAGE_TOO_LARGE")
        self.assertEqual(self.w.send({"type": "ping", "request_id": "p"})["type"], "pong")


class WorkerStartupTest(unittest.TestCase):
    def _manifest_with(self, mutate):
        m = manifest()
        mutate(m)
        fd, path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(m, f)
        return path

    def _fatal(self, path):
        w = Worker(manifest=path)
        try:
            self.assertEqual(w.ready["type"], "fatal")
            return w.ready["error_code"]
        finally:
            w.p.wait(timeout=20)
            w.p.stdin.close(); w.p.stdout.close(); w.p.stderr.close()
            os.remove(path)

    def test_hash_mismatch_refuses_to_unpickle(self):
        path = self._manifest_with(lambda m: m["artifacts"].__setitem__("models/stage1_gate.joblib", "0" * 64))
        self.assertEqual(self._fatal(path), "E_ARTIFACT_HASH_MISMATCH")

    def test_missing_artifact(self):
        path = self._manifest_with(lambda m: m["artifacts"].__setitem__("models/absent.joblib", "0" * 64))
        self.assertEqual(self._fatal(path), "E_ARTIFACT_MISSING")

    def test_unsupported_metadata(self):
        path = self._manifest_with(lambda m: m["stage2"].__setitem__("classes", ["ACO", "GWO", "PSO"]))
        self.assertEqual(self._fatal(path), "E_METADATA_UNSUPPORTED")

    def test_invalid_manifest(self):
        path = self._manifest_with(lambda m: m.__setitem__("schema", "other"))
        self.assertEqual(self._fatal(path), "E_MANIFEST_INVALID")


def main():
    global ARGS
    p = argparse.ArgumentParser()
    p.add_argument("--worker", required=True)
    p.add_argument("--manifest", required=True)
    p.add_argument("--model-dir", default="")
    ARGS, rest = p.parse_known_args()
    if not ARGS.model_dir or not os.path.isdir(ARGS.model_dir):
        skip("model directory not configured")
    try:
        import numpy, pandas, sklearn, lightgbm, joblib  # noqa: F401
    except ImportError as e:
        skip("model packages missing: %s" % e)
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)


if __name__ == "__main__":
    main()
