#!/usr/bin/env bash

set -uo pipefail

warn() { echo "warn: $*"; WARNINGS=$((WARNINGS+1)); }
err()  { echo "error: $*" >&2; ERRORS=$((ERRORS+1)); }
ERRORS=0
WARNINGS=0


# kernel
EXPECTED_KERNEL="6.8"
KERNEL_VER=$(uname -r)
[[ "$KERNEL_VER" == ${EXPECTED_KERNEL}* ]] || warn "kernel is $KERNEL_VER; tested on $EXPECTED_KERNEL"

# cmake
command -v cmake      >/dev/null || err "cmake not installed"

# python
command -v python3    >/dev/null || err "python3 not installed"

# pkg-config
command -v pkg-config >/dev/null || err "pkg-config not installed"

# dpdk
# pkg-config --exists libdpdk      || err "DPDK not found (pkg-config can't find libdpdk)"
# EXPECTED_DPDK="22.11.2504.1.0"
# DPDK_VER=$(pkg-config --modversion libdpdk 2>/dev/null)
# if [ "$DPDK_VER" != "$EXPECTED_DPDK" ]; then
#     warn "DPDK version mismatch. Expected $EXPECTED_DPDK, but found ${DPDK_VER:-none}"
# fi

# dpdk
EXPECTED_DPDK="22.11.2504.1.0"

if pkg-config --exists libdpdk 2>/dev/null; then
    DPDK_VER=$(pkg-config --modversion libdpdk 2>/dev/null)
    if [ "$DPDK_VER" != "$EXPECTED_DPDK" ]; then
        warn "DPDK version mismatch. Expected $EXPECTED_DPDK, but found ${DPDK_VER:-none}"
    fi
else
    PC_FOUND=$(find /opt/mellanox -name libdpdk.pc 2>/dev/null | head -n1)
    if [ -n "$PC_FOUND" ]; then
        warn "DPDK found under $PC_FOUND but pkg-config cannot see it. \
              Use export PKG_CONFIG_PATH=$(dirname "$PC_FOUND"):\$PKG_CONFIG_PATH or put it in .bashrc"
        DPDK_VER=$(grep -E '^Version:' "$PC_FOUND" | awk '{print $2}')
        if [ "$DPDK_VER" != "$EXPECTED_DPDK" ]; then
            warn "DPDK version mismatch. Expected $EXPECTED_DPDK, but found ${DPDK_VER:-none}"
        fi
    else
        warn "DPDK not found. If installed, set PKG_CONFIG_PATH to the directory containing libdpdk.pc"
    fi
fi

# mellanox connectx-6 dx
LSPCI_OUT=$(lspci)
echo "$LSPCI_OUT" | grep -qi 'ConnectX-6 Dx' || warn "NIC is not ConnectX-6 Dx; perf numbers may differ"

# mlx5 driver
LSMOD_OUT=$(lsmod)
echo "$LSMOD_OUT" | grep -q '^mlx5_core' || err "mlx5_core kernel module not loaded"

# hugepages
HP_1G=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages 2>/dev/null || echo 0)
HP_2M=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages    2>/dev/null || echo 0)
if [ "$HP_1G" -gt 0 ]; then
    :
elif [ "$HP_2M" -gt 0 ]; then
    warn "hugepages are 2M ($HP_2M pages); 1G is ideal — reboot and run dpdk-hugepages.sh --enable"
else
    warn "no hugepages allocated. Run: sudo ./dpdk-hugepages.sh --enable"
fi

# nvidia gpu
echo "$LSPCI_OUT" | grep -i "VGA\|3D" | grep -qi "NVIDIA" || warn "No physical NVIDIA GPU detected on PCI bus"
command -v nvidia-smi >/dev/null || err "nvidia driver not installed (nvidia-smi missing)"
if command -v nvidia-smi >/dev/null; then
    nvidia-smi -L >/dev/null 2>&1 || err "nvidia-smi cannot communicate with GPU (driver/hardware error)"
fi

if [ "$ERRORS" -gt 0 ]; then
    echo "❌ Check failed with $ERRORS error(s) and $WARNINGS warning(s). Please fix the errors." >&2
    exit 1
elif [ "$WARNINGS" -gt 0 ]; then
    echo "⚠️ Check passed with $WARNINGS warning(s). Node is \"probably\" ready."
    exit 0
else
    echo "✅ Check passed. Node is ready."
    exit 0
fi