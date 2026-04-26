#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 <config.json> <dpa_config> <dpa_install> <sde_install> [build_dir]"
  echo "  <dpa_config>   Path to config.json (required)"
  echo "  <dpa_install>  Path to DPA_INSTALL dir (required)"
  echo "  <sde_install>  Path to SDE_INSTALL dir (required)"
  echo "  [p4build_dir]  Path to the p4build dir (default: ./p4build)"
}

[[ "${1-}" == "-h" || "${1-}" == "--help" ]] && { usage; exit 0; }
[ $# -lt 4 ] && { usage; exit 1; }

DPA_CONFIG="$(realpath "$1")"
DPA_INSTALL="$(realpath "$2")"
SDE_INSTALL="$(realpath "$3")"
P4BUILD_DIR="$(realpath "${4:-./p4build}")"

[ -f "$DPA_CONFIG" ]  || { echo "Error: DPA_CONFIG '$DPA_CONFIG' not found."; exit 1; }
[ -d "$SDE_INSTALL" ] || { echo "Error: SDE_INSTALL '$SDE_INSTALL' not found."; exit 1; }
[ -d "$DPA_INSTALL" ] || { echo "Error: DPA_INSTALL '$DPA_INSTALL' not found."; exit 1; }
[ -d "$P4BUILD_DIR" ] || { echo "Error: p4build '$P4BUILD_DIR' not found."; exit 1; }

# [ -d "$SDE" ]         || { echo "Error: SDE '$SDE' not found."; exit 1; }
BF_KDRV="$SDE_INSTALL/bin/bf_kdrv_mod_load"
SWITCHD="$SDE_INSTALL/../run_switchd.sh"
[ -x "$BF_KDRV" ] || { echo "Error: '$BF_KDRV' not executable."; exit 1; }
[ -x "$SWITCHD" ] || { echo "Error: '$SWITCHD' not executable."; exit 1; }

cat <<EOF

DPA_CONFIG  : $DPA_CONFIG
SDE_INSTALL : $SDE_INSTALL
P4BUILD_DIR : $P4BUILD_DIR

EOF

mkdir -p "$P4BUILD_DIR/log"

# Clean session, create panes with commands (no send-keys => only results shown)
tmux kill-session -t switch 2>/dev/null || true

  # echo SDE_ISNTALL: $SDE_INSTALL;
  # echo DPA_INSTALL: $DPA_INSTALL;
# tmux new-session -d -s switch "bash -lc 'clear;
#   export SDE_INSTALL=$SDE_INSTALL;
#   export DPA_CONFIG=$DPA_CONFIG;
#   export DPA_INSTALL=$DPA_INSTALL;
#   export P4BUILD=$P4BUILD_DIR;
#   export BFRT_CLI=$BFRT_CLI;
#   \$SDE_INSTALL/../run_bfshell.sh -b $BFRT_CLI;
#   exec bash -i'"

tmpfile="$(mktemp -t bfrt-cli.XXXXXX)"
cat >> "$tmpfile" <<EOF
import sys, os
sys.path.append(os.path.join("$DPA_INSTALL", "lib"))
import dpa_switch_controller.cli as controller
controller.setup_switch(bfrt, "$DPA_CONFIG")
EOF

tmux new-session -d -s switch \
  env SDE_INSTALL="$SDE_INSTALL" tmpfile="$tmpfile" \
  bash -lc '
    clear
    (
      trap "rm -f -- \"$tmpfile\"" EXIT INT TERM
      "$SDE_INSTALL/../run_bfshell.sh" -b "$tmpfile"
    )
    rc=$?
    echo "bfshell exited with $rc"
    exec bash -i
  '

tmux split-window -v -t switch:0 "bash -lc 'set -e;
  ( sudo \"$BF_KDRV\" \"$SDE_INSTALL\" 2>/dev/null || lsmod | grep -q \"^bf_kdrv\\b\" );
  echo bf_kdrv ready;
  cd \"$P4BUILD_DIR/log\";
  \"$SWITCHD\" -c \"$P4BUILD_DIR/switch/switch.conf\"'"

tmux select-pane -t switch.1
tmux attach -t switch
