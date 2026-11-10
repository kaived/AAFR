#include "network/tshark/tshark_fields.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace aafr::tshark {
namespace {

enum Field {
  kTimeEpoch = 0,
  kFrameLen,
  kProtocols,
  kTcpAckRtt,
  kTcpRetransmission,
  kDnsResponseFlag,
  kDnsTime,
  kQuicVersion,
  kFieldCount
};

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : line) {
    if (ch == '\t') {
      parts.push_back(current);
      current.clear();
    } else if (ch != '\r' && ch != '\n') {
      current += ch;
    }
  }
  parts.push_back(current);
  return parts;
}

std::optional<double> ParseDouble(const std::string& text) {
  if (text.empty()) return std::nullopt;
  // Fields may repeat when a frame carries several values; use the first.
  const std::string first = text.substr(0, text.find(','));
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(first.c_str(), &end);
  if (errno != 0 || end == first.c_str() || *end != '\0' || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

bool ParseFlag(const std::string& text) {
  // TShark prints "1" for present FT_NONE fields and 1/True for set booleans.
  return text == "1" || text == "True" || text == "true";
}

bool HasProtocol(const std::string& protocols, const char* name) {
  std::istringstream stream(protocols);
  std::string token;
  while (std::getline(stream, token, ':')) {
    if (token == name) return true;
  }
  return false;
}

telemetry::TelemetryEvent MetricEvent(const std::string& metric, double value,
                                      const std::string& unit, double interval_ms) {
  telemetry::TelemetryEvent event;
  event.type = telemetry::EventType::NetworkMetric;
  event.source = telemetry::Source::TShark;
  event.scope = telemetry::MeasurementScope::CaptureInterface;
  event.attribution = telemetry::AttributionQuality::Unattributed;
  event.availability = telemetry::Availability::Available;
  event.metric = metric;
  event.value = value;
  event.unit = unit;
  std::ostringstream interval;
  interval << interval_ms;
  event.attributes.push_back({"interval_ms", interval.str()});
  return event;
}

}  // namespace

const std::vector<std::string>& RequestedFields() {
  static const std::vector<std::string> fields = {
      "frame.time_epoch",          "frame.len",
      "frame.protocols",           "tcp.analysis.ack_rtt",
      "tcp.analysis.retransmission", "dns.flags.response",
      "dns.time",                  "quic.version",
  };
  return fields;
}

std::vector<std::string> BuildCaptureArguments(const std::string& interface_name,
                                               const std::string& capture_filter) {
  std::vector<std::string> args;
  if (!interface_name.empty()) {
    args.push_back("-i");
    args.push_back(interface_name);
  }
  if (!capture_filter.empty()) {
    args.push_back("-f");
    args.push_back(capture_filter);
  }
  // -l: flush per packet; -n: no name resolution; -Q: errors only on stderr.
  for (const char* fixed : {"-l", "-n", "-Q", "-T", "fields", "-E", "separator=/t", "-E",
                            "occurrence=f"}) {
    args.push_back(fixed);
  }
  for (const std::string& field : RequestedFields()) {
    args.push_back("-e");
    args.push_back(field);
  }
  return args;
}

std::optional<PacketObservation> ParseFieldsLine(const std::string& line) {
  const std::vector<std::string> parts = SplitTabs(line);
  if (parts.size() != kFieldCount) return std::nullopt;
  const auto epoch = ParseDouble(parts[kTimeEpoch]);
  const auto length = ParseDouble(parts[kFrameLen]);
  if (!epoch || !length || *length < 0) return std::nullopt;

  PacketObservation observation;
  observation.epoch_seconds = *epoch;
  observation.frame_bytes = static_cast<std::uint64_t>(*length);
  const std::string& protocols = parts[kProtocols];
  observation.tcp = HasProtocol(protocols, "tcp");
  observation.udp = HasProtocol(protocols, "udp");
  observation.quic = HasProtocol(protocols, "quic");
  observation.dns = HasProtocol(protocols, "dns");
  observation.tcp_retransmission = ParseFlag(parts[kTcpRetransmission]);
  observation.tcp_ack_rtt_seconds = ParseDouble(parts[kTcpAckRtt]);
  observation.dns_response = ParseFlag(parts[kDnsResponseFlag]);
  observation.dns_response_time_seconds = ParseDouble(parts[kDnsTime]);
  observation.quic_version = parts[kQuicVersion];
  if (observation.tcp_ack_rtt_seconds && *observation.tcp_ack_rtt_seconds < 0) {
    observation.tcp_ack_rtt_seconds.reset();
  }
  if (observation.dns_response_time_seconds && *observation.dns_response_time_seconds < 0) {
    observation.dns_response_time_seconds.reset();
  }
  return observation;
}

void SampleStats::Add(double value) {
  if (count == 0) {
    min = max = value;
  } else {
    min = std::min(min, value);
    max = std::max(max, value);
  }
  sum += value;
  latest = value;
  ++count;
}

void IntervalAggregator::AddLine(const std::string& line) {
  if (line.find_first_not_of(" \t\r\n") == std::string::npos) return;
  if (const auto observation = ParseFieldsLine(line)) {
    Add(*observation);
  } else {
    ++current_.malformed_lines;
  }
}

void IntervalAggregator::Add(const PacketObservation& observation) {
  ++current_.packets;
  current_.bytes += observation.frame_bytes;
  ++total_packets_;
  total_bytes_ += observation.frame_bytes;
  if (observation.tcp) ++current_.tcp_packets;
  if (observation.udp) ++current_.udp_packets;
  if (observation.quic) {
    ++current_.quic_packets;
    if (!observation.quic_version.empty()) current_.quic_versions.insert(observation.quic_version);
  }
  if (observation.tcp_retransmission) ++current_.tcp_retransmissions;
  if (observation.tcp_ack_rtt_seconds) current_.tcp_ack_rtt_ms.Add(*observation.tcp_ack_rtt_seconds * 1000.0);
  if (observation.dns && observation.dns_response) ++current_.dns_responses;
  if (observation.dns_response_time_seconds) {
    current_.dns_response_time_ms.Add(*observation.dns_response_time_seconds * 1000.0);
  }
}

IntervalSummary IntervalAggregator::Flush() {
  IntervalSummary summary = current_;
  summary.total_packets = total_packets_;
  summary.total_bytes = total_bytes_;
  current_ = IntervalSummary{};
  return summary;
}

namespace {
std::string Trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::string Loose(const std::string& text) {
  std::string out;
  for (const char ch : text) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
  }
  return out;
}

bool SameIgnoringCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}
}  // namespace

