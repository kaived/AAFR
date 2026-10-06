"""Stability gate helpers for AAFR experiments."""


class StabilityGate:
    """Placeholder for dwell-time, hysteresis, confidence, and cooldown checks."""

    def allow(self, *, confidence: float, threshold: float) -> bool:
        return confidence >= threshold
