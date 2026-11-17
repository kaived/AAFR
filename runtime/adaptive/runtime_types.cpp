#include "runtime/adaptive/runtime_types.h"

namespace aafr::runtime {

bool IsValidTransition(RuntimeLifecycle from, RuntimeLifecycle to) {
  using L = RuntimeLifecycle;
  switch (from) {
    case L::Uninitialized: return to == L::Initializing || to == L::Stopping;
    case L::Initializing: return to == L::Ready || to == L::Degraded || to == L::Failed || to == L::Stopping;
    case L::Ready: return to == L::Evaluating || to == L::Degraded || to == L::Stopping;
    case L::Evaluating: return to == L::Ready || to == L::Switching || to == L::Degraded || to == L::Stopping;
    case L::Switching: return to == L::Ready || to == L::Degraded || to == L::Stopping;
    case L::Degraded: return to == L::Evaluating || to == L::Stopping || to == L::Failed;
    case L::Stopping: return to == L::Stopped;
    case L::Stopped: return false;
    case L::Failed: return to == L::Stopping;
  }
  return false;
}

const char* ToString(RuntimeLifecycle value) {
  switch (value) {
    case RuntimeLifecycle::Uninitialized: return "uninitialized";
    case RuntimeLifecycle::Initializing: return "initializing";
    case RuntimeLifecycle::Ready: return "ready";
    case RuntimeLifecycle::Evaluating: return "evaluating";
    case RuntimeLifecycle::Switching: return "switching";
    case RuntimeLifecycle::Degraded: return "degraded";
    case RuntimeLifecycle::Stopping: return "stopping";
    case RuntimeLifecycle::Stopped: return "stopped";
    case RuntimeLifecycle::Failed: return "failed";
  }
  return "unknown";
}

const char* ToString(EvaluationTrigger value) {
  switch (value) {
    case EvaluationTrigger::Startup: return "startup";
    case EvaluationTrigger::MeaningfulChange: return "meaningful_change";
    case EvaluationTrigger::ConstraintChange: return "constraint_change";
    case EvaluationTrigger::TopologyChange: return "topology_change";
    case EvaluationTrigger::FailureRecovery: return "failure_recovery";
    case EvaluationTrigger::Manual: return "manual";
    case EvaluationTrigger::Periodic: return "periodic";
  }
  return "unknown";
}

int Priority(EvaluationTrigger value) {
  switch (value) {
    case EvaluationTrigger::FailureRecovery: return 6;
    case EvaluationTrigger::Startup: return 5;
    case EvaluationTrigger::TopologyChange: return 4;
    case EvaluationTrigger::ConstraintChange: return 3;
    case EvaluationTrigger::MeaningfulChange: return 2;
    case EvaluationTrigger::Manual: return 1;
    case EvaluationTrigger::Periodic: return 0;
  }
  return 0;
}

bool IsSafetyTrigger(EvaluationTrigger value) {
  return value == EvaluationTrigger::FailureRecovery || value == EvaluationTrigger::Startup;
}

const char* ToString(RuntimeEventType value) {
  switch (value) {
    case RuntimeEventType::RuntimeInitialized: return "RuntimeInitialized";
    case RuntimeEventType::LifecycleChanged: return "LifecycleChanged";
    case RuntimeEventType::NetworkChangeDetected: return "NetworkChangeDetected";
    case RuntimeEventType::EvaluationStarted: return "EvaluationStarted";
    case RuntimeEventType::RecommendationGenerated: return "RecommendationGenerated";
    case RuntimeEventType::ProbingStarted: return "ProbingStarted";
    case RuntimeEventType::ProbingCompleted: return "ProbingCompleted";
    case RuntimeEventType::StatisticalEvaluationCompleted: return "StatisticalEvaluationCompleted";
    case RuntimeEventType::AlgorithmRetained: return "AlgorithmRetained";
    case RuntimeEventType::SwitchRequested: return "SwitchRequested";
    case RuntimeEventType::SwitchCommitted: return "SwitchCommitted";
    case RuntimeEventType::SwitchFailed: return "SwitchFailed";
    case RuntimeEventType::RecoveryStarted: return "RecoveryStarted";
    case RuntimeEventType::RecoveryCompleted: return "RecoveryCompleted";
    case RuntimeEventType::EvaluationDiscarded: return "EvaluationDiscarded";
  }
  return "unknown";
}

}  // namespace aafr::runtime
