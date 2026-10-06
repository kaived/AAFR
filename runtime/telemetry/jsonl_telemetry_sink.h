#pragma once

#include <fstream>
#include <string>

#include "runtime/interfaces/interfaces.h"

namespace ngr {

class JsonlTelemetrySink final : public ITelemetrySink {
 public:
  explicit JsonlTelemetrySink(std::string output_path);
  void Record(const TransitionDecision& decision) override;

 private:
  std::string output_path_;
  std::ofstream stream_;
};

}  // namespace ngr
