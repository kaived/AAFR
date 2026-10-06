"""Serializable decision records for AAFR experiments."""

from dataclasses import dataclass
from typing import Mapping


@dataclass(frozen=True)
class DecisionRecord:
    subsystem: str
    selected_strategy: str
    confidence: float
    scores: Mapping[str, float]
