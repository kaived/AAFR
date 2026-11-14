#pragma once

// Registry of algorithm capability profiles. Plain C++17. A value type: copy
// it freely; it is not synchronized, so share it as const or guard it.
//
// Registering a profile does not make an algorithm available. Every built-in
// profile starts as ImplementationUnavailable, and the registry refuses to
// mark an algorithm VerifiedExecutable without verification evidence.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "algorithms/algorithm_capability.h"
#include "runtime/observability/runtime_contracts.h"

namespace aafr::algorithms {

// Problems with a profile; empty when it is well-formed.
std::vector<std::string> ValidateProfile(const AlgorithmCapability& capability);

class CapabilityRegistry {
 public:
  // An empty registry.
  CapabilityRegistry() = default;
  // The ten planned algorithms, none of them implemented.
  static CapabilityRegistry BuiltIn();

  // Refuses (returns false, with the reasons in `errors` when given) a
  // profile that fails ValidateProfile or whose id is already registered.
  bool Register(AlgorithmCapability capability, std::vector<std::string>* errors = nullptr);

  const AlgorithmCapability* Find(AlgorithmId id) const;
  // By machine-readable id; nullptr for unknown text.
  const AlgorithmCapability* Find(const std::string& id) const;
  // In AlgorithmId order.
  std::vector<AlgorithmCapability> All() const;
  std::size_t Size() const { return profiles_.size(); }

  // Changes availability. VerifiedExecutable additionally needs a profile
  // with measured runtime and memory, an execution environment and a
  // verification note; without them the call is refused.
  bool SetAvailability(AlgorithmId id, ImplementationAvailability availability,
                       std::vector<std::string>* errors = nullptr);
  // Replaces the implementation facts of a registered algorithm. Refused if
  // it would leave a VerifiedExecutable algorithm without its evidence.
  bool SetImplementation(AlgorithmId id, ImplementationProfile implementation,
                         std::vector<std::string>* errors = nullptr);

  std::vector<AlgorithmId> ExecutableAlgorithms() const;

 private:
  std::map<AlgorithmId, AlgorithmCapability> profiles_;
};

// The V0.3 viewer contract for the same capability, for consumers of
// runtime/observability. Only VerifiedExecutable maps to Available.
observability::AlgorithmCapability ToObservabilityCapability(const AlgorithmCapability& capability);

}  // namespace aafr::algorithms
