#pragma once

// TShark "-T fields" output parsing and per-interval aggregation. Plain C++17
// so it can be tested with deterministic fixtures and no capture driver.
//
// Only protocol metadata fields are requested. Packet payloads, IP addresses
// and ports are never requested from TShark, so they never enter AAFR.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "network/telemetry/telemetry_event.h"

namespace aafr::tshark {

// Field order for `tshark -T fields`; the parser depends on it.
const std::vector<std::string>& RequestedFields();

// Arguments after the executable, for a live capture. `interface_name` empty
// lets TShark choose its default interface.
std::vector<std::string> BuildCaptureArguments(const std::string& interface_name,
                                               const std::string& capture_filter);

struct PacketObservation {
  double epoch_seconds{0.0};
  std::uint64_t frame_bytes{0};
  bool tcp{false};
  bool udp{false};
  bool quic{false};
  bool dns{false};
  bool tcp_retransmission{false};
  std::optional<double> tcp_ack_rtt_seconds;
  bool dns_response{false};
  std::optional<double> dns_response_time_seconds;
  std::string quic_version;
};

// Parses one tab-separated line. Returns nullopt for blank or malformed lines
// (for example TShark status text that reached stdout).
std::optional<PacketObservation> ParseFieldsLine(const std::string& line);

struct SampleStats {
  std::uint64_t count{0};
  double min{0.0};
  double max{0.0};
  double sum{0.0};
  double latest{0.0};

  void Add(double value);
  double Mean() const { return count == 0 ? 0.0 : sum / static_cast<double>(count); }
};

struct IntervalSummary {
  std::uint64_t packets{0};
  std::uint64_t bytes{0};
  std::uint64_t tcp_packets{0};
  std::uint64_t udp_packets{0};
  std::uint64_t quic_packets{0};
  std::uint64_t dns_responses{0};
  std::uint64_t tcp_retransmissions{0};
  std::uint64_t malformed_lines{0};
  SampleStats tcp_ack_rtt_ms;
  SampleStats dns_response_time_ms;
  std::set<std::string> quic_versions;
  // Totals since the capture started.
  std::uint64_t total_packets{0};
  std::uint64_t total_bytes{0};
};

// Accumulates observations between flushes. The monitor flushes on a timer,
// so the UI and the pipeline see one summary per interval rather than one
// event per packet.
class IntervalAggregator {
 public:
  void AddLine(const std::string& line);
  void Add(const PacketObservation& observation);
  // Returns the current interval and starts a new one. Totals carry over.
  IntervalSummary Flush();
  bool Empty() const { return current_.packets == 0 && current_.malformed_lines == 0; }

 private:
  IntervalSummary current_;
  std::uint64_t total_packets_{0};
  std::uint64_t total_bytes_{0};
};

// Converts a summary to unstamped NetworkMetric events. Metrics without
// samples in the interval (RTT, DNS timing) are omitted, never zero-filled.
std::vector<telemetry::TelemetryEvent> ToTelemetryEvents(const IntervalSummary& summary,
                                                         double interval_ms);

// Metric names emitted by ToTelemetryEvents, in display order.
const std::vector<std::string>& NetworkMetricNames();

}  // namespace aafr::tshark
