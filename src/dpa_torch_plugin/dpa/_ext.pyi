class DataplaneContext:
    """Context manager for enabling dataplane acceleration."""
    def __init__(self, quantization: int = 1, averaging: bool = True, 
                 prescaled: bool = False, straggle: int = 0, pipes: int = 0) -> None: ...
    def enter(self) -> None: ...
    def exit(self) -> None: ...
    def __enter__(self) -> 'DataplaneContext': ...
    def __exit__(self, *args) -> None: ...

# Add DPDK classes:
class DPADpdkBackendOptions:
    # ... similar to DPASocketOptions
    pass

class ProcessGroupDPADpdkOptions:
    dpa_device: DPADeviceOptions
    dpa_backend: DPADpdkBackendOptions

class ProcessGroupDPADpdk:
    def dataplane_allreduce(self, tensors, opts: DPAAllreduceOptions = ...) -> None: ...