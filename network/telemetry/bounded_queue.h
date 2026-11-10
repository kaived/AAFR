#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace aafr::telemetry {

// Thread-safe FIFO with a fixed capacity. Producers never block: TryPush
// rejects the item (and counts it) when the queue is full, so a slow consumer
// such as a disk writer cannot stall the GUI thread.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  bool TryPush(T item) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_ || items_.size() >= capacity_) {
        ++dropped_;
        return false;
      }
      items_.push_back(std::move(item));
    }
    ready_.notify_one();
    return true;
  }

  // Waits up to `timeout` for at least one item, then drains up to `max_items`.
  // Returns an empty vector on timeout or after Close() once drained.
  std::vector<T> PopBatch(std::size_t max_items, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, timeout, [this] { return closed_ || !items_.empty(); });
    std::vector<T> batch;
    while (!items_.empty() && batch.size() < max_items) {
      batch.push_back(std::move(items_.front()));
      items_.pop_front();
    }
    return batch;
  }

  // Rejects further pushes and wakes waiting consumers. Queued items remain
  // available to PopBatch.
  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    ready_.notify_all();
  }

  bool Closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  std::size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

  std::size_t Capacity() const { return capacity_; }

  std::uint64_t Dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<T> items_;
  std::uint64_t dropped_{0};
  bool closed_{false};
};

}  // namespace aafr::telemetry
