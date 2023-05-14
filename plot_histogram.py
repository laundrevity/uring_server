import sys
import numpy as np
import matplotlib.pyplot as plt

if len(sys.argv) != 2:
    print("Usage: python plot_histogram.py <Number of Clients>")
    sys.exit(1)

num_clients = int(sys.argv[1])

all_latencies = {}

for i in range(num_clients):
    file_name = f"build/latency_data_client_{i+1}.txt"
    with open(file_name, "r") as file:
        latencies = [int(line) for line in file.readlines()]
        all_latencies[i] = latencies

def plot_histogram(data, title, xlabel, ylabel, filename):
    plt.hist(data, bins='auto', edgecolor='black', alpha=0.7)
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

plot_histogram(median_latencies, "Median Latency Histogram", "Latency (ms)", "Frequency", "median_hist.png")
plot_histogram(mean_latencies, "Mean Latency Histogram", "Latency (ms)", "Frequency", "mean_hist.png")
plot_histogram(stdev_latencies, "Standard Deviation Latency Histogram", "Latency (ms)", "Frequency", "stdev_hist.png")
plot_histogram(percentile_99_99_latencies, "99.99th Percentile Latency Histogram", "Latency (ms)", "Frequency", "percentile_99_99_hist.png")
plot_histogram(min_latencies, "Minimum Latency Histogram", "Latency (ms)", "Frequency", "min_hist.png")
plot_histogram(max_latencies, "Maximum Latency Histogram", "Latency (ms)", "Frequency", "max_hist.png")
