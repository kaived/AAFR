#pragma once

// A controlled RFC 6455 WebSocket server for the V0.7.3 protocol
// verification tests. Header-only, Qt Network, single-threaded on the test's
// Qt event loop. Plain (ws://, QTcpServer) or TLS (wss://, QSslServer with
// the test-only certificate, Schannel).
//
// A plain GET without Upgrade gets a small HTML page, so a test can first
// navigate to the server's origin (and, for wss, accept exactly the test
// certificate once) before opening sockets from that page.
//
// Handshake: GET with "Upgrade: websocket", "Connection: Upgrade",
// Sec-WebSocket-Version 13 and a 16-byte base64 Sec-WebSocket-Key; the answer
// carries the RFC 6455 accept hash. Anything else gets 400.
// Frames: masked client frames are required; fragmented messages are
// reassembled; 7/16/64-bit lengths; messages are limited to 16 MiB (close
// 1009 beyond that). Control frames: ping (answered with pong), pong
// (counted), close (echoed, then the TCP connection is closed).
//
// Application protocol (text messages):
//   echo:<x>   -> text echo:<x>
//   id?        -> text conn:<server connection id>
//   ping-me    -> server ping "aafr-ping"; when the browser's pong arrives,
//                 text pong-received
//   big:<n>    -> binary message of n bytes (byte i = i % 251)
//   close-me   -> server close 1000 "server closing"
//   drop-me    -> TCP connection aborted without a close frame
// Binary messages are echoed unchanged.

#include <QCryptographicHash>
#include <QFile>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aafr::test {

class WsTestServer : public QObject {
 public:
  struct ConnectionStats {
    int id{0};
    bool upgraded{false};
    int text_in{0};
    int binary_in{0};
    int messages_out{0};
    qint64 bytes_in{0};
    qint64 largest_message_in{0};
    int pongs{0};
    int pings_from_client{0};
    int close_code_received{0};
    bool closed{false};
    bool aborted_by_server{false};
    std::string end;  // close_frame | tcp_closed | aborted
  };

