# DPA Switch Control Plane Library

This library provides a Python API for configuring DPA switches, extracted from the bfrt-cli.py functionality for reusable components.

## Usage

### Installation

The library is automatically installed when you build the project with CMake:

```bash
# Build the project
cd straggle-ml
mkdir -p build && cd build
cmake .. && make install
```

This installs the library to `build/install/lib/dpa_switch_controlplane/`.

### Setup Python Path

#### Option 1: Use the provided setup script

```bash
# Source the setup script to add to PYTHONPATH
source build/install/bin/setup_dpa_controlplane_path.sh
```

#### Option 2: Manual Python path setup

Add the library to your Python path in your BFRT script:

```python
import sys
sys.path.append('/path/to/straggle-ml/build/install/lib')

# Now you can import the library
from dpa_switch_controlplane import setup_switch
```

### Basic Usage

```python
# In your BFRT script
from dpa_switch_controlplane import setup_switch

# Quick setup - configures both network and DPA
cp = setup_switch('config.json')
```

### Manual Control

```python
from dpa_switch_controlplane.cli import BfrtCLIController

# Create controller
cp = BfrtCLIController('config.json')

# Setup only network forwarding
cp.setup_network()

# Setup only DPA sessions
cp.setup_dpa()

# Or setup everything
cp.setup_all()
```

### Configuration

The library expects a JSON configuration file with the same format as used in the original bfrt-cli.py:

```json
{
  "hosts": {
    "h1": { "mac": "42:00:00:00:00:01", "ip": "42.0.0.1" },
    "h2": { "mac": "42:00:00:00:00:02", "ip": "42.0.0.2" }
  },
  "switch": {
    "name": "mininet-tofino",
    "mac": "42:00:00:00:00:00",
    "ip": "42.0.0.0",
    "flood_mgid": 1,
    "ports": {
      "1/0": { "host": "h1" },
      "2/0": { "host": "h2" }
    },
    "sessions": {
      "main": {
        "id": 1,
        "ports": ["1/0", "2/0"],
        "straggle-timeout": 2,
        "mgid": 2,
        "dropsim": { "ingress": 0.0, "egress": 0.0 }
      }
    }
  },
  "dpa": {
    "comm": "udp"
  }
}
```

### Installation Location

After building, the library will be installed to:
- Library files: `build/install/lib/dpa_switch_controlplane/`
- Setup script: `build/install/bin/setup_dpa_controlplane_path.sh`

### Requirements

- This library must be run within the BFRT shell context (where `bfrt` global variable is available)
- Requires `netaddr` package for MAC/IP address handling

### Functions

- `setup_switch(config_path)` - Complete switch setup (network + DPA)
- `setup_network_only(config_path)` - Only network forwarding setup
- `setup_dpa_only(config_path)` - Only DPA session setup
- `PORT(port_string)` - Convert port string to device port number
- `TSC(milliseconds)` - Convert milliseconds to Tofino clock ticks

### API Classes

- `BfrtCLIController` - Main controller class with all setup methods
- `DPAConfig` - Configuration management utility
- `TofinoModel` - Hardware model definitions and port calculations