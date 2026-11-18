#include "routing/gateway/proxy_http.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "routing/forwarding/egress_policy.h"

namespace aafr::gateway {
namespace {

bool TChar(unsigned char c) {
  if (std::isalnum(c)) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

bool IsToken(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return TChar(static_cast<unsigned char>(c)); });
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string Trim(const std::string& s) {
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) ++begin;
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
  return s.substr(begin, end - begin);
}

ParseOutcome Bad(int status, const char* reason) { return {false, status, reason}; }

// Splits a head into CRLF lines; false on a bare CR or LF.
bool SplitLines(const std::string& head, std::vector<std::string>* lines) {
  if (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) return false;
  std::size_t start = 0;
  const std::size_t body_end = head.size() - 2;  // Drop the final empty line.
  while (start < body_end) {
    const std::size_t cr = head.find('\r', start);
    if (cr == std::string::npos || cr + 1 >= head.size() || head[cr + 1] != '\n') return false;
    const std::string line = head.substr(start, cr - start);
    if (line.find('\n') != std::string::npos) return false;
    lines->push_back(line);
    start = cr + 2;
  }
  return !lines->empty();
}

// Parses "name: value" lines (strictly). Empty reason when fine.
std::string ParseHeaders(const std::vector<std::string>& lines, std::vector<HeaderField>* out) {
  if (lines.size() - 1 > kMaxHeaderFields) return "too_many_headers";
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (line.empty()) return "empty_header_line";
    if (line[0] == ' ' || line[0] == '\t') return "obs_fold";
    const auto colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return "malformed_header";
    const std::string name = line.substr(0, colon);
    if (!IsToken(name)) return "invalid_header_name";  // Also catches "Name :".
    const std::string value = Trim(line.substr(colon + 1));
    for (const char ch : value) {
      const unsigned char c = static_cast<unsigned char>(ch);
      if ((c < 0x20 && c != '\t') || c == 0x7F) return "control_character";
    }
    out->push_back({name, value});
  }
  return std::string();
}

