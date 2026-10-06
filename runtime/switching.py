"""Switching policy helpers for AAFR experiments."""


class SwitchingPolicy:
    """Placeholder for strategy publication and switch-rate coordination."""

    def should_switch(self, *, confidence_ok: bool, stability_ok: bool) -> bool:
        return confidence_ok and stability_ok
