#include "runtime/observability/runtime_contracts.h"

namespace aafr::observability {

const char* ToString(ExecutionState value) {
  switch (value) {
    case ExecutionState::Unknown: return "UNKNOWN";
    case ExecutionState::Initializing: return "INITIALIZING";
    case ExecutionState::Active: return "ACTIVE";
    case ExecutionState::Draining: return "DRAINING";
    case ExecutionState::Inactive: return "INACTIVE";
    case ExecutionState::Failed: return "FAILED";
  }
  return "UNKNOWN";
}

const char* ToString(ImplementationAvailability value) {
  switch (value) {
    case ImplementationAvailability::NotImplemented: return "NOT_IMPLEMENTED";
    case ImplementationAvailability::Experimental: return "EXPERIMENTAL";
    case ImplementationAvailability::Available: return "AVAILABLE";
  }
  return "UNKNOWN";
}

const char* ToString(DecisionType value) {
  switch (value) {
    case DecisionType::Retain: return "RETAIN";
    case DecisionType::SwitchApproved: return "SWITCH_APPROVED";
    case DecisionType::SwitchRejected: return "SWITCH_REJECTED";
  }
  return "UNKNOWN";
}

const char* ToString(ExecutionConfirmation value) {
  switch (value) {
    case ExecutionConfirmation::NotApplicable: return "NOT_APPLICABLE";
    case ExecutionConfirmation::Pending: return "PENDING";
    case ExecutionConfirmation::Committed: return "COMMITTED";
    case ExecutionConfirmation::Failed: return "FAILED";
    case ExecutionConfirmation::RolledBack: return "ROLLED_BACK";
  }
  return "UNKNOWN";
}

const char* ToString(RuntimeState value) {
  switch (value) {
    case RuntimeState::NotIntegrated: return "NOT_INTEGRATED";
    case RuntimeState::Starting: return "STARTING";
    case RuntimeState::Running: return "RUNNING";
    case RuntimeState::Degraded: return "DEGRADED";
    case RuntimeState::Stopped: return "STOPPED";
  }
  return "UNKNOWN";
}

std::string SwitchOutcome(const DecisionRecord& record) {
  switch (record.decision_type) {
    case DecisionType::Retain:
      return "Retained";
    case DecisionType::SwitchRejected:
      return "Switch rejected";
    case DecisionType::SwitchApproved:
      switch (record.execution) {
        case ExecutionConfirmation::Committed: return "Switch committed";
        case ExecutionConfirmation::Pending: return "Switch approved, awaiting commit";
        case ExecutionConfirmation::Failed: return "Switch approved, commit failed";
        case ExecutionConfirmation::RolledBack: return "Switch approved, rolled back";
        case ExecutionConfirmation::NotApplicable: return "Switch approved, commit status unknown";
      }
  }
  return "Unknown";
}

std::string FailedChecks(const DecisionRecord& record) {
  std::string failed;
  auto append = [&failed](const std::string& name) {
    if (!failed.empty()) failed += ", ";
    failed += name;
  };
  for (const ConstraintResult& constraint : record.constraints) {
    if (!constraint.satisfied) append(constraint.constraint);
  }
  for (const StabilityGateResult& gate : record.stability_gates) {
    if (!gate.passed) append(gate.gate);
  }
  return failed;
}

}  // namespace aafr::observability
