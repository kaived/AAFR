// Deterministic tests for the V0.3 telemetry core: serialization, bounded
// queues, the pipeline, JSONL retention, TShark field parsing and the
// runtime observability contracts. No Qt, network or capture driver needed.

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "network/telemetry/bounded_queue.h"
#include "network/telemetry/jsonl_telemetry_writer.h"
#include "network/telemetry/telemetry_event.h"
#include "network/telemetry/telemetry_pipeline.h"
#include "network/tshark/tshark_fields.h"
#include "runtime/observability/runtime_contracts.h"

namespace fs = std::filesystem;
using namespace aafr::telemetry;

namespace {
int failures = 0;
int checks = 0;
std::string current_test;

#define CHECK(condition)                                                              \
  do {                                                                                \
    ++checks;                                                                         \
    if (!(condition)) {                                                               \
      ++failures;                                                                     \
      std::cerr << "FAIL [" << current_test << "] " << __FILE__ << ':' << __LINE__  \
                << ": " #condition "\n";                                            \
    }                                                                                 \
  } while (0)

#define CHECK_NEAR(actual, expected, tolerance) CHECK(std::fabs((actual) - (expected)) <= (tolerance))

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::vector<std::string> ReadLines(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  return lines;
}

fs::path FreshDirectory(const std::string& name) {
  const fs::path directory = fs::temp_directory_path() / ("aafr-telemetry-test-" + name);
  std::error_code error;
  fs::remove_all(directory, error);
  fs::create_directories(directory);
  return directory;
}

const TelemetryEvent* FindMetric(const std::vector<TelemetryEvent>& events, const std::string& metric) {
  for (const TelemetryEvent& event : events) {
    if (event.metric == metric) return &event;
  }
  return nullptr;
}

void TestSerialization() {
  TelemetryEvent event;
  event.event_id = 42;
  event.session_id = "s1";
  event.wall_time = WallClock::time_point(std::chrono::milliseconds(1731225600123));
  event.monotonic_ns = 1500;
  event.type = EventType::NavigationSucceeded;
  event.source = Source::QtWebEngine;
  event.tab_id = 3;
  event.metric = "navigation.duration";
  event.value = 12.5;
  event.unit = "ms";
  event.scope = MeasurementScope::Navigation;
  event.attribution = AttributionQuality::Direct;
  event.attributes.push_back({"url", "https://example.com/a\"b\n\x01"});

  const std::string json = ToJson(event);
  CHECK(Contains(json, "\"schema\":1"));
  CHECK(Contains(json, "\"event_id\":42"));
  CHECK(Contains(json, "\"ts\":\"2024-11-10T08:00:00.123Z\""));
  CHECK(Contains(json, "\"monotonic_ns\":1500"));
  CHECK(Contains(json, "\"type\":\"navigation.succeeded\""));
  CHECK(Contains(json, "\"source\":\"qt_webengine\""));
  CHECK(Contains(json, "\"tab_id\":3"));
  CHECK(Contains(json, "\"navigation_id\":null"));
  CHECK(Contains(json, "\"value\":12.5"));
  CHECK(Contains(json, "\"unit\":\"ms\""));
  CHECK(Contains(json, "\"scope\":\"navigation\""));
  CHECK(Contains(json, "\"availability\":\"available\""));
  CHECK(Contains(json, "\"attribution\":\"direct\""));
  CHECK(Contains(json, "a\\\"b\\n\\u0001"));
  CHECK(json.find('\n') == std::string::npos);

  event.value = std::numeric_limits<double>::quiet_NaN();
  CHECK(Contains(ToJson(event), "\"value\":null"));
  event.value.reset();
  CHECK(Contains(ToJson(event), "\"value\":null"));
  CHECK(FormatUtc(WallClock::time_point{}) == "1970-01-01T00:00:00.000Z");
}

void TestBoundedQueue() {
  BoundedQueue<int> queue(3);
  CHECK(queue.TryPush(1));
  CHECK(queue.TryPush(2));
  CHECK(queue.TryPush(3));
  CHECK(!queue.TryPush(4));
  CHECK(queue.Dropped() == 1);
  CHECK(queue.Size() == 3);
  auto batch = queue.PopBatch(2, std::chrono::milliseconds(0));
  CHECK(batch.size() == 2 && batch[0] == 1 && batch[1] == 2);
  queue.Close();
  CHECK(!queue.TryPush(5));
  batch = queue.PopBatch(10, std::chrono::milliseconds(0));
  CHECK(batch.size() == 1 && batch[0] == 3);
  const auto started = std::chrono::steady_clock::now();
  CHECK(queue.PopBatch(10, std::chrono::seconds(5)).empty());
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));  // Close wakes waiters.

  // Producers never block, even with no consumer.
  BoundedQueue<int> full(8);
  std::atomic<int> accepted{0};
  std::vector<std::thread> producers;
  const auto begin = std::chrono::steady_clock::now();
  for (int t = 0; t < 4; ++t) {
    producers.emplace_back([&full, &accepted] {
      for (int i = 0; i < 10000; ++i) {
        if (full.TryPush(i)) ++accepted;
      }
    });
  }
  for (auto& producer : producers) producer.join();
  CHECK(accepted.load() == 8);
  CHECK(full.Dropped() == 40000 - 8);
  CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(5));
}

