#include "runtime/telemetry/jsonl_telemetry_sink.h"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ngr {
namespace {

std::string EscapeJson(const std::string& input) {
  std::string escaped;
  escaped.reserve(input.size());
  for (const char ch : input) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

std::string FormatTimestamp(Timestamp timestamp) {
  const std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
  const std::tm* utc = std::gmtime(&time);
  if (utc == nullptr) {
    return "1970-01-01T00:00:00Z";
  }

  std::ostringstream out;
  out << std::put_time(utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

}  // namespace

JsonlTelemetrySink::JsonlTelemetrySink(std::string output_path)
    : output_path_(std::move(output_path)) {
  stream_.open(output_path_, std::ios::app);
  if (!stream_) {
    throw std::runtime_error("failed to open telemetry output: " + output_path_);
  }
}

void JsonlTelemetrySink::Record(const TransitionDecision& decision) {
  stream_ << "{";
  stream_ << "\"ts\":\"" << FormatTimestamp(decision.source.evaluated_at) << "\",";
  stream_ << "\"subsystem\":\"" << ToString(decision.subsystem) << "\",";
  stream_ << "\"outcome\":\"" << ToString(decision.outcome) << "\",";
  stream_ << "\"active_before\":\"" << EscapeJson(decision.active_before) << "\",";
  stream_ << "\"active_after\":\"" << EscapeJson(decision.active_after) << "\",";
  stream_ << "\"confidence\":" << decision.source.confidence << ",";
  stream_ << "\"gate\":{";
  stream_ << "\"dwell_ok\":" << (decision.gates.dwell_ok ? "true" : "false") << ",";
  stream_ << "\"hysteresis_ok\":" << (decision.gates.hysteresis_ok ? "true" : "false") << ",";
  stream_ << "\"confidence_ok\":" << (decision.gates.confidence_ok ? "true" : "false") << ",";
  stream_ << "\"cooldown_ok\":" << (decision.gates.cooldown_ok ? "true" : "false");
  stream_ << "},";
  stream_ << "\"scores\":{";

  bool first_candidate = true;
  for (const ScoredCandidate& candidate : decision.source.ranked) {
    if (!first_candidate) {
      stream_ << ",";
    }
    first_candidate = false;

    stream_ << "\"" << EscapeJson(candidate.id) << "\":{";
    stream_ << "\"total\":" << candidate.score;

    for (const auto& contribution_entry : candidate.contributions) {
      stream_ << ",\"" << EscapeJson(contribution_entry.first)
              << "\":" << contribution_entry.second;
    }

    stream_ << "}";
  }

  stream_ << "}";
  stream_ << "}\n";
  stream_.flush();
}

}  // namespace ngr
