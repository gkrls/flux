
#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 <config.json> <dpa_config>"
  echo "  <dpa_config>   Path to config.json (required)"
  echo "  <dpa_install>  Path to DPA_INSTALL dir (required)"
  # echo "  [tmux_panel]   Run the configuration in tmux panel"
}


DPA_CONFIG="$(realpath "$1")"
DPA_INSTALL="$(realpath "$2")"

tmpfile="$(mktemp -t myscript.XXXXXX)"

cleanup() {
  rm -f "$tmpfile"
}
trap cleanup EXIT  # run cleanup on normal exit or errors


cat >> "$tmpfile" <<EOF
import sys, os
sys.path.append(os.path.join("$DPA_INSTALL", "lib"))
import dpa_switch_controller.cli as controller
controller.setup_switch(bfrt,"$DPA_CONFIG")
EOF

${SDE_INSTALL}/../run_bfshell.sh -b "$tmpfile"