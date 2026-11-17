# Inference

Runtime-facing inference adapters and serving experiments.

`aafr_model_worker.py` (V0.7.2) is the persistent local inference worker the
AAFR runtime starts: JSON Lines over stdin/stdout, artifacts verified against
`../models/model_manifest.json` before anything is unpickled. Protocol and
lifecycle: [expert model README](../README.md#inference-protocol-version-1).
