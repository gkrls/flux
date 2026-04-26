#!/usr/bin/env bash

set -euo pipefail

# Usage/help
if [[ "${1-}" == "-h" || "${1-}" == "--help" ]]; then
  echo "Usage: $0 <config.json> <dpa_install> <sde_install> [build_dir]"
  echo "  <dpa_config>   Path to config.json (required)"
  echo "  <dpa_install>  DPA install dir (required)"
  echo "  <sde_install>  SDE_INSTALL dir (required)"
  echo "  <tofino_ver>   Tofino target, 'tofino' or 'tofino2'"
  echo "  [p4build_dir]    Directory to build the P4 program (default: ./p4build)"
  exit 0
fi

# Args
[ $# -lt 4 ] && { echo "Error: need <config.json> <dpa_install> <sde_install>"; exit 1; }
[ ! -f "$1" ] && { echo "Error: Config '$1' does not exist."; exit 1; }

DPA_CONFIG="$(realpath "$1")"
DPA_INSTALL="$(realpath "$2")"
SDE_INSTALL="$(realpath "$3")"
TOFINO_TARGET=$4
P4BUILD_DIR="$(realpath "${5:-./p4build}")"

# Checks
[ -f "$DPA_CONFIG" ]  || { echo "Error: DPA_CONFIG '$DPA_CONFIG' not found."; exit 1; }
[ -d "$DPA_INSTALL" ]   || { echo "Error: DPA_INSTALL '$DPA_INSTALL' not found."; exit 1; }
[ -d "$SDE_INSTALL" ]   || { echo "Error: SDE_INSTALL '$SDE_INSTALL' not found."; exit 1; }

mkdir -p "$P4BUILD_DIR"

P4C="$SDE_INSTALL/bin/bf-p4c"

# Info
echo "DPA_CONFIG  : $DPA_CONFIG"
echo "DPA_INSTALL : $DPA_INSTALL"
echo "SDE_INSTALL : $SDE_INSTALL"
echo "P4BUILD_DIR : $P4BUILD_DIR"

# Generate & build
P4_PROGRAM="$P4BUILD_DIR/switch.p4"
P4C_OUTPUT="$P4BUILD_DIR/switch"

echo "-> Generating P4..."
python3 "$DPA_INSTALL/bin/dpa-p4gen" --config "$DPA_CONFIG" --output "$P4_PROGRAM" -v

echo
echo "********************************************************"
cat $P4_PROGRAM
echo "********************************************************"

echo
echo "-> Compiling for '$TOFINO_TARGET'..."
"$P4C" -b "$TOFINO_TARGET" --program-name switch --std p4_16 -o "$P4C_OUTPUT" "$P4_PROGRAM" --archive

echo "-- p4build at: $P4BUILD_DIR"
echo "-- P4 program: $P4_PROGRAM"
echo "-- P4C output: $P4C_OUTPUT"

