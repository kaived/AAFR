#pragma once

// A minimal strict JSON reader for the research model's small metadata files
// (scaler, path-objective statistics, provenance). Plain C++17, read-only.
// Numbers are parsed with strtod, so values written by Python's json module
// round-trip exactly. Rejects NaN / Infinity, trailing content and invalid
// escapes; never throws.

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aafr::research {

struct JsonNode {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type{Type::Null};
  bool boolean{false};
  double number{0.0};
  std::string string;
  std::vector<JsonNode> array;
  std::map<std::string, JsonNode> object;

  const JsonNode* Find(const std::string& key) const;
  std::optional<double> Number(const std::string& key) const;
  std::optional<std::string> String(const std::string& key) const;
  // Array of numbers / strings; nullopt when absent or of another type.
  std::optional<std::vector<double>> Numbers(const std::string& key) const;
  std::optional<std::vector<std::string>> Strings(const std::string& key) const;
};

// Parses `text`; on failure returns nullopt and sets `error` (with offset).
std::optional<JsonNode> ParseJson(const std::string& text, std::string* error = nullptr);

}  // namespace aafr::research
