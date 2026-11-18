#pragma once

// A controlled HTTP/1.1 origin server on 127.0.0.1 for the V0.7.3 gateway and
// browser forwarding tests. Header-only, plain C++17 over the AAFR socket
// helpers; starts on an ephemeral port and stops (joining every thread) in
// its destructor. No public website is ever contacted.
//
// Routes (query strings are parsed where noted):
//   /, /page          small HTML page titled "AAFR forwarded page"
//   /page2            another small page
//   /redirect         302 -> /target
//   /target           redirect target page
//   /large?bytes=N    N bytes (default 4 MiB), Content-Length framing
//   /lines?bytes=N    an HTML page of about N bytes in 80-column lines
//   /chunked          5 chunks, 60 ms apart (streaming, chunked framing)
//   /slow?ms=N        answers after N ms
//   /hold?ms=N        sends the head and 1 KiB, holds the body open N ms, then
//                     finishes (a long-lived response)
//   /echo             returns "received=<n>" for a Content-Length body
//   /close            closes without answering (server disconnect)
//   /keepalive        answers but ignores "Connection: close" and stays open
//   /download         1 MiB with Content-Disposition: attachment
//   /favicon.ico      204

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "routing/forwarding/socket_io.h"

namespace aafr::test {

class HttpTestServer {
 public:
  HttpTestServer() = default;
  ~HttpTestServer() { Stop(); }
  HttpTestServer(const HttpTestServer&) = delete;
  HttpTestServer& operator=(const HttpTestServer&) = delete;

  bool Start() {
    namespace net = forwarding::net;
    listener_ = net::ListenLoopback(0, 64, &port_, nullptr);
    if (listener_ == net::kInvalidSocket) return false;
    stopping_ = false;
    acceptor_ = std::thread([this] { AcceptLoop(); });
    return true;
  }