bool DigitsOnly(const std::string& s) {
  return !s.empty() && s.size() <= 18 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::string> CommaTokens(const std::string& value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto comma = value.find(',', start);
    const std::string part = Lower(Trim(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
    if (!part.empty()) out.push_back(part);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

const std::set<std::string>& HopByHop() {
  static const std::set<std::string> names = {"connection", "proxy-connection", "keep-alive", "proxy-authorization",
                                              "proxy-authenticate", "te", "trailer", "transfer-encoding", "upgrade"};
  return names;
}

std::set<std::string> ConnectionNamed(const std::vector<HeaderField>& headers) {
  std::set<std::string> named;
  for (const HeaderField& h : headers) {
    if (Lower(h.name) != "connection" && Lower(h.name) != "proxy-connection") continue;
    for (const std::string& token : CommaTokens(h.value)) named.insert(token);
  }
  return named;
}

bool TargetChars(const std::string& t) {
  return std::all_of(t.begin(), t.end(), [](char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return c > 0x20 && c < 0x7F;
  });
}

}  // namespace

bool EqualsIgnoreCase(const std::string& a, const std::string& b) { return a.size() == b.size() && Lower(a) == Lower(b); }

ParseOutcome ParseProxyRequestHead(const std::string& head, ProxyRequest* out) {
  std::vector<std::string> lines;
  if (!SplitLines(head, &lines)) return Bad(400, "bad_line_endings");
  // Request line: exactly "METHOD SP target SP HTTP/x.y".
  const std::string& line = lines.front();
  const auto sp1 = line.find(' ');
  const auto sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos || line.find(' ', sp2 + 1) != std::string::npos) {
    return Bad(400, "malformed_request_line");
  }
  ProxyRequest r;
  r.method = line.substr(0, sp1);
  const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  r.version = line.substr(sp2 + 1);
  if (!IsToken(r.method) || target.empty() || !TargetChars(target)) return Bad(400, "malformed_request_line");
  if (r.version != "HTTP/1.1" && r.version != "HTTP/1.0") return Bad(505, "unsupported_http_version");
  const std::string reason = ParseHeaders(lines, &r.headers);
  if (!reason.empty()) return Bad(400, reason.c_str());

  int hosts = 0;
  bool transfer_encoding = false;
  for (const HeaderField& h : r.headers) {
    const std::string name = Lower(h.name);
    if (name == "host") ++hosts;
    if (name == "transfer-encoding") transfer_encoding = true;
    if (name == "content-length") {
      // A list ("5, 5") is ambiguous framing too.
      if (!DigitsOnly(h.value)) return Bad(400, "invalid_content_length");
      const std::uint64_t value = std::stoull(h.value);
      if (r.content_length && *r.content_length != value) return Bad(400, "conflicting_content_length");
      r.content_length = value;
    }
  }
  if (hosts > 1) return Bad(400, "multiple_host_headers");
  if (transfer_encoding && r.content_length) return Bad(400, "smuggling_ambiguity");

  if (r.method == "CONNECT") {
    r.kind = ProxyRequestKind::Connect;
    if (transfer_encoding || (r.content_length && *r.content_length != 0)) return Bad(400, "connect_with_body");
    if (!forwarding::ParseTarget(target, &r.target_host, &r.target_port)) return Bad(400, "invalid_connect_target");
    *out = std::move(r);
    return {true, 200, ""};
  }

  r.kind = ProxyRequestKind::Forward;
  if (r.method == "TRACE" || r.method == "TRACK") return Bad(405, "method_not_allowed");
  static const std::set<std::string> allowed = {"GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "PATCH"};
  if (allowed.count(r.method) == 0) return Bad(405, "method_not_allowed");
  if (transfer_encoding) return Bad(501, "request_transfer_encoding_unsupported");
  if (target.front() == '/') return Bad(400, "origin_form_to_proxy");
  const std::string lowered = Lower(target);
  if (lowered.rfind("https://", 0) == 0) return Bad(400, "https_requires_connect");
  if (lowered.rfind("http://", 0) != 0) return Bad(400, "unsupported_scheme");
  if (target.find('#') != std::string::npos) return Bad(400, "fragment_in_target");
  const std::string rest = target.substr(7);
  const auto path_start = rest.find_first_of("/?");
  const std::string authority = rest.substr(0, path_start);
  r.origin_form = path_start == std::string::npos ? "/" : rest.substr(path_start);
  if (r.origin_form.front() == '?') r.origin_form = "/" + r.origin_form;
  if (authority.empty() || authority.find('@') != std::string::npos) return Bad(400, "invalid_authority");
  const bool has_port = authority.back() != ']' && authority.find(':') != std::string::npos &&
                        (authority.front() != '[' || authority.find("]:") != std::string::npos);
  if (has_port) {
    if (!forwarding::ParseTarget(authority, &r.target_host, &r.target_port)) return Bad(400, "invalid_authority");
  } else {
    std::string host = authority;
    if (host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (!forwarding::ValidTargetHost(host)) return Bad(400, "invalid_authority");
    r.target_host = Lower(host);
    r.target_port = 80;
  }
  *out = std::move(r);
  return {true, 200, ""};
}

std::string BuildOriginRequestHead(const ProxyRequest& r) {
  std::string out = r.method + " " + r.origin_form + " HTTP/1.1\r\n";
  std::string host = r.target_host.find(':') != std::string::npos ? "[" + r.target_host + "]" : r.target_host;
  if (r.target_port != 80) host += ":" + std::to_string(r.target_port);
  out += "Host: " + host + "\r\n";
  const std::set<std::string> named = ConnectionNamed(r.headers);
  for (const HeaderField& h : r.headers) {
    const std::string name = Lower(h.name);
    if (name == "host" || HopByHop().count(name) != 0 || named.count(name) != 0) continue;
    out += h.name + ": " + h.value + "\r\n";
  }
  out += "Connection: close\r\n\r\n";
  return out;
}

bool ParseResponseHead(const std::string& head, ResponseHead* out, std::string* reason) {
  const auto fail = [reason](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  std::vector<std::string> lines;
  if (!SplitLines(head, &lines)) return fail("bad_line_endings");
  const std::string& line = lines.front();
  if (line.size() < 12 || (line.compare(0, 9, "HTTP/1.1 ") != 0 && line.compare(0, 9, "HTTP/1.0 ") != 0)) {
    return fail("malformed_status_line");
  }
  ResponseHead r;
  r.version = line.substr(0, 8);
  const std::string code = line.substr(9, 3);
  if (!DigitsOnly(code) || (line.size() > 12 && line[12] != ' ')) return fail("malformed_status_line");
  r.status = std::stoi(code);
  if (r.status < 100 || r.status > 599) return fail("malformed_status_line");
  r.reason_phrase = line.size() > 13 ? line.substr(13) : std::string();
  const std::string header_error = ParseHeaders(lines, &r.headers);
  if (!header_error.empty()) return fail(header_error.c_str());
  *out = std::move(r);
  return true;
}

std::string BuildClientResponseHead(const ResponseHead& head) {
  std::string out = head.version + " " + std::to_string(head.status) + " " + head.reason_phrase + "\r\n";
  const std::set<std::string> named = ConnectionNamed(head.headers);
  for (const HeaderField& h : head.headers) {
    const std::string name = Lower(h.name);
    if (name == "connection" || name == "proxy-connection" || name == "keep-alive" || name == "upgrade" ||
        (named.count(name) != 0 && name != "transfer-encoding" && name != "content-length")) {
      continue;
    }
    out += h.name + ": " + h.value + "\r\n";
  }
  if (head.status >= 200) out += "Connection: close\r\n";
  out += "\r\n";
  return out;
}

const char* ToString(BodyFraming value) {
  switch (value) {
    case BodyFraming::None: return "none";
    case BodyFraming::ContentLength: return "content_length";
    case BodyFraming::Chunked: return "chunked";
    case BodyFraming::UntilClose: return "until_close";
    case BodyFraming::Invalid: return "invalid";
  }
  return "unknown";
}

BodyFraming ResponseFraming(const ResponseHead& head, const std::string& request_method, std::uint64_t* length) {
  if (request_method == "HEAD" || (head.status >= 100 && head.status < 200) || head.status == 204 ||
      head.status == 304) {
    return BodyFraming::None;
  }
  std::optional<std::uint64_t> content_length;
  std::vector<std::string> codings;
  for (const HeaderField& h : head.headers) {
    const std::string name = Lower(h.name);
    if (name == "transfer-encoding") {
      for (const std::string& t : CommaTokens(h.value)) codings.push_back(t);
    } else if (name == "content-length") {
      if (!DigitsOnly(h.value)) return BodyFraming::Invalid;
      const std::uint64_t value = std::stoull(h.value);
      if (content_length && *content_length != value) return BodyFraming::Invalid;
      content_length = value;
    }
  }
  if (!codings.empty()) return codings.back() == "chunked" ? BodyFraming::Chunked : BodyFraming::UntilClose;
  if (content_length) {
    if (length) *length = *content_length;
    return *content_length == 0 ? BodyFraming::None : BodyFraming::ContentLength;
  }
  return BodyFraming::UntilClose;
}

std::size_t ChunkedTracker::Feed(const char* data, std::size_t size) {
  std::size_t i = 0;
  constexpr std::size_t kMaxLine = 4096;
  while (i < size && state_ != State::Done && state_ != State::Error) {
    const char c = data[i];
    switch (state_) {
      case State::Size:
        if (std::isxdigit(static_cast<unsigned char>(c))) {
          if (++size_digits_ > 15) {
            state_ = State::Error;
            break;
          }
          const int digit = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(c) - 'a' + 10);
          remaining_ = remaining_ * 16 + static_cast<std::uint64_t>(digit);
          ++i;
        } else if (size_digits_ > 0 && (c == ';' || c == ' ' || c == '\t')) {
          state_ = State::Extension;
          line_bytes_ = 0;
          ++i;
        } else if (size_digits_ > 0 && c == '\r') {
          state_ = State::SizeLf;
          ++i;
        } else {
          state_ = State::Error;
        }
        break;
      case State::Extension:
        if (c == '\r') state_ = State::SizeLf;
        else if (c == '\n' || ++line_bytes_ > kMaxLine) state_ = State::Error;
        ++i;
        break;
      case State::SizeLf:
        if (c != '\n') {
          state_ = State::Error;
          break;
        }
        ++i;
        size_digits_ = 0;
        state_ = remaining_ == 0 ? State::TrailerStart : State::Data;
        break;
      case State::Data: {
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, size - i));
        i += take;
        remaining_ -= take;
        if (remaining_ == 0) state_ = State::DataCr;
        break;
      }
      case State::DataCr:
        state_ = c == '\r' ? State::DataLf : State::Error;
        if (state_ != State::Error) ++i;
        break;
      case State::DataLf:
        state_ = c == '\n' ? State::Size : State::Error;
        if (state_ != State::Error) ++i;
        break;
      case State::TrailerStart:
        if (c == '\r') state_ = State::FinalLf;
        else {
          state_ = State::Trailer;
          line_bytes_ = 1;
        }
        ++i;
        break;
      case State::Trailer:
        if (c == '\r') state_ = State::TrailerLf;
        else if (c == '\n' || ++line_bytes_ > kMaxLine) state_ = State::Error;
        ++i;
        break;
      case State::TrailerLf:
        state_ = c == '\n' ? State::TrailerStart : State::Error;
        if (state_ != State::Error) ++i;
        break;
      case State::FinalLf:
        state_ = c == '\n' ? State::Done : State::Error;
        if (state_ != State::Error) ++i;
        break;
      case State::Done:
      case State::Error:
        break;
    }
  }
  return i;
}

const char* StatusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Error";
  }
}

std::string GatewayErrorResponse(int status, const std::string& reason_code) {
  const std::string body = "AAFR browser forwarding gateway: " + std::to_string(status) + " " + StatusText(status) +
                           "\nreason: " + reason_code + "\n";
  return "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) +
         "\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nX-AAFR-Reason: " + reason_code +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

}  // namespace aafr::gateway
