#include "runtime/model/json_value.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace aafr::model {

namespace {
const JsonValue::Array kEmptyArray;
const JsonValue::Object kEmptyObject;
}  // namespace

JsonValue JsonValue::Bool(bool v) {
  JsonValue j;
  j.type_ = Type::Bool;
  j.bool_ = v;
  return j;
}

JsonValue JsonValue::Number(double v) {
  JsonValue j;
  j.type_ = Type::Number;
  j.number_ = v;
  return j;
}

JsonValue JsonValue::String(std::string v) {
  JsonValue j;
  j.type_ = Type::String;
  j.string_ = std::move(v);
  return j;
}

JsonValue JsonValue::MakeArray(Array v) {
  JsonValue j;
  j.type_ = Type::Array;
  j.array_ = std::make_shared<Array>(std::move(v));
  return j;
}

JsonValue JsonValue::MakeObject(Object v) {
  JsonValue j;
  j.type_ = Type::Object;
  j.object_ = std::make_shared<Object>(std::move(v));
  return j;
}

const JsonValue::Array& JsonValue::AsArray() const { return array_ ? *array_ : kEmptyArray; }
const JsonValue::Object& JsonValue::AsObject() const { return object_ ? *object_ : kEmptyObject; }

JsonValue::Array& JsonValue::MutableArray() {
  if (type_ != Type::Array || !array_) {
    type_ = Type::Array;
    array_ = std::make_shared<Array>();
  }
  return *array_;
}

JsonValue::Object& JsonValue::MutableObject() {
  if (type_ != Type::Object || !object_) {
    type_ = Type::Object;
    object_ = std::make_shared<Object>();
  }
  return *object_;
}

const JsonValue* JsonValue::Find(const std::string& key) const {
  if (type_ != Type::Object || !object_) return nullptr;
  const auto found = object_->find(key);
  return found == object_->end() ? nullptr : &found->second;
}

std::optional<std::string> JsonValue::GetString(const std::string& key) const {
  const JsonValue* v = Find(key);
  if (!v || !v->IsString()) return std::nullopt;
  return v->AsString();
}

std::optional<double> JsonValue::GetNumber(const std::string& key) const {
  const JsonValue* v = Find(key);
  if (!v || !v->IsNumber()) return std::nullopt;
  return v->AsNumber();
}

std::optional<bool> JsonValue::GetBool(const std::string& key) const {
  const JsonValue* v = Find(key);
  if (!v || !v->IsBool()) return std::nullopt;
  return v->AsBool();
}

JsonValue& JsonValue::Set(const std::string& key, JsonValue value) {
  Object& o = MutableObject();
  o[key] = std::move(value);
  return o[key];
}

