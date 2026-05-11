# Benchmark System Design for C270 Driver

## 1. Overview
The goal is to evaluate the performance of the C270 custom UVC driver across different typical operational modes without modifying the core C codebase. A Python wrapper will be used to launch the application, collect system metrics (CPU, Memory), and output data to CSV files. A separate Python script will analyze these CSVs and generate visualization charts.

## 2. Architecture & Approach
- **Data Collection (Python Wrapper):** `results/run_benchmark.py`
  - Uses `subprocess.Popen` to launch the compiled C executable (`./c270_driver`).
  - Uses `psutil` to track the process ID (PID) and its children, collecting `% CPU` and `Memory (MB)` at a fixed interval (e.g., 0.5s).
  - Also captures `stdout` to parse the FPS reported by the C driver if possible.
  - Runs the process for a fixed duration (e.g., 10 seconds per mode).
  - Gracefully terminates the process (`SIGINT`) after the duration.
  - Outputs metrics to `results/modeX_<name>.csv`.
  
- **Data Analysis & Visualization:** `results/analyze.py`
  - Reads the generated CSV files using `pandas` or built-in `csv` + `matplotlib`.
  - Generates the following charts:
    - `benchmark_cpu.png`: Line chart comparing CPU usage over time across all 3 modes.
    - `benchmark_timeline.png`: CPU and Memory stability over time (combining metrics).
    - `benchmark_cumulative.png`: Bar chart of Average CPU and Average Memory for each mode.

## 3. Benchmarking Modes
1. **Mode 1: Baseline (Headless Capture)**
   - **Command:** `./c270_driver --no-display --no-stream`
   - **Purpose:** Measure base CPU usage of USB fetching and MJPEG decoding.
2. **Mode 2: Local Monitor (Capture + Display)**
   - **Command:** `./c270_driver --no-stream`
   - **Purpose:** Measure overhead of SDL2 rendering on top of capture.
3. **Mode 3: IPC Server (Capture + RTSP Streaming H264)**
   - **Command:** `./c270_driver --no-display -C h264`
   - **Purpose:** Measure GStreamer encoding and network streaming cost.

## 4. Documentation
- **`results/README.md`**: A documentation file explaining how to run the benchmarks, the architecture of the Python wrapper, the algorithms used for process monitoring (like `psutil` polling), and explanations of the generated charts.

## 5. Expected Directory Structure Updates
```text
results/
├── run_benchmark.py
├── analyze.py
├── README.md
├── mode1_baseline.csv
├── mode2_display.csv
├── mode3_rtsp.csv
├── benchmark_cpu.png
├── benchmark_timeline.png
└── benchmark_cumulative.png
```
