// Phase 5B.3: the bounded research decision-record store
// (research/record_store). Records are real pipeline records (controlled
// telemetry, seeded synthetic probes, both labelled). Verifies byte-exact
// storage of the original record JSON, survival across a restart, the
// capacity bound, rejection of unreadable or mislabelled lines, write
// failures and determinism. Needs the XGBoost library (fails without it).

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

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

void CapacityIsBounded() {
  const fs::path file = TempFile("bounded.jsonl");
  const auto recs = Records();
  ResearchRecordStore store(file.string(), 3, "s");
  for (int i = 0; i < 5; ++i) store.Append(recs[static_cast<std::size_t>(i % 3)]);
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

}  // namespace

int main() {
  const std::string library = AAFR_TEST_XGBOOST_LIBRARY;
  if (library.empty() || !fs::exists(library)) {
    std::cout << "FAIL: XGBoost library not available (AAFR_XGBOOST_LIBRARY='" << library << "')\n";
    return 1;
  }
  const int rc = aafr::test::RunTests({
      {"lines_preserve_the_original_record", LinesPreserveTheOriginalRecord},
      {"records_survive_a_restart", RecordsSurviveARestart},
      {"capacity_is_bounded", CapacityIsBounded},
      {"unreadable_lines_are_skipped_not_repaired", UnreadableLinesAreSkippedNotRepaired},
      {"write_failures_are_reported", WriteFailuresAreReported},
      {"stored_records_are_deterministic", StoredRecordsAreDeterministic},
  });
  std::error_code ec;
  fs::remove_all(fs::temp_directory_path() / "aafr_record_store_test", ec);
  return rc;
}
