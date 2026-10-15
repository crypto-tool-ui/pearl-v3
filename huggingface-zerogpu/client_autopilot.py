#!/usr/bin/env python3
"""
ZeroGPU Auto-Pilot Daemon
Connects to your Hugging Face ZeroGPU Space and executes continuous real FlashAttention evaluation bursts.
"""
import sys
import time
import argparse

try:
    from gradio_client import Client
except ImportError:
    print("[ERROR] Please install gradio_client: pip install gradio_client")
    sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description="ZeroGPU Tensor Acceleration Auto-Pilot Daemon")
    parser.add_argument("--space", type=str, required=True, help="Hugging Face Space name (e.g. username/distributed-torch-engine)")
    parser.add_argument("--token", type=str, default="", help="Hugging Face User Access Token (optional, for higher quota)")
    parser.add_argument("--sweeps", type=int, default=30, help="Number of matrix sweeps per burst (default: 30)")
    parser.add_argument("--cooldown", type=int, default=3, help="Rest seconds between bursts (default: 3s)")
    args = parser.parse_args()

    print(f"[*] Connecting to Hugging Face Space: {args.space}...")
    hf_token = args.token if args.token else None
    client = Client(args.space, hf_token=hf_token)

    burst_count = 0
    total_shares = 0
    print(f"[✓] Successfully connected! Starting autonomous ZeroGPU compute loop...\n")

    while True:
        burst_count += 1
        print(f"[*] Triggering Quantum Burst #{burst_count} on NVIDIA A100 / RTX 6000 ({args.sweeps} sweeps)...", flush=True)
        t0 = time.time()
        try:
            result = client.predict(
                sweeps=args.sweeps,
                api_name="/run_quantum_burst"
            )
            elapsed = time.time() - t0
            print(f"[+] Burst #{burst_count} completed in {elapsed:.1f}s!")
            print(f"    Summary: {result}\n", flush=True)
            time.sleep(args.cooldown)
        except Exception as e:
            elapsed = time.time() - t0
            err_msg = str(e)
            if "quota" in err_msg.lower():
                print(f"[!] Quota limit reached ({err_msg}). Waiting 45s for quota recharge...", flush=True)
                time.sleep(45)
            else:
                print(f"[!] Warning on burst #{burst_count} ({err_msg}). Retrying in 10s...", flush=True)
                time.sleep(10)

if __name__ == "__main__":
    main()
