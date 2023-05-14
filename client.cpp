#include <iostream>
#include <chrono>
#include <thread>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <thread>
#include <charconv>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <latch>
#include <fstream>

constexpr int BUFFER_SIZE = sizeof(int64_t);

struct LatencyStats {
  int64_t min = std::numeric_limits<int64_t>::max();
  int64_t max = std::numeric_limits<int64_t>::min();
  double average = 0;
  int64_t median = 0;
  double stdev = 0;
  int64_t percentile_99_9 = 0;
  int64_t percentile_99_99 = 0;
};

// Calculate latency statistics (min, max, avg, median, stdev, percentiles)
LatencyStats calculate_latency_stats(const std::vector<int64_t>& latencies) {
  LatencyStats stats;
  size_t n = latencies.size();

  // Calculate min, max, and average
  double sum = 0;
  for (const auto& latency : latencies) {
    stats.min = std::min(stats.min, latency);
    stats.max = std::max(stats.max, latency);
    sum += latency;
  }
  stats.average = sum / n;

  // Calculate median
  std::vector<int64_t> sorted_latencies = latencies;
  std::sort(sorted_latencies.begin(), sorted_latencies.end());
  if (n % 2 == 1) {
    stats.median = sorted_latencies[n / 2];
  } else {
    stats.median = (sorted_latencies[n / 2] + sorted_latencies[(n / 2) - 1]) / 2;
  }

  // Calculate standard deviation
  double squared_diff_sum = 0;
  for (const auto& latency : latencies) {
    squared_diff_sum += std::pow(latency - stats.average, 2);
  }
  stats.stdev = std::sqrt(squared_diff_sum / n);

  // Calculate percentiles
  size_t idx_99_9 = static_cast<size_t>(std::ceil(0.999 * n)) - 1;
  size_t idx_99_99 = static_cast<size_t>(std::ceil(0.9999 * n)) - 1;
  stats.percentile_99_9 = sorted_latencies[idx_99_9];
  stats.percentile_99_99 = sorted_latencies[idx_99_99];

  return stats;
}

void print_latency_stats(const LatencyStats& stats, int client_id) {
  std::cout << "Client " << client_id << " latency statistics:\n"
            << "  Min: " << stats.min << " us\n"
            << "  Max: " << stats.max << " us\n"
            << "  Average: " << stats.average << " us\n"
            << "  Median: " << stats.median << " us\n"
            << "  Standard Deviation: " << stats.stdev << " us\n"
            << "  99.9th Percentile: " << stats.percentile_99_9 << " us\n"
            << "  99.99th Percentile: " << stats.percentile_99_99 << " us\n";
}

void client(int client_id, const char* ip, int port, int num_messages,LatencyStats& client_stats) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return;
  }

  sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = inet_addr(ip);


  if (connect(sockfd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    perror("connect");
    close(sockfd);
    return;
  }

  std::vector<int64_t> latencies;
  latencies.resize(num_messages);

  for (int i = 0; i < num_messages; ++i) {
    char buffer[BUFFER_SIZE];

    int received = recv(sockfd, buffer, BUFFER_SIZE, 0);

    if (received <= 0) {
      if (received < 0 && errno == EAGAIN) {
        --i;
        std::this_thread::yield();
        continue;
      }
      perror("recv");
      break;
    }

//    std::cout << "Bytes received: " << received << ", size of server_timestamp: " << sizeof(int64_t) << std::endl;

    int64_t server_timestamp;
    if (received == sizeof(server_timestamp)) {
      memcpy(&server_timestamp, buffer, received);

      auto now = std::chrono::steady_clock::now();
      auto now_us = std::chrono::time_point_cast<std::chrono::microseconds >(now);
      auto since_epoch_us = now_us.time_since_epoch();
      auto since_epoch_us_count = since_epoch_us.count();
      int64_t latency = since_epoch_us_count - server_timestamp;
      std::chrono::microseconds latency_micros(latency);
      latencies[i] = (std::chrono::duration_cast<std::chrono::microseconds>(latency_micros).count());
    }
  }

  close(sockfd);

  // Output raw latencies to a text file
  std::ofstream latency_data_file("build/latency_data_client_" + std::to_string(client_id) + ".txt");
  for (const auto& latency : latencies) {
    latency_data_file << latency << '\n';
  }
  latency_data_file.close();

  LatencyStats stats = calculate_latency_stats(latencies);

  client_stats = stats;
}

int main(int argc, char* argv[]) {
  if (argc != 5) {
    std::cerr << "Usage: " << argv[0] << " <IP> <Port> <Number of Clients> <Number of Messages per Client>" << std::endl;
    return 1;
  }

  const char* ip = argv[1];
  int port = std::stoi(argv[2]);
  int num_clients = std::stoi(argv[3]);
  int num_messages = std::stoi(argv[4]);

  std::vector<std::thread> clients; clients.reserve(num_clients);
  std::vector<LatencyStats> client_stats(num_clients);
//  std::latch start_latch(num_clients);

  for (int i = 0; i < num_clients; ++i) {
    clients.emplace_back(
            client, i+1, ip, port, num_messages, /*std::ref(start_latch),*/ std::ref(client_stats[i]));
  }

  for (auto& thread : clients) {
    thread.join();
  }

  for (int i = 0; i < num_clients; ++i) {
    print_latency_stats(client_stats[i], i + 1);
  }


  return 0;
}