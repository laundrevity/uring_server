import sys
import os
import numpy as np
import matplotlib.pyplot as plt

if len(sys.argv) != 2:
    print("Usage: python plot_histogram.py <Number of Clients>")
    sys.exit(1)

num_clients = int(sys.argv[1])

# Check if running on CI/CD or local environment
is_ci_cd = os.environ.get("GITHUB_ACTIONS", "false") == "true"
data_file_prefix = "build/" if not is_ci_cd else ""

all_latencies = {}

for i in range(num_clients):
    file_name = f"{data_file_prefix}latency_data_client_{i+1}.txt"
    with open(file_name, "r") as file:
        latencies = [int(line) for line in file.readlines()]
        all_latencies[i] = latencies

def print_statistics(stat, stat_name):
    mean = np.mean(stat)
    std_dev = np.std(stat)
    median = np.median(stat)
    tail = np.percentile(stat, 95)

    print(f"{stat_name}")
    print(f"  Mean: {mean:.2f}")
    print(f"  Standard Deviation: {std_dev:.2f}")
    print(f"  Median: {median:.2f}")
    print(f"  95th percentile (tail latency): {tail:.2f}\n")

def plot_histogram(data, title, xlabel, ylabel, filename):
    plt.hist(data, bins='auto', edgecolor='black', alpha=0.7, density=True)
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True)
    plt.savefig(filename)
    plt.clf()

median_latencies = [np.median(np.array(all_latencies[i])) for i in all_latencies]
mean_latencies = [np.mean(np.array(all_latencies[i])) for i in all_latencies]
stdev_latencies = [np.std(np.array(all_latencies[i])) for i in all_latencies]
percentile_99_99_latencies = [np.percentile(np.array(all_latencies[i]), 99.99) for i in all_latencies]
min_latencies = [np.min(np.array(all_latencies[i])) for i in all_latencies]
max_latencies = [np.max(np.array(all_latencies[i])) for i in all_latencies]

print_statistics(median_latencies, "Median Latencies Statistics")
print_statistics(mean_latencies, "Mean Latencies Statistics")
print_statistics(stdev_latencies, "Standard Deviation Latencies Statistics")
print_statistics(percentile_99_99_latencies, "99.99th Percentile Latencies Statistics")
print_statistics(min_latencies, "Minimum Latencies Statistics")
print_statistics(max_latencies, "Maximum Latencies Statistics")

plot_histogram(median_latencies, "Median Latency Histogram", r"Latency ($\mu$s)", "Frequency", "median_hist.png")
plot_histogram(mean_latencies, "Mean Latency Histogram", r"Latency ($\mu$s)", "Frequency", "mean_hist.png")
plot_histogram(stdev_latencies, "Standard Deviation Latency Histogram", r"Latency ($\mu$s)", "Frequency", "stdev_hist.png")
plot_histogram(percentile_99_99_latencies, "99.99th Percentile Latency Histogram", r"Latency ($\mu$s)", "Frequency", "percentile_99_99_hist.png")
plot_histogram(min_latencies, "Minimum Latency Histogram", r"Latency ($\mu$s)", "Frequency", "min_hist.png")
plot_histogram(max_latencies, "Maximum Latency Histogram", r"Latency ($\mu$s)", "Frequency", "max_hist.png")
