# #!/usr/bin/env bash

DPDK_PREFIX=$(pkg-config --variable=prefix libdpdk 2>/dev/null || echo "/opt/mellanox/dpdk")
HUGE_TOOL="${DPDK_PREFIX}/bin/dpdk-hugepages.py"
[ -x "$HUGE_TOOL" ] || { echo "dpdk-hugepages.py tool not found at ${HUGE_TOOL}"; exit 1; }

case "${1:-}" in
  --status|--check)
    "$HUGE_TOOL" --show
    ;;
  --enable)
    if "$HUGE_TOOL" --show 2>/dev/null | grep -q '[KMG]b'; then
      echo "Hugepages already allocated:"
      "$HUGE_TOOL" --show
      exit 1
    fi
    echo "Allocating 2GB with 1GB pages..."
    if ! sudo "$HUGE_TOOL" --pagesize 1G --setup 2G; then
      echo "-- 1GB failed, likely due to fragmentation. Retry after reboot. Using 2MB pages for now"
      sudo "$HUGE_TOOL" --clear || true
      sudo "$HUGE_TOOL" --pagesize 2M --setup 2G || { echo "-- 2MB failed"; exit 1; }
    fi
    echo "Hugepages enabled."
    ;;
  --disable)
    sudo "$HUGE_TOOL" --clear || true
    sudo "$HUGE_TOOL" --unmount || true
    echo "Hugepages disabled."
    ;;
  *)
    echo "Usage: sudo $0 {--enable|--disable|--status}"
    exit 1
    ;;
esac