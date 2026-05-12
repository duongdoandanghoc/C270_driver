# Benchmark System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Python-based benchmarking suite to measure and visualize CPU/Memory usage of the C270 driver across different execution modes.

**Architecture:** A wrapper script (`run_benchmark.py`) will launch the C executable and poll `psutil` to collect system metrics, saving to CSV. An analysis script (`analyze.py`) will process these CSVs using `matplotlib` to generate charts. `README.md` documents usage.

**Tech Stack:** Python 3, `psutil`, `matplotlib`, `csv` (built-in).

---

### Task 1: Create `results/run_benchmark.py`

**Files:**
- Create: `results/run_benchmark.py`
- Modify: None
- Test: Manual execution test

- [ ] **Step 1: Write the python wrapper script**

```python
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
```

- [ ] **Step 2: Commit**

```bash
git add results/run_benchmark.py
git commit -m "test: add run_benchmark.py script"
```

---

### Task 2: Create `results/analyze.py`

**Files:**
- Create: `results/analyze.py`
- Modify: None
- Test: Run script on generated CSVs

- [ ] **Step 1: Write the analysis script**

```python
import csv
import os
import matplotlib.pyplot as plt

def load_data(filename):
    if not os.path.exists(filename):
        return []
    with open(filename, 'r') as f:
        reader = csv.DictReader(f)
        return [{"time": float(r["time"]), "cpu": float(r["cpu"]), "memory": float(r["memory"])} for r in reader]

def main():
    modes = ["mode1_baseline", "mode2_display", "mode3_rtsp"]
    datasets = {m: load_data(f"results/{m}.csv") for m in modes}
    
    # 1. CPU Comparison
    plt.figure(figsize=(10, 5))
    for m in modes:
        if datasets[m]:
            times = [d["time"] for d in datasets[m]]
            cpus = [d["cpu"] for d in datasets[m]]
            plt.plot(times, cpus, label=m)
    plt.title("CPU Usage Over Time")
    plt.xlabel("Time (s)")
    plt.ylabel("CPU %")
    plt.legend()
    plt.grid(True)
    plt.savefig("results/benchmark_cpu.png")
    plt.close()
    
    # 2. Timeline (CPU and Mem combined)
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8))
    for m in modes:
        if datasets[m]:
            times = [d["time"] for d in datasets[m]]
            ax1.plot(times, [d["cpu"] for d in datasets[m]], label=m)
            ax2.plot(times, [d["memory"] for d in datasets[m]], label=m)
    
    ax1.set_title("CPU Stability")
    ax1.set_ylabel("CPU %")
    ax1.grid(True)
    ax1.legend()
    
    ax2.set_title("Memory Stability")
    ax2.set_ylabel("Memory (MB)")
    ax2.set_xlabel("Time (s)")
    ax2.grid(True)
    
    plt.tight_layout()
    plt.savefig("results/benchmark_timeline.png")
    plt.close()
    
    # 3. Cumulative Averages
    avg_cpu = []
    avg_mem = []
    valid_modes = []
    
    for m in modes:
        if datasets[m] and len(datasets[m]) > 0:
            avg_cpu.append(sum(d["cpu"] for d in datasets[m]) / len(datasets[m]))
            avg_mem.append(sum(d["memory"] for d in datasets[m]) / len(datasets[m]))
            valid_modes.append(m)
            
    if valid_modes:
        fig, ax1 = plt.subplots(figsize=(8, 5))
        ax2 = ax1.twinx()
        
        x = range(len(valid_modes))
        ax1.bar([i - 0.2 for i in x], avg_cpu, 0.4, color='b', alpha=0.7, label='Avg CPU %')
        ax2.bar([i + 0.2 for i in x], avg_mem, 0.4, color='g', alpha=0.7, label='Avg Memory (MB)')
        
        ax1.set_ylabel('CPU %', color='b')
        ax2.set_ylabel('Memory (MB)', color='g')
        ax1.set_xticks(x)
        ax1.set_xticklabels(valid_modes)
        plt.title('Cumulative Averages')
        fig.legend(loc="upper left", bbox_to_anchor=(0.1, 0.9))
        
        plt.savefig("results/benchmark_cumulative.png")
        plt.close()
        print("Generated benchmark_cpu.png, benchmark_timeline.png, benchmark_cumulative.png")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Commit**

```bash
git add results/analyze.py
git commit -m "test: add analyze.py script for chart generation"
```

---

### Task 3: Create `results/README.md`

**Files:**
- Create: `results/README.md`
- Modify: None
- Test: View markdown

- [ ] **Step 1: Write README**

```markdown
# C270 Driver Benchmarks

This folder contains the automated benchmarking suite to evaluate the performance of the custom C270 driver.

## Execution

1. **Ensure dependencies are installed:**
   ```bash
   pip3 install psutil matplotlib pandas
   ```
2. **Run the benchmarks:**
   ```bash
   python3 results/run_benchmark.py
   ```
   *This will run `./c270_driver` in 3 different modes, measuring CPU and Memory every 0.5s via `psutil`. It saves the raw data to CSV files.*
3. **Generate charts:**
   ```bash
   python3 results/analyze.py
   ```
   *This reads the CSV files and outputs three PNG charts.*

## Methodology

- **Wrapper Approach:** We use `psutil` externally instead of logging from within the C code. This ensures our measurements don't introduce overhead (file I/O) into the critical video processing loop.
- **Metrics:** `psutil` reads `/proc/<pid>/stat` to calculate exact CPU percentage for the process over the polling interval, and `/proc/<pid>/statm` for RSS memory footprint.

## Modes
1. **mode1_baseline**: Headless Capture (`--no-display --no-stream`). Tests decoding cost.
2. **mode2_display**: Local Monitor (`--no-stream`). Tests SDL rendering overhead.
3. **mode3_rtsp**: IPC Server (`--no-display -C h264`). Tests H264 encode and RTSP stream cost.

## Output Files
- `benchmark_cpu.png`: Tracks CPU utilization over the 10-second run.
- `benchmark_timeline.png`: Side-by-side stability view of CPU and Memory.
- `benchmark_cumulative.png`: Bar chart comparing average CPU and Memory between the modes.
```

- [ ] **Step 2: Commit**

```bash
git add results/README.md
git commit -m "docs: add benchmarking readme"
```