void TestPipeline() {
  PipelineConfig config;
  config.history_capacity = 5;
  config.session_id = "test-session";
  TelemetryPipeline pipeline(config);
  CHECK(pipeline.SessionId() == "test-session");
  std::int64_t previous_ns = -1;
  for (int i = 0; i < 8; ++i) {
    TelemetryEvent event;
    event.type = EventType::NetworkMetric;
    event.metric = "net.packets";
    event.value = i;
    const auto id = pipeline.Publish(event);
    CHECK(id == static_cast<std::uint64_t>(i + 1));
  }
  auto events = pipeline.EventsAfter(0, 100);
  CHECK(events.size() == 5);
  CHECK(events.front().event_id == 4 && events.back().event_id == 8);
  for (const auto& event : events) {
    CHECK(event.session_id == "test-session");
    CHECK(event.monotonic_ns >= previous_ns);
    previous_ns = event.monotonic_ns;
  }
  CHECK(pipeline.EventsAfter(6, 100).size() == 2);
  CHECK(pipeline.EventsAfter(6, 1).size() == 1);
  CHECK(pipeline.EventsAfter(8, 100).empty());
  const auto stats = pipeline.Stats();
  CHECK(stats.published == 8);
  CHECK(stats.evicted_from_history == 3);
  CHECK(!stats.jsonl_enabled);
  const auto latest = pipeline.LatestMetric("net.packets");
  CHECK(latest && latest->value && *latest->value == 7.0);
  CHECK(!pipeline.LatestMetric("net.tcp.ack_rtt.mean"));

  // Events without a value do not replace the latest measurement.
  TelemetryEvent missing;
  missing.metric = "net.packets";
  pipeline.Publish(missing);
  CHECK(*pipeline.LatestMetric("net.packets")->value == 7.0);
  CHECK(TelemetryPipeline::GenerateSessionId() != TelemetryPipeline::GenerateSessionId());
}

void TestJsonlWriter() {
  const fs::path directory = FreshDirectory("jsonl");
  {
    JsonlRetention retention;
    retention.max_file_bytes = 1024;
    retention.max_files = 3;
    JsonlTelemetryWriter writer(directory, "capture", retention);
    CHECK(writer.Healthy());
    const std::string line(100, 'x');
    for (int i = 0; i < 100; ++i) CHECK(writer.Write(line));
    writer.Close();
    writer.Close();  // Idempotent.
    CHECK(writer.Written() == 100);
    CHECK(!writer.Write(line));
  }
  int files = 0;
  for (const auto& entry : fs::directory_iterator(directory)) {
    ++files;
    CHECK(entry.file_size() <= 1024);
  }
  CHECK(files == 3);  // Retention bound, oldest deleted.
  CHECK(fs::exists(directory / "capture.jsonl"));
  CHECK(fs::exists(directory / "capture.2.jsonl"));
  CHECK(!fs::exists(directory / "capture.3.jsonl"));

  // A directory that cannot be created reports itself unhealthy, never throws.
  const fs::path blocker = directory / "not-a-directory";
  std::ofstream(blocker) << "file";
  JsonlTelemetryWriter broken(blocker / "child", "capture");
  CHECK(!broken.Healthy());
  CHECK(!broken.Write("line"));
  broken.Close();
}

