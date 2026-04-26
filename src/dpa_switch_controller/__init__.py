"""
DPA Switch Control Plane Library

This library provides a Python API for configuring DPA switches,
extracted from the bfrt-cli.py functionality for reusable components.

Usage:
    import sys
    sys.path.append('/path/to/straggle-ml/src')

    from dpa_switch_controlplane import setup_switch, BfrtCLIController
    from dpa_switch_controlplane.cli import PORT, TSC

    # Quick setup
    cp = setup_switch('config.json')

    # Manual control
    cp = BfrtCLIController('config.json')
    cp.setup_network()
    cp.setup_dpa()
"""

"""DPA switch controller package."""
from .cli_controller import CLIController
from .cli_controller_sa import CLIControllerSA
from .cli_controller_su import CLIControllerSU

__all__ = [
    "CLIController", "CLIControllerSA", "CLIControllerSU",
    "setup_switch", "read_counters",
]


def make_controller(bfrt, config_path: str) -> CLIController:
    """Pick SA or SU controller based on config."""
    # Small peek at the config to decide which subclass to build.
    import json
    with open(config_path, "r") as f:
        conf = json.load(f)
    straggle_aware = bool(conf["switch"]["program"].get("straggle_aware", False))
    cls = CLIControllerSA if straggle_aware else CLIControllerSU
    return cls(bfrt, config_path)


def setup_switch(bfrt, config_path: str) -> CLIController:
    """Build controller and run full network + DPA setup."""
    cp = make_controller(bfrt, config_path)
    cp.setup_network()
    cp.setup_dpa()
    return cp


def read_counters(bfrt, config_path: str):
    cp = make_controller(bfrt, config_path)
    cp.read_counters()