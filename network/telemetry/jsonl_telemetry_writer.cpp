#include "network/telemetry/jsonl_telemetry_writer.h"

#include <system_error>
#include <utility>
#include <vector>

namespace aafr::telemetry {

JsonlTelemetryWriter::JsonlTelemetryWriter(std::filesystem::path directory,
                                           std::string file_prefix,
                                           JsonlRetention retention)
    : directory_(std::move(directory)),
      prefix_(std::move(file_prefix)),
      retention_([&retention] {
        JsonlRetention fixed = retention;
        if (fixed.max_files < 1) fixed.max_files = 1;
        if (fixed.max_file_bytes < 1024) fixed.max_file_bytes = 1024;
        return fixed;
      }()),
      queue_(retention_.queue_capacity) {
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  OpenActive();
  worker_ = std::thread([this] { Run(); });
}

JsonlTelemetryWriter::~JsonlTelemetryWriter() { Close(); }

bool JsonlTelemetryWriter::Write(std::string line) {
  if (!healthy_.load()) return false;
  return queue_.TryPush(std::move(line));
}

void JsonlTelemetryWriter::Close() {
  queue_.Close();
  if (worker_.joinable()) worker_.join();
  if (stream_.is_open()) stream_.close();
}

std::filesystem::path JsonlTelemetryWriter::ActivePath() const { return PathFor(0); }

std::filesystem::path JsonlTelemetryWriter::PathFor(int index) const {
  if (index == 0) return directory_ / (prefix_ + ".jsonl");
  return directory_ / (prefix_ + "." + std::to_string(index) + ".jsonl");
}

void JsonlTelemetryWriter::OpenActive() {
  stream_.open(PathFor(0), std::ios::out | std::ios::app | std::ios::binary);
  if (!stream_) {
    healthy_ = false;
    return;
  }
  std::error_code error;
  const auto size = std::filesystem::file_size(PathFor(0), error);
  active_bytes_ = error ? 0 : size;
}

void JsonlTelemetryWriter::Rotate() {
  stream_.close();
  std::error_code error;
  // Drop the oldest, then shift every file up by one index.
  std::filesystem::remove(PathFor(retention_.max_files - 1), error);
  for (int index = retention_.max_files - 2; index >= 0; --index) {
    if (std::filesystem::exists(PathFor(index), error)) {
      std::filesystem::rename(PathFor(index), PathFor(index + 1), error);
    }
  }
  if (retention_.max_files == 1) std::filesystem::remove(PathFor(0), error);
  active_bytes_ = 0;
  OpenActive();
}

void JsonlTelemetryWriter::Run() {
  for (;;) {
    std::vector<std::string> batch =
        queue_.PopBatch(256, std::chrono::milliseconds(250));
    if (batch.empty()) {
      if (queue_.Closed() && queue_.Size() == 0) break;
      continue;
    }
    if (!healthy_.load()) continue;
    for (std::string& line : batch) {
      if (active_bytes_ > 0 && active_bytes_ + line.size() + 1 > retention_.max_file_bytes) {
        Rotate();
        if (!healthy_.load()) break;
      }
      stream_ << line << '\n';
      active_bytes_ += line.size() + 1;
      ++written_;
    }
    stream_.flush();
    if (!stream_) healthy_ = false;
  }
}

}  // namespace aafr::telemetry
