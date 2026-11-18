#pragma once

// A controlled HTTP/2 server for the V0.7.3 protocol verification tests.
// Header-only, Qt Network (QSslServer, Schannel), single-threaded on the Qt
// event loop of the test. TLS with the test-only certificate in
// tests/fixtures/tls, ALPN restricted to "h2": a client that cannot speak
// HTTP/2 fails the handshake or the connection preface instead of silently
// falling back to HTTP/1.1.
//
// Implemented: the connection preface, SETTINGS (+ACK), HEADERS and
// CONTINUATION with a full HPACK decoder (static and dynamic table, size
// updates, every literal form), DATA with connection- and stream-level flow
// control in both directions, WINDOW_UPDATE, PING (+ACK), RST_STREAM and
// GOAWAY. Responses are HPACK-encoded without Huffman and without indexing.
//
// Huffman: RFC 7541's code is canonical, so the decoder rebuilds the codes
// from their lengths. Only symbols whose codes are 19 bits or shorter are
// included: NUL and every printable ASCII character, which is everything a
// browser puts in request headers. Any other code makes the request fail
// with a recorded HPACK error rather than decoding wrongly.
//
// Evidence kept per connection: negotiated ALPN protocol, requests decoded
// from HTTP/2 frames (path and stream id), the maximum number of streams
// open at once, RST_STREAM and GOAWAY frames received, protocol errors.
//
// Routes:
//   /h2/page            HTML page with 8 async scripts (/h2/asset.js, 150 ms each)
//   /h2/asset.js?i=&ms= a script that counts itself, answered after ms
//   /h2/large?bytes=N   N bytes (flow-controlled DATA)
//   /h2/slow?ms=N       answered after N ms
//   /h2/drop            response HEADERS, then the TCP connection is aborted
//   /favicon.ico        204

#include <QFile>
#include <QHash>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aafr::test {

// ---------------------------------------------------------------- HPACK

class HpackDecoder {
 public:
  using Header = std::pair<std::string, std::string>;

  // Decodes one complete header block. False (with a reason) on any error.
  bool Decode(const std::uint8_t* data, std::size_t size, std::vector<Header>* out, std::string* error) {
    std::size_t i = 0;
    while (i < size) {
      const std::uint8_t b = data[i];
      if (b & 0x80) {  // Indexed field.
        std::uint64_t index;
        if (!Int(data, size, &i, 7, &index) || index == 0) return Fail(error, "bad indexed field");
        Header h;
        if (!Lookup(index, &h)) return Fail(error, "index out of range");
        out->push_back(h);
      } else if ((b & 0xC0) == 0x40) {  // Literal with incremental indexing.
        Header h;
        if (!Literal(data, size, &i, 6, &h, error)) return false;
        Insert(h);
        out->push_back(h);
      } else if ((b & 0xE0) == 0x20) {  // Dynamic table size update.
        std::uint64_t max;
        if (!Int(data, size, &i, 5, &max) || max > 4096) return Fail(error, "bad table size update");
        max_size_ = static_cast<std::size_t>(max);
        Evict();
      } else {  // Literal without indexing (0000) or never indexed (0001).
        Header h;
        if (!Literal(data, size, &i, 4, &h, error)) return false;
        out->push_back(h);
      }
    }
    return true;
  }

 private:
  static bool Fail(std::string* error, const char* why) {
    if (error) *error = why;
    return false;
  }

  static bool Int(const std::uint8_t* d, std::size_t n, std::size_t* i, int prefix, std::uint64_t* value) {
    if (*i >= n) return false;
    const std::uint64_t limit = (1u << prefix) - 1;
    std::uint64_t v = d[(*i)++] & limit;
    if (v < limit) {
      *value = v;
      return true;
    }
    int shift = 0;
    for (;;) {
      if (*i >= n || shift > 56) return false;
      const std::uint8_t b = d[(*i)++];
      v += static_cast<std::uint64_t>(b & 0x7F) << shift;
      shift += 7;
      if (!(b & 0x80)) break;
    }
    *value = v;
    return true;
  }