std::vector<CaptureInterface> ParseInterfaceList(const std::string& text) {
  std::vector<CaptureInterface> interfaces;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    line = Trim(line);
    const auto dot = line.find(". ");
    if (dot == std::string::npos || dot == 0) continue;
    const std::string number = line.substr(0, dot);
    if (number.find_first_not_of("0123456789") != std::string::npos) continue;
    std::string rest = Trim(line.substr(dot + 2));
    CaptureInterface entry;
    entry.number = number;
    // The friendly name, when present, is the final parenthesised part. It
    // may contain brackets itself ("Event Tracing for Windows (ETW) reader"),
    // so find the "(" that matches the final ")".
    if (!rest.empty() && rest.back() == ')') {
      int depth = 0;
      std::size_t open = std::string::npos;
      for (std::size_t i = rest.size(); i-- > 0;) {
        if (rest[i] == ')') ++depth;
        else if (rest[i] == '(' && --depth == 0) {
          open = i;
          break;
        }
      }
      if (open != std::string::npos && open > 0 && rest[open - 1] == ' ') {
        entry.name = rest.substr(open + 1, rest.size() - open - 2);
        rest = Trim(rest.substr(0, open));
      }
    }
    entry.device = rest;
    if (entry.name.empty()) entry.name = entry.device;
    if (!entry.device.empty()) interfaces.push_back(std::move(entry));
  }
  return interfaces;
}