std::string JsonQuote(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

namespace {
void DumpTo(const JsonValue& v, std::string& out) {
  switch (v.type()) {
    case JsonValue::Type::Null: out += "null"; break;
    case JsonValue::Type::Bool: out += v.AsBool() ? "true" : "false"; break;
    case JsonValue::Type::Number: {
      const double d = v.AsNumber();
      if (!std::isfinite(d)) {
        out += "null";
      } else if (d == std::floor(d) && std::fabs(d) < 9.0e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", d);
        out += buf;
      } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        out += buf;
      }
      break;
    }
    case JsonValue::Type::String: out += JsonQuote(v.AsString()); break;
    case JsonValue::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const JsonValue& e : v.AsArray()) {
        if (!first) out.push_back(',');
        first = false;
        DumpTo(e, out);
      }
      out.push_back(']');
      break;
    }
    case JsonValue::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& e : v.AsObject()) {
        if (!first) out.push_back(',');
        first = false;
        out += JsonQuote(e.first);
        out.push_back(':');
        DumpTo(e.second, out);
      }
      out.push_back('}');
      break;
    }
  }
}

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  JsonParseResult Run() {
    JsonParseResult r;
    SkipSpace();
    JsonValue v;
    if (!Value(v, 0)) {
      r.error = error_;
      r.offset = pos_;
      return r;
    }
    SkipSpace();
    if (pos_ != s_.size()) {
      r.error = "trailing characters after the JSON value";
      r.offset = pos_;
      return r;
    }
    r.value = std::move(v);
    return r;
  }

 private:
  bool Fail(const char* message) {
    if (error_.empty()) error_ = message;
    return false;
  }
  void SkipSpace() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }
  bool Literal(const char* word) {
    std::size_t n = 0;
    while (word[n]) ++n;
    if (s_.compare(pos_, n, word) != 0) return Fail("invalid literal");
    pos_ += n;
    return true;
  }

  bool Value(JsonValue& out, int depth) {
    if (depth > kMaxJsonDepth) return Fail("nesting too deep");
    if (pos_ >= s_.size()) return Fail("unexpected end of input");
    const char c = s_[pos_];
    if (c == '{') return ObjectValue(out, depth);
    if (c == '[') return ArrayValue(out, depth);
    if (c == '"') {
      std::string str;
      if (!StringValue(str)) return false;
      out = JsonValue::String(std::move(str));
      return true;
    }
    if (c == 't') {
      if (!Literal("true")) return false;
      out = JsonValue::Bool(true);
      return true;
    }
    if (c == 'f') {
      if (!Literal("false")) return false;
      out = JsonValue::Bool(false);
      return true;
    }
    if (c == 'n') {
      if (!Literal("null")) return false;
      out = JsonValue::Null();
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return NumberValue(out);
    return Fail("unexpected character (NaN and Infinity are not JSON)");
  }

  bool NumberValue(JsonValue& out) {
    const std::size_t start = pos_;
    if (s_[pos_] == '-') ++pos_;
    if (pos_ >= s_.size()) return Fail("invalid number");
    if (s_[pos_] == '0') {
      ++pos_;
    } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
      while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    } else {
      return Fail("invalid number");
    }
    if (pos_ < s_.size() && s_[pos_] == '.') {
      ++pos_;
      if (pos_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) return Fail("invalid number");
      while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      if (pos_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) return Fail("invalid number");
      while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    const std::string text = s_.substr(start, pos_ - start);
    char* end = nullptr;
    const double d = std::strtod(text.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(d)) return Fail("number out of range");
    out = JsonValue::Number(d);
    return true;
  }

  static void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool Hex4(unsigned& out) {
    if (pos_ + 4 > s_.size()) return Fail("invalid unicode escape");
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
      else return Fail("invalid unicode escape");
    }
    return true;
  }

  bool StringValue(std::string& out) {
    ++pos_;  // opening quote
    while (true) {
      if (pos_ >= s_.size()) return Fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(s_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c < 0x20) return Fail("control character in string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++pos_;
        continue;
      }
      ++pos_;
      if (pos_ >= s_.size()) return Fail("unterminated escape");
      const char e = s_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned cp = 0;
          if (!Hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') return Fail("unpaired surrogate");
            pos_ += 2;
            unsigned low = 0;
            if (!Hex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return Fail("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("unpaired surrogate");
          }
          AppendUtf8(out, cp);
          break;
        }
        default: return Fail("invalid escape");
      }
    }
  }

  bool ArrayValue(JsonValue& out, int depth) {
    ++pos_;
    JsonValue::Array items;
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      out = JsonValue::MakeArray(std::move(items));
      return true;
    }
    while (true) {
      SkipSpace();
      JsonValue item;
      if (!Value(item, depth + 1)) return false;
      items.push_back(std::move(item));
      SkipSpace();
      if (pos_ >= s_.size()) return Fail("unterminated array");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        out = JsonValue::MakeArray(std::move(items));
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  bool ObjectValue(JsonValue& out, int depth) {
    ++pos_;
    JsonValue::Object members;
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      out = JsonValue::MakeObject(std::move(members));
      return true;
    }
    while (true) {
      SkipSpace();
      if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("expected a string key");
      std::string key;
      if (!StringValue(key)) return false;
      SkipSpace();
      if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("expected ':'");
      ++pos_;
      SkipSpace();
      JsonValue value;
      if (!Value(value, depth + 1)) return false;
      if (members.count(key)) return Fail("duplicate key");
      members.emplace(std::move(key), std::move(value));
      SkipSpace();
      if (pos_ >= s_.size()) return Fail("unterminated object");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        out = JsonValue::MakeObject(std::move(members));
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  const std::string& s_;
  std::size_t pos_{0};
  std::string error_;
};
}  // namespace

std::string JsonValue::Dump() const {
  std::string out;
  DumpTo(*this, out);
  return out;
}

JsonParseResult ParseJson(const std::string& text) { return Parser(text).Run(); }

}  // namespace aafr::model
