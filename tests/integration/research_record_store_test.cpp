// Phase 5B.3: the bounded research decision-record store
// (research/record_store). Records are real pipeline records (controlled
// telemetry, seeded synthetic probes, both labelled). Verifies byte-exact
// storage of the original record JSON, survival across a restart, the
// capacity bound, rejection of unreadable or mislabelled lines, write
// failures and determinism, and several browser instances (stores, threads
// and processes) sharing one file without losing records. Needs the XGBoost
// library (fails without it).

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "algorithms/common/path_algorithm.h"
#include "research/record_store.h"
#include "tests/fixtures/research/reference_values.h"
#include "tests/support/test_support.h"

using namespace aafr::research;
namespace rf = aafr::research_fixture;
namespace alg = aafr::algorithms;
namespace fs = std::filesystem;

namespace {

TelemetrySnapshot Congested() {
  TelemetrySnapshot t;
  t.origin = DataOrigin::Controlled;
  t.description = "controlled: six-switch testbed, s1-s2 at 22 % loss and 99 % utilization";
  t.graph.nodes = {"s1", "s2", "s3", "s4", "s5", "s6"};
  for (const auto& l : rf::kTestbedLinks) {
    for (const auto& [from, to] : {std::pair{l.a, l.b}, std::pair{l.b, l.a}}) {
      ResearchLink link;
      const bool s1s2 = (std::string(from) == "s1" && std::string(to) == "s2") ||
                        (std::string(from) == "s2" && std::string(to) == "s1");
      link.observation = LinkObservation{from, to, l.delay_ms, s1s2 ? 0.22 : 0.0, l.capacity_mbps, s1s2 ? 0.99 : 0.0};
      link.startup_delay_ms = l.delay_ms;
      t.graph.links.push_back(link);
    }
  }
  t.source = "s1";
  t.destination = "s6";
  t.installed_path = {"s1", "s2", "s6"};
  return t;
}

ProbeObservations Synthetic(const TournamentPlan& plan, std::uint64_t seed) {
  alg::Rng rng(seed);
  std::vector<double> drift(kFrames);
  for (double& d : drift) d = std::exp(0.03 * rng.Normal());
  const auto clean = plan.candidates[2].candidate.path;
  ProbeObservations o;
  o.origin = DataOrigin::Synthetic;
  o.description = "synthetic: seeded log-normal echoes, seed " + std::to_string(seed);
  for (std::size_t a = 0; a < plan.arms.size(); ++a) {
    const auto& pa = plan.arms[a];
    const double base = pa.incumbent ? 81.5 : (pa.path == clean ? 20.4 : 60.0);
    const std::size_t replies = pa.incumbent ? 3 : (pa.path == clean ? 8 : 4);
    ProbeArm arm;
    arm.arm_id = pa.arm_id;
    arm.incumbent = pa.incumbent;
    arm.path = pa.path;
    for (std::size_t f = 0; f < kFrames; ++f) {
      ProbeFrame frame;
      frame.round = f;
      frame.position_in_round = (a + f) % plan.arms.size();
      for (std::size_t b = 0; b < kBlocks; ++b) {
        ProbeBlock block;
        for (std::size_t e = 0; e < kEchoes; ++e) {
          const double rtt = base * drift[f] * std::exp(0.02 * rng.Normal());
          block.echoes.push_back(e < replies ? std::optional<double>(rtt) : std::nullopt);
        }
        frame.blocks.push_back(block);
      }
      arm.frames.push_back(frame);
    }
    o.tournament.arms.push_back(arm);
  }
  return o;
}

ResearchRuntimeAdapter& Adapter() {
  static ResearchRuntimeAdapter adapter([] {
    ResearchRuntimeAdapter::Options o;
    o.model_dir = AAFR_TEST_XGBOOST_MODEL_DIR;
    o.xgboost_library = AAFR_TEST_XGBOOST_LIBRARY;
    return o;
  }());
  return adapter;
}

// Three distinct real records: switch (with probes), keep (missing
// measurement), undetermined (no probes).
std::vector<ResearchRecommendation> Records() {
  std::vector<ResearchRecommendation> out;
  const auto plan = Adapter().Plan(Congested());
  out.push_back(*Adapter().Evaluate(Congested(), Synthetic(*plan, 11)).recommendation);
  TelemetrySnapshot missing = Congested();
  for (auto& l : missing.graph.links) {
    if (l.observation.from == "s3" && l.observation.to == "s5") l.observation.utilization.reset();
  }
  out.push_back(*Adapter().Evaluate(missing, std::nullopt).recommendation);
  out.push_back(*Adapter().Evaluate(Congested(), std::nullopt).recommendation);
  return out;
}

fs::path TempFile(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / "aafr_record_store_test";
  fs::create_directories(dir);
  const fs::path p = dir / name;
  fs::remove(p);
  fs::remove(p.string() + ".tmp");
  return p;
}

std::vector<std::string> Lines(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::vector<std::string> lines;
  for (std::string l; std::getline(in, l);) lines.push_back(l);
  return lines;
}

void LinesPreserveTheOriginalRecord() {
  const auto recs = Records();
  CHECK(recs[0].verdict == ArbitrationVerdict::Switch && recs[1].verdict == ArbitrationVerdict::Keep &&
        recs[2].verdict == ArbitrationVerdict::Undetermined);
  for (const auto& r : recs) {
    const std::string line = ResearchRecordStore::FormatLine("session-a", r.sequence, r.record_json);
    CHECK(line.find('\n') == std::string::npos);
    std::string error;
    const auto parsed = ResearchRecordStore::ParseLine(line, &error);
    CHECK(parsed.has_value() && error.empty());
    if (!parsed) continue;
    CHECK(parsed->record_json == r.record_json);  // Byte-for-byte.
    CHECK(parsed->session_id == "session-a" && parsed->sequence == r.sequence);
    CHECK(parsed->verdict == ToString(r.verdict));
    CHECK(parsed->telemetry_origin == "controlled");
    CHECK(parsed->telemetry_description == r.record.telemetry_description);
    CHECK(parsed->installed_path == r.installed_path);
    CHECK(parsed->model_source_commit == "6b6c008fc484bcbadfb8d8447acf36fe5f39e10d");
  }
  const auto sw = ResearchRecordStore::ParseLine(ResearchRecordStore::FormatLine("s", 1, recs[0].record_json));
  CHECK(sw->probe_origin == "synthetic" && sw->selected_path == recs[0].recommended_path);
  const auto keep = ResearchRecordStore::ParseLine(ResearchRecordStore::FormatLine("s", 2, recs[1].record_json));
  CHECK(keep->probe_origin.empty() && keep->selected_path.empty());
}

void RecordsSurviveARestart() {
  const fs::path file = TempFile("restart.jsonl");
  const auto recs = Records();
  {
    ResearchRecordStore first(file.string(), 50, "session-1");
    first.Load();  // No file yet: empty, no error.
    CHECK(first.Status().stored == 0 && first.Status().last_error.empty());
    for (const auto& r : recs) CHECK(first.Append(r));
    CHECK(first.Status().writes == 3);
  }
  CHECK(Lines(file).size() == 3 && !fs::exists(file.string() + ".tmp"));
  ResearchRecordStore second(file.string(), 50, "session-2");
  second.Load();
  const auto loaded = second.Records();
  CHECK(second.Status().loaded == 3 && second.Status().malformed == 0);
  CHECK(loaded.size() == 3);
  for (std::size_t i = 0; i < loaded.size() && i < recs.size(); ++i) {
    CHECK(loaded[i].record_json == recs[i].record_json && loaded[i].session_id == "session-1");
  }
  // A new record of the second session is appended after the earlier ones.
  CHECK(second.Append(recs[0]));
  const auto after = second.Records();
  CHECK(after.size() == 4 && after.back().session_id == "session-2" && after.front().session_id == "session-1");
}

// The same record under another sequence number (a record is identified by
// its session and sequence; re-appending one is a duplicate).
ResearchRecommendation WithSequence(ResearchRecommendation r, std::uint64_t sequence) {
  r.sequence = sequence;
  return r;
}

void CapacityIsBounded() {
  const fs::path file = TempFile("bounded.jsonl");
  const auto recs = Records();
  ResearchRecordStore store(file.string(), 3, "s");
  for (int i = 0; i < 5; ++i) store.Append(WithSequence(recs[static_cast<std::size_t>(i % 3)], 100 + i));
  CHECK(store.Records().size() == 3 && Lines(file).size() == 3);
  // The oldest were dropped: the kept ones are the records appended 3rd..5th.
  CHECK(store.Records()[0].record_json == recs[2].record_json);
  CHECK(store.Records()[2].record_json == recs[1].record_json);
  ResearchRecordStore reloaded(file.string(), 2, "s");
  reloaded.Load();
  CHECK(reloaded.Records().size() == 2 && reloaded.Status().loaded == 3);  // Read 3, keep the newest 2.
}

void UnreadableLinesAreSkippedNotRepaired() {
  const fs::path file = TempFile("malformed.jsonl");
  const auto recs = Records();
  const std::string good = ResearchRecordStore::FormatLine("s", 1, recs[1].record_json);
  std::string live = good;
  live.replace(live.find("\"origin\":\"controlled\""), 21, "\"origin\":\"live\"");
  std::string wrong_schema = good;
  wrong_schema.replace(wrong_schema.find("record_store.v1"), 15, "record_store.v9");
  {
    std::ofstream out(file, std::ios::binary);
    out << "not json\n" << wrong_schema << "\n" << live << "\n" << good << "\n" << good.substr(0, good.size() / 2) << "\n";
  }
  ResearchRecordStore store(file.string(), 10, "s");
  store.Load();
  CHECK(store.Status().loaded == 1 && store.Status().malformed == 4);
  CHECK(!store.Status().last_error.empty());
  CHECK(store.Records().size() == 1 && store.Records()[0].record_json == recs[1].record_json);
  // A live origin is never accepted.
  std::string why;
  CHECK(!ResearchRecordStore::ParseLine(live, &why) && aafr::test::Contains(why, "not controlled, recorded or synthetic"));
}

void WriteFailuresAreReported() {
  const auto recs = Records();
  // The parent "directory" is a file, so the record file cannot be created.
  const fs::path blocker = TempFile("blocker");
  { std::ofstream(blocker) << "x"; }
  ResearchRecordStore store((blocker / "records.jsonl").string(), 10, "s");
  CHECK(!store.Append(recs[0]));
  CHECK(!store.Status().last_error.empty());
  CHECK(store.Records().size() == 1);  // Still inspectable in memory.
  ResearchRecordStore disabled("", 10, "s");
  CHECK(!disabled.Status().configured && !disabled.Append(recs[0]) && disabled.Records().empty());
}

void StoredRecordsAreDeterministic() {
  const fs::path a = TempFile("det_a.jsonl"), b = TempFile("det_b.jsonl");
  const auto ra = Records(), rb = Records();
  ResearchRecordStore sa(a.string(), 10, "same"), sb(b.string(), 10, "same");
  for (std::size_t i = 0; i < ra.size(); ++i) {
    CHECK(ra[i].record_json == rb[i].record_json);
    sa.Append(ra[i]);
    sb.Append(rb[i]);
  }
  // Sequence numbers differ (one adapter), so compare the embedded records.
  const auto la = sa.Records(), lb = sb.Records();
  for (std::size_t i = 0; i < la.size(); ++i) CHECK(la[i].record_json == lb[i].record_json);
}

// ---- Several browser instances on one file ----

std::string g_self;  // This executable, for the multi-process case.

std::set<std::pair<std::string, std::uint64_t>> Keys(const std::vector<StoredResearchRecord>& records) {
  std::set<std::pair<std::string, std::uint64_t>> keys;
  for (const auto& r : records) keys.emplace(r.session_id, r.sequence);
  return keys;
}

// What a fresh instance reads from the file.
std::vector<StoredResearchRecord> Reread(const fs::path& file) {
  ResearchRecordStore reader(file.string(), 1000, "reader");
  reader.Load();
  CHECK(reader.Status().malformed == 0 && reader.Status().last_error.empty());
  return reader.Records();
}

void InstancesSharingAFileKeepEachOthersRecords() {
  const fs::path file = TempFile("shared.jsonl");
  const auto recs = Records();
  ResearchRecordStore a(file.string(), 50, "instance-a"), b(file.string(), 50, "instance-b");
  a.Load();
  b.Load();  // Both start before either has written.
  CHECK(a.Append(WithSequence(recs[0], 1)));
  CHECK(b.Append(WithSequence(recs[1], 1)));  // Would overwrite a's record without merging.
  CHECK(a.Append(WithSequence(recs[2], 2)));
  const auto file_records = Reread(file);
  CHECK(file_records.size() == 3);
  CHECK(Keys(file_records) == (std::set<std::pair<std::string, std::uint64_t>>{
                                  {"instance-a", 1}, {"instance-b", 1}, {"instance-a", 2}}));
  if (file_records.size() == 3) {  // In write order, each byte-for-byte.
    CHECK(file_records[0].record_json == recs[0].record_json && file_records[0].session_id == "instance-a");
    CHECK(file_records[1].record_json == recs[1].record_json && file_records[1].session_id == "instance-b");
    CHECK(file_records[2].record_json == recs[2].record_json);
  }
  CHECK(a.Records().size() == 3);  // a sees b's record after its next write.
  CHECK(!fs::exists(file.string() + ".tmp"));
  // A duplicate (same session and sequence) is not stored twice.
  CHECK(a.Append(WithSequence(recs[0], 1)));
  CHECK(Reread(file).size() == 3);
}

void ConcurrentWritersLoseNothing() {
  const fs::path file = TempFile("threads.jsonl");
  const auto recs = Records();
  constexpr int kWriters = 4, kEach = 15;
  std::vector<std::unique_ptr<ResearchRecordStore>> stores;
  for (int w = 0; w < kWriters; ++w) {
    stores.push_back(std::make_unique<ResearchRecordStore>(file.string(), 200, "thread-" + std::to_string(w)));
    stores.back()->Load();
  }
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kEach; ++i) {
        auto& store = *stores[static_cast<std::size_t>(w)];
        if (!store.Append(WithSequence(recs[static_cast<std::size_t>(i % 3)], i + 1))) {
          ++failures;
          std::cout << "  append failed: " << store.Status().last_error << "\n";
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  CHECK(failures.load() == 0);
  const auto file_records = Reread(file);
  CHECK(file_records.size() == kWriters * kEach);
  CHECK(Keys(file_records).size() == kWriters * kEach);
  for (const auto& r : file_records) CHECK(r.record_json == recs[(r.sequence - 1) % 3].record_json);
  CHECK(!fs::exists(file.string() + ".tmp"));
}

void SharedCapacityDoesNotResurrectTrimmedRecords() {
  const fs::path file = TempFile("shared_bounded.jsonl");
  const auto recs = Records();
  ResearchRecordStore a(file.string(), 3, "a"), b(file.string(), 3, "b");
  a.Load();
  b.Load();
  for (int i = 1; i <= 3; ++i) CHECK(a.Append(WithSequence(recs[0], i)));
  CHECK(b.Append(WithSequence(recs[1], 1)));  // Trims a:1 from the shared file.
  CHECK(a.Append(WithSequence(recs[2], 4)));  // Must not re-add a:1 from a's stale memory.
  const auto stored = Reread(file);
  CHECK(stored.size() == 3);
  CHECK(Keys(stored) == (std::set<std::pair<std::string, std::uint64_t>>{{"a", 3}, {"b", 1}, {"a", 4}}));
  if (stored.size() == 3) {
    CHECK(stored[0].session_id == "a" && stored[0].sequence == 3);
    CHECK(stored[1].session_id == "b" && stored[1].sequence == 1);
    CHECK(stored[2].session_id == "a" && stored[2].sequence == 4);
  }
}

void ConcurrentBrowserProcessesLoseNothing() {
  const fs::path file = TempFile("processes.jsonl");
  constexpr int kProcesses = 3, kEach = 8;
  std::vector<std::thread> launchers;
  std::atomic<int> failed{0};
  for (int p = 0; p < kProcesses; ++p) {
    launchers.emplace_back([&, p] {
      // cmd /c needs the whole command quoted once more.
      const std::string command = "\"\"" + g_self + "\" --append-child \"" + file.string() + "\" process-" +
                                  std::to_string(p) + " " + std::to_string(kEach) + "\"";
      if (std::system(command.c_str()) != 0) ++failed;
    });
  }
  for (auto& t : launchers) t.join();
  CHECK(failed.load() == 0);
  const auto file_records = Reread(file);
  CHECK(file_records.size() == kProcesses * kEach);
  CHECK(Keys(file_records).size() == kProcesses * kEach);
  CHECK(!fs::exists(file.string() + ".tmp"));
}

void BusyStoreKeepsTheRecordAndWritesItLater() {
#ifdef _WIN32
  const fs::path file = TempFile("busy.jsonl");
  const auto recs = Records();
  ResearchRecordStore store(file.string(), 10, "s");
  CHECK(store.Append(WithSequence(recs[0], 1)));
  // Another instance holds the lock (for longer than the 2 s wait).
  HANDLE lock = CreateFileW((file.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(lock != INVALID_HANDLE_VALUE);
  CHECK(!store.Append(WithSequence(recs[1], 2)));
  CHECK(aafr::test::Contains(store.Status().last_error, "busy"));
  CHECK(store.Records().size() == 2 && store.Status().unsaved == 1);  // Still inspectable, counted unsaved.
  CHECK(!store.Flush(200));  // Still locked: nothing written.
  CHECK(store.Status().unsaved == 1);
  CloseHandle(lock);
  CHECK(Reread(file).size() == 1);  // Not written while busy.
  // Flush (as at shutdown) writes the record kept in memory.
  CHECK(store.Flush(1000));
  CHECK(store.Status().unsaved == 0 && store.Status().last_error.empty());
  CHECK(Reread(file).size() == 2);
  CHECK(store.Flush(1000));  // Nothing unsaved: no-op.
  CHECK(store.Append(WithSequence(recs[2], 3)));
  const auto file_records = Reread(file);
  CHECK(file_records.size() == 3 && Keys(file_records).count({"s", 2}) == 1);
#else
  CHECK(true);
#endif
}

void ReadOnlyFileIsReportedAndRecovered() {
  const fs::path file = TempFile("readonly.jsonl");
  const auto recs = Records();
  ResearchRecordStore store(file.string(), 10, "s");
  CHECK(store.Append(WithSequence(recs[0], 1)));
  fs::permissions(file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                  fs::perm_options::remove);
  CHECK(!store.Append(WithSequence(recs[1], 2)));
  CHECK(aafr::test::Contains(store.Status().last_error, "cannot replace"));
  CHECK(store.Records().size() == 2);
  CHECK(!fs::exists(file.string() + ".tmp"));
  CHECK(Reread(file).size() == 1);  // The file is unchanged, not half-written.
  fs::permissions(file, fs::perms::owner_write, fs::perm_options::add);
  CHECK(store.Append(WithSequence(recs[2], 3)));
  CHECK(Reread(file).size() == 3);
}

void UnsavedCountTracksOnlyRetainedRecords() {
  const fs::path file = TempFile("unsaved_bounded.jsonl");
  const auto recs = Records();
  ResearchRecordStore store(file.string(), 2, "s");
  CHECK(store.Append(WithSequence(recs[0], 1)));
  fs::permissions(file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                  fs::perm_options::remove);
  for (int i = 2; i <= 4; ++i) CHECK(!store.Append(WithSequence(recs[1], i)));
  CHECK(store.Records().size() == 2 && store.Status().unsaved == 2);
  CHECK(store.Records()[0].sequence == 3 && store.Records()[1].sequence == 4);
  fs::permissions(file, fs::perms::owner_write, fs::perm_options::add);
  CHECK(store.Flush(1000));
  CHECK(store.Status().unsaved == 0);
  const auto stored = Reread(file);
  CHECK(stored.size() == 2);
  CHECK(Keys(stored) == (std::set<std::pair<std::string, std::uint64_t>>{{"s", 3}, {"s", 4}}));
}

#ifdef _WIN32
HANDLE HoldLock(const fs::path& file) {
  return CreateFileW((file.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool WaitFor(const fs::path& marker, int seconds) {
  for (int i = 0; i < seconds * 20; ++i) {
    if (fs::exists(marker)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}
#endif

// A browser instance whose write found the store busy and that then exits
// normally (flushing) or crashes (no flush): the documented limitation is
// that only a crash loses the unsaved record.
void UnsavedRecordsAreFlushedAtExitAndLostOnACrash() {
#ifdef _WIN32
  for (const std::string mode : {"flush", "crash"}) {
    const fs::path file = TempFile("exit_" + mode + ".jsonl");
    const fs::path marker = TempFile("exit_" + mode + ".failed");
    {
      ResearchRecordStore earlier(file.string(), 10, "earlier");
      CHECK(earlier.Append(WithSequence(Records()[0], 1)));
    }
    HANDLE lock = HoldLock(file);
    CHECK(lock != INVALID_HANDLE_VALUE);
    int child_exit = -1;
    std::thread child([&] {
      const std::string command = "\"\"" + g_self + "\" --exit-child \"" + file.string() + "\" \"" +
                                  marker.string() + "\" " + mode + "\"";
      child_exit = std::system(command.c_str());
    });
    CHECK(WaitFor(marker, 60));  // The child's write failed (store busy).
    CloseHandle(lock);           // The other instance releases the store.
    child.join();
    CHECK(child_exit == (mode == "crash" ? 3 : 0));
    const auto file_records = Reread(file);
    if (mode == "flush") {
      CHECK(file_records.size() == 2 && Keys(file_records).count({"exiting", 7}) == 1);
    } else {
      CHECK(file_records.size() == 1 && Keys(file_records).count({"exiting", 7}) == 0);  // Lost, as documented.
    }
  }
#else
  CHECK(true);
#endif
}

// Child of UnsavedRecordsAreFlushedAtExitAndLostOnACrash.
int ExitChild(const std::string& path, const std::string& marker, const std::string& mode) {
  const auto recs = Records();
  ResearchRecordStore store(path, 10, "exiting");
  store.Load();
  if (store.Append(WithSequence(recs[1], 7))) return 10;  // The parent holds the lock: must not succeed.
  if (store.Status().unsaved != 1) return 11;
  std::ofstream(marker) << "write failed";
  if (mode == "crash") std::_Exit(3);  // No flush, no destructors: a crash.
  return store.Flush(30000) ? 0 : 12;  // Normal exit: waits for the lock, then writes.
}

// Child process of ConcurrentBrowserProcessesLoseNothing: one "browser
// instance" appending `count` records to the shared file.
int AppendChild(const std::string& path, const std::string& session, int count) {
  const auto recs = Records();
  ResearchRecordStore store(path, 200, session);
  store.Load();
  int failures = 0;
  for (int i = 0; i < count; ++i) {
    if (!store.Append(WithSequence(recs[static_cast<std::size_t>(i % 3)], static_cast<std::uint64_t>(i + 1)))) {
      std::cout << "append failed: " << store.Status().last_error << "\n";
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string library = AAFR_TEST_XGBOOST_LIBRARY;
  if (library.empty() || !fs::exists(library)) {
    std::cout << "FAIL: XGBoost library not available (AAFR_XGBOOST_LIBRARY='" << library << "')\n";
    return 1;
  }
  if (argc == 5 && std::string(argv[1]) == "--append-child") return AppendChild(argv[2], argv[3], std::atoi(argv[4]));
  if (argc == 5 && std::string(argv[1]) == "--exit-child") return ExitChild(argv[2], argv[3], argv[4]);
  g_self = fs::absolute(argv[0]).string();
  if (fs::path(g_self).extension().empty()) g_self += ".exe";
  const int rc = aafr::test::RunTests({
      {"lines_preserve_the_original_record", LinesPreserveTheOriginalRecord},
      {"records_survive_a_restart", RecordsSurviveARestart},
      {"capacity_is_bounded", CapacityIsBounded},
      {"unreadable_lines_are_skipped_not_repaired", UnreadableLinesAreSkippedNotRepaired},
      {"write_failures_are_reported", WriteFailuresAreReported},
      {"stored_records_are_deterministic", StoredRecordsAreDeterministic},
      {"instances_sharing_a_file_keep_each_others_records", InstancesSharingAFileKeepEachOthersRecords},
      {"concurrent_writers_lose_nothing", ConcurrentWritersLoseNothing},
      {"shared_capacity_does_not_resurrect_trimmed_records", SharedCapacityDoesNotResurrectTrimmedRecords},
      {"concurrent_browser_processes_lose_nothing", ConcurrentBrowserProcessesLoseNothing},
      {"busy_store_keeps_the_record_and_writes_it_later", BusyStoreKeepsTheRecordAndWritesItLater},
      {"read_only_file_is_reported_and_recovered", ReadOnlyFileIsReportedAndRecovered},
      {"unsaved_count_tracks_only_retained_records", UnsavedCountTracksOnlyRetainedRecords},
      {"unsaved_records_are_flushed_at_exit_and_lost_on_a_crash", UnsavedRecordsAreFlushedAtExitAndLostOnACrash},
  });
  std::error_code ec;
  fs::remove_all(fs::temp_directory_path() / "aafr_record_store_test", ec);
  return rc;
}