std::optional<CaptureInterface> ResolveInterface(const std::string& input,
                                                 const std::vector<CaptureInterface>& interfaces) {
  const std::string wanted = Trim(input);
  if (wanted.empty()) return std::nullopt;
  for (const CaptureInterface& entry : interfaces) {
    if (entry.number == wanted || SameIgnoringCase(entry.device, wanted) ||
        SameIgnoringCase(entry.name, wanted)) {
      return entry;
    }
  }
  const std::string loose = Loose(wanted);
  if (loose.empty()) return std::nullopt;
  std::optional<CaptureInterface> match;
  for (const CaptureInterface& entry : interfaces) {
    if (Loose(entry.name) != loose) continue;
    if (match) return std::nullopt;  // Ambiguous.
    match = entry;
  }
  return match;
}

const std::vector<std::string>& NetworkMetricNames() {
  static const std::vector<std::string> names = {
      "net.tcp.ack_rtt.latest",       "net.tcp.ack_rtt.min",
      "net.tcp.ack_rtt.mean",         "net.tcp.ack_rtt.max",
      "net.tcp.retransmissions",      "net.dns.response_time.mean",
      "net.dns.response_time.max",    "net.dns.responses",
      "net.packets",                  "net.bytes",
      "net.tcp.packets",              "net.udp.packets",
      "net.quic.packets",             "net.capture.packets_total",
      "net.capture.bytes_total",
  };
  return names;
}

std::vector<telemetry::TelemetryEvent> ToTelemetryEvents(const IntervalSummary& summary,
                                                         double interval_ms) {
  std::vector<telemetry::TelemetryEvent> events;
  auto add = [&](const char* metric, double value, const char* unit) {
    events.push_back(MetricEvent(metric, value, unit, interval_ms));
    return &events.back();
  };
  if (summary.tcp_ack_rtt_ms.count > 0) {
    const std::string samples = std::to_string(summary.tcp_ack_rtt_ms.count);
    add("net.tcp.ack_rtt.latest", summary.tcp_ack_rtt_ms.latest, "ms")->attributes.push_back({"samples", samples});
    add("net.tcp.ack_rtt.min", summary.tcp_ack_rtt_ms.min, "ms")->attributes.push_back({"samples", samples});
    add("net.tcp.ack_rtt.mean", summary.tcp_ack_rtt_ms.Mean(), "ms")->attributes.push_back({"samples", samples});
    add("net.tcp.ack_rtt.max", summary.tcp_ack_rtt_ms.max, "ms")->attributes.push_back({"samples", samples});
  }
  add("net.tcp.retransmissions", static_cast<double>(summary.tcp_retransmissions), "segments");
  if (summary.dns_response_time_ms.count > 0) {
    const std::string samples = std::to_string(summary.dns_response_time_ms.count);
    add("net.dns.response_time.mean", summary.dns_response_time_ms.Mean(), "ms")->attributes.push_back({"samples", samples});
    add("net.dns.response_time.max", summary.dns_response_time_ms.max, "ms")->attributes.push_back({"samples", samples});
  }
  add("net.dns.responses", static_cast<double>(summary.dns_responses), "responses");
  add("net.packets", static_cast<double>(summary.packets), "packets");
  add("net.bytes", static_cast<double>(summary.bytes), "bytes");
  add("net.tcp.packets", static_cast<double>(summary.tcp_packets), "packets");
  add("net.udp.packets", static_cast<double>(summary.udp_packets), "packets");
  auto* quic = add("net.quic.packets", static_cast<double>(summary.quic_packets), "packets");
  if (!summary.quic_versions.empty()) {
    std::string versions;
    for (const std::string& version : summary.quic_versions) {
      if (!versions.empty()) versions += ',';
      versions += version;
    }
    quic->attributes.push_back({"quic_versions", versions});
  }
  add("net.capture.packets_total", static_cast<double>(summary.total_packets), "packets");
  add("net.capture.bytes_total", static_cast<double>(summary.total_bytes), "bytes");
  return events;
}

}  // namespace aafr::tshark
