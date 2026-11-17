// V0.7.1 performance-scoring reconciliation regressions.
//
// The reference repository scores paths with a z-scored, demand-capped
// "Goodness" and an ML link weight; its audit found a lower-bandwidth fast
// path wrongly penalised, latency parity masking loss, and weights that
// saturate to zero below 10 ms. AAFR keeps its own objective (J(P) v1:
// additive latency + -ln(1 - loss) + hop + policy, bandwidth as a hard
// per-edge constraint) and these tests pin down that it has none of those
// defects, with no score imported from the reference.

#include <cmath>

#include "routing/topology/forwarding_problem.h"
#include "runtime/adaptive/performance_evaluator.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
namespace fwd = aafr::forwarding;
using aafr::test::kPathA;
using aafr::test::kPathB;

namespace {

// Two paths entry->relay_a->dest (A) and entry->relay_b->dest (B), each
// edge with the given latency, loss and bandwidth.
struct EdgeSpec {
  std::optional<double> latency;
  std::optional<double> loss;
  std::optional<double> bandwidth;
};

fwd::ForwardingGraph Graph(EdgeSpec a, EdgeSpec b) {
  fwd::ForwardingGraph g("scoring");
  g.AddNode({"entry", fwd::NodeRole::Entry, true});
  g.AddNode({"relay_a", fwd::NodeRole::Relay, true});
  g.AddNode({"relay_b", fwd::NodeRole::Relay, true});
  g.AddNode({"dest", fwd::NodeRole::Destination, true});
  const auto m = [](std::optional<double> v) {
    return v ? fwd::Measurement::Of(*v, fwd::Provenance::Configured) : fwd::Measurement{};
  };
  for (const auto& [relay, spec] : {std::pair<const char*, EdgeSpec>{"relay_a", a}, {"relay_b", b}}) {
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"entry", relay}, {relay, "dest"}}) {
      fwd::ForwardingEdge e;
      e.from = from;
      e.to = to;
      e.latency_ms = m(spec.latency);
      e.loss_ratio = m(spec.loss);
      e.bandwidth_mbps = m(spec.bandwidth);
      g.AddEdge(e);
    }
  }
  return g;
}

fwd::ForwardingProblem Problem(const fwd::ForwardingGraph& g, std::optional<double> demand_mbps = std::nullopt) {
  fwd::ForwardingProblem p = aafr::test::ProblemFor(g);
  p.constraints.min_bandwidth_mbps = demand_mbps;
  return p;
}

// The path with the lower J(P) among those that validate and are feasible.
std::optional<fwd::Path> Best(const fwd::ForwardingGraph& g, const fwd::ForwardingProblem& p) {
  std::optional<fwd::Path> best;
  std::optional<double> best_j;
  for (const fwd::Path& path : {kPathA, kPathB}) {
    const fwd::PathValidation v = fwd::ValidatePath(g, p, path);
    if (!v.valid || !v.feasible || !v.objective) continue;
    if (!best_j || *v.objective < *best_j) {
      best_j = v.objective;
      best = path;
    }
  }
  return best;
}

// 1. A lower-latency path whose (lower) bandwidth still covers the demand is
//    chosen: capacity beyond the demand is no benefit and no penalty.
void LowerLatencySufficientBandwidth() {
  const auto g = Graph({2.0, 0.0, 10.0}, {20.0, 0.0, 1000.0});
  CHECK(Best(g, Problem(g, 2.0)) == kPathA);   // demand 2 Mb/s: A suffices
  CHECK(Best(g, Problem(g)) == kPathA);        // no demand known
  // Only when the demand exceeds A's capacity does B win, and then because
  // A is infeasible, not because of a score blend.
  const fwd::PathValidation v = fwd::ValidatePath(g, Problem(g, 50.0), kPathA);
  CHECK(!v.valid && v.Has(fwd::PathIssue::BandwidthBelowMinimum));
  CHECK(Best(g, Problem(g, 50.0)) == kPathB);
}