  void Stop() {
    namespace net = forwarding::net;
    if (listener_ == net::kInvalidSocket) return;
    stopping_ = true;
    if (acceptor_.joinable()) acceptor_.join();
    std::vector<std::thread> workers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      workers.swap(workers_);
    }
    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }
    net::CloseSocket(listener_);
    listener_ = net::kInvalidSocket;
  }

  std::uint16_t port() const { return port_; }
  std::string Base() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::uint64_t Requests() const { return requests_.load(); }
  std::uint64_t Count(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = counts_.find(path);
    return found == counts_.end() ? 0 : found->second;
  }
  std::string LastHead() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_head_;
  }
  std::uint64_t Connections() const { return connections_.load(); }

 private:
  void AcceptLoop() {
    namespace net = forwarding::net;
    while (!stopping_) {
      if (net::WaitSocket(listener_, false, 20) != net::WaitResult::Ready) continue;
      const net::SocketHandle client = net::Accept(listener_, nullptr, nullptr);
      if (client == net::kInvalidSocket) continue;
      ++connections_;
      std::lock_guard<std::mutex> lock(mutex_);
      workers_.emplace_back([this, client] { Serve(client); });
    }
  }

  static std::string Query(const std::string& target, const std::string& key) {
    const auto q = target.find('?');
    if (q == std::string::npos) return std::string();
    const std::string query = target.substr(q + 1);
    const std::string needle = key + "=";
    const auto at = query.find(needle);
    if (at == std::string::npos) return std::string();
    const auto end = query.find('&', at);
    return query.substr(at + needle.size(), end == std::string::npos ? std::string::npos : end - at - needle.size());
  }

  static std::string Html(const std::string& title, const std::string& body) {
    return "<!doctype html><html><head><title>" + title + "</title></head><body><p id=\"content\">" + body +
           "</p></body></html>";
  }

  bool Wait(int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
      if (stopping_) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

  void Serve(forwarding::net::SocketHandle client) {
    namespace net = forwarding::net;
    net::Connection c(client);
    const auto interrupted = [this] { return stopping_.load(); };
    for (;;) {
      std::string head;
      if (c.ReadHead(head, 64 * 1024, net::After(60'000), interrupted) != net::ReadResult::Ok) return;
      ++requests_;
      const auto sp1 = head.find(' ');
      const auto sp2 = head.find(' ', sp1 + 1);
      const std::string method = head.substr(0, sp1);
      const std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
      const std::string path = target.substr(0, target.find('?'));
      std::uint64_t content_length = 0;
      {
        std::string lower = head;
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto cl = lower.find("\r\ncontent-length:");
        if (cl != std::string::npos) content_length = std::stoull(head.substr(cl + 17));
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counts_[path];
        last_head_ = head;
      }
      std::string body_in;
      while (body_in.size() < content_length) {
        if (c.ReadSome(body_in, static_cast<std::size_t>(content_length - body_in.size()), net::After(10'000),
                       interrupted) != net::ReadResult::Ok) {
          return;
        }
      }
      const auto respond = [&](int status, const char* text, const std::string& type, const std::string& body,
                               const std::string& extra = "") {
        std::string out = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: " + type +
                          "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra +
                          "Connection: close\r\n\r\n";
        if (method != "HEAD") out += body;
        c.SendAll(out);
      };
      if (path == "/" || path == "/page") {
        respond(200, "OK", "text/html", Html("AAFR forwarded page", "forwarded-page-body"));
      } else if (path == "/page2") {
        respond(200, "OK", "text/html", Html("AAFR second page", "second-page-body"));
      } else if (path == "/target") {
        respond(200, "OK", "text/html", Html("AAFR redirect target", "redirect-target-body"));
      } else if (path == "/redirect") {
        respond(302, "Found", "text/plain", "moved", "Location: /target\r\n");
      } else if (path == "/large" || path == "/download") {
        const std::string n = Query(target, "bytes");
        const std::size_t size = path == "/download" ? 1u << 20 : (n.empty() ? 4u << 20 : std::stoul(n));
        std::string body(size, 'a');
        for (std::size_t i = 0; i < size; i += 4096) body[i] = static_cast<char>('A' + (i / 4096) % 26);
        respond(200, "OK", path == "/download" ? "application/octet-stream" : "text/plain", body,
                path == "/download" ? "Content-Disposition: attachment; filename=\"aafr-download.bin\"\r\n" : "");
      } else if (path == "/lines") {
        const std::string n = Query(target, "bytes");
        const std::size_t size = n.empty() ? 1u << 20 : std::stoul(n);
        std::string body = "<!doctype html><html><head><title>AAFR lines page</title></head><body><pre>";
        const std::string line(79, 'l');
        while (body.size() < size) body += line + "\n";
        body += "</pre></body></html>";
        respond(200, "OK", "text/html", body);
      } else if (path == "/chunked") {
        c.SendAll("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        for (int i = 0; i < 5; ++i) {
          const std::string piece = "chunk-" + std::to_string(i) + ";";
          char size[16];
          std::snprintf(size, sizeof(size), "%zx\r\n", piece.size());
          if (!c.SendAll(std::string(size) + piece + "\r\n")) return;
          if (!Wait(60)) return;
        }
        c.SendAll("0\r\n\r\n");
      } else if (path == "/slow") {
        const std::string ms = Query(target, "ms");
        if (!Wait(ms.empty() ? 500 : std::stoi(ms))) return;
        respond(200, "OK", "text/html", Html("AAFR slow page", "slow-body"));
      } else if (path == "/hold") {
        const std::string ms = Query(target, "ms");
        const std::string first(1024, 'h');
        const std::string last = "<p id=\"done\">held-response-complete</p>";
        c.SendAll("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                  std::to_string(first.size() + last.size()) + "\r\nConnection: close\r\n\r\n" + first);
        if (!Wait(ms.empty() ? 2000 : std::stoi(ms))) return;
        c.SendAll(last);
      } else if (path == "/echo") {
        respond(200, "OK", "text/plain", "received=" + std::to_string(body_in.size()));
      } else if (path == "/close") {
        return;
      } else if (path == "/keepalive") {
        const std::string body = "kept-alive";
        c.SendAll("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nConnection: keep-alive\r\n\r\n" + body);
        continue;  // Ignores "Connection: close" and waits for another request.
      } else if (path == "/favicon.ico") {
        c.SendAll("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
      } else {
        respond(404, "Not Found", "text/plain", "not found");
      }
      return;
    }
  }

  forwarding::net::SocketHandle listener_{forwarding::net::kInvalidSocket};
  std::uint16_t port_{0};
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;
  mutable std::mutex mutex_;
  std::vector<std::thread> workers_;
  std::map<std::string, std::uint64_t> counts_;
  std::string last_head_;
  std::atomic<std::uint64_t> requests_{0};
  std::atomic<std::uint64_t> connections_{0};
};

// A raw client of the gateway, for tests: sends bytes, reads until close.
struct RawResponse {
  bool connected{false};
  std::string data;
  int status{0};
  std::string head;
  std::string body;
};

inline RawResponse RawExchange(std::uint16_t port, const std::string& request, double timeout_ms = 10'000.0) {
  namespace net = forwarding::net;
  RawResponse r;
  const net::SocketHandle s = net::ConnectLoopback(port, 2000);
  if (s == net::kInvalidSocket) return r;
  r.connected = true;
  net::Connection c(s);
  c.SendAll(request);
  const auto deadline = net::After(timeout_ms);
  for (;;) {
    std::string chunk;
    if (c.ReadSome(chunk, 1 << 20, deadline, {}) != net::ReadResult::Ok) break;
    r.data += chunk;
  }
  const auto end = r.data.find("\r\n\r\n");
  if (end != std::string::npos) {
    r.head = r.data.substr(0, end + 4);
    r.body = r.data.substr(end + 4);
  }
  if (r.data.size() >= 12 && r.data.compare(0, 5, "HTTP/") == 0) r.status = std::atoi(r.data.c_str() + 9);
  return r;
}

inline RawResponse ProxyGet(std::uint16_t gateway_port, const std::string& absolute_url,
                            const std::string& extra_headers = "", double timeout_ms = 10'000.0) {
  const auto scheme_end = absolute_url.find("://");
  const auto host_start = scheme_end + 3;
  const auto path_start = absolute_url.find('/', host_start);
  const std::string authority = absolute_url.substr(host_start, path_start - host_start);
  return RawExchange(gateway_port,
                     "GET " + absolute_url + " HTTP/1.1\r\nHost: " + authority +
                         "\r\nProxy-Connection: keep-alive\r\nUser-Agent: aafr-test\r\n" + extra_headers + "\r\n",
                     timeout_ms);
}

// Opens a CONNECT tunnel; on success `connection` speaks to the target.
inline int ProxyConnect(std::uint16_t gateway_port, const std::string& authority, forwarding::net::Connection* connection,
                        std::string* reply_head = nullptr) {
  namespace net = forwarding::net;
  const net::SocketHandle s = net::ConnectLoopback(gateway_port, 2000);
  if (s == net::kInvalidSocket) return 0;
  net::Connection c(s);
  c.SendAll("CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n\r\n");
  std::string head;
  if (c.ReadHead(head, 64 * 1024, net::After(15'000), {}) != net::ReadResult::Ok) return 0;
  if (reply_head) *reply_head = head;
  const int status = std::atoi(head.c_str() + 9);
  *connection = std::move(c);
  return status;
}

// One HTTP request over an established tunnel (the tunnel is opaque, so plain
// HTTP inside CONNECT exercises the same byte path as TLS would).
inline RawResponse RequestOverTunnel(forwarding::net::Connection& tunnel, const std::string& host, const std::string& path,
                                     bool close = false, double timeout_ms = 10'000.0) {
  namespace net = forwarding::net;
  RawResponse r;
  r.connected = tunnel.valid();
  tunnel.SendAll("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n" + (close ? "Connection: close\r\n" : "") + "\r\n");
  std::string head;
  if (tunnel.ReadHead(head, 64 * 1024, net::After(timeout_ms), {}) != net::ReadResult::Ok) return r;
  r.head = head;
  r.status = std::atoi(head.c_str() + 9);
  std::string lower = head;
  for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const auto cl = lower.find("\r\ncontent-length:");
  std::size_t length = cl == std::string::npos ? 0 : std::stoul(head.substr(cl + 17));
  while (r.body.size() < length) {
    if (tunnel.ReadSome(r.body, length - r.body.size(), net::After(timeout_ms), {}) != net::ReadResult::Ok) break;
  }
  return r;
}

}  // namespace aafr::test
