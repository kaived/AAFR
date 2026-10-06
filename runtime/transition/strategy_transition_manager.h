#pragma once

#include "runtime/interfaces/interfaces.h"

namespace ngr {

class StrategyTransitionManager final : public ITransitionPolicy {
 public:
  TransitionDecision Evaluate(const DecisionResult& proposal,
                              const TransitionState& state,
                              const TransitionGateConfig& config) const override;
};

}  // namespace ngr
