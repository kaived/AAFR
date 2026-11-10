#include "runtime/telemetry/jsonl_telemetry_sink.h"

#include <cmath>
#include <cstdio>
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
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(ch)));
          escaped += buffer;
        } else {
          escaped += ch;
        }
        break;
    }
  }
  return escaped;
}

std::string FormatTimestamp(Timestamp timestamp) {
  const std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
  std::tm utc{};
  // std::gmtime shares one static buffer across threads.
#ifdef _WIN32
  if (gmtime_s(&utc, &time) != 0) {
    return "1970-01-01T00:00:00Z";
  }
#else
  if (gmtime_r(&time, &utc) == nullptr) {
    return "1970-01-01T00:00:00Z";
  }
#endif

  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

// JSON has no NaN or Infinity literals; emit null so each line stays parseable.
std::string JsonNumber(double value) {
  if (!std::isfinite(value)) {
    return "null";
  }
  std::ostringstream out;
  out << value;
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
  stream_ << "\"confidence\":" << JsonNumber(decision.source.confidence) << ",";
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
    stream_ << "\"total\":" << JsonNumber(candidate.score);

    for (const auto& contribution_entry : candidate.contributions) {
      stream_ << ",\"" << EscapeJson(contribution_entry.first)
              << "\":" << JsonNumber(contribution_entry.second);
    }

    stream_ << "}";
  }

  stream_ << "}";
  stream_ << "}\n";
  stream_.flush();
}

}  // namespace ngr
