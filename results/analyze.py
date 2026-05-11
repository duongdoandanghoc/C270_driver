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
