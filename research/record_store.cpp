#include "research/record_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "research/json_reader.h"

namespace aafr::research {
namespace {

namespace fs = std::filesystem;

// Exclusive, cross-process lock on `<file>.lock`, held while the file is
// read or rewritten. The operating system releases it when the holder exits,
// so a crashed instance never leaves the store locked.
class FileLock {
 public:
  FileLock(const std::string& path, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const fs::path lock(path + ".lock");
    std::error_code ec;
    if (lock.has_parent_path()) fs::create_directories(lock.parent_path(), ec);
    while (!TryLock(lock)) {
      if (std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ~FileLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (fd_ >= 0) close(fd_);  // Releases the flock.
#endif
  }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  bool held() const {
#ifdef _WIN32
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return fd_ >= 0;
#endif
  }

 private:
  bool TryLock(const fs::path& lock) {
#ifdef _WIN32
    handle_ = CreateFileW(lock.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0 /* no sharing */, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle_ != INVALID_HANDLE_VALUE;
#else
    fd_ = open(lock.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) return false;
    if (flock(fd_, LOCK_EX | LOCK_NB) == 0) return true;
    close(fd_);
    fd_ = -1;
    return false;
#endif
  }
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

// Reads and writes run on the research worker thread, never the GUI thread.
constexpr int kLockTimeoutMs = 10000;

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

ResearchRecordStore::FileContents ResearchRecordStore::ReadFile(
    const std::string& path, const std::unordered_map<std::string, const StoredResearchRecord*>* known) {
  FileContents c;
  std::ifstream in(path, std::ios::binary);
  if (!in) return c;  // No file yet: an empty store.
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (known) {  // A line this instance already parsed (byte-identical): no need to parse it again.
      const auto hit = known->find(line);
      if (hit != known->end()) {
        c.records.push_back(*hit->second);
        continue;
      }
    }
    std::string why;
    auto r = ParseLine(line, &why);
    if (!r) {
      ++c.malformed;
      c.last_skip = "skipped a stored line: " + why;
      continue;
    }
    c.records.push_back(std::move(*r));
  }
  return c;
}

void ResearchRecordStore::Load() {
  std::lock_guard<std::mutex> lock(mutex_);
  records_.clear();
  unsaved_sequences_.clear();
  status_.unsaved = 0;
  status_.loaded = status_.malformed = 0;
  status_.last_error.clear();
  if (!status_.configured) return;
  const FileLock file_lock(status_.path, kLockTimeoutMs);
  // Reading without the lock is still safe (writers replace the file whole),
  // but say that another instance kept it busy.
  FileContents c = ReadFile(status_.path);
  status_.loaded = c.records.size();
  status_.malformed = c.malformed;
  status_.last_error = c.last_skip;
  if (!file_lock.held()) {
    status_.last_error = "record store busy (locked by another instance); read without the lock";
  }
  records_ = std::move(c.records);
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
  unsaved_sequences_.insert(rec.sequence);
  status_.unsaved = unsaved_sequences_.size();
  return WriteLocked(kLockTimeoutMs);
}

bool ResearchRecordStore::Flush(int lock_timeout_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!status_.configured || status_.unsaved == 0) return true;
  return WriteLocked(lock_timeout_ms);
}

bool ResearchRecordStore::WriteLocked(int lock_timeout_ms) {
  const FileLock file_lock(status_.path, lock_timeout_ms);
  if (!file_lock.held()) {
    TrimToCapacity();
    status_.last_error = "record store busy (locked by another instance); " + std::to_string(status_.unsaved) +
                         " record(s) kept in memory only";
    return false;
  }
  // Other instances may have written since this one last read the file:
  // keep everything in the file, then add only this session's unwritten
  // records. Previously written records absent from the file were trimmed
  // by another instance and must not be resurrected from stale memory.
  std::unordered_map<std::string, const StoredResearchRecord*> known;
  for (const auto& r : records_) known.emplace(FormatLine(r.session_id, r.sequence, r.record_json), &r);
  FileContents c = ReadFile(status_.path, &known);
  known.clear();  // Points into records_, which is replaced below.
  std::set<std::pair<std::string, std::uint64_t>> present;
  for (const auto& r : c.records) present.emplace(r.session_id, r.sequence);
  std::deque<StoredResearchRecord> merged = std::move(c.records);
  for (auto& r : records_) {
    if (r.session_id == status_.session_id && unsaved_sequences_.count(r.sequence) &&
        present.emplace(r.session_id, r.sequence).second) {
      merged.push_back(std::move(r));
    }
  }
  records_ = std::move(merged);
  TrimToCapacity();
  return WriteFile();
}

void ResearchRecordStore::TrimToCapacity() {
  while (records_.size() > status_.capacity) {
    const auto& oldest = records_.front();
    if (oldest.session_id == status_.session_id) unsaved_sequences_.erase(oldest.sequence);
    records_.pop_front();
  }
  status_.stored = records_.size();
  status_.unsaved = unsaved_sequences_.size();
}

bool ResearchRecordStore::WriteFile() {
  const fs::path target(status_.path);
  const fs::path temporary = fs::path(status_.path + ".tmp");  // Under the file lock: one writer.
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
      out.close();
      fs::remove(temporary, ec);
      status_.last_error = "write failed for " + temporary.string();
      return false;
    }
  }
  fs::rename(temporary, target, ec);  // Replaces the old file in one step.
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    status_.last_error = "cannot replace " + target.string() + ": " + ec.message();
    return false;
  }
  ++status_.writes;
  unsaved_sequences_.clear();
  status_.unsaved = 0;  // Every record of this session held in memory is now in the file.
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
