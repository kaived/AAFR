"""AAFR runtime orchestration anchor.

The production runtime is currently implemented in C++ under this folder. This
module reserves the Python-facing entrypoint for experiments, notebooks, and
future integration glue that should not depend on Qt.
"""


class AAFRRuntime:
    """Coordinates sensing, decisions, stability checks, execution, and logging."""

    def __init__(self) -> None:
        self.started = False

    def start(self) -> None:
        self.started = True

    def stop(self) -> None:
        self.started = False