  bool String(const std::uint8_t* d, std::size_t n, std::size_t* i, std::string* out, std::string* error) {
    if (*i >= n) return Fail(error, "truncated string");
    const bool huffman = d[*i] & 0x80;
    std::uint64_t length;
    if (!Int(d, n, i, 7, &length) || *i + length > n) return Fail(error, "bad string length");
    if (huffman) {
      if (!Huffman(d + *i, static_cast<std::size_t>(length), out)) return Fail(error, "huffman code outside the supported set");
    } else {
      out->assign(reinterpret_cast<const char*>(d + *i), static_cast<std::size_t>(length));
    }
    *i += static_cast<std::size_t>(length);
    return true;
  }

  bool Literal(const std::uint8_t* d, std::size_t n, std::size_t* i, int prefix, Header* h, std::string* error) {
    std::uint64_t index;
    if (!Int(d, n, i, prefix, &index)) return Fail(error, "bad literal");
    if (index != 0) {
      Header named;
      if (!Lookup(index, &named)) return Fail(error, "literal name index out of range");
      h->first = named.first;
    } else if (!String(d, n, i, &h->first, error)) {
      return false;
    }
    return String(d, n, i, &h->second, error);
  }

  bool Lookup(std::uint64_t index, Header* h) const {
    static const char* const kStatic[61][2] = {
        {":authority", ""}, {":method", "GET"}, {":method", "POST"}, {":path", "/"}, {":path", "/index.html"},
        {":scheme", "http"}, {":scheme", "https"}, {":status", "200"}, {":status", "204"}, {":status", "206"},
        {":status", "304"}, {":status", "400"}, {":status", "404"}, {":status", "500"}, {"accept-charset", ""},
        {"accept-encoding", "gzip, deflate"}, {"accept-language", ""}, {"accept-ranges", ""}, {"accept", ""},
        {"access-control-allow-origin", ""}, {"age", ""}, {"allow", ""}, {"authorization", ""},
        {"cache-control", ""}, {"content-disposition", ""}, {"content-encoding", ""}, {"content-language", ""},
        {"content-length", ""}, {"content-location", ""}, {"content-range", ""}, {"content-type", ""},
        {"cookie", ""}, {"date", ""}, {"etag", ""}, {"expect", ""}, {"expires", ""}, {"from", ""}, {"host", ""},
        {"if-match", ""}, {"if-modified-since", ""}, {"if-none-match", ""}, {"if-range", ""},
        {"if-unmodified-since", ""}, {"last-modified", ""}, {"link", ""}, {"location", ""}, {"max-forwards", ""},
        {"proxy-authenticate", ""}, {"proxy-authorization", ""}, {"range", ""}, {"referer", ""}, {"refresh", ""},
        {"retry-after", ""}, {"server", ""}, {"set-cookie", ""}, {"strict-transport-security", ""},
        {"transfer-encoding", ""}, {"user-agent", ""}, {"vary", ""}, {"via", ""}, {"www-authenticate", ""}};
    if (index >= 1 && index <= 61) {
      *h = {kStatic[index - 1][0], kStatic[index - 1][1]};
      return true;
    }
    const std::size_t dyn = static_cast<std::size_t>(index - 62);
    if (dyn >= dynamic_.size()) return false;
    *h = dynamic_[dyn];
    return true;
  }

  void Insert(const Header& h) {
    dynamic_.push_front(h);
    size_ += h.first.size() + h.second.size() + 32;
    Evict();
  }

  void Evict() {
    while (size_ > max_size_ && !dynamic_.empty()) {
      size_ -= dynamic_.back().first.size() + dynamic_.back().second.size() + 32;
      dynamic_.pop_back();
    }
  }

