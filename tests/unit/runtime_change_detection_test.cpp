// V0.6 network change detection and evaluation scheduling.

#include "runtime/adaptive/network_change_detector.h"

#include <algorithm>

#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using aafr::test::MakeFeature;
using aafr::test::MakeState;

namespace {

constexpr const char* kRtt = "rtt.handshake.median_ms";
constexpr const char* kRetx = "tcp.retransmission_ratio";

ChangeObservation Obs(std::int64_t ms, const netstate::NetworkState* state, const forwarding::ForwardingGraph* graph = nullptr,
                      const netstate::NetworkConstraints* constraints = nullptr) {
  ChangeDetectorConfig config;
  return MakeObservation(ms * 1'000'000, state, constraints, graph, config);
}

bool HasTrigger(const ChangeAssessment& a, EvaluationTrigger t) {
  return std::find(a.triggers.begin(), a.triggers.end(), t) != a.triggers.end();
}

const FeatureChange* Find(const ChangeAssessment& a, const std::string& id) {
  for (const auto& f : a.features)
    if (f.feature_id == id) return &f;
  return nullptr;
}

void MeaningfulChangeNeedsSustain() {
  NetworkChangeDetector d;
  auto s1 = MakeState(1, {MakeFeature(kRtt, 20.0)});
  auto a = d.Observe(Obs(0, &s1));
  CHECK(!a.meaningful);
  CHECK(Find(a, kRtt)->status == "baseline_set");
  // 20 -> 60: |40| >= max(10, 0.3*20=6) -> first observation only pending.
  auto s2 = MakeState(2, {MakeFeature(kRtt, 60.0)});
  a = d.Observe(Obs(1000, &s2));
  CHECK(!a.meaningful);
  CHECK(Find(a, kRtt)->status == "pending_sustain");
  auto s3 = MakeState(3, {MakeFeature(kRtt, 62.0)});
  a = d.Observe(Obs(2000, &s3));
  CHECK(a.meaningful);
  CHECK(HasTrigger(a, EvaluationTrigger::MeaningfulChange));
  CHECK(std::find(a.reasons.begin(), a.reasons.end(), RuntimeReason::NetworkChangeMeaningful) != a.reasons.end());
  CHECK(aafr::test::Contains(a.explanation, kRtt));
  // The baseline moved to 62: staying there is no change.
  auto s4 = MakeState(4, {MakeFeature(kRtt, 63.0)});
  a = d.Observe(Obs(3000, &s4));
  CHECK(!a.meaningful);
  CHECK(Find(a, kRtt)->status == "within_threshold");
}

void MinorFluctuationIgnored() {
  NetworkChangeDetector d;
  const double values[] = {20, 24, 17, 26, 21, 15, 23};  // all within 10 ms of 20
  std::uint64_t id = 1;
  for (double v : values) {
    auto s = MakeState(id, {MakeFeature(kRtt, v)});
    const auto a = d.Observe(Obs(static_cast<std::int64_t>(id) * 1000, &s));
    CHECK(!a.meaningful);
    CHECK(std::find(a.reasons.begin(), a.reasons.end(), RuntimeReason::NetworkChangeIgnored) != a.reasons.end());
    ++id;
  }
  // A single spike followed by a return is not sustained.
  auto spike = MakeState(id, {MakeFeature(kRtt, 90.0)});
  CHECK(!d.Observe(Obs(10'000, &spike)).meaningful);
  auto back = MakeState(id + 1, {MakeFeature(kRtt, 21.0)});
  const auto a = d.Observe(Obs(11'000, &back));
  CHECK(!a.meaningful);
  CHECK(Find(a, kRtt)->streak == 0);
}

void MissingAndStaleMeasurements() {
  NetworkChangeDetector d;
  auto s1 = MakeState(1, {MakeFeature(kRtt, 20.0)});
  d.Observe(Obs(0, &s1));
  auto missing = MakeState(2, {MakeFeature(kRtt, std::nullopt, netstate::FeatureQuality::Missing)});
  auto a = d.Observe(Obs(1000, &missing));
  CHECK(!a.meaningful);
  CHECK(Find(a, kRtt)->status == "unusable:missing");
  auto stale = MakeState(3, {MakeFeature(kRtt, 500.0, netstate::FeatureQuality::Stale)});
  a = d.Observe(Obs(2000, &stale));
  CHECK(!a.meaningful);
  CHECK(Find(a, kRtt)->status == "unusable:stale");
  // Valid quality but too old by age.
  auto old = MakeState(4, {MakeFeature(kRtt, 500.0, netstate::FeatureQuality::Valid, 60'000.0)});
  a = d.Observe(Obs(3000, &old));
  CHECK(Find(a, kRtt)->status == "stale_age");
  CHECK(!a.meaningful);
  // Insufficient samples.
  auto few = MakeState(5, {MakeFeature(kRtt, 500.0, netstate::FeatureQuality::InsufficientSamples)});
  CHECK(!d.Observe(Obs(4000, &few)).meaningful);
  // No NetworkState at all is not a change.
  CHECK(!d.Observe(Obs(5000, nullptr)).meaningful);
}

void ZeroBaselineUsesAbsoluteFloor() {
  NetworkChangeDetector d;
  auto s1 = MakeState(1, {MakeFeature(kRetx, 0.0)});
  d.Observe(Obs(0, &s1));
  // 0 -> 0.005 is an infinite relative change but below the 0.02 floor.
  for (std::uint64_t i = 2; i < 6; ++i) {
    auto s = MakeState(i, {MakeFeature(kRetx, 0.005)});
    CHECK(!d.Observe(Obs(static_cast<std::int64_t>(i) * 1000, &s)).meaningful);
  }
  auto s6 = MakeState(6, {MakeFeature(kRetx, 0.08)});
  d.Observe(Obs(6000, &s6));
  auto s7 = MakeState(7, {MakeFeature(kRetx, 0.09)});
  CHECK(d.Observe(Obs(7000, &s7)).meaningful);
}

void DuplicateAndRapidUpdates() {
  NetworkChangeDetector d;
  auto s1 = MakeState(1, {MakeFeature(kRtt, 20.0)});
  d.Observe(Obs(0, &s1));
  // The same snapshot observed repeatedly (fast ticks) counts once.
  for (int i = 0; i < 5; ++i) {
    const auto a = d.Observe(Obs(10 * (i + 1), &s1));
    CHECK(a.duplicate);
    CHECK(!a.meaningful);
  }
}

void ConstraintAndInterfaceChanges() {
  NetworkChangeDetector d;
  auto s1 = MakeState(1, {MakeFeature(kRtt, 20.0)}, "Wi-Fi");
  netstate::NetworkConstraints c1;
  netstate::Constraint fresh;
  fresh.id = netstate::constraint_id::kStateFresh;
  fresh.type = netstate::ConstraintType::Boolean;
  fresh.value = true;
  fresh.validity = netstate::ConstraintValidity::Valid;
  c1.constraints.push_back(fresh);
  d.Observe(Obs(0, &s1, nullptr, &c1));
  netstate::NetworkConstraints c2 = c1;
  c2.constraints[0].value = false;
  auto s2 = MakeState(2, {MakeFeature(kRtt, 20.0)}, "Wi-Fi");
  auto a = d.Observe(Obs(1000, &s2, nullptr, &c2));
  CHECK(a.meaningful);
  CHECK(a.constraint_changed);
  CHECK(HasTrigger(a, EvaluationTrigger::ConstraintChange));
  // The list of available measurements is not a constraint change.
  netstate::NetworkConstraints c3 = c2;
  netstate::Constraint measurements;
  measurements.id = netstate::constraint_id::kAvailableMeasurements;
  measurements.type = netstate::ConstraintType::List;
  measurements.value = std::vector<std::string>{"rtt"};
  measurements.validity = netstate::ConstraintValidity::Valid;
  c3.constraints.push_back(measurements);
  auto s3 = MakeState(3, {MakeFeature(kRtt, 20.0)}, "Wi-Fi");
  CHECK(!d.Observe(Obs(2000, &s3, nullptr, &c3)).meaningful);
  // Interface change: meaningful, and baselines restart.
  auto s4 = MakeState(4, {MakeFeature(kRtt, 80.0)}, "Ethernet");
  a = d.Observe(Obs(3000, &s4, nullptr, &c3));
  CHECK(a.meaningful);
  CHECK(a.interface_changed);
  CHECK(Find(a, kRtt)->status == "baseline_set");
}

void TopologyChanges() {
  NetworkChangeDetector d;
  aafr::test::GraphHolder holder(aafr::test::TwoPathGraph());
  d.Observe(Obs(0, nullptr, holder.Get().get()));
  // 1.0 -> 1.5 ms: below max(2 ms, 25%).
  holder.SetLatency("entry", "relay_a", 1.5);
  auto a = d.Observe(Obs(1000, nullptr, holder.Get().get()));
  CHECK(!a.meaningful);
  // Small drifts are measured against the original baseline, so they
  // cannot accumulate unseen: 1.0 -> 3.5 crosses 2 ms.
  holder.SetLatency("entry", "relay_a", 3.5);
  a = d.Observe(Obs(2000, nullptr, holder.Get().get()));
  CHECK(a.meaningful);
  CHECK(HasTrigger(a, EvaluationTrigger::TopologyChange));
  CHECK(!a.topology_changes.empty());
  holder.Mutate([](forwarding::ForwardingGraph& g) { g.SetEdgeAvailable("relay_b", "dest", false); });
  a = d.Observe(Obs(3000, nullptr, holder.Get().get()));
  CHECK(a.meaningful);
  CHECK(aafr::test::Contains(a.explanation, "relay_b->dest unavailable"));
  // Graph version unchanged: duplicate.
  CHECK(d.Observe(Obs(4000, nullptr, holder.Get().get())).duplicate);
}

void SchedulerCoalescesAndSpaces() {
  EvaluationScheduler s(SchedulerConfig{1000.0});
  s.Request(EvaluationTrigger::MeaningfulChange, 0, "a");
  auto p = s.Take(0);
  CHECK(p.has_value());
  // Rapid requests are coalesced into one pending evaluation.
  for (int i = 0; i < 10; ++i) s.Request(EvaluationTrigger::MeaningfulChange, (i + 1) * 10'000'000, "burst");
  s.Request(EvaluationTrigger::ConstraintChange, 150'000'000, "constraint");
  CHECK(!s.Take(200'000'000).has_value());  // within the minimum interval
  p = s.Take(1'100'000'000);
  CHECK(p.has_value());
  CHECK(p->requests == 11);
  CHECK(p->trigger == EvaluationTrigger::ConstraintChange);  // highest priority kept
  CHECK(p->coalesced.count(EvaluationTrigger::MeaningfulChange) == 1);
  CHECK(s.coalesced_requests() == 10);
  CHECK(!s.HasPending());
  // A safety trigger ignores the spacing.
  s.Request(EvaluationTrigger::FailureRecovery, 1'200'000'000, "failed");
  CHECK(s.Take(1'200'000'000).has_value());
}

void ConfigValidation() {
  ChangeDetectorConfig c;
  CHECK(c.Validate().empty());
  c.features.push_back({"x", 0.0, 0.5});  // zero absolute floor is rejected
  CHECK(!c.Validate().empty());
  c = ChangeDetectorConfig{};
  c.sustain_observations = 0;
  CHECK(!c.Validate().empty());
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"meaningful_change_needs_sustain", MeaningfulChangeNeedsSustain},
      {"minor_fluctuation_ignored", MinorFluctuationIgnored},
      {"missing_and_stale_measurements", MissingAndStaleMeasurements},
      {"zero_baseline_uses_absolute_floor", ZeroBaselineUsesAbsoluteFloor},
      {"duplicate_and_rapid_updates", DuplicateAndRapidUpdates},
      {"constraint_and_interface_changes", ConstraintAndInterfaceChanges},
      {"topology_changes", TopologyChanges},
      {"scheduler_coalesces_and_spaces", SchedulerCoalescesAndSpaces},
      {"config_validation", ConfigValidation},
  });
}