void TestPipelineJsonl() {
  const fs::path directory = FreshDirectory("pipeline");
  PipelineConfig config;
  config.session_id = "jsonl-session";
  {
    TelemetryPipeline pipeline(config);
    CHECK(pipeline.EnableJsonl(directory));
    CHECK(Contains(pipeline.JsonlPath(), "aafr-telemetry-jsonl-session.jsonl"));
    for (int i = 0; i < 10; ++i) {
      TelemetryEvent event;
      event.type = EventType::TabCreated;
      event.tab_id = static_cast<std::uint64_t>(i);
      pipeline.Publish(event);
    }
    pipeline.Shutdown();
    CHECK(!pipeline.Stats().jsonl_enabled);
    // Publishing after shutdown is still safe and stays in memory.
    pipeline.Publish(TelemetryEvent{});
    CHECK(pipeline.Stats().published == 11);
  }
  const auto lines = ReadLines(directory / "aafr-telemetry-jsonl-session.jsonl");
  CHECK(lines.size() == 10);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    CHECK(lines[i].front() == '{' && lines[i].back() == '}');
    CHECK(Contains(lines[i], "\"event_id\":" + std::to_string(i + 1) + ","));
    CHECK(Contains(lines[i], "\"session_id\":\"jsonl-session\""));
  }
}

