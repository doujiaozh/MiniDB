#ifndef MINIDB_NET_CLIENT_H
#define MINIDB_NET_CLIENT_H

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace net {

std::string EncodeLength(size_t value) {
  char buffer[32];
  int length = 0;
  do {
    buffer[length++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value > 0);
  std::string result;
  while (length > 0) result.push_back(buffer[--length]);
  return result;
}

struct Response {
  char type;
  long long number;
  std::string text;

  Response() : type(0), number(0) {}
  bool IsError() const { return type == '-'; }
  bool IsNil() const { return type == '$' && number < 0; }
};

class Connection {
 public:
  Connection() : socket_(INVALID_SOCKET) {}
  ~Connection() { Close(); }

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  bool Connect(const std::string& host, int port, int timeout_ms) {
    Close();
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%d", port);
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), port_text, &hints, &addresses) != 0) return false;
    SOCKET connected = INVALID_SOCKET;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
      SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (candidate == INVALID_SOCKET) continue;
      u_long nonblocking = 1;
      ioctlsocket(candidate, FIONBIO, &nonblocking);
      int result = connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
      bool established = result == 0;
      if (!established) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
          fd_set write_set;
          FD_ZERO(&write_set);
          FD_SET(candidate, &write_set);
          timeval wait_time;
          wait_time.tv_sec = timeout_ms / 1000;
          wait_time.tv_usec = (timeout_ms % 1000) * 1000;
          int ready = select(0, nullptr, &write_set, nullptr, &wait_time);
          if (ready > 0) {
            int socket_error = 0;
            int option_length = static_cast<int>(sizeof(socket_error));
            getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socket_error), &option_length);
            established = socket_error == 0;
          }
        }
      }
      if (established) {
        u_long blocking = 0;
        ioctlsocket(candidate, FIONBIO, &blocking);
        connected = candidate;
        break;
      }
      closesocket(candidate);
    }
    freeaddrinfo(addresses);
    if (connected == INVALID_SOCKET) return false;
    socket_ = connected;
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    return true;
  }

  void Close() {
    if (socket_ != INVALID_SOCKET) {
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
    buffer_.clear();
  }

  bool IsOpen() const { return socket_ != INVALID_SOCKET; }

  bool SendLine(const std::string& line) {
    std::string data = line;
    data.append("\r\n");
    return SendAll(data.data(), data.size());
  }

  bool SendCommand(const std::vector<std::string>& parts) {
    std::string encoded;
    encoded.push_back('*');
    encoded.append(EncodeLength(parts.size()));
    encoded.append("\r\n");
    for (size_t i = 0; i < parts.size(); ++i) {
      encoded.push_back('$');
      encoded.append(EncodeLength(parts[i].size()));
      encoded.append("\r\n");
      encoded.append(parts[i]);
      encoded.append("\r\n");
    }
    return SendAll(encoded.data(), encoded.size());
  }

  bool SendRaw(const char* data, size_t size) { return SendAll(data, size); }

  bool ReadLine(std::string* line) {
    for (;;) {
      size_t position = buffer_.find("\r\n");
      if (position != std::string::npos) {
        *line = buffer_.substr(0, position);
        buffer_.erase(0, position + 2);
        return true;
      }
      if (buffer_.size() > 32u * 1024u * 1024u) return false;
      if (!Fill()) return false;
    }
  }

  bool ReadExact(void* data, size_t size) {
    char* destination = static_cast<char*>(data);
    size_t copied = 0;
    while (copied < size) {
      if (buffer_.empty() && !Fill()) return false;
      size_t chunk = buffer_.size() < size - copied ? buffer_.size() : size - copied;
      std::memcpy(destination + copied, buffer_.data(), chunk);
      buffer_.erase(0, chunk);
      copied += chunk;
    }
    return true;
  }

  bool ReadResponse(Response* response) {
    std::string line;
    if (!ReadLine(&line)) return false;
    response->text.clear();
    response->number = 0;
    if (line.empty()) {
      response->type = 0;
      return false;
    }
    response->type = line[0];
    if (response->type == '$') {
      long long length = ParseSigned(line.substr(1));
      response->number = length;
      if (length < 0) return true;
      if (length > 256ll * 1024ll * 1024ll) return false;
      response->text.resize(static_cast<size_t>(length));
      if (length > 0 && !ReadExact(&response->text[0], static_cast<size_t>(length))) return false;
      char terminator[2];
      return ReadExact(terminator, 2);
    }
    if (response->type == ':') {
      response->number = ParseSigned(line.substr(1));
      response->text = line.substr(1);
      return true;
    }
    response->text = line.substr(1);
    return true;
  }

 private:
  bool SendAll(const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
      size_t remaining = size - sent;
      int chunk = static_cast<int>(remaining > (1u << 20) ? (1u << 20) : remaining);
      int written = send(socket_, data + sent, chunk, 0);
      if (written <= 0) return false;
      sent += static_cast<size_t>(written);
    }
    return true;
  }

  bool Fill() {
    char chunk[8192];
    int received = recv(socket_, chunk, sizeof(chunk), 0);
    if (received <= 0) return false;
    buffer_.append(chunk, static_cast<size_t>(received));
    return true;
  }

  static long long ParseSigned(const std::string& text) {
    if (text.empty()) return 0;
    size_t position = 0;
    bool negative = false;
    if (text[0] == '-') {
      negative = true;
      position = 1;
    }
    long long value = 0;
    for (; position < text.size(); ++position) {
      char c = text[position];
      if (c < '0' || c > '9') break;
      value = value * 10 + (c - '0');
    }
    return negative ? -value : value;
  }

  SOCKET socket_;
  std::string buffer_;
};

}

#endif
