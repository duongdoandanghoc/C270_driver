import subprocess
import psutil
import time
import csv
import sys
import os

MODES = [
    {"name": "mode1_baseline", "cmd": ["./c270_driver", "--no-display", "--no-stream"]},
    {"name": "mode2_display", "cmd": ["./c270_driver", "--no-stream"]},
    {"name": "mode3_rtsp", "cmd": ["./c270_driver", "--no-display", "-C", "h264"]}
]

DURATION = 10  # seconds per mode
INTERVAL = 0.5 # polling interval

def run_benchmark():
    os.makedirs("results", exist_ok=True)
    
    for mode in MODES:
        name = mode["name"]
        cmd = mode["cmd"]
        csv_file = f"results/{name}.csv"
        
        print(f"Running {name}: {' '.join(cmd)}")
        
        # Launch process
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        
        try:
            p = psutil.Process(proc.pid)
        except psutil.NoSuchProcess:
            print(f"Failed to start {name}")
            continue
            
        data = []
        
        # Give it a second to initialize before recording
        time.sleep(1.0)
        start_time = time.time()
        
        while time.time() - start_time < DURATION:
            try:
                cpu = p.cpu_percent(interval=INTERVAL)
                mem = p.memory_info().rss / (1024 * 1024) # MB
                elapsed = time.time() - start_time
                data.append({"time": round(elapsed, 2), "cpu": cpu, "memory": round(mem, 2)})
            except psutil.NoSuchProcess:
                break
                
        # Terminate
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            
        # Write CSV
        with open(csv_file, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=["time", "cpu", "memory"])
            writer.writeheader()
            writer.writerows(data)
            
        print(f"Saved {len(data)} records to {csv_file}\n")

if __name__ == "__main__":
    run_benchmark()
