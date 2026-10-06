#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/interfaces/interfaces.h"

namespace ngr {

struct CandidatePolicy {
  StrategyId id;
  std::unordered_map<std::string, double> weights;
};

struct SubsystemPolicy {
  StrategyId default_strategy;
  std::vector<CandidatePolicy> candidates;
};

class PolicyRegistry {
 public:
  void SetDecisionModelVersion(std::string version);
  const std::string& DecisionModelVersion() const;

  void SetPolicy(SubsystemId subsystem, SubsystemPolicy policy);
  const SubsystemPolicy* FindPolicy(SubsystemId subsystem) const;

 private:
  std::string decision_model_version_{"aafr-seed-0.1"};
  std::map<SubsystemId, SubsystemPolicy> policies_;
};

class WeightedDecisionModel final : public IDecisionModel {
 public:
  DecisionResult Evaluate(const EnvironmentSnapshot& snap,
                          SubsystemId subsystem,
                          const PolicyRegistry& registry) const override;

 private:
  static double FeatureValue(const EnvironmentSnapshot& snap,
                             SubsystemId subsystem,
                             const std::string& feature);
};

}  // namespace ngr
