# Data Plane Allreduce

This repository is used as an artifact for the paper **FluxReduce: Straggle-resilient in-network aggregation for distributed ML training**

This software performs in-network allreduce on a programmable ToR switch. In addition, it incorporates straggle-resilience when used for
ML traininig, by reactively switching to fastest-K aggregation when slow workers are detected. Setup has two parts, worker(s) and switch.


<!-- #### Dependencies:
  - `python3`, `cmake >= 3.14`
  - DPDK (tested with 22.11 )
  - Any NVidia/Mellanox NIC supported by the the bifurcated `mlx5_core` driver should work (we have only tested ConnectX-6 Dx though)
    - `vfio-pci` devices should be easily supported but we haven't currently tried
 -->

## Worker setup

Requires an NVIDIA GPU with working drivers and any NVIDIA/Mellanox NIC supported by the bifurcated `mlx5_core` driver (only tested on ConnectX-6 Dx). 

`vfio-pci` devices should work but we haven't tried.

```bash
# System packages
sudo apt install -y build-essential cmake pkg-config python3-venv linux-headers-$(uname -r)

# Networking. The following was tested on Ubuntu 24.04. Check https://developer.nvidia.com/doca for your platform
wget https://www.mellanox.com/downloads/DOCA/DOCA_v3.0.0/host/doca-host_3.0.0-058000-25.04-ubuntu2404_amd64.deb
sudo dpkg -i doca-host_*.deb && sudo apt-get update && sudo apt-get -y install doca-networking
echo 'export PKG_CONFIG_PATH=/opt/mellanox/dpdk/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH' >> ~/.bashrc
source ~/.bashrc
```
For custom DPDK: `echo 'export PKG_CONFIG_PATH=/path/to/your/dpdk/lib/pkgconfig:$PKG_CONFIG_PATH' >> ~/.bashrc`

The system was evaluated with Driver Version: **560.28.03**, and CUDA Version: **12.6**. While newer ones
*should* work, we cannot guarantee compatibility, especially with the (currently) pinned Pytorch version. 
To downgrade the driver/cuda try: 
```bash
cd /tmp
wget -c https://developer.download.nvidia.com/compute/cuda/12.6.0/local_installers/cuda-repo-ubuntu2404-12-6-local_12.6.0-560.28.03-1_amd64.deb
sudo dpkg -i cuda-repo-ubuntu2404-12-6-local_12.6.0-560.28.03-1_amd64.deb
sudo cp /var/cuda-repo-ubuntu2404-12-6-local/cuda-*-keyring.gpg /usr/share/keyrings/
sudo apt-get update && sudo ubuntu-drivers install --include-dkms nvidia:560

# remove previous CUDA toolkit and install 12.6:
sudo apt purge -y nvidia-cuda-toolkit && sudo apt install -y cuda-toolkit-12-6=12.6.0-1
sudo update-alternatives --install /usr/local/cuda cuda /usr/local/cuda-12.6 126 && sudo update-alternatives --set cuda /usr/local/cuda-12.6
```
You probaly also want to export `CUDA_HOME` and put CUDA tools in path (Torch plugin only tested to compile with `CUDA_HOME`):
```bash
printf '%s\n' '# CUDA 12.6' 'export CUDA_HOME=/usr/local/cuda' 'export PATH=/usr/local/cuda/bin:$PATH' 'export LD_LIBRARY_PATH=/usr/local/cuda/lib64:${LD_LIBRARY_PATH:-}' | sudo tee /etc/profile.d/cuda.sh >/dev/null
sudo chmod 644 /etc/profile.d/cuda.sh && source /etc/profile.d/cuda.sh && hash -r
```


When you are done please run the following. If no errors, you are good to go:
```bash
./scripts/dpa-worker-check.sh
```

## Build `libdpa` for standalone use
```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
make install # optional, will create a clean tree under build/install
```

## Build PyTorch plugin (includes `libdpa`)

```bash
python -m venv env
source env/bin/activate
pip install -r src/dpa_torch_plugin/requirements.txt
mkdir -p build && cd build
make torch-plugin # builds and install plugin in the python env
```

## Swith setup
This step requires the Intel Tofino SDE is already installed and env `$SDE` and `$SDE_INSTALL` are properly configured.

Compiling the program can be done on any machine with a Tofino SDE.
Configuring the switch can only be done **ON** the switch (because we currently only support a CLI controller).

You will also need a `config.json` file. You will find some under [configs](./configs/). 

```bash
cd <PATH_TO_REPO>/build
cmake -DDPA_HOST=OFF -DDPA_SWITCH .. && make install

# build the P4 program
../scripts/dpa-switch-compile.sh ../configs/edgecore.json install $SDE_INSTALL p4build

# start the switch and run the controller once. Switch logs written at p4build/log
../scripts/dpa-switch-start.sh ../configs/edgecore.json install $SDE_INSTALL p4build
```

We have only tested the program on Netberg Aurora 710 (Tofino1, 2-pipe) and Edgecore DCS810 (Tofino2,4-pipe) but it should be easy to
write a `config.json` for others too.

We cannot assist you in obtaining an Intel Tofino SDE. Please reach out to [Intel](https://community.intel.com/t5/Intel-Connectivity-Research/cmp-p/grouphub:connectivity-research-program).
Some more info can be found at [p4lang/open-p4studio](https://github.com/p4lang/open-p4studio)



## Environment Variables

Runtime behavior of DPA and the torch plugin can be controlled by environment variables. We support the following:

**`DPA_TORCH_MODE`** — selects the allreduce work implementation.
- `worksteal` *(default)*  — pool of workers pull from a shared queue; overlap across ops.
- `pipeline` — chunks one op across D2H/network/H2D streams for intra-op overlap.
- `hybrid` — one persistent worker, picks throughput or latency mode per op based on size.
- `simple` — calling thread runs D2H + submit inline; no overlap.

**`DPA_TORCH_PIPELINE_CHUNKS`** *(default: 4)* — number of chunks used in pipeline and hybrid modes. More chunks → finer-grained overlap but higher per-chunk overhead.

**`DPA_TORCH_PIPELINE_THRESH`** *(default: 4194304)* — byte threshold for hybrid and pipeline mode. Ops below this size use throughput are not chunked

**`DPA_LOG`** — (default: `info`) select the logging level. `error, warn, info, debug, trace` → Each level implies also the levels left of it

**`DPA_PREEMPTIVE`** — (default: `OFF`) enable preemptive fastest-k instead of reactive. Preemptive always produces a result out of the first K workers to arrive, even if no straggler is detected.

**`DPA_SYN_DISABLE`** — (default: `OFF`) disable SYN packets. This usually causes an extra RTT for straggling workers, on average.

**`DPA_SCHEDULER`** — (default: `OFF`) enable the scheduler thread in `dpa::Context`. When `OFF` caller itself pushes to backend workers

**`DPA_DPDK_MONITOR`** — (default: `OFF`) monitor the NICs rx and tx bytes

**`DPA_DPDK_MONITOR_INTERVAL_US`** — (default: `100`) amount of `us` to wait before polling the NIC's stats

**`DPA_DPDK_MONITOR_OUTPUT`** — (default: `/tmp/nic_throughput.csv`) file to write monitor results

## TODOs

1. Add back the su packet loop
2. Add back the socket backend
3. Add back the remaining unit tests
4. Example usage code
5. Other docs
6. `htonv` and `ntohv` simd threshold
