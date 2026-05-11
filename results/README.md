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
