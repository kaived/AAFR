#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "network/telemetry/bounded_queue.h"

namespace aafr::telemetry {

struct JsonlRetention {
  // Rotate the active file once it exceeds this size.
  std::uintmax_t max_file_bytes{4 * 1024 * 1024};
  // Total files kept per session prefix, including the active one.
  int max_files{4};
  // Pending lines held in memory; further lines are dropped and counted.
  std::size_t queue_capacity{4096};
};

// Diagnostic JSONL output on a background thread. Write() never blocks the
// caller. Files rotate as <prefix>.jsonl, <prefix>.1.jsonl, ... and the oldest
// is deleted, so disk use stays below max_file_bytes * max_files (plus one line).
class JsonlTelemetryWriter {
 public:
  JsonlTelemetryWriter(std::filesystem::path directory, std::string file_prefix,
                       JsonlRetention retention = {});
  ~JsonlTelemetryWriter();

  JsonlTelemetryWriter(const JsonlTelemetryWriter&) = delete;
  JsonlTelemetryWriter& operator=(const JsonlTelemetryWriter&) = delete;

  // Queues one line (without trailing newline). Returns false if dropped.
  bool Write(std::string line);

  // Flushes queued lines and joins the worker. Safe to call more than once.
  void Close();

  bool Healthy() const { return healthy_.load(); }
  std::uint64_t Dropped() const { return queue_.Dropped(); }
  std::uint64_t Written() const { return written_.load(); }
  std::filesystem::path ActivePath() const;

 private:
  void Run();
  void OpenActive();
  void Rotate();
  std::filesystem::path PathFor(int index) const;

  const std::filesystem::path directory_;
  const std::string prefix_;
  const JsonlRetention retention_;
  BoundedQueue<std::string> queue_;
  std::ofstream stream_;
  std::uintmax_t active_bytes_{0};
  std::atomic<bool> healthy_{true};
  std::atomic<std::uint64_t> written_{0};
  std::thread worker_;
};

}  // namespace aafr::telemetry