  // Canonical RFC 7541 Huffman codes for the supported symbols.
  struct Code {
    std::uint32_t code;
    int length;
    int symbol;
  };
  static const std::vector<Code>& Codes() {
    static const std::vector<Code> codes = [] {
      std::map<int, int> lengths = {{0, 13}};  // symbol -> code length
      const char* five = "012aceiost";
      for (const char* c = five; *c; ++c) lengths[*c] = 5;
      const char* six = " %-./3456789=A_bdfghlmnpru";
      for (const char* c = six; *c; ++c) lengths[*c] = 6;
      const char* seven = ":BCDEFGHIJKLMNOPQRSTUVWYjkqvwxyz";
      for (const char* c = seven; *c; ++c) lengths[*c] = 7;
      for (const char* c = "&*,;XZ"; *c; ++c) lengths[*c] = 8;
      for (const char* c = "!\"()?"; *c; ++c) lengths[*c] = 10;
      for (const char* c = "'+|"; *c; ++c) lengths[*c] = 11;
      for (const char* c = "#>"; *c; ++c) lengths[*c] = 12;
      for (const char* c = "$@[]~"; *c; ++c) lengths[*c] = 13;
      for (const char* c = "^}"; *c; ++c) lengths[*c] = 14;
      for (const char* c = "<`{"; *c; ++c) lengths[*c] = 15;
      lengths['\\'] = 19;
      std::vector<std::pair<int, int>> order;  // (length, symbol)
      for (const auto& e : lengths) order.push_back({e.second, e.first});
      std::sort(order.begin(), order.end());
      std::vector<Code> out;
      std::uint32_t code = 0;
      int previous = order.front().first;
      for (const auto& e : order) {
        code <<= (e.first - previous);
        previous = e.first;
        out.push_back({code, e.first, e.second});
        ++code;
      }
      return out;
    }();
    return codes;
  }

  static bool Huffman(const std::uint8_t* d, std::size_t n, std::string* out) {
    static const std::map<std::pair<int, std::uint32_t>, int> table = [] {
      std::map<std::pair<int, std::uint32_t>, int> t;
      for (const Code& c : Codes()) t[{c.length, c.code}] = c.symbol;
      return t;
    }();
    out->clear();
    std::uint32_t code = 0;
    int length = 0;
    for (std::size_t byte = 0; byte < n; ++byte) {
      for (int bit = 7; bit >= 0; --bit) {
        code = (code << 1) | ((d[byte] >> bit) & 1);
        ++length;
        const auto found = table.find({length, code});
        if (found != table.end()) {
          out->push_back(static_cast<char>(found->second));
          code = 0;
          length = 0;
        } else if (length > 19) {
          return false;
        }
      }
    }
    // Padding: fewer than 8 bits, all ones (a prefix of EOS).
    return length < 8 && code == ((1u << length) - 1);
  }

  std::deque<Header> dynamic_;
  std::size_t size_{0};
  std::size_t max_size_{4096};
};

inline void HpackLiteral(QByteArray* out, const std::string& name, const std::string& value) {
  // Literal without indexing, new name, no Huffman (lengths < 127).
  out->append(char(0x00));
  out->append(char(name.size()));
  out->append(name.data(), static_cast<int>(name.size()));
  out->append(char(value.size()));
  out->append(value.data(), static_cast<int>(value.size()));
}

// ---------------------------------------------------------------- server

class H2TestServer : public QObject {
 public:
  struct Request {
    int connection{0};
    std::uint32_t stream{0};
    std::string method;
    std::string path;
  };
  struct ConnectionStats {
    int id{0};
    QByteArray alpn;
    int requests{0};
    int max_open_streams{0};
    int rst_received{0};
    int goaway_received{0};
    int pings{0};
    std::string protocol_error;
    bool closed{false};
  };

