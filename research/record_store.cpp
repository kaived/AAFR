#include "research/record_store.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "research/json_reader.h"

namespace aafr::research {
namespace {

namespace fs = std::filesystem;

std::string Escape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20) continue;  // Session ids never contain control characters.
    out.push_back(c);
  }
  return out;
}

std::vector<std::string> PathOf(const JsonNode* node) {
  std::vector<std::string> path;
  if (!node || node->type != JsonNode::Type::Array) return path;
  for (const JsonNode& n : node->array) {
    if (n.type == JsonNode::Type::String) path.push_back(n.string);
  }
  return path;
}

bool KnownOrigin(const std::string& origin) {
  return origin == "controlled" || origin == "recorded" || origin == "synthetic";
}

constexpr const char* kRecordKey = ",\"record\":";

}  // namespace

ResearchRecordStore::ResearchRecordStore(std::string path, std::size_t capacity, std::string session_id) {
  status_.configured = !path.empty() && capacity > 0;
  status_.path = std::move(path);
  status_.capacity = capacity;
  status_.session_id = std::move(session_id);
}

std::string ResearchRecordStore::FormatLine(const std::string& session_id, std::uint64_t sequence,
                                            const std::string& record_json) {
  std::ostringstream line;
  line << "{\"schema\":\"" << kRecordStoreSchema << "\",\"session\":\"" << Escape(session_id)
       << "\",\"sequence\":" << sequence << kRecordKey << record_json << "}";
  return line.str();
}

std::optional<StoredResearchRecord> ResearchRecordStore::ParseLine(const std::string& line, std::string* error) {
  const auto fail = [error](const std::string& why) -> std::optional<StoredResearchRecord> {
    if (error) *error = why;
    return std::nullopt;
  };
  std::string why;
  const auto envelope = ParseJson(line, &why);
  if (!envelope) return fail("not JSON: " + why);
  if (envelope->String("schema") != std::string(kRecordStoreSchema)) return fail("unknown store schema");
  const auto session = envelope->String("session");
  const auto sequence = envelope->Number("sequence");
  const JsonNode* record = envelope->Find("record");
  if (!session || !sequence || *sequence < 0 || !record || record->type != JsonNode::Type::Object) {
    return fail("incomplete envelope");
  }
  if (record->String("schema") != std::string(kDecisionRecordSchema)) return fail("unknown decision record schema");
  // The record's original bytes: everything between ',"record":' and the
  // envelope's closing brace (FormatLine writes it last).
  const auto at = line.find(kRecordKey);
  if (at == std::string::npos || line.empty() || line.back() != '}') return fail("record not in the stored layout");
  StoredResearchRecord r;
  r.record_json = line.substr(at + std::char_traits<char>::length(kRecordKey),
                              line.size() - 1 - (at + std::char_traits<char>::length(kRecordKey)));
  if (!ParseJson(r.record_json)) return fail("embedded record is not JSON");
  r.session_id = *session;
  r.sequence = static_cast<std::uint64_t>(*sequence);
  r.verdict = record->String("verdict").value_or("");
  const JsonNode* telemetry = record->Find("telemetry");
  const JsonNode* probes = record->Find("probes");
  r.telemetry_origin = telemetry ? telemetry->String("origin").value_or("") : "";
  r.telemetry_description = telemetry ? telemetry->String("description").value_or("") : "";
  if (!KnownOrigin(r.telemetry_origin)) return fail("telemetry origin '" + r.telemetry_origin + "' is not controlled, recorded or synthetic");
  if (probes) {
    r.probe_origin = probes->String("origin").value_or("");
    r.probe_description = probes->String("description").value_or("");
    if (!r.probe_origin.empty() && !KnownOrigin(r.probe_origin)) return fail("probe origin is not controlled, recorded or synthetic");
  }
  if (r.verdict != "keep" && r.verdict != "switch" && r.verdict != "undetermined") return fail("unknown verdict");
  r.reason = record->String("reason").value_or("");
  if (const JsonNode* model = record->Find("model")) r.model_source_commit = model->String("source_commit").value_or("");
  r.installed_path = PathOf(record->Find("installed_path"));
  r.selected_path = PathOf(record->Find("selected_path"));
  return r;
}

void ResearchRecordStore::Load() {
  std::lock_guard<std::mutex> lock(mutex_);
  records_.clear();
  status_.loaded = status_.malformed = 0;
  status_.last_error.clear();
  if (!status_.configured) return;
  std::ifstream in(status_.path, std::ios::binary);
  if (!in) {
    status_.stored = 0;  // No file yet: an empty store.
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::string why;
    auto r = ParseLine(line, &why);
    if (!r) {
      ++status_.malformed;
      status_.last_error = "skipped a stored line: " + why;
      continue;
    }
    records_.push_back(std::move(*r));
    ++status_.loaded;
  }
  while (records_.size() > status_.capacity) records_.pop_front();
  status_.stored = records_.size();
}

bool ResearchRecordStore::Append(const ResearchRecommendation& rec) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!status_.configured) return false;
  const std::string line = FormatLine(status_.session_id, rec.sequence, rec.record_json);
  auto parsed = ParseLine(line);
  if (!parsed) {  // Never store what could not be read back.
    status_.last_error = "record could not be stored (not a readable decision record)";
    return false;
  }
  records_.push_back(std::move(*parsed));
  while (records_.size() > status_.capacity) records_.pop_front();
  status_.stored = records_.size();
  return WriteFile();
}

bool ResearchRecordStore::WriteFile() {
  const fs::path target(status_.path);
  const fs::path temporary = fs::path(status_.path + ".tmp");
  std::error_code ec;
  if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      status_.last_error = "cannot write " + temporary.string();
      return false;
    }
    for (const auto& r : records_) out << FormatLine(r.session_id, r.sequence, r.record_json) << '\n';
    out.flush();
    if (!out) {
      status_.last_error = "write failed for " + temporary.string();
      return false;
    }
  }
  fs::rename(temporary, target, ec);  // Replaces the old file in one step.
  if (ec) {
    fs::remove(temporary, ec);
    status_.last_error = "cannot replace " + target.string();
    return false;
  }
  ++status_.writes;
  status_.last_error.clear();
  return true;
}

std::vector<StoredResearchRecord> ResearchRecordStore::Records() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::vector<StoredResearchRecord>(records_.begin(), records_.end());
}

RecordStoreStatus ResearchRecordStore::Status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

}  // namespace aafr::research