// 2. Equal latency, different loss: the lossless path scores better.
void EqualLatencyDifferentLoss() {
  const auto g = Graph({5.0, 0.05, 100.0}, {5.0, 0.0, 100.0});
  const auto p = Problem(g);
  const auto a = fwd::ValidatePath(g, p, kPathA);
  const auto b = fwd::ValidatePath(g, p, kPathB);
  CHECK(*a.objective > *b.objective);
  // Loss enters as -ln(1-p) per edge with weight 100: 2 * 100 * -ln(0.95).
  CHECK_NEAR(*a.objective - *b.objective, 200.0 * -std::log(0.95), 1e-9);
  CHECK_NEAR(a.delivery_probability, 0.95 * 0.95, 1e-12);
  CHECK(Best(g, p) == kPathB);
}

// 3. Huge bandwidth does not buy back excessive latency, and a latency
//    budget makes such a path infeasible rather than merely penalised.
void HighBandwidthExcessiveLatency() {
  const auto g = Graph({5.0, 0.0, 10.0}, {200.0, 0.0, 10'000.0});
  auto p = Problem(g, 5.0);
  CHECK(Best(g, p) == kPathA);
  p.constraints.latency_budget_ms = 100.0;
  const auto b = fwd::ValidatePath(g, p, kPathB);
  CHECK(b.valid && !b.feasible && b.Has(fwd::PathIssue::LatencyBudgetExceeded));
  CHECK(b.objective.has_value());  // score still reported, never silently penalised
}

// 4. Congestion and recovery move the score both ways, symmetrically.
void CongestedPathRecovers() {
  auto g = Graph({2.0, 0.0, 100.0}, {10.0, 0.0, 100.0});
  const auto p0 = Problem(g);
  const double before = *fwd::ValidatePath(g, p0, kPathA).objective;
  CHECK(Best(g, p0) == kPathA);
  g.SetEdgeLatency("entry", "relay_a", fwd::Measurement::Of(60.0, fwd::Provenance::Measured));
  CHECK(Best(g, Problem(g)) == kPathB);
  g.SetEdgeLatency("entry", "relay_a", fwd::Measurement::Of(2.0, fwd::Provenance::Measured));
  const auto p2 = Problem(g);
  CHECK(Best(g, p2) == kPathA);
  CHECK_NEAR(*fwd::ValidatePath(g, p2, kPathA).objective, before, 1e-12);  // no residual penalty
}

// 5. Missing throughput: AAFR never substitutes a value. Without a demand
//    constraint bandwidth is not part of J(P); with one, an edge of unknown
//    bandwidth is unusable (AttributeUnknown), never assumed sufficient.
//    Measured goodput with no successful request is reported missing.
void MissingThroughput() {
  const auto g = Graph({2.0, 0.0, std::nullopt}, {10.0, 0.0, 100.0});
  CHECK(Best(g, Problem(g)) == kPathA);
  const auto v = fwd::ValidatePath(g, Problem(g, 5.0), kPathA);
  CHECK(!v.valid && v.Has(fwd::PathIssue::AttributeUnknown));
  CHECK(Best(g, Problem(g, 5.0)) == kPathB);
  // Unknown latency or loss makes an edge unusable, never zero-cost.
  const auto unknown = Graph({std::nullopt, 0.0, 100.0}, {10.0, 0.0, 100.0});
  CHECK(!fwd::ValidatePath(unknown, Problem(unknown), kPathA).valid);
  ForwardingPerformanceEvaluator evaluator;
  const ForwardingPerformance perf = evaluator.Summarize({{false, true, 1500.0, 64}, {false, false, 0.0, 64}});
  CHECK(!perf.latency_median_ms.has_value());
  CHECK(!perf.goodput_bytes_per_s.has_value());
  CHECK(!perf.missing.empty());
}

// 6. Stale link weights: a path validated against a graph version it was
//    not computed for is rejected, and J(P) from different graph versions
//    or objective versions is never compared.
void StaleLinkWeights() {
  auto g = Graph({2.0, 0.0, 100.0}, {10.0, 0.0, 100.0});
  const auto pinned = Problem(g);  // pinned to the current version
  g.SetEdgeLatency("entry", "relay_a", fwd::Measurement::Of(50.0, fwd::Provenance::Measured));
  const auto v = fwd::ValidatePath(g, pinned, kPathA);
  CHECK(!v.valid && v.Has(fwd::PathIssue::GraphVersionMismatch));
  std::string why;
  CHECK(!ForwardingPerformanceEvaluator::CompatibleObjectives({"p", 1, 1, 3.0}, {"p", 2, 1, 4.0}, &why));
  CHECK(!why.empty());
  CHECK(!ForwardingPerformanceEvaluator::CompatibleObjectives({"p", 1, 1, 3.0}, {"p", 1, 2, 4.0}));
  CHECK(ForwardingPerformanceEvaluator::CompatibleObjectives({"p", 1, 1, 3.0}, {"p", 1, 1, 4.0}));
}

// 7. Very low latencies stay distinguishable (the reference regressor
//    saturated to 0 below 10 ms): J(P) is linear in latency.
void VeryLowLatencyDifferentiation() {
  const auto g = Graph({0.05, 0.0, 100.0}, {0.10, 0.0, 100.0});
  const auto p = Problem(g);
  const double a = *fwd::ValidatePath(g, p, kPathA).objective;
  const double b = *fwd::ValidatePath(g, p, kPathB).objective;
  CHECK_NEAR(b - a, 0.10, 1e-12);  // 2 edges x 0.05 ms difference, weight 1 per ms
  CHECK(Best(g, p) == kPathA);
  // Zero latency is valid and still ordered by the other terms.
  const auto z = Graph({0.0, 0.0, 100.0}, {0.0, 0.01, 100.0});
  CHECK(Best(z, Problem(z)) == kPathA);
}

// 8. Zero or near-zero denominators: relative improvements and evaluator
//    summaries never divide by zero or produce non-finite values.
void ZeroDenominators() {
  // All-zero costs: J(P) = hop term only, finite.
  fwd::ForwardingProblem p;
  const auto g = Graph({0.0, 0.0, 100.0}, {0.0, 0.0, 100.0});
  p = Problem(g);
  p.objective.hop_weight = 0.0;
  const auto v = fwd::ValidatePath(g, p, kPathA);
  CHECK(v.valid && v.objective && std::isfinite(*v.objective) && *v.objective == 0.0);
  // Objective weights: all-zero is rejected, negatives are rejected.
  fwd::ForwardingObjective o;
  o.latency_weight = o.loss_weight = o.hop_weight = 0.0;
  o.policy_weight = 0.0;
  CHECK(!o.Validate().empty());
  o = {};
  o.loss_weight = -1.0;
  CHECK(!o.Validate().empty());
  // Zero-duration samples (sub-resolution timing) keep jitter and goodput finite.
  ForwardingPerformanceEvaluator evaluator;
  const ForwardingPerformance perf = evaluator.Summarize({{true, false, 0.0, 64}, {true, false, 0.0, 64}});
  CHECK(perf.latency_median_ms && *perf.latency_median_ms == 0.0);
  if (perf.jitter_ms) CHECK(std::isfinite(*perf.jitter_ms));
  if (perf.goodput_bytes_per_s) CHECK(std::isfinite(*perf.goodput_bytes_per_s));
  // Loss close to 1 stays finite (the graph rejects loss >= 1 itself).
  const auto lossy = Graph({1.0, 0.999999, 100.0}, {1.0, 0.0, 100.0});
  const auto lv = fwd::ValidatePath(lossy, Problem(lossy), kPathA);
  CHECK(lv.objective && std::isfinite(*lv.objective));
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"lower_latency_sufficient_bandwidth", LowerLatencySufficientBandwidth},
      {"equal_latency_different_loss", EqualLatencyDifferentLoss},
      {"high_bandwidth_excessive_latency", HighBandwidthExcessiveLatency},
      {"congested_path_recovers", CongestedPathRecovers},
      {"missing_throughput", MissingThroughput},
      {"stale_link_weights", StaleLinkWeights},
      {"very_low_latency_differentiation", VeryLowLatencyDifferentiation},
      {"zero_denominators", ZeroDenominators},
  });
}
