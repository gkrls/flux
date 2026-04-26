# dpa/__init__.py
import torch

from ._ext import (
    # Context manager
    DataplaneContext,
    
    # Core options
    DPADeviceSession,
    DPADeviceOptions,
    # DPAAllreduceOptions,
    
    # Socket backend
    DPASocketBackendOptions,
    ProcessGroupDPASocketOptions,
    ProcessGroupDPASocket,
    
    # DPDK backend
    DPADpdkBackendOptions,
    ProcessGroupDPADpdkOptions,
    ProcessGroupDPADpdk,
)

# Import the ddp module's main function directly
# from .ddp import DDPWrapper
from .ddp import DDPWrapper, DDPStraggleSim

__all__ = [
    # Main exports
    'DataplaneContext',
    'DDPWrapper',
    'DDPStraggleSim',
    
    # Options
    'DPADeviceSession',
    'DPADeviceOptions',
    # 'DPAAllreduceOptions',
    
    # Socket backend
    'ProcessGroupDPASocket',
    'ProcessGroupDPASocketOptions',
    'DPASocketBackendOptions',
    
    # DPDK backend  
    'ProcessGroupDPADpdk',
    'ProcessGroupDPADpdkOptions',
    'DPADpdkBackendOptions',
]

# import json

# def _pretty_repr(self):
#     name = type(self).__name__
#     d = dict(self)  # works because make_dict_like adds __getitem__ / iteration
#     body = json.dumps(d, indent=2, default=str)
#     return f"{name}({body})"

# for cls in (DPASocketBackendOptions, DPADpdkBackendOptions, DPADeviceOptions,
#             DPADeviceSession, ProcessGroupDPASocketOptions, ProcessGroupDPADpdkOptions):
#     cls.__repr__ = _pretty_repr