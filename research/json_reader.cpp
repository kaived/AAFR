#include "research/json_reader.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace aafr::research {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  std::optional<JsonNode> Parse(std::string* error) {
    JsonNode root;
    if (!Value(root, 0)) return Fail(error);
    Skip();
    if (i_ != s_.size()) {
      error_ = "trailing content";
      return Fail(error);
    }
    return root;
  }

 private:
  std::optional<JsonNode> Fail(std::string* error) {
    if (error) *error = error_ + " at offset " + std::to_string(i_);
    return std::nullopt;
  }
  void Skip() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }
  bool Literal(const char* word) {
    std::size_t n = 0;
    while (word[n]) ++n;
    if (s_.compare(i_, n, word) != 0) return false;
    i_ += n;
    return true;
  }
  bool Value(JsonNode& out, int depth) {
    if (depth > kMaxDepth) {
      error_ = "nested too deeply";
      return false;
    }
    Skip();
    if (i_ >= s_.size()) {
      error_ = "unexpected end";
      return false;
    }
    const char c = s_[i_];
    if (c == '{') return Object(out, depth);
    if (c == '[') return Array(out, depth);
    if (c == '"') {
      out.type = JsonNode::Type::String;
      return String(out.string);
    }
    if (Literal("true")) { out.type = JsonNode::Type::Bool; out.boolean = true; return true; }
    if (Literal("false")) { out.type = JsonNode::Type::Bool; out.boolean = false; return true; }
    if (Literal("null")) { out.type = JsonNode::Type::Null; return true; }
    return Number(out);
  }
  bool Number(JsonNode& out) {
    const std::size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) {
      error_ = "invalid value";
      return false;
    }
    if (s_[i_] == '0') ++i_;
    else while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) { error_ = "invalid number"; return false; }
      while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) { error_ = "invalid number"; return false; }
      while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    const std::string token = s_.substr(start, i_ - start);
    char* end = nullptr;
    const double v = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(v)) {
      error_ = "invalid number";
      return false;
    }
    out.type = JsonNode::Type::Number;
    out.number = v;
    return true;
  }
  bool String(std::string& out) {
    ++i_;  // Opening quote.
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) { error_ = "control character in string"; return false; }
      if (c != '\\') { out.push_back(c); continue; }
      if (i_ >= s_.size()) break;
      const char e = s_[i_++];
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
          if (i_ + 4 > s_.size()) { error_ = "invalid escape"; return false; }
          unsigned code = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
            else { error_ = "invalid escape"; return false; }
          }
          // UTF-8 encode (basic plane; surrogate pairs are not needed here).
          if (code < 0x80) out.push_back(static_cast<char>(code));
          else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default: error_ = "invalid escape"; return false;
      }
    }
    error_ = "unterminated string";
    return false;
  }
  bool Array(JsonNode& out, int depth) {
    out.type = JsonNode::Type::Array;
    ++i_;
    Skip();
    if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
    while (true) {
      JsonNode item;
      if (!Value(item, depth + 1)) return false;
      out.array.push_back(std::move(item));
      Skip();
      if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
      if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
      error_ = "expected , or ]";
      return false;
    }
  }
  bool Object(JsonNode& out, int depth) {
    out.type = JsonNode::Type::Object;
    ++i_;
    Skip();
    if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
    while (true) {
      Skip();
      if (i_ >= s_.size() || s_[i_] != '"') { error_ = "expected key"; return false; }
      std::string key;
      if (!String(key)) return false;
      Skip();
      if (i_ >= s_.size() || s_[i_] != ':') { error_ = "expected :"; return false; }
      ++i_;
      JsonNode value;
      if (!Value(value, depth + 1)) return false;
      if (out.object.count(key)) { error_ = "duplicate key '" + key + "'"; return false; }
      out.object.emplace(std::move(key), std::move(value));
      Skip();
      if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
      if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
      error_ = "expected , or }";
      return false;
    }
  }

  const std::string& s_;
  std::size_t i_{0};
  std::string error_;
};

}  // namespace

const JsonNode* JsonNode::Find(const std::string& key) const {
  if (type != Type::Object) return nullptr;
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &it->second;
}

std::optional<double> JsonNode::Number(const std::string& key) const {
  const JsonNode* n = Find(key);
  if (!n || n->type != Type::Number) return std::nullopt;
  return n->number;
}

std::optional<std::string> JsonNode::String(const std::string& key) const {
  const JsonNode* n = Find(key);
  if (!n || n->type != Type::String) return std::nullopt;
  return n->string;
}

std::optional<std::vector<double>> JsonNode::Numbers(const std::string& key) const {
  const JsonNode* n = Find(key);
  if (!n || n->type != Type::Array) return std::nullopt;
  std::vector<double> out;
  for (const JsonNode& v : n->array) {
    if (v.type != Type::Number) return std::nullopt;
    out.push_back(v.number);
  }
  return out;
}

std::optional<std::vector<std::string>> JsonNode::Strings(const std::string& key) const {
  const JsonNode* n = Find(key);
  if (!n || n->type != Type::Array) return std::nullopt;
  std::vector<std::string> out;
  for (const JsonNode& v : n->array) {
    if (v.type != Type::String) return std::nullopt;
    out.push_back(v.string);
  }
  return out;
}

std::optional<JsonNode> ParseJson(const std::string& text, std::string* error) { return Parser(text).Parse(error); }

}  // namespace aafr::research
