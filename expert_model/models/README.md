# Models

Versioned model artifacts and metadata. Avoid committing large binaries unless
the repository policy explicitly allows them.

`model_manifest.json` (V0.7.2) pins the SHA-256 of the external model release
(`models/*.joblib`, `model_metadata.json`, `ml/*.py`), its version, the raw
feature contract `aafr.model_features.v1` with units and training ranges, the
stage semantics and the label map. The binaries themselves stay outside the
repository (`AAFR_MODEL_DIR`).