void TestTSharkParsing() {
  const auto& fields = aafr::tshark::RequestedFields();
  CHECK(fields.size() == 10);
  const auto args = aafr::tshark::BuildCaptureArguments("Wi-Fi", "tcp or udp");
  std::string joined;
  for (const auto& arg : args) joined += arg + ' ';
  CHECK(Contains(joined, "-i Wi-Fi "));
  CHECK(Contains(joined, "-f tcp or udp "));
  CHECK(Contains(joined, "-T fields "));
  CHECK(Contains(joined, "-e tcp.analysis.ack_rtt "));
  CHECK(Contains(joined, "-e tcp.stream "));
  CHECK(Contains(joined, "-e tcp.analysis.initial_rtt "));
  CHECK(!Contains(joined, "port"));
  // Payload and address fields are never requested.
  CHECK(!Contains(joined, "-x"));
  CHECK(!Contains(joined, "ip.src"));
  CHECK(!Contains(joined, "data"));
  CHECK(!Contains(joined, "http"));
  CHECK(!Contains(std::string(aafr::tshark::BuildCaptureArguments("", "")[0]), "-i"));

  CHECK(!aafr::tshark::ParseFieldsLine("Capturing on 'Wi-Fi'"));
  CHECK(!aafr::tshark::ParseFieldsLine("abc\t60\teth:ip:tcp\t\t\t\t\t\t\t"));
  CHECK(!aafr::tshark::ParseFieldsLine("1.0\t60\teth:ip:tcp\t\t"));
  const auto rtt = aafr::tshark::ParseFieldsLine("1.5\t66\teth:ethertype:ip:tcp\t0.012500,0.3\t\t\t\t\r\t\t");
  CHECK(rtt && rtt->tcp && !rtt->udp && rtt->tcp_ack_rtt_seconds);
  CHECK_NEAR(*rtt->tcp_ack_rtt_seconds, 0.0125, 1e-9);
  const auto negative = aafr::tshark::ParseFieldsLine("1.5\t66\teth:ip:tcp\t-1\t\t\t\t\t\t");
  CHECK(negative && !negative->tcp_ack_rtt_seconds);
  CHECK(negative && !negative->tcp_stream && !negative->tcp_handshake_rtt_seconds);
  const auto handshake = aafr::tshark::ParseFieldsLine("1.5\t66\teth:ip:tcp\t\t\t\t\t\t7\t0.043741300");
  CHECK(handshake && handshake->tcp_stream && *handshake->tcp_stream == 7);
  CHECK(handshake && handshake->tcp_handshake_rtt_seconds);
  if (handshake && handshake->tcp_handshake_rtt_seconds) CHECK_NEAR(*handshake->tcp_handshake_rtt_seconds, 0.0437413, 1e-9);
  const auto badHandshake = aafr::tshark::ParseFieldsLine("1.5\t66\teth:ip:tcp\t\t\t\t\t\t7\t-0.1");
  CHECK(badHandshake && !badHandshake->tcp_handshake_rtt_seconds);

  // TShark "-T fields" sample, inline so no fixture files are needed. Two
  // lines are malformed (status text and a bad timestamp), one is blank.
  const char* const sample[] = {
      "1731225600.000100\t74\teth:ethertype:ip:tcp\t\t\t\t\t\t\t",
      "1731225600.010200\t66\teth:ethertype:ip:tcp\t0.012500\t\t\t\t\t3\t0.030000",
      "1731225600.020300\t1514\teth:ethertype:ip:tcp\t0.020000\t1\t\t\t\t3\t0.030000",
      "1731225600.030400\t1514\teth:ethertype:ip:tcp\t\t1\t\t\t\t\t",
      "1731225600.040500\t80\teth:ethertype:ip:udp:dns\t\t\tFalse\t\t\t\t",
      "1731225600.045600\t120\teth:ethertype:ip:udp:dns\t\t\tTrue\t0.005100\t\t\t",
      "1731225600.050700\t1250\teth:ethertype:ipv6:udp:quic\t\t\t\t\t0x00000001\t\t",
      "1731225600.060800\t1250\teth:ethertype:ipv6:udp:quic\t\t\t\t\t\t\t",
      "Capturing on 'Wi-Fi'",
      "1731225600.070900\t60\teth:ethertype:ip:tcp\t0.008000\t\t\t\t\t4\t0.050000",
      "abc\t60\teth:ethertype:ip:tcp\t\t\t\t\t\t\t",
      "",
  };
  aafr::tshark::IntervalAggregator aggregator;
  for (const char* line : sample) aggregator.AddLine(line);
  const auto summary = aggregator.Flush();
  CHECK(summary.packets == 9);
  CHECK(summary.bytes == 5928);
  CHECK(summary.tcp_packets == 5);
  CHECK(summary.udp_packets == 4);
  CHECK(summary.quic_packets == 2);
  CHECK(summary.dns_responses == 1);
  CHECK(summary.tcp_retransmissions == 2);
  CHECK(summary.malformed_lines == 2);
  CHECK(summary.tcp_ack_rtt_ms.count == 3);
  CHECK_NEAR(summary.tcp_ack_rtt_ms.min, 8.0, 1e-9);
  CHECK_NEAR(summary.tcp_ack_rtt_ms.max, 20.0, 1e-9);
  CHECK_NEAR(summary.tcp_ack_rtt_ms.Mean(), 40.5 / 3.0, 1e-9);
  CHECK_NEAR(summary.tcp_ack_rtt_ms.latest, 8.0, 1e-9);
  // Samples 12.5, 20, 8 ms: consecutive differences 7.5 and 12.
  CHECK_NEAR(summary.tcp_ack_rtt_ms.Jitter(), 9.75, 1e-9);
  // Connection 3 appears on two packets but is sampled once: 30 and 50 ms.
  CHECK(summary.tcp_handshake_rtt_ms.count == 2);
  CHECK_NEAR(summary.tcp_handshake_rtt_ms.min, 30.0, 1e-9);
  CHECK_NEAR(summary.tcp_handshake_rtt_ms.max, 50.0, 1e-9);
  CHECK_NEAR(summary.tcp_handshake_rtt_ms.Mean(), 40.0, 1e-9);
  CHECK(summary.dns_response_time_ms.count == 1);
  CHECK_NEAR(summary.dns_response_time_ms.max, 5.1, 1e-9);
  CHECK(summary.quic_versions.size() == 1 && *summary.quic_versions.begin() == "0x00000001");
  CHECK(summary.total_packets == 9);

  const auto events = aafr::tshark::ToTelemetryEvents(summary, 1000.0);
  for (const auto& event : events) {
    CHECK(event.type == EventType::NetworkMetric);
    CHECK(event.source == Source::TShark);
    CHECK(event.scope == MeasurementScope::CaptureInterface);
    CHECK(event.attribution == AttributionQuality::Unattributed);
    CHECK(event.value.has_value());
    CHECK(!event.unit.empty());
    CHECK(event.FindAttribute("interval_ms") != nullptr);
  }
  const auto* latest = FindMetric(events, "net.tcp.ack_rtt.latest");
  CHECK(latest && *latest->value == 8.0 && latest->unit == "ms");
  CHECK(latest && *latest->FindAttribute("samples") == "3");
  const auto* quic = FindMetric(events, "net.quic.packets");
  CHECK(quic && quic->FindAttribute("quic_versions") && *quic->FindAttribute("quic_versions") == "0x00000001");
  CHECK(FindMetric(events, "net.dns.response_time.mean") != nullptr);
  const auto* latency = FindMetric(events, "net.tcp.handshake_rtt.mean");
  CHECK(latency && latency->unit == "ms" && *latency->FindAttribute("connections") == "2");
  if (latency) CHECK_NEAR(*latency->value, 40.0, 1e-9);
  CHECK(FindMetric(events, "net.tcp.handshake_rtt.min") && FindMetric(events, "net.tcp.handshake_rtt.max"));
  const auto* jitter = FindMetric(events, "net.tcp.ack_rtt.jitter");
  CHECK(jitter && jitter->unit == "ms");
  if (jitter) CHECK_NEAR(*jitter->value, 9.75, 1e-9);
  // 5,928 bytes in one second is 47,424 bit/s; over half a second it doubles.
  const auto* throughput = FindMetric(events, "net.throughput");
  CHECK(throughput && throughput->unit == "Mbps");
  if (throughput) CHECK_NEAR(*throughput->value, 0.047424, 1e-9);
  const auto half = aafr::tshark::ToTelemetryEvents(summary, 500.0);
  CHECK(FindMetric(half, "net.throughput") != nullptr);
  if (FindMetric(half, "net.throughput")) CHECK_NEAR(*FindMetric(half, "net.throughput")->value, 0.094848, 1e-9);
  // No interval length means no rate; a single RTT sample means no jitter.
  CHECK(FindMetric(aafr::tshark::ToTelemetryEvents(summary, 0.0), "net.throughput") == nullptr);
  aafr::tshark::IntervalAggregator single;
  single.AddLine("1.5\t66\teth:ethertype:ip:tcp\t0.012500\t\t\t\t\t\t");
  const auto one = aafr::tshark::ToTelemetryEvents(single.Flush(), 1000.0);
  CHECK(FindMetric(one, "net.tcp.ack_rtt.mean") != nullptr);
  CHECK(FindMetric(one, "net.tcp.ack_rtt.jitter") == nullptr);

  // Totals carry across intervals; a quiet interval has no RTT or DNS timing.
  aggregator.AddLine("1.0\t40\teth:ethertype:ip:udp\t\t\t\t\t\t\t");
  const auto second = aggregator.Flush();
  CHECK(second.packets == 1 && second.total_packets == 10 && second.total_bytes == 5968);
  const auto quiet = aafr::tshark::ToTelemetryEvents(second, 1000.0);
  CHECK(FindMetric(quiet, "net.tcp.ack_rtt.latest") == nullptr);
  CHECK(FindMetric(quiet, "net.dns.response_time.mean") == nullptr);
  CHECK(FindMetric(quiet, "net.tcp.ack_rtt.jitter") == nullptr);
  CHECK(FindMetric(quiet, "net.tcp.handshake_rtt.mean") == nullptr);
  // A connection still active in a later interval is sampled again there.
  aggregator.AddLine("2.0\t60\teth:ethertype:ip:tcp\t\t\t\t\t\t3\t0.030000");
  CHECK(aggregator.Flush().tcp_handshake_rtt_ms.count == 1);
  CHECK(FindMetric(quiet, "net.tcp.retransmissions") && *FindMetric(quiet, "net.tcp.retransmissions")->value == 0.0);
  CHECK(aggregator.Empty());
  for (const auto& event : events) {
    bool listed = false;
    for (const auto& name : aafr::tshark::NetworkMetricNames()) listed = listed || name == event.metric;
    CHECK(listed);
  }
}