  bool start(const QString& certFile, const QString& keyFile) {
    QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
    const auto certs = QSslCertificate::fromData(read(certFile));
    if (certs.isEmpty()) return false;
    certificate_ = certs.first();
    conf.setLocalCertificate(certificate_);
    conf.setPrivateKey(QSslKey(read(keyFile), QSsl::Rsa));
    conf.setAllowedNextProtocols({QByteArrayLiteral("h2")});
    server_.setSslConfiguration(conf);
    QObject::connect(&server_, &QSslServer::pendingConnectionAvailable, this, [this] {
      while (auto* s = qobject_cast<QSslSocket*>(server_.nextPendingConnection())) accept(s);
    });
    return server_.listen(QHostAddress::LocalHost, 0);
  }

  quint16 port() const { return server_.serverPort(); }
  QSslCertificate certificate() const { return certificate_; }
  const std::vector<Request>& requests() const { return requests_; }
  int requestCount(const std::string& prefix) const {
    int n = 0;
    for (const auto& r : requests_) n += r.path.rfind(prefix, 0) == 0 ? 1 : 0;
    return n;
  }
  std::vector<ConnectionStats> connections() const {
    std::vector<ConnectionStats> out;
    for (const auto& c : conns_) out.push_back(c.second.stats);
    std::sort(out.begin(), out.end(), [](const ConnectionStats& a, const ConnectionStats& b) { return a.id < b.id; });
    return out;
  }
  int openConnections() const {
    int n = 0;
    for (const auto& c : conns_) n += c.second.stats.closed ? 0 : 1;
    return n;
  }
  // Ends every connection (GOAWAY, then close).
  void closeAll() {
    for (auto& c : conns_) {
      if (c.second.stats.closed || !c.second.socket) continue;
      QByteArray payload(8, '\0');
      frame(c.second, 7, 0, 0, payload);
      c.second.socket->disconnectFromHost();
    }
  }

 private:
  struct Stream {
    QByteArray block;
    bool end_stream{false};
    bool headers_done{false};
    bool open{true};
    bool cancelled{false};
    std::int64_t window{65535};
    QByteArray body;
    int sent{0};
    bool body_queued{false};
  };
  struct Conn {
    QPointer<QSslSocket> socket;
    QByteArray in;
    bool preface{false};
    HpackDecoder decoder;
    std::map<std::uint32_t, Stream> streams;
    std::uint32_t continuation_stream{0};
    std::int64_t window{65535};
    std::int64_t peer_initial_window{65535};
    std::uint32_t peer_max_frame{16384};
    ConnectionStats stats;
  };

  static QByteArray read(const QString& p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
  }

  void accept(QSslSocket* s) {
    const int id = ++next_id_;
    Conn& c = conns_[id];
    c.socket = s;
    c.stats.id = id;
    c.stats.alpn = s->sslConfiguration().nextNegotiatedProtocol();
    QObject::connect(s, &QSslSocket::readyRead, this, [this, id] { onRead(id); });
    QObject::connect(s, &QSslSocket::disconnected, this, [this, id] {
      auto found = conns_.find(id);
      if (found != conns_.end()) found->second.stats.closed = true;
    });
    QObject::connect(s, &QSslSocket::disconnected, s, &QObject::deleteLater);
    // Server preface: SETTINGS (MAX_CONCURRENT_STREAMS = 100).
    QByteArray settings;
    appendSetting(&settings, 0x3, 100);
    frame(c, 4, 0, 0, settings);
    if (c.stats.alpn != "h2") fail(c, "ALPN did not select h2");
  }

  static void appendSetting(QByteArray* out, std::uint16_t id, std::uint32_t value) {
    out->append(char(id >> 8));
    out->append(char(id & 0xFF));
    for (int shift = 24; shift >= 0; shift -= 8) out->append(char((value >> shift) & 0xFF));
  }

