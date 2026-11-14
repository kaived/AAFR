// Deterministic tests for the V0.4 algorithm capability registry, the reason
// code catalog and the eligibility evaluator. No Qt, no clock, no capture.
//
// Tests that need an executable algorithm build a registry fixture with
// invented measurements. That fixture exists only here: the built-in registry
// advertises no executable algorithm.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "algorithms/algorithm_capability.h"
#include "algorithms/capability_registry.h"
#include "algorithms/eligibility_evaluator.h"
#include "algorithms/eligibility_reason.h"
#include "network/network_state/network_constraints.h"
#include "tests/support/test_support.h"

using namespace aafr::algorithms;
using namespace aafr::test;
namespace ns = aafr::netstate;
using aafr::telemetry::EventType;
using aafr::telemetry::WallClock;

namespace {

ImplementationProfile MeasuredProfile() {
  ImplementationProfile profile;
  profile.worst_case_runtime_ms = 5.0;
  profile.peak_memory_bytes = 1'000'000;
  profile.max_nodes = 1000;
  profile.max_edges = 10'000;
  profile.execution_environments = {"in_process_cpp"};
  profile.verification_note = "test fixture";
  return profile;
}

// The built-in registry with `ids` marked verified, using fixture measurements.
CapabilityRegistry RegistryWithVerified(std::initializer_list<AlgorithmId> ids) {
  CapabilityRegistry registry = CapabilityRegistry::BuiltIn();
  for (const AlgorithmId id : ids) {
    CHECK(registry.SetImplementation(id, MeasuredProfile()));
    CHECK(registry.SetAvailability(id, ImplementationAvailability::VerifiedExecutable));
  }
  return registry;
}

// A graph topology where a verified Dijkstra, A* or Bellman-Ford can run.
ns::ExecutionEnvironment GraphEnvironment() {
  ns::ExecutionEnvironment environment;
  ns::ExecutionEnvironment::Topology topology;
  topology.available = true;
  topology.info.representations = {ns::TopologyRepresentation::WeightedDirectedGraph};
  topology.info.node_count = 10;
  topology.info.edge_count = 20;
  topology.info.reachable = true;
  topology.info.edge_weights = ns::EdgeWeightInfo{true, false, std::nullopt};
  topology.info.heuristic_available = true;
  environment.topology = topology;
  environment.forwarding_operations = std::set<ns::ForwardingOperation>{ns::ForwardingOperation::SelectPath};
  environment.supported_protocols = std::set<ns::TransportProtocol>{ns::TransportProtocol::Tcp};
  environment.problem = ns::ProblemRepresentation::SinglePairShortestPath;
  environment.objective = ns::OptimizationObjective::MinimizeAdditivePathCost;
  environment.computation_deadline_ms = 50.0;
  environment.memory_budget_bytes = 64'000'000;
  environment.algorithm_configuration["astar"] = {{"heuristic", "zero"}};
  return environment;
}

EligibilityResult Evaluate(const CapabilityRegistry& registry, AlgorithmId id,
                           const ns::ExecutionEnvironment& environment,
                           const ns::NetworkState& state, EligibilityPolicy policy = {}) {
  const ns::NetworkConstraints constraints = ns::ConstraintEvaluator().Evaluate(state, environment);
  const AlgorithmCapability* capability = registry.Find(id);
  CHECK(capability != nullptr);
  return AlgorithmEligibilityEvaluator(policy).Evaluate(state, constraints, *capability, environment);
}

EligibilityResult Evaluate(const CapabilityRegistry& registry, AlgorithmId id,
                           const ns::ExecutionEnvironment& environment) {
  return Evaluate(registry, id, environment, FreshCaptureState(environment));
}

CheckOutcome OutcomeOf(const EligibilityResult& result, EligibilityCheck check) {
  const CheckResult* found = result.Find(check);
  CHECK(found != nullptr);
  return found ? found->outcome : CheckOutcome::Unknown;
}

// ---- Capability registry ----

void TestRegistryProfiles() {
  const CapabilityRegistry registry = CapabilityRegistry::BuiltIn();
  CHECK(registry.Size() == 10);
  std::set<std::string> ids;
  std::set<std::string> names;
  for (const AlgorithmId id : kAllAlgorithmIds) {
    const AlgorithmCapability* capability = registry.Find(id);
    CHECK(capability != nullptr);
    if (!capability) continue;
    CHECK(capability->id == id);
    CHECK(capability->schema_version == kCapabilitySchemaVersion);
    CHECK(ids.insert(ToString(id)).second);
    CHECK(names.insert(capability->display_name).second);
    CHECK(ValidateProfile(*capability).empty());
    CHECK(registry.Find(std::string(ToString(id))) == capability);
    CHECK(ParseAlgorithmId(ToString(id)) == id);
    const AlgorithmProperties& p = capability->properties;
    CHECK(!p.problems.empty() && !p.topologies.empty() && !p.objectives.empty());
    CHECK(!p.time_complexity.empty() && !p.space_complexity.empty());
    CHECK(!p.limitations.empty());

    // No false executable claims: a profile is metadata, not an implementation.
    CHECK(capability->availability == ImplementationAvailability::ImplementationUnavailable);
    CHECK(!capability->implementation.worst_case_runtime_ms.has_value());
    CHECK(!capability->implementation.peak_memory_bytes.has_value());
    CHECK(capability->implementation.execution_environments.empty());
    CHECK(ToObservabilityCapability(*capability).availability ==
          aafr::observability::ImplementationAvailability::NotImplemented);
  }
  CHECK(ids.size() == 10);
  CHECK(ids.count("dijkstra") && ids.count("astar") && ids.count("bellman_ford") && ids.count("aco") &&
        ids.count("pso") && ids.count("ga") && ids.count("abc") && ids.count("gwo") &&
        ids.count("firefly") && ids.count("woa"));
  CHECK(registry.ExecutableAlgorithms().empty());
  CHECK(registry.All().size() == 10);
  CHECK(registry.All().front().id == AlgorithmId::Dijkstra);

  // Unknown algorithms.
  CHECK(registry.Find(std::string("simulated_annealing")) == nullptr);
  CHECK(registry.Find(std::string("")) == nullptr);
  CHECK(!ParseAlgorithmId("Dijkstra").has_value());
  CHECK(CapabilityRegistry().Find(AlgorithmId::Dijkstra) == nullptr);
}

void TestProfileMetadata() {
  const CapabilityRegistry registry = CapabilityRegistry::BuiltIn();
  const AlgorithmProperties& dijkstra = registry.Find(AlgorithmId::Dijkstra)->properties;
  CHECK(dijkstra.family == AlgorithmFamily::ClassicalShortestPath);
  CHECK(dijkstra.edge_weights == EdgeWeightRequirement::NonNegative);
  CHECK(dijkstra.optimality == OptimalityGuarantee::Exact);
  CHECK(dijkstra.deterministic && !dijkstra.requires_heuristic && !dijkstra.detects_negative_cycles);
  CHECK(dijkstra.topologies.count(ns::TopologyRepresentation::WeightedDirectedGraph) == 1);
  CHECK(dijkstra.topologies.count(ns::TopologyRepresentation::CandidatePathSet) == 0);
  CHECK(dijkstra.required_configuration.empty());

  const AlgorithmProperties& astar = registry.Find(AlgorithmId::AStar)->properties;
  CHECK(astar.requires_heuristic);
  CHECK(astar.optimality == OptimalityGuarantee::ConditionalOnHeuristic);
  CHECK(astar.required_configuration == std::vector<std::string>{"heuristic"});
  CHECK(astar.problems.count(ns::ProblemRepresentation::SingleSourceShortestPath) == 0);

  const AlgorithmProperties& bellman = registry.Find(AlgorithmId::BellmanFord)->properties;
  CHECK(bellman.edge_weights == EdgeWeightRequirement::NoNegativeCycle);
  CHECK(bellman.detects_negative_cycles);
  CHECK(bellman.optimality == OptimalityGuarantee::Exact);
  CHECK(bellman.time_complexity != dijkstra.time_complexity);

  for (const AlgorithmId id :
       {AlgorithmId::AntColonyOptimization, AlgorithmId::ParticleSwarmOptimization,
        AlgorithmId::GeneticAlgorithm, AlgorithmId::ArtificialBeeColony, AlgorithmId::GreyWolfOptimizer,
        AlgorithmId::FireflyAlgorithm, AlgorithmId::WhaleOptimizationAlgorithm}) {
    const AlgorithmProperties& p = registry.Find(id)->properties;
    CHECK(p.family == AlgorithmFamily::Metaheuristic);
    CHECK(!p.deterministic);
    CHECK(p.optimality == OptimalityGuarantee::None);
    CHECK(p.edge_weights == EdgeWeightRequirement::DefinedByObjective);
    CHECK(p.problems.count(ns::ProblemRepresentation::CandidateSolutionOptimization) == 1);
    // An explicit solution representation, an objective and a budget.
    for (const char* key : {"solution_encoding", "objective_function", "max_iterations", "random_seed"}) {
      CHECK(std::find(p.required_configuration.begin(), p.required_configuration.end(), key) !=
            p.required_configuration.end());
    }
    CHECK(p.required_configuration.size() > 4);
    // No profile claims superiority under any network condition.
    for (const std::string& limitation : p.limitations) {
      CHECK(!Contains(limitation, "best") && !Contains(limitation, "superior") &&
            !Contains(limitation, "outperform"));
    }
  }
  // The ten profiles are not interchangeable.
  CHECK(registry.Find(AlgorithmId::AntColonyOptimization)->properties.topologies !=
        registry.Find(AlgorithmId::ParticleSwarmOptimization)->properties.topologies);
  CHECK(registry.Find(AlgorithmId::FireflyAlgorithm)->properties.time_complexity !=
        registry.Find(AlgorithmId::GreyWolfOptimizer)->properties.time_complexity);

  const auto viewer = ToObservabilityCapability(*registry.Find(AlgorithmId::Dijkstra));
  CHECK(viewer.algorithm_id == "dijkstra");
  CHECK(viewer.required_inputs == dijkstra.required_features);
  CHECK(viewer.resources.time_complexity == dijkstra.time_complexity);
  CHECK(!viewer.resources.memory_bytes.has_value());
}

void TestRegistryValidationAndAvailability() {
  CapabilityRegistry registry = CapabilityRegistry::BuiltIn();
  std::vector<std::string> errors;

  // Duplicate ids are refused.
  CHECK(!registry.Register(*registry.Find(AlgorithmId::Dijkstra), &errors));
  CHECK(errors.size() == 1 && Contains(errors.front(), "already registered"));
  CHECK(registry.Size() == 10);

  // Malformed profiles are refused.
  AlgorithmCapability broken = *registry.Find(AlgorithmId::Dijkstra);
  broken.display_name.clear();
  broken.properties.problems.clear();
  broken.properties.required_features = {"not.a.feature"};
  CapabilityRegistry empty;
  CHECK(!empty.Register(broken, &errors));
  CHECK(errors.size() == 3);
  CHECK(empty.Size() == 0);
  AlgorithmCapability overclaiming = *registry.Find(AlgorithmId::GeneticAlgorithm);
  overclaiming.properties.optimality = OptimalityGuarantee::Exact;
  CHECK(!ValidateProfile(overclaiming).empty());
  AlgorithmCapability no_heuristic = *registry.Find(AlgorithmId::AStar);
  no_heuristic.properties.required_configuration.clear();
  CHECK(!ValidateProfile(no_heuristic).empty());

  // Metadata alone can never make an algorithm executable.
  CHECK(!registry.SetAvailability(AlgorithmId::Dijkstra, ImplementationAvailability::VerifiedExecutable,
                                  &errors));
  CHECK(errors.size() == 4);  // Runtime, memory, environment and verification note.
  CHECK(registry.Find(AlgorithmId::Dijkstra)->availability ==
        ImplementationAvailability::ImplementationUnavailable);
  CHECK(registry.ExecutableAlgorithms().empty());

  // Other states are tracked without evidence.
  CHECK(registry.SetAvailability(AlgorithmId::Dijkstra, ImplementationAvailability::ImplementationPresent));
  CHECK(registry.Find(AlgorithmId::Dijkstra)->availability ==
        ImplementationAvailability::ImplementationPresent);
  CHECK(ToObservabilityCapability(*registry.Find(AlgorithmId::Dijkstra)).availability ==
        aafr::observability::ImplementationAvailability::Experimental);
  CHECK(registry.ExecutableAlgorithms().empty());
  CHECK(registry.SetAvailability(AlgorithmId::WhaleOptimizationAlgorithm,
                                 ImplementationAvailability::Unsupported));

  // With measured evidence the claim is accepted, for that algorithm only.
  ImplementationProfile partial = MeasuredProfile();
  partial.verification_note.clear();
  CHECK(registry.SetImplementation(AlgorithmId::Dijkstra, partial));
  CHECK(!registry.SetAvailability(AlgorithmId::Dijkstra, ImplementationAvailability::VerifiedExecutable));
  CHECK(registry.SetImplementation(AlgorithmId::Dijkstra, MeasuredProfile()));
  CHECK(registry.SetAvailability(AlgorithmId::Dijkstra, ImplementationAvailability::VerifiedExecutable));
  CHECK(registry.ExecutableAlgorithms() == std::vector<AlgorithmId>{AlgorithmId::Dijkstra});
  CHECK(ToObservabilityCapability(*registry.Find(AlgorithmId::Dijkstra)).availability ==
        aafr::observability::ImplementationAvailability::Available);
  // Evidence cannot be withdrawn from under a verified claim.
  CHECK(!registry.SetImplementation(AlgorithmId::Dijkstra, ImplementationProfile{}));
  CHECK(registry.Find(AlgorithmId::Dijkstra)->implementation.worst_case_runtime_ms.has_value());
  // Impossible measurements are refused.
  ImplementationProfile impossible = MeasuredProfile();
  impossible.worst_case_runtime_ms = -1.0;
  CHECK(!registry.SetImplementation(AlgorithmId::AStar, impossible));
  CHECK(!CapabilityRegistry().SetAvailability(AlgorithmId::AStar, ImplementationAvailability::Unsupported));

  CHECK(std::string(ToString(ImplementationAvailability::ProfileDefined)) == "profile_defined");
  CHECK(std::string(ToString(ImplementationAvailability::VerifiedExecutable)) == "verified_executable");
}

void TestReasonCatalog() {
  std::set<std::string> texts;
  for (const ReasonCode code : AllReasonCodes()) {
    const std::string text = ToString(code);
    CHECK(!text.empty() && text != "UNKNOWN_REASON");
    CHECK(texts.insert(text).second);
    CHECK(std::string(Describe(code)).size() > 10);
    // Stable machine-readable form: upper case with underscores.
    CHECK(text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_") == std::string::npos);
  }
  CHECK(texts.size() == 33);
  for (const char* required :
       {"REQUIRED_FEATURE_MISSING", "TOPOLOGY_UNAVAILABLE", "UNSUPPORTED_EDGE_WEIGHTS",
        "COMPUTATION_BUDGET_EXCEEDED", "MEMORY_BUDGET_EXCEEDED", "IMPLEMENTATION_UNAVAILABLE",
        "FORWARDING_CAPABILITY_UNAVAILABLE", "STALE_NETWORK_STATE", "ELIGIBILITY_CONFIRMED"}) {
    CHECK(texts.count(required) == 1);
  }
  CHECK(std::string(ToString(ReasonCode::EligibilityConfirmed)) == "ELIGIBILITY_CONFIRMED");
  CHECK(ReasonCodesText({ReasonCode::TopologyUnavailable, ReasonCode::StaleNetworkState}) ==
        "TOPOLOGY_UNAVAILABLE, STALE_NETWORK_STATE");
}

// ---- Eligibility ----

void TestEligible() {
  const CapabilityRegistry registry = RegistryWithVerified({AlgorithmId::Dijkstra});
  const ns::ExecutionEnvironment environment = GraphEnvironment();
  const ns::NetworkState state = FreshCaptureState(environment);
  const EligibilityResult result = Evaluate(registry, AlgorithmId::Dijkstra, environment, state);
  CHECK(result.algorithm == AlgorithmId::Dijkstra);
  CHECK(result.network_state_id == state.state_id);
  CHECK(result.status == EligibilityStatus::Eligible);
  CHECK(result.reason_codes == std::vector<ReasonCode>{ReasonCode::EligibilityConfirmed});
  CHECK(result.checks.size() == 10);
  CHECK(result.ChecksWith(CheckOutcome::Passed).size() == 10);
  CHECK(result.ChecksWith(CheckOutcome::Failed).empty());
  CHECK(result.ChecksWith(CheckOutcome::Unknown).empty());
  CHECK(result.missing_information.empty());
  for (std::size_t i = 0; i < result.checks.size(); ++i) {
    CHECK(static_cast<std::size_t>(result.checks[i].check) == i);
    CHECK(result.checks[i].reason == ReasonCode::CheckPassed);
    CHECK(!result.checks[i].explanation.empty());
  }
  // Eligibility is not a performance claim.
  CHECK(Contains(result.explanation, "does not mean best"));
  // Explanations quote the compared values.
  CHECK(Contains(result.Find(EligibilityCheck::ComputationBudget)->explanation, "5 ms"));
  CHECK(Contains(result.Find(EligibilityCheck::ComputationBudget)->explanation, "50 ms"));

  // Deterministic: the same inputs give the same result.
  const EligibilityResult again = Evaluate(registry, AlgorithmId::Dijkstra, environment, state);
  CHECK(again.status == result.status && again.explanation == result.explanation &&
        again.reason_codes == result.reason_codes);

  // In the same environment, the nine unimplemented algorithms are not eligible.
  const ns::NetworkConstraints constraints = ns::ConstraintEvaluator().Evaluate(state, environment);
  const auto all = AlgorithmEligibilityEvaluator().EvaluateAll(state, constraints, registry, environment);
  CHECK(all.size() == 10);
  int eligible = 0;
  int not_implemented = 0;
  for (std::size_t i = 0; i < all.size(); ++i) {
    CHECK(all[i].algorithm == kAllAlgorithmIds[i]);
    CHECK(!all[i].reason_codes.empty() && !all[i].explanation.empty());
    eligible += all[i].status == EligibilityStatus::Eligible;
    not_implemented += all[i].status == EligibilityStatus::NotImplemented;
  }
  CHECK(eligible == 1 && not_implemented == 9);
}

void TestNotImplemented() {
  const CapabilityRegistry registry = CapabilityRegistry::BuiltIn();
  // Even in an environment where everything else is satisfiable.
  const EligibilityResult graph = Evaluate(registry, AlgorithmId::Dijkstra, GraphEnvironment());
  CHECK(graph.status == EligibilityStatus::NotImplemented);
  CHECK(graph.reason_codes.front() == ReasonCode::ImplementationUnavailable);
  CHECK(OutcomeOf(graph, EligibilityCheck::ImplementationAvailability) == CheckOutcome::Failed);
  CHECK(Contains(graph.explanation, "Not implemented"));
  // Without an implementation its cost is unknown, never assumed to fit.
  CHECK(OutcomeOf(graph, EligibilityCheck::ComputationBudget) == CheckOutcome::Unknown);
  CHECK(graph.HasReason(ReasonCode::ComputationCostUnknown));
  CHECK(graph.HasReason(ReasonCode::MemoryCostUnknown));
  CHECK(!graph.HasReason(ReasonCode::EligibilityConfirmed));

  // The browser as shipped: nothing is eligible, and each result says why.
  const ns::ExecutionEnvironment browser = ns::BrowserDefaultEnvironment();
  const ns::NetworkState state = FreshCaptureState(browser);
  const ns::NetworkConstraints constraints = ns::ConstraintEvaluator().Evaluate(state, browser);
  for (const EligibilityResult& result :
       AlgorithmEligibilityEvaluator().EvaluateAll(state, constraints, registry, browser)) {
    CHECK(result.status == EligibilityStatus::NotImplemented);
    CHECK(result.HasReason(ReasonCode::ImplementationUnavailable));
    CHECK(result.HasReason(ReasonCode::TopologyUnavailable));
    CHECK(result.HasReason(ReasonCode::ForwardingCapabilityUnavailable));
    CHECK(result.HasReason(ReasonCode::ProblemUndefined));
    CHECK(!result.missing_information.empty());
    CHECK(result.checks.size() == 10);
  }

  // Present but unverified, and unsupported, are ineligible, not "not implemented".
  CapabilityRegistry staged = CapabilityRegistry::BuiltIn();
  staged.SetAvailability(AlgorithmId::Dijkstra, ImplementationAvailability::ImplementationPresent);
  staged.SetAvailability(AlgorithmId::AStar, ImplementationAvailability::Unsupported);
  staged.SetAvailability(AlgorithmId::BellmanFord, ImplementationAvailability::ProfileDefined);
  const EligibilityResult unverified = Evaluate(staged, AlgorithmId::Dijkstra, GraphEnvironment());
  CHECK(unverified.status == EligibilityStatus::Ineligible);
  CHECK(unverified.reason_codes.front() == ReasonCode::ImplementationUnverified);
  const EligibilityResult unsupported = Evaluate(staged, AlgorithmId::AStar, GraphEnvironment());
  CHECK(unsupported.status == EligibilityStatus::Ineligible);
  CHECK(unsupported.reason_codes.front() == ReasonCode::ImplementationUnsupported);
  CHECK(Evaluate(staged, AlgorithmId::BellmanFord, GraphEnvironment()).status ==
        EligibilityStatus::NotImplemented);
}

void TestFeaturesAndDataQuality() {
  const CapabilityRegistry registry = RegistryWithVerified({AlgorithmId::Dijkstra});
  const ns::ExecutionEnvironment environment = GraphEnvironment();

  // No telemetry at all.
  ns::NetworkStateBuilder empty({}, environment);
  const EligibilityResult missing = Evaluate(registry, AlgorithmId::Dijkstra, environment,
                                             empty.Build(0, WallClock::time_point{}));
  CHECK(missing.status == EligibilityStatus::Ineligible);
  CHECK(missing.HasReason(ReasonCode::RequiredFeatureMissing));
  CHECK(missing.HasReason(ReasonCode::NetworkStateUnusable));
  CHECK(OutcomeOf(missing, EligibilityCheck::RequiredFeatures) == CheckOutcome::Failed);
  CHECK(std::find(missing.missing_information.begin(), missing.missing_information.end(),
                  "rtt.handshake.mean_ms") != missing.missing_information.end());
  CHECK(Contains(missing.Find(EligibilityCheck::RequiredFeatures)->explanation, "has not been started"));

  // Browser telemetry only (no TShark): the state is usable but latency is missing.
  ns::NetworkStateBuilder browser_only({}, environment);
  for (int i = 1; i <= 3; ++i) {
    browser_only.Ingest(Navigation(EventType::NavigationSucceeded, 100.0 * i, i * 1000));
  }
  const EligibilityResult reduced = Evaluate(registry, AlgorithmId::Dijkstra, environment,
                                             browser_only.Build(Ns(3000), WallClock::time_point{}));
  CHECK(reduced.status == EligibilityStatus::Ineligible);
  CHECK(reduced.reason_codes == std::vector<ReasonCode>{ReasonCode::RequiredFeatureMissing});
  CHECK(OutcomeOf(reduced, EligibilityCheck::DataQuality) == CheckOutcome::Passed);

  // Stale measurements.
  ns::NetworkStateBuilder stale({}, environment);
  stale.Ingest(Capture(EventType::CaptureStarted, 0));
  for (int second = 1; second <= 5; ++second) IngestInterval(stale, second * 1000);
  stale.Ingest(Navigation(EventType::NavigationSucceeded, 100.0, 5000));
  const EligibilityResult old = Evaluate(registry, AlgorithmId::Dijkstra, environment,
                                         stale.Build(Ns(40'000), WallClock::time_point{}));
  CHECK(old.status == EligibilityStatus::Ineligible);
  CHECK(old.HasReason(ReasonCode::RequiredFeatureUnusable));
  CHECK(old.HasReason(ReasonCode::StaleNetworkState));
  CHECK(Contains(old.Find(EligibilityCheck::RequiredFeatures)->explanation, "stale"));
  CHECK(Contains(old.Find(EligibilityCheck::DataQuality)->explanation, "35000 ms old"));

  // Too few samples for the required statistic.
  ns::NetworkStateBuilder sparse({}, environment);
  sparse.Ingest(Capture(EventType::CaptureStarted, 0));
  IngestInterval(sparse, 1000);
  const EligibilityResult thin = Evaluate(registry, AlgorithmId::Dijkstra, environment,
                                          sparse.Build(Ns(1000), WallClock::time_point{}));
  CHECK(thin.HasReason(ReasonCode::RequiredFeatureUnusable));
  CHECK(Contains(thin.Find(EligibilityCheck::RequiredFeatures)->explanation, "insufficient_samples"));
}

void TestTopologyAndProblem() {
  const CapabilityRegistry registry = RegistryWithVerified(
      {AlgorithmId::Dijkstra, AlgorithmId::AStar, AlgorithmId::AntColonyOptimization});

  // Missing topology: stated as absent.
  ns::ExecutionEnvironment none = GraphEnvironment();
  none.topology = ns::ExecutionEnvironment::Topology{};
  EligibilityResult result = Evaluate(registry, AlgorithmId::Dijkstra, none);
  CHECK(result.status == EligibilityStatus::Ineligible);
  CHECK(result.HasReason(ReasonCode::TopologyUnavailable));
  // With no topology its edge weights are unknown, not assumed fine.
  CHECK(OutcomeOf(result, EligibilityCheck::EdgeWeights) == CheckOutcome::Unknown);

  // Unknown topology: undecidable, not eligible.
  ns::ExecutionEnvironment unknown = GraphEnvironment();
  unknown.topology.reset();
  result = Evaluate(registry, AlgorithmId::Dijkstra, unknown);
  CHECK(result.status == EligibilityStatus::Unknown);
  CHECK(result.HasReason(ReasonCode::TopologyUnknown));
  CHECK(result.HasReason(ReasonCode::EdgeWeightsUnknown));
  CHECK(result.ChecksWith(CheckOutcome::Failed).empty());
  CHECK(std::find(result.missing_information.begin(), result.missing_information.end(),
                  ns::constraint_id::kTopologyAvailable) != result.missing_information.end());
  CHECK(Contains(result.explanation, "cannot be decided"));

  // Unsupported topology representation.
  ns::ExecutionEnvironment paths = GraphEnvironment();
  paths.topology->info.representations = {ns::TopologyRepresentation::CandidatePathSet};
  paths.topology->info.candidate_path_count = 4;
  result = Evaluate(registry, AlgorithmId::Dijkstra, paths);
  CHECK(result.status == EligibilityStatus::Ineligible);
  CHECK(result.HasReason(ReasonCode::UnsupportedTopology));
  CHECK(Contains(result.Find(EligibilityCheck::TopologyInformation)->explanation, "candidate_path_set"));

  // Unsupported problem and objective.
  ns::ExecutionEnvironment search = GraphEnvironment();
  search.problem = ns::ProblemRepresentation::CandidateSolutionOptimization;
  CHECK(Evaluate(registry, AlgorithmId::Dijkstra, search)
            .HasReason(ReasonCode::UnsupportedProblemRepresentation));
  ns::ExecutionEnvironment scalar = GraphEnvironment();
  scalar.objective = ns::OptimizationObjective::MinimizeScalarObjective;
  CHECK(Evaluate(registry, AlgorithmId::Dijkstra, scalar).HasReason(ReasonCode::UnsupportedObjective));
  // A* answers single-pair queries only.
  ns::ExecutionEnvironment all_pairs = GraphEnvironment();
  all_pairs.problem = ns::ProblemRepresentation::SingleSourceShortestPath;
  CHECK(Evaluate(registry, AlgorithmId::Dijkstra, all_pairs).status == EligibilityStatus::Eligible);
  CHECK(Evaluate(registry, AlgorithmId::AStar, all_pairs)
            .HasReason(ReasonCode::UnsupportedProblemRepresentation));
  ns::ExecutionEnvironment undefined = GraphEnvironment();
  undefined.problem.reset();
  result = Evaluate(registry, AlgorithmId::Dijkstra, undefined);
  CHECK(result.status == EligibilityStatus::Unknown && result.HasReason(ReasonCode::ProblemUndefined));

  // A metaheuristic needs its own problem form and its configuration.
  result = Evaluate(registry, AlgorithmId::AntColonyOptimization, GraphEnvironment());
  CHECK(result.status == EligibilityStatus::Ineligible);
  CHECK(result.HasReason(ReasonCode::UnsupportedProblemRepresentation));
  CHECK(result.HasReason(ReasonCode::RequiredConfigurationMissing));
  CHECK(OutcomeOf(result, EligibilityCheck::EdgeWeights) == CheckOutcome::Passed);
  ns::ExecutionEnvironment configured = GraphEnvironment();
  configured.problem = ns::ProblemRepresentation::CandidateSolutionOptimization;
  configured.objective = ns::OptimizationObjective::MinimizeScalarObjective;
  for (const std::string& key :
       registry.Find(AlgorithmId::AntColonyOptimization)->properties.required_configuration) {
    configured.algorithm_configuration["aco"][key] = "1";
  }
  CHECK(Evaluate(registry, AlgorithmId::AntColonyOptimization, configured).status ==
        EligibilityStatus::Eligible);
  configured.algorithm_configuration["aco"]["random_seed"].clear();
  result = Evaluate(registry, AlgorithmId::AntColonyOptimization, configured);
  CHECK(result.reason_codes == std::vector<ReasonCode>{ReasonCode::RequiredConfigurationMissing});
  CHECK(result.missing_information == std::vector<std::string>{"config:random_seed"});
}

void TestEdgeWeightsAndHeuristic() {
  const CapabilityRegistry registry = RegistryWithVerified(
      {AlgorithmId::Dijkstra, AlgorithmId::AStar, AlgorithmId::BellmanFord});

  ns::ExecutionEnvironment negative = GraphEnvironment();
  negative.topology->info.edge_weights = ns::EdgeWeightInfo{true, true, false};
  EligibilityResult result = Evaluate(registry, AlgorithmId::Dijkstra, negative);
  CHECK(result.status == EligibilityStatus::Ineligible);
  CHECK(result.reason_codes == std::vector<ReasonCode>{ReasonCode::UnsupportedEdgeWeights});
  CHECK(Contains(result.explanation, "negative edge weights"));
  // Bellman-Ford accepts negative weights when no negative cycle exists.
  CHECK(Evaluate(registry, AlgorithmId::BellmanFord, negative).status == EligibilityStatus::Eligible);

  negative.topology->info.edge_weights->has_negative_cycle = true;
  result = Evaluate(registry, AlgorithmId::BellmanFord, negative);
  CHECK(result.status == EligibilityStatus::Ineligible);
  CHECK(result.HasReason(ReasonCode::NegativeCyclePresent));

  negative.topology->info.edge_weights->has_negative_cycle.reset();
  result = Evaluate(registry, AlgorithmId::BellmanFord, negative);
  CHECK(result.status == EligibilityStatus::Unknown);
  CHECK(result.HasReason(ReasonCode::EdgeWeightsUnknown));
  // Dijkstra fails regardless of the cycle question.
  CHECK(Evaluate(registry, AlgorithmId::Dijkstra, negative).status == EligibilityStatus::Ineligible);

  ns::ExecutionEnvironment broken = GraphEnvironment();
  broken.topology->info.edge_weights = ns::EdgeWeightInfo{false, false, std::nullopt};
  CHECK(Evaluate(registry, AlgorithmId::BellmanFord, broken).HasReason(ReasonCode::UnsupportedEdgeWeights));
  ns::ExecutionEnvironment undescribed = GraphEnvironment();
  undescribed.topology->info.edge_weights.reset();
  result = Evaluate(registry, AlgorithmId::Dijkstra, undescribed);
  CHECK(result.status == EligibilityStatus::Unknown && result.HasReason(ReasonCode::EdgeWeightsUnknown));

  // A*: configuration and heuristic.
  CHECK(Evaluate(registry, AlgorithmId::AStar, GraphEnvironment()).status == EligibilityStatus::Eligible);
  ns::ExecutionEnvironment unconfigured = GraphEnvironment();
  unconfigured.algorithm_configuration.clear();
  result = Evaluate(registry, AlgorithmId::AStar, unconfigured);
  CHECK(result.status == EligibilityStatus::Ineligible);
  CHECK(result.HasReason(ReasonCode::RequiredConfigurationMissing));
  CHECK(result.missing_information == std::vector<std::string>{"config:heuristic"});
  ns::ExecutionEnvironment no_heuristic = GraphEnvironment();
  no_heuristic.topology->info.heuristic_available = false;
  CHECK(Evaluate(registry, AlgorithmId::AStar, no_heuristic).HasReason(ReasonCode::HeuristicUnavailable));
  no_heuristic.topology->info.heuristic_available.reset();
  result = Evaluate(registry, AlgorithmId::AStar, no_heuristic);
  CHECK(result.status == EligibilityStatus::Unknown && result.HasReason(ReasonCode::HeuristicUnknown));
  // Dijkstra needs no heuristic and is unaffected.
  CHECK(Evaluate(registry, AlgorithmId::Dijkstra, no_heuristic).status == EligibilityStatus::Eligible);
}

void TestBudgetsAndForwarding() {
  const CapabilityRegistry registry = RegistryWithVerified({AlgorithmId::Dijkstra});

  ns::ExecutionEnvironment tight = GraphEnvironment();
  tight.computation_deadline_ms = 1.0;
  EligibilityResult result = Evaluate(registry, AlgorithmId::Dijkstra, tight);
  CHECK(result.status == EligibilityStatus::Ineligible);
  CHECK(result.reason_codes == std::vector<ReasonCode>{ReasonCode::ComputationBudgetExceeded});
  CHECK(Contains(result.explanation, "5 ms exceeds the deadline of 1 ms"));

  ns::ExecutionEnvironment small = GraphEnvironment();
  small.memory_budget_bytes = 4096;
  result = Evaluate(registry, AlgorithmId::Dijkstra, small);
  CHECK(result.reason_codes == std::vector<ReasonCode>{ReasonCode::MemoryBudgetExceeded});

  // Budgets that are not configured are unknown, not unlimited.
  ns::ExecutionEnvironment open = GraphEnvironment();
  open.computation_deadline_ms.reset();
  open.memory_budget_bytes.reset();
  result = Evaluate(registry, AlgorithmId::Dijkstra, open);
  CHECK(result.status == EligibilityStatus::Unknown);
  CHECK(result.HasReason(ReasonCode::ComputationBudgetUnknown));
  CHECK(result.HasReason(ReasonCode::MemoryBudgetUnknown));
  // The explicit policy for unknown information.
  EligibilityPolicy strict;
  strict.unknown_is_ineligible = true;
  result = Evaluate(registry, AlgorithmId::Dijkstra, open, FreshCaptureState(open), strict);
  CHECK(result.status == EligibilityStatus::Ineligible);
  CHECK(result.ChecksWith(CheckOutcome::Unknown).size() == 2);
  CHECK(result.ChecksWith(CheckOutcome::Failed).empty());

  // A runtime measured on small topologies does not cover a larger one.
  ns::ExecutionEnvironment large = GraphEnvironment();
  large.topology->info.node_count = 5000;
  result = Evaluate(registry, AlgorithmId::Dijkstra, large);
  CHECK(result.status == EligibilityStatus::Unknown);
  CHECK(result.HasReason(ReasonCode::ComputationCostUnknown));

  // A contradictory constraint fails the check that depends on it.
  ns::ExecutionEnvironment contradictory = GraphEnvironment();
  contradictory.latency_budget_ms = 10.0;  // Shorter than the 50 ms deadline.
  result = Evaluate(registry, AlgorithmId::Dijkstra, contradictory);
  CHECK(result.status == EligibilityStatus::Ineligible);
  CHECK(result.HasReason(ReasonCode::ConstraintInvalid));
  CHECK(OutcomeOf(result, EligibilityCheck::ComputationBudget) == CheckOutcome::Failed);

  // Forwarding execution.
  ns::ExecutionEnvironment inert = GraphEnvironment();
  inert.forwarding_operations = std::set<ns::ForwardingOperation>{};
  result = Evaluate(registry, AlgorithmId::Dijkstra, inert);
  CHECK(result.reason_codes == std::vector<ReasonCode>{ReasonCode::ForwardingCapabilityUnavailable});
  inert.forwarding_operations = std::set<ns::ForwardingOperation>{ns::ForwardingOperation::SelectEndpoint};
  CHECK(Evaluate(registry, AlgorithmId::Dijkstra, inert)
            .HasReason(ReasonCode::ForwardingCapabilityUnavailable));
  inert.forwarding_operations.reset();
  result = Evaluate(registry, AlgorithmId::Dijkstra, inert);
  CHECK(result.status == EligibilityStatus::Unknown);
  CHECK(result.HasReason(ReasonCode::ForwardingCapabilityUnknown));

  ns::ExecutionEnvironment quic = GraphEnvironment();
  quic.required_protocols = {ns::TransportProtocol::Quic};
  CHECK(Evaluate(registry, AlgorithmId::Dijkstra, quic).HasReason(ReasonCode::UnsupportedProtocol));
  quic.supported_protocols.reset();
  CHECK(Evaluate(registry, AlgorithmId::Dijkstra, quic).HasReason(ReasonCode::ProtocolSupportUnknown));
}

void TestMultipleFailures() {
  const CapabilityRegistry registry = RegistryWithVerified({AlgorithmId::Dijkstra});
  ns::ExecutionEnvironment environment = GraphEnvironment();
  environment.topology->info.edge_weights = ns::EdgeWeightInfo{true, true, false};
  environment.computation_deadline_ms = 1.0;
  environment.memory_budget_bytes = 4096;
  environment.forwarding_operations = std::set<ns::ForwardingOperation>{};
  environment.objective.reset();
  const EligibilityResult result = Evaluate(registry, AlgorithmId::Dijkstra, environment);
  CHECK(result.status == EligibilityStatus::Ineligible);
  // Failed checks in rule order, then unknown ones.
  CHECK(result.reason_codes ==
        (std::vector<ReasonCode>{ReasonCode::UnsupportedEdgeWeights, ReasonCode::ComputationBudgetExceeded,
                                 ReasonCode::MemoryBudgetExceeded,
                                 ReasonCode::ForwardingCapabilityUnavailable,
                                 ReasonCode::ObjectiveUndefined}));
  CHECK(result.ChecksWith(CheckOutcome::Failed) ==
        (std::vector<EligibilityCheck>{EligibilityCheck::EdgeWeights, EligibilityCheck::ComputationBudget,
                                       EligibilityCheck::MemoryBudget,
                                       EligibilityCheck::ForwardingExecution}));
  CHECK(result.ChecksWith(CheckOutcome::Unknown) ==
        std::vector<EligibilityCheck>{EligibilityCheck::ProblemRepresentation});
  CHECK(result.ChecksWith(CheckOutcome::Passed).size() == 5);
  // The explanation names every failed check with its code.
  for (const char* expected : {"edge_weights [UNSUPPORTED_EDGE_WEIGHTS]",
                               "computation_budget [COMPUTATION_BUDGET_EXCEEDED]",
                               "memory_budget [MEMORY_BUDGET_EXCEEDED]",
                               "forwarding_execution [FORWARDING_CAPABILITY_UNAVAILABLE]",
                               "problem_representation [OBJECTIVE_UNDEFINED]"}) {
    CHECK(Contains(result.explanation, expected));
  }
  CHECK(std::string(ToString(EligibilityStatus::NotImplemented)) == "not_implemented");
}

}  // namespace

int main() {
  return RunTests({
      {"registry_profiles", TestRegistryProfiles},
      {"profile_metadata", TestProfileMetadata},
      {"registry_validation_and_availability", TestRegistryValidationAndAvailability},
      {"reason_catalog", TestReasonCatalog},
      {"eligible", TestEligible},
      {"not_implemented", TestNotImplemented},
      {"features_and_data_quality", TestFeaturesAndDataQuality},
      {"topology_and_problem", TestTopologyAndProblem},
      {"edge_weights_and_heuristic", TestEdgeWeightsAndHeuristic},
      {"budgets_and_forwarding", TestBudgetsAndForwarding},
      {"multiple_failures", TestMultipleFailures},
  });
}
