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
// at most `capacity` records, dropping the oldest. Several browser instances
// may share one file: reads and writes hold an exclusive lock on
// `<file>.lock`, and a write merges with what other instances wrote.
//
// Durability: a record is in the file once Append (or Flush) returns true;
// the file is then replaced whole, so a crash never leaves it half-written.
// A record whose write failed (store busy, file not writable) is kept in
// memory, counted as unsaved and written by the next successful write or by
// the Flush at shutdown; if the process crashes first, it is lost. Writes
// are not forced to disk, so a power loss right after a write may still
// lose it.
//
// Loading accepts only well-formed lines whose envelope and record schemas
// match and whose input origins are controlled, recorded or synthetic (there
// is no measured or live origin); any other line is skipped and counted,
// never repaired. A missing file is an empty store.

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
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
  // Records of this session not yet in the file (a busy or failed write).
  // They are written by the next successful write or by Flush(); a crash
  // before that loses them.
  std::size_t unsaved{0};
  std::string last_error;     // Empty when the last read and write succeeded.
};

class ResearchRecordStore {
 public:
  ResearchRecordStore(std::string path, std::size_t capacity, std::string session_id);

  // Reads the file; replaces the in-memory records.
  void Load();
  // Adds a record of this session. Under the cross-process file lock it
  // re-reads the file, keeps every record in it (other browser instances may
  // share the file), adds this session's unwritten records that are not in it yet,
  // trims to capacity and replaces the file. False (with last_error) when the
  // lock could not be taken or the file could not be written; the record is
  // then kept in memory and written by the next successful Append.
  bool Append(const ResearchRecommendation& recommendation);
  // Writes records left unsaved by a busy or failed write (no-op when none).
  // Called at shutdown with a short lock wait. True when nothing is unsaved.
  bool Flush(int lock_timeout_ms);

  std::vector<StoredResearchRecord> Records() const;  // Oldest first.
  RecordStoreStatus Status() const;

  static std::string FormatLine(const std::string& session_id, std::uint64_t sequence, const std::string& record_json);
  static std::optional<StoredResearchRecord> ParseLine(const std::string& line, std::string* error = nullptr);

 private:
  struct FileContents {
    std::deque<StoredResearchRecord> records;
    std::size_t malformed{0};
    std::string last_skip;
  };
  // `known`: lines already parsed by this instance (exact bytes), reused as is.
  static FileContents ReadFile(const std::string& path,
                               const std::unordered_map<std::string, const StoredResearchRecord*>* known = nullptr);
  bool WriteLocked(int lock_timeout_ms);  // Lock, merge with the file, write.
  bool WriteFile();
  void TrimToCapacity();

  mutable std::mutex mutex_;
  RecordStoreStatus status_;
  std::deque<StoredResearchRecord> records_;
  // Only unwritten records may be merged back from this instance's memory.
  std::set<std::uint64_t> unsaved_sequences_;
};

}  // namespace aafr::research
