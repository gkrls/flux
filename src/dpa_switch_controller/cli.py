import json

from .cli_controller import CLIController
from .cli_controller_sa import CLIControllerSA
from .cli_controller_su import CLIControllerSU

__all__ = ["make_controller", "setup_switch", "read_counters"]


def make_controller(bfrt, config_path: str) -> CLIController:
    """Pick SA or SU controller based on the config file."""
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


def read_counters(bfrt, config_path: str) -> None:
    cp = make_controller(bfrt, config_path)
    cp.read_counters()