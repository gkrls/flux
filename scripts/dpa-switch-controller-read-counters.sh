#!/usr/bin/env bash
set -euo pipefail

# Usage/help
if [[ "${1-}" == "-h" || "${1-}" == "--help" ]]; then
  echo "Usage: $0 <config.json> <dpa_install>"
  echo "  <dpa_config>   Path to config.json (required)"
  echo "  <dpa_install>  Path to DPA_INSTALL dir (required)"
  exit 0
fi

DPA_CONFIG="$(realpath "$1")"
DPA_INSTALL="$(realpath "$2")"

tmpfile="$(mktemp -t dpa-switch-controller-read-counters.XXXXXX)"
cleanup() {
  rm -f "$tmpfile"
}
trap cleanup EXIT  # run cleanup on normal exit or errors


cat >> "$tmpfile" <<EOF
import sys, os
sys.path.append(os.path.join("$DPA_INSTALL", "lib"))
import dpa_switch_controller.cli as controller
controller.read_counters(bfrt,"$DPA_CONFIG")
EOF

${SDE_INSTALL}/../run_bfshell.sh -b "$tmpfile"