  void frame(Conn& c, std::uint8_t type, std::uint8_t flags, std::uint32_t stream, const QByteArray& payload) {
    if (!c.socket) return;
    QByteArray f;
    const int n = payload.size();
    f.append(char((n >> 16) & 0xFF));
    f.append(char((n >> 8) & 0xFF));
    f.append(char(n & 0xFF));
    f.append(char(type));
    f.append(char(flags));
    for (int shift = 24; shift >= 0; shift -= 8) f.append(char((stream >> shift) & 0xFF));
    f.append(payload);
    c.socket->write(f);
  }

  void fail(Conn& c, const std::string& why) {
    if (c.stats.protocol_error.empty()) c.stats.protocol_error = why;
    QByteArray goaway(8, '\0');
    goaway[7] = 1;  // PROTOCOL_ERROR
    frame(c, 7, 0, 0, goaway);
    if (c.socket) c.socket->disconnectFromHost();
  }

  static std::uint32_t u32(const char* p) {
    return (std::uint32_t(std::uint8_t(p[0])) << 24) | (std::uint32_t(std::uint8_t(p[1])) << 16) |
           (std::uint32_t(std::uint8_t(p[2])) << 8) | std::uint32_t(std::uint8_t(p[3]));
  }

  int openStreams(const Conn& c) const {
    int n = 0;
    for (const auto& s : c.streams) n += s.second.open && s.second.headers_done ? 1 : 0;
    return n;
  }

