#pragma once

// A small JSON value, parser and writer for the V0.7.2 model-worker protocol.
// Plain C++17.
//
// Strict by design: rejects NaN/Infinity tokens, trailing garbage, invalid
// escapes and documents nested deeper than kMaxDepth. Numbers are doubles.
// The parser never throws; it reports the first error with its offset.

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aafr::model {

class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue>;

  JsonValue() = default;
  static JsonValue Null() { return JsonValue(); }
  static JsonValue Bool(bool v);
  static JsonValue Number(double v);
  static JsonValue String(std::string v);
  static JsonValue MakeArray(Array v = {});
  static JsonValue MakeObject(Object v = {});

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::Null; }
  bool IsBool() const { return type_ == Type::Bool; }
  bool IsNumber() const { return type_ == Type::Number; }
  bool IsString() const { return type_ == Type::String; }
  bool IsArray() const { return type_ == Type::Array; }
  bool IsObject() const { return type_ == Type::Object; }

  bool AsBool() const { return bool_; }
  double AsNumber() const { return number_; }
  const std::string& AsString() const { return string_; }
  const Array& AsArray() const;
  const Object& AsObject() const;
  Array& MutableArray();
  Object& MutableObject();

  // Object member or nullptr (also nullptr when this is not an object).
  const JsonValue* Find(const std::string& key) const;
  // Typed member access; nullopt when absent or of another type.
  std::optional<std::string> GetString(const std::string& key) const;
  std::optional<double> GetNumber(const std::string& key) const;
  std::optional<bool> GetBool(const std::string& key) const;

  // Object insertion (turns a Null value into an object).
  JsonValue& Set(const std::string& key, JsonValue value);

  // Compact serialization. Non-finite numbers are written as null (the
  // protocol never sends them; callers validate first).
  std::string Dump() const;

 private:
  Type type_{Type::Null};
  bool bool_{false};
  double number_{0.0};
  std::string string_;
  std::shared_ptr<Array> array_;
  std::shared_ptr<Object> object_;
};

struct JsonParseResult {
  std::optional<JsonValue> value;
  std::string error;  // Empty on success.
  std::size_t offset{0};
};

inline constexpr int kMaxJsonDepth = 32;

JsonParseResult ParseJson(const std::string& text);

// Escapes a string for inclusion in JSON (with quotes).
std::string JsonQuote(const std::string& text);

}  // namespace aafr::model