void TestInterfaceResolution() {
  // `tshark -D` output from Windows 11 with Npcap (TShark 4.6.9).
  const std::string listing =
      "1. \\Device\\NPF_{743F6A31-D726-40D5-AD65-E4BA6E2861D4} (Local Area Connection* 10)\r\n"
      "2. \\Device\\NPF_{31CAD4CF-D1C0-47C6-8C3A-B5BF054C04F0} (Local Area Connection* 9)\r\n"
      "5. \\Device\\NPF_{B022318B-F74C-4281-82E8-261300B27E9F} (Wi-Fi)\r\n"
      "7. \\Device\\NPF_{4D087B53-DCB7-4BB2-A6B2-11F93E4C1F4C} (Local Area Connection* 1)\r\n"
      "8. \\Device\\NPF_Loopback (Adapter for loopback traffic capture)\r\n"
      "9. \\Device\\NPF_{2AF28E6F-3E18-49D9-9477-9B94601A1D53} (Ethernet)\r\n"
      "10. etwdump (Event Tracing for Windows (ETW) reader)\r\n"
      "11. randpkt\r\n"
      "not an interface line\r\n";
  const auto interfaces = aafr::tshark::ParseInterfaceList(listing);
  CHECK(interfaces.size() == 8);
  CHECK(interfaces[2].number == "5");
  CHECK(interfaces[2].device == "\\Device\\NPF_{B022318B-F74C-4281-82E8-261300B27E9F}");
  CHECK(interfaces[2].name == "Wi-Fi");
  CHECK(interfaces[6].name == "Event Tracing for Windows (ETW) reader");
  CHECK(interfaces[6].device == "etwdump");
  CHECK(interfaces[7].name == "randpkt" && interfaces[7].device == "randpkt");

  using aafr::tshark::ResolveInterface;
  const auto name = [&interfaces](const char* input) {
    const auto found = ResolveInterface(input, interfaces);
    return found ? found->name : std::string("<none>");
  };
  // Exact name, any case, number, device, and loose spellings.
  CHECK(name("Wi-Fi") == "Wi-Fi");
  CHECK(name("wifi") == "Wi-Fi");
  CHECK(name("WI FI") == "Wi-Fi");
  CHECK(name(" 5 ") == "Wi-Fi");
  CHECK(name("\\device\\npf_{b022318b-f74c-4281-82e8-261300b27e9f}") == "Wi-Fi");
  CHECK(name("ethernet") == "Ethernet");
  CHECK(name("Local Area Connection* 1") == "Local Area Connection* 1");
  CHECK(name("local area connection 10") == "Local Area Connection* 10");
  CHECK(name("no-such-adapter") == "<none>");
  CHECK(name("") == "<none>");
  CHECK(name("---") == "<none>");
  // A loose spelling matching two interfaces is refused, not guessed.
  const auto twins = aafr::tshark::ParseInterfaceList("1. dev1 (Wi-Fi)\n2. dev2 (WiFi)\n");
  CHECK(!ResolveInterface("wi fi", twins));
  CHECK(ResolveInterface("WiFi", twins)->device == "dev2");  // Exact still wins.
}