  // Plain ws:// when certFile is empty.
  bool start(const QString& certFile = QString(), const QString& keyFile = QString()) {
    if (certFile.isEmpty()) {
      server_ = std::make_unique<QTcpServer>();
      QObject::connect(server_.get(), &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* s = server_->nextPendingConnection()) accept(s);
      });
    } else {
      auto tls = std::make_unique<QSslServer>();
      QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
      const auto certs = QSslCertificate::fromData(read(certFile));
      if (certs.isEmpty()) return false;
      certificate_ = certs.first();
      conf.setLocalCertificate(certificate_);
      conf.setPrivateKey(QSslKey(read(keyFile), QSsl::Rsa));
      tls->setSslConfiguration(conf);
      QSslServer* raw = tls.get();
      QObject::connect(raw, &QSslServer::pendingConnectionAvailable, this, [this, raw] {
        while (QTcpSocket* s = raw->nextPendingConnection()) accept(s);
      });
      server_ = std::move(tls);
    }
    return server_->listen(QHostAddress::LocalHost, 0);
  }

  quint16 port() const { return server_->serverPort(); }
  QSslCertificate certificate() const { return certificate_; }
  int pagesServed() const { return pages_; }
  int handshakes() const { return handshakes_; }
  int rejectedHandshakes() const { return rejected_; }
  std::vector<ConnectionStats> connections() const {
    std::vector<ConnectionStats> out;
    for (const auto& c : conns_) out.push_back(c.second.stats);
    return out;
  }
  ConnectionStats connection(int id) const {
    const auto found = conns_.find(id);
    return found == conns_.end() ? ConnectionStats{} : found->second.stats;
  }
  int openWebSockets() const {
    int n = 0;
    for (const auto& c : conns_) n += (c.second.stats.upgraded && !c.second.stats.closed) ? 1 : 0;
    return n;
  }
  // Sends a text message to one connection (server push).
  bool push(int id, const QByteArray& text) {
    auto found = conns_.find(id);
    if (found == conns_.end() || found->second.stats.closed) return false;
    sendFrame(found->second, 0x1, text);
    return true;
  }

 private:
  struct Conn {
    QPointer<QTcpSocket> socket;
    QByteArray in;
    QByteArray message;
    int message_opcode{0};
    bool close_sent{false};
    ConnectionStats stats;
  };

  static QByteArray read(const QString& p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
  }

  void accept(QTcpSocket* s) {
    const int id = ++next_id_;
    Conn& c = conns_[id];
    c.socket = s;
    c.stats.id = id;
    QObject::connect(s, &QTcpSocket::readyRead, this, [this, id] { onRead(id); });
    QObject::connect(s, &QTcpSocket::disconnected, this, [this, id] {
      auto found = conns_.find(id);
      if (found == conns_.end()) return;
      found->second.stats.closed = true;
      if (found->second.stats.end.empty()) found->second.stats.end = "tcp_closed";
    });
    QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
  }

  static QByteArray headerValue(const QByteArray& head, const QByteArray& name) {
    for (const QByteArray& line : head.split('\n')) {
      const int colon = line.indexOf(':');
      if (colon > 0 && line.left(colon).trimmed().toLower() == name) return line.mid(colon + 1).trimmed();
    }
    return QByteArray();
  }

  void onRead(int id) {
    auto found = conns_.find(id);
    if (found == conns_.end() || !found->second.socket) return;
    Conn& c = found->second;
    c.in += c.socket->readAll();
    if (!c.stats.upgraded) {
      const int end = c.in.indexOf("\r\n\r\n");
      if (end < 0) {
        if (c.in.size() > 64 * 1024) c.socket->abort();
        return;
      }
      const QByteArray head = c.in.left(end);
      c.in.remove(0, end + 4);
      if (headerValue(head, "upgrade").toLower() != "websocket") {
        ++pages_;
        const QByteArray body =
            "<!doctype html><html><head><title>AAFR WebSocket page</title></head><body><p>ws-page</p></body></html>";
        c.socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        c.socket->disconnectFromHost();
        return;
      }
      const QByteArray key = headerValue(head, "sec-websocket-key");
      if (!headerValue(head, "connection").toLower().contains("upgrade") ||
          headerValue(head, "sec-websocket-version") != "13" || QByteArray::fromBase64(key).size() != 16) {
        ++rejected_;
        c.socket->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        c.socket->disconnectFromHost();
        return;
      }
      const QByteArray accept =
          QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
      c.socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
      c.stats.upgraded = true;
      ++handshakes_;
    }
    parseFrames(c);
  }

  void parseFrames(Conn& c) {
    for (;;) {
      if (c.in.size() < 2 || !c.socket) return;
      const auto b0 = static_cast<std::uint8_t>(c.in[0]);
      const auto b1 = static_cast<std::uint8_t>(c.in[1]);
      const bool fin = b0 & 0x80;
      const int opcode = b0 & 0x0F;
      const bool masked = b1 & 0x80;
      qint64 length = b1 & 0x7F;
      int offset = 2;
      if (length == 126) {
        if (c.in.size() < 4) return;
        length = (static_cast<std::uint8_t>(c.in[2]) << 8) | static_cast<std::uint8_t>(c.in[3]);
        offset = 4;
      } else if (length == 127) {
        if (c.in.size() < 10) return;
        length = 0;
        for (int k = 0; k < 8; ++k) length = (length << 8) | static_cast<std::uint8_t>(c.in[2 + k]);
        offset = 10;
      }
      if (!masked) return closeWith(c, 1002, "client frames must be masked");
      if (length > 16 * 1024 * 1024) return closeWith(c, 1009, "message too big");
      if (c.in.size() < offset + 4 + length) return;
      const QByteArray mask = c.in.mid(offset, 4);
      QByteArray payload = c.in.mid(offset + 4, static_cast<int>(length));
      for (int k = 0; k < payload.size(); ++k) payload[k] = char(payload[k] ^ mask[k % 4]);
      c.in.remove(0, offset + 4 + static_cast<int>(length));
      c.stats.bytes_in += length;
      if (opcode >= 0x8) {
        control(c, opcode, payload);
        continue;
      }
      if (opcode != 0) {
        c.message = payload;
        c.message_opcode = opcode;
      } else {
        c.message += payload;
      }
      if (c.message.size() > 16 * 1024 * 1024) return closeWith(c, 1009, "message too big");
      if (fin) {
        message(c, c.message_opcode, c.message);
        c.message.clear();
      }
    }
  }

  void control(Conn& c, int opcode, const QByteArray& payload) {
    if (opcode == 0x8) {  // close
      if (payload.size() >= 2)
        c.stats.close_code_received = (static_cast<std::uint8_t>(payload[0]) << 8) | static_cast<std::uint8_t>(payload[1]);
      if (c.stats.end.empty()) c.stats.end = "close_frame";
      // Answer a client-initiated close once; after our own close frame the
      // client's close completes the handshake and only TCP closes.
      if (!c.close_sent) {
        c.close_sent = true;
        sendFrame(c, 0x8, payload.left(2));
      }
      c.socket->disconnectFromHost();
    } else if (opcode == 0x9) {  // ping from the client
      ++c.stats.pings_from_client;
      sendFrame(c, 0xA, payload);
    } else if (opcode == 0xA) {  // pong
      ++c.stats.pongs;
      if (payload == "aafr-ping") sendFrame(c, 0x1, "pong-received");
    }
  }

  void message(Conn& c, int opcode, const QByteArray& m) {
    c.stats.largest_message_in = std::max<qint64>(c.stats.largest_message_in, m.size());
    if (opcode == 0x2) {
      ++c.stats.binary_in;
      sendFrame(c, 0x2, m);
      return;
    }
    ++c.stats.text_in;
    if (m.startsWith("echo:")) {
      sendFrame(c, 0x1, m);
    } else if (m == "id?") {
      sendFrame(c, 0x1, "conn:" + QByteArray::number(c.stats.id));
    } else if (m == "ping-me") {
      sendFrame(c, 0x9, "aafr-ping");
    } else if (m.startsWith("big:")) {
      const int n = m.mid(4).toInt();
      QByteArray big(n, '\0');
      for (int k = 0; k < n; ++k) big[k] = char(k % 251);
      sendFrame(c, 0x2, big);
    } else if (m == "close-me") {
      closeWith(c, 1000, "server closing");
    } else if (m == "drop-me") {
      c.stats.aborted_by_server = true;
      c.stats.end = "aborted";
      c.stats.closed = true;
      c.socket->abort();
    } else {
      sendFrame(c, 0x1, "unknown:" + m.left(32));
    }
  }

  void closeWith(Conn& c, int code, const QByteArray& reason) {
    QByteArray payload;
    payload.append(char(code >> 8));
    payload.append(char(code & 0xFF));
    payload += reason;
    c.close_sent = true;
    sendFrame(c, 0x8, payload);
    if (c.stats.end.empty()) c.stats.end = "server_close_" + std::to_string(code);
    QPointer<QTcpSocket> s = c.socket;
    QTimer::singleShot(1000, this, [s] {
      if (s) s->disconnectFromHost();
    });
  }

  void sendFrame(Conn& c, int opcode, const QByteArray& payload) {
    if (!c.socket) return;
    QByteArray f;
    f.append(char(0x80 | opcode));
    const qint64 n = payload.size();
    if (n < 126) {
      f.append(char(n));
    } else if (n < 65536) {
      f.append(char(126));
      f.append(char((n >> 8) & 0xFF));
      f.append(char(n & 0xFF));
    } else {
      f.append(char(127));
      for (int k = 7; k >= 0; --k) f.append(char((n >> (8 * k)) & 0xFF));
    }
    f.append(payload);
    c.socket->write(f);
    if (opcode < 0x8) ++c.stats.messages_out;
  }

  std::unique_ptr<QTcpServer> server_;
  QSslCertificate certificate_;
  std::map<int, Conn> conns_;
  int next_id_{0};
  int pages_{0};
  int handshakes_{0};
  int rejected_{0};
};

}  // namespace aafr::test