  void onRead(int id) {
    auto found = conns_.find(id);
    if (found == conns_.end() || !found->second.socket) return;
    Conn& c = found->second;
    c.in += c.socket->readAll();
    static const QByteArray kPreface("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
    if (!c.preface) {
      if (c.in.size() < kPreface.size()) {
        if (!kPreface.startsWith(c.in)) fail(c, "not an HTTP/2 connection preface");
        return;
      }
      if (!c.in.startsWith(kPreface)) return fail(c, "not an HTTP/2 connection preface");
      c.in.remove(0, kPreface.size());
      c.preface = true;
    }
    while (c.in.size() >= 9) {
      const int length = (std::uint8_t(c.in[0]) << 16) | (std::uint8_t(c.in[1]) << 8) | std::uint8_t(c.in[2]);
      if (length > 1 << 20) return fail(c, "frame too large");
      if (c.in.size() < 9 + length) return;
      const std::uint8_t type = std::uint8_t(c.in[3]);
      const std::uint8_t flags = std::uint8_t(c.in[4]);
      const std::uint32_t stream = u32(c.in.constData() + 5) & 0x7FFFFFFF;
      const QByteArray payload = c.in.mid(9, length);
      c.in.remove(0, 9 + length);
      handle(c, type, flags, stream, payload);
      if (!c.socket || !c.stats.protocol_error.empty()) return;
    }
  }

  void handle(Conn& c, std::uint8_t type, std::uint8_t flags, std::uint32_t stream, const QByteArray& payload) {
    if (c.continuation_stream != 0 && (type != 9 || stream != c.continuation_stream)) {
      return fail(c, "expected CONTINUATION");
    }
    switch (type) {
      case 0: {  // DATA
        Stream& s = c.streams[stream];
        if (flags & 0x1) s.end_stream = true;
        if (!payload.isEmpty()) {  // Give the credit back at once.
          QByteArray inc(4, '\0');
          const std::uint32_t n = static_cast<std::uint32_t>(payload.size());
          for (int k = 0; k < 4; ++k) inc[k] = char((n >> (24 - 8 * k)) & 0xFF);
          frame(c, 8, 0, 0, inc);
          frame(c, 8, 0, stream, inc);
        }
        if (s.end_stream && s.headers_done) respond(c, stream);
        break;
      }
      case 1: {  // HEADERS
        if (stream == 0 || (stream % 2) == 0) return fail(c, "bad stream id for HEADERS");
        QByteArray block = payload;
        int pad = 0;
        if (flags & 0x8) {
          pad = std::uint8_t(block[0]);
          block.remove(0, 1);
        }
        if (flags & 0x20) block.remove(0, 5);  // Priority.
        if (pad > 0) block.chop(pad);
        Stream& s = c.streams[stream];
        s.block = block;
        s.end_stream = flags & 0x1;
        if (flags & 0x4) headersDone(c, stream);
        else c.continuation_stream = stream;
        break;
      }
      case 9: {  // CONTINUATION
        c.streams[stream].block += payload;
        if (flags & 0x4) {
          c.continuation_stream = 0;
          headersDone(c, stream);
        }
        break;
      }
      case 3: {  // RST_STREAM
        ++c.stats.rst_received;
        auto it = c.streams.find(stream);
        if (it != c.streams.end()) {
          it->second.cancelled = true;
          it->second.open = false;
        }
        break;
      }
      case 4: {  // SETTINGS
        if (flags & 0x1) break;  // ACK of ours.
        for (int k = 0; k + 6 <= payload.size(); k += 6) {
          const std::uint16_t id = (std::uint8_t(payload[k]) << 8) | std::uint8_t(payload[k + 1]);
          const std::uint32_t value = u32(payload.constData() + k + 2);
          if (id == 0x4) {
            const std::int64_t delta = std::int64_t(value) - c.peer_initial_window;
            c.peer_initial_window = value;
            for (auto& s : c.streams) s.second.window += delta;
          } else if (id == 0x5) {
            c.peer_max_frame = value;
          }
        }
        frame(c, 4, 0x1, 0, QByteArray());
        pump(c);
        break;
      }
      case 6: {  // PING
        if (!(flags & 0x1)) {
          ++c.stats.pings;
          frame(c, 6, 0x1, 0, payload);
        }
        break;
      }
      case 7:  // GOAWAY
        ++c.stats.goaway_received;
        break;
      case 8: {  // WINDOW_UPDATE
        if (payload.size() != 4) return fail(c, "bad WINDOW_UPDATE");
        const std::int64_t inc = u32(payload.constData()) & 0x7FFFFFFF;
        if (stream == 0) c.window += inc;
        else c.streams[stream].window += inc;
        pump(c);
        break;
      }
      default:  // PRIORITY, PRIORITY_UPDATE, unknown: ignored as the RFC requires.
        break;
    }
  }

  void headersDone(Conn& c, std::uint32_t stream) {
    Stream& s = c.streams[stream];
    s.window = c.peer_initial_window;
    std::vector<HpackDecoder::Header> headers;
    std::string error;
    if (!c.decoder.Decode(reinterpret_cast<const std::uint8_t*>(s.block.constData()),
                          static_cast<std::size_t>(s.block.size()), &headers, &error)) {
      return fail(c, "HPACK: " + error);
    }
    s.headers_done = true;
    Request r;
    r.connection = c.stats.id;
    r.stream = stream;
    for (const auto& h : headers) {
      if (h.first == ":method") r.method = h.second;
      if (h.first == ":path") r.path = h.second;
    }
    requests_.push_back(r);
    ++c.stats.requests;
    c.stats.max_open_streams = std::max(c.stats.max_open_streams, openStreams(c));
    if (s.end_stream) respond(c, stream);
  }

  void respond(Conn& c, std::uint32_t stream) {
    const Request* request = nullptr;
    for (auto it = requests_.rbegin(); it != requests_.rend(); ++it) {
      if (it->connection == c.stats.id && it->stream == stream) {
        request = &*it;
        break;
      }
    }
    if (!request) return;
    const QUrl url(QStringLiteral("https://x") + QString::fromStdString(request->path));
    const QUrlQuery q(url);
    const QString path = url.path();
    const int id = c.stats.id;
    const auto later = [this, id, stream](int ms, int status, QByteArray type, QByteArray body) {
      QTimer::singleShot(ms, this, [this, id, stream, status, type, body] {
        auto found = conns_.find(id);
        if (found == conns_.end() || found->second.stats.closed) return;
        send(found->second, stream, status, type, body);
      });
    };
    if (path == "/h2/page") {
      QByteArray html = "<!doctype html><html><head><title>AAFR h2 page</title>";
      for (int i = 0; i < 8; ++i) html += "<script async src=\"/h2/asset.js?i=" + QByteArray::number(i) + "&ms=150\"></script>";
      html += "</head><body><p id=\"content\">h2-page-body</p></body></html>";
      later(0, 200, "text/html", html);
    } else if (path == "/h2/asset.js") {
      later(q.queryItemValue("ms").toInt(), 200, "application/javascript",
            "window.__h2assets = (window.__h2assets || 0) + 1;");
    } else if (path == "/h2/large") {
      later(0, 200, "application/octet-stream", QByteArray(q.queryItemValue("bytes").toInt(), 'h'));
    } else if (path == "/h2/slow") {
      later(q.queryItemValue("ms").toInt(), 200, "text/html",
            "<!doctype html><title>AAFR h2 slow</title><p>slow-done</p>");
    } else if (path == "/h2/drop") {
      QByteArray block;
      block.append(char(0x88));  // :status 200
      HpackLiteral(&block, "content-type", "text/html");
      HpackLiteral(&block, "content-length", "100000");
      frame(c, 1, 0x4, stream, block);
      QPointer<QSslSocket> s = c.socket;
      QTimer::singleShot(50, this, [s] {
        if (s) s->abort();
      });
    } else if (path == "/favicon.ico") {
      later(0, 204, "", QByteArray());
    } else {
      later(0, 404, "text/plain", "not found");
    }
  }

  void send(Conn& c, std::uint32_t stream, int status, const QByteArray& type, const QByteArray& body) {
    Stream& s = c.streams[stream];
    if (s.cancelled || !s.open) return;
    QByteArray block;
    if (status == 200) block.append(char(0x88));
    else if (status == 204) block.append(char(0x89));
    else if (status == 404) block.append(char(0x8D));
    else HpackLiteral(&block, ":status", std::to_string(status));
    if (!type.isEmpty()) HpackLiteral(&block, "content-type", type.toStdString());
    HpackLiteral(&block, "content-length", std::to_string(body.size()));
    HpackLiteral(&block, "cache-control", "no-store");
    HpackLiteral(&block, "x-aafr-protocol", "h2");
    HpackLiteral(&block, "x-aafr-connection", std::to_string(c.stats.id));
    HpackLiteral(&block, "x-aafr-stream", std::to_string(stream));
    frame(c, 1, body.isEmpty() ? 0x5 : 0x4, stream, block);
    if (body.isEmpty()) {
      s.open = false;
      return;
    }
    s.body = body;
    s.sent = 0;
    s.body_queued = true;
    pump(c);
  }

  // Sends queued DATA within the connection and stream windows.
  void pump(Conn& c) {
    for (auto& entry : c.streams) {
      Stream& s = entry.second;
      while (s.body_queued && !s.cancelled && s.sent < s.body.size() && c.window > 0 && s.window > 0) {
        const std::int64_t room = std::min<std::int64_t>({c.window, s.window, std::int64_t(c.peer_max_frame),
                                                          std::int64_t(s.body.size() - s.sent)});
        const QByteArray chunk = s.body.mid(s.sent, static_cast<int>(room));
        s.sent += chunk.size();
        c.window -= chunk.size();
        s.window -= chunk.size();
        const bool last = s.sent == s.body.size();
        frame(c, 0, last ? 0x1 : 0x0, entry.first, chunk);
        if (last) {
          s.body_queued = false;
          s.open = false;
          s.body.clear();
        }
      }
    }
  }

  QSslServer server_;
  QSslCertificate certificate_;
  std::map<int, Conn> conns_;
  std::vector<Request> requests_;
  int next_id_{0};
};

}  // namespace aafr::test
