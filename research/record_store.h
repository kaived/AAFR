#pragma once

// Phase 5B.3: bounded, durable storage of research decision records, so they
// can be inspected after a restart. Plain C++17, thread-safe, no Qt.
//
// File format (aafr.research.record_store.v1): JSON Lines, one record per
// line, oldest first:
//   {"schema":"aafr.research.record_store.v1","session":"<id>","sequence":<n>,"record":<record>}
// <record> is the ResearchDecisionRecord JSON exactly as ToJson() wrote it
// (no timing), embedded byte-for-byte, so a stored record is the original
// calculation and is never re-serialized or re-evaluated. Every write
// replaces the whole file atomically (temporary file, then rename) and keeps
// at most `capacity` records, dropping the oldest.
//
// Loading accepts only well-formed lines whose envelope and record schemas
// match and whose input origins are controlled, recorded or synthetic (there
// is no measured or live origin); any other line is skipped and counted,
// never repaired. A missing file is an empty store.

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "research/runtime_adapter.h"

namespace aafr::research {

inline constexpr const char* kRecordStoreSchema = "aafr.research.record_store.v1";

struct StoredResearchRecord {
  std::string session_id;   // The session that produced the record.
  std::uint64_t sequence{0};  // Its sequence number within that session.
  std::string record_json;  // The original decision record, byte-for-byte.
  // Read from record_json for display.
  std::string verdict;
  std::string telemetry_origin, telemetry_description;
  std::string probe_origin, probe_description;  // probe_origin empty: no probes.
  std::string reason, model_source_commit;
  std::vector<std::string> installed_path, selected_path;
};

struct RecordStoreStatus {
  bool configured{false};
  std::string path;
  std::string session_id;
  std::size_t capacity{0};
  std::size_t stored{0};
  std::size_t loaded{0};      // Records read from the file at load.
  std::size_t malformed{0};   // Lines skipped at load.
  std::uint64_t writes{0};
  std::string last_error;     // Empty when the last read and write succeeded.
};

class ResearchRecordStore {
 public:
  ResearchRecordStore(std::string path, std::size_t capacity, std::string session_id);

  // Reads the file; replaces the in-memory records.
  void Load();
  // Adds a record of this session and rewrites the file. False (with
  // last_error) when the file could not be written; the record is still kept
  // in memory.
  bool Append(const ResearchRecommendation& recommendation);

  std::vector<StoredResearchRecord> Records() const;  // Oldest first.
  RecordStoreStatus Status() const;

  static std::string FormatLine(const std::string& session_id, std::uint64_t sequence, const std::string& record_json);
  static std::optional<StoredResearchRecord> ParseLine(const std::string& line, std::string* error = nullptr);

 private:
  bool WriteFile();

  mutable std::mutex mutex_;
  RecordStoreStatus status_;
  std::deque<StoredResearchRecord> records_;
};

}  // namespace aafr::research
