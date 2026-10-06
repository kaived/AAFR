#pragma once

#include <memory>
#include <string>
#include <vector>

#include "runtime/common/types.h"

namespace ngr {

class PolicyRegistry;

class ISensor {
 public:
  virtual ~ISensor() = default;
  virtual void Sample(EnvironmentSnapshot& out) = 0;
  virtual std::string Name() const = 0;
};

class IStrategy {
 public:
  virtual ~IStrategy() = default;
  virtual StrategyId Id() const = 0;
  virtual void Apply(RequestContext& ctx) = 0;
};

template <typename TStrategy>
class IStrategyPool {
 public:
  virtual ~IStrategyPool() = default;
  virtual void Register(std::unique_ptr<TStrategy> strategy) = 0;
  virtual TStrategy* Get(const StrategyId& id) const = 0;
  virtual std::vector<StrategyId> AllIds() const = 0;
};

class IDecisionModel {
 public:
  virtual ~IDecisionModel() = default;
  virtual DecisionResult Evaluate(const EnvironmentSnapshot& snap,
                                  SubsystemId subsystem,
                                  const PolicyRegistry& registry) const = 0;
};

class ITransitionPolicy {
 public:
  virtual ~ITransitionPolicy() = default;
  virtual TransitionDecision Evaluate(const DecisionResult& proposal,
                                      const TransitionState& state,
                                      const TransitionGateConfig& config) const = 0;
};

class ITelemetrySink {
 public:
  virtual ~ITelemetrySink() = default;
  virtual void Record(const TransitionDecision& decision) = 0;
};

}  // namespace ngr