void TestRuntimeContracts() {
  namespace obs = aafr::observability;
  obs::DecisionRecord approved;
  approved.decision_type = obs::DecisionType::SwitchApproved;
  approved.execution = obs::ExecutionConfirmation::Pending;
  obs::DecisionRecord committed = approved;
  committed.execution = obs::ExecutionConfirmation::Committed;
  CHECK(obs::SwitchOutcome(approved) == "Switch approved, awaiting commit");
  CHECK(obs::SwitchOutcome(committed) == "Switch committed");
  CHECK(obs::SwitchOutcome(approved) != obs::SwitchOutcome(committed));
  obs::DecisionRecord rejected;
  rejected.decision_type = obs::DecisionType::SwitchRejected;
  rejected.constraints = {{"topology_supported", true, ""}, {"inputs_available", false, "no link loss"}};
  rejected.stability_gates = {{"min_dwell", false, "1.2 s < 5 s"}, {"confidence", true, ""}};
  CHECK(obs::FailedChecks(rejected) == "inputs_available, min_dwell");
  CHECK(obs::SwitchOutcome(rejected) == "Switch rejected");
  CHECK(obs::FailedChecks(committed).empty());
  CHECK(std::string(obs::ToString(obs::RuntimeState::NotIntegrated)) == "NOT_INTEGRATED");
  obs::RuntimeSnapshot snapshot;
  CHECK(snapshot.state == obs::RuntimeState::NotIntegrated && !snapshot.active_algorithm);
}
}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"serialization", TestSerialization},
      {"bounded_queue", TestBoundedQueue},
      {"pipeline", TestPipeline},
      {"jsonl_writer", TestJsonlWriter},
      {"pipeline_jsonl", TestPipelineJsonl},
      {"tshark_parsing", TestTSharkParsing},
      {"interface_resolution", TestInterfaceResolution},
      {"runtime_contracts", TestRuntimeContracts},
  };
  for (const auto& test : tests) {
    current_test = test.first;
    const int before = failures;
    test.second();
    std::cout << (failures == before ? "PASS " : "FAIL ") << test.first << "\n";
  }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
