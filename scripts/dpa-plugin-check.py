#!/usr/bin/env python3
"""Smoke test: import DPA and instantiate classes as a user would."""
import torch
import dpa

print(f"torch {torch.__version__}")
print(f"dpa from {dpa.__file__}")
print(f"CUDA available: {torch.cuda.is_available()}")
if torch.cuda.is_available():
    print(f"CUDA device: {torch.cuda.get_device_name(0)}")
print("DPA Public API:")
for name in dpa.__all__:
    print(f"  dpa.{name}")

print()

# Instantiate option classes
# sock_opts = dpa.DPASocketBackendOptions()
print(f"{dpa.DPASocketBackendOptions()}")
print("----------------------------------------------------------")
print(f"{dpa.DPADpdkBackendOptions()}")
print("----------------------------------------------------------")
print(f"{dpa.DPADeviceOptions()}")
print("----------------------------------------------------------")
print(f"{dpa.ProcessGroupDPASocketOptions()}")
print("----------------------------------------------------------")
print(f"{dpa.ProcessGroupDPADpdkOptions()}")


print("\nOK.")


