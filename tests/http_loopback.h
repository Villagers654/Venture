#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace venture::test {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid = INVALID_SOCKET;
inline void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket invalid = -1;
inline void close_socket(Socket s) { close(s); }
#endif
// A real loopback HTTP server exercises libcurl framing/status handling. All
// credentials and identities here are synthetic; no SecretStore is opened.
class Server {
public:
  Server() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
      throw std::runtime_error("WSAStartup");
#endif
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ == invalid)
      throw std::runtime_error("test socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
      throw std::runtime_error("test bind");
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &size))
      throw std::runtime_error("test getsockname");
    port = ntohs(address.sin_port);
    if (listen(socket_, 4))
      throw std::runtime_error("test listen");
    worker_ = std::jthread([this](std::stop_token stop) { serve(stop); });
  }
  ~Server() {
    worker_.request_stop();
    worker_.join();
    close_socket(socket_);
#ifdef _WIN32
    WSACleanup();
#endif
  }
  void push(std::string body, int status = 200) {
    std::lock_guard lock(mutex_);
    responses_.emplace_back(status, std::move(body));
  }
  std::vector<std::string> requests() {
    std::lock_guard lock(mutex_);
    return requests_;
  }
  std::uint16_t port = 0;

private:
  void serve(std::stop_token stop) {
    while (!stop.stop_requested()) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(socket_, &set);
      timeval timeout{0, 100000};
      if (select(static_cast<int>(socket_ + 1), &set, nullptr, nullptr,
                 &timeout) <= 0)
        continue;
      Socket client = accept(socket_, nullptr, nullptr);
      if (client == invalid)
        continue;
      std::string request;
      char buffer[4096];
      while (!stop.stop_requested()) {
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(client, &readers);
        timeval read_timeout{0, 100000};
        if (select(static_cast<int>(client + 1), &readers, nullptr, nullptr,
                   &read_timeout) <= 0)
          continue;
        auto n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0)
          break;
        request.append(buffer, n);
        auto end = request.find("\r\n\r\n");
        if (end == std::string::npos)
          continue;
        std::size_t length = 0;
        auto at = request.find("Content-Length: ");
        if (at != std::string::npos)
          length = std::stoul(request.substr(at + 16));
        if (request.size() >= end + 4 + length)
          break;
      }
      std::pair<int, std::string> response{500, "{}"};
      {
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
        if (!responses_.empty()) {
          response = std::move(responses_.front());
          responses_.pop_front();
        }
      }
      auto bytes =
          "HTTP/1.1 " + std::to_string(response.first) +
          " Test\r\nContent-Type: application/json\r\nContent-Length: " +
          std::to_string(response.second.size()) +
          "\r\nConnection: close\r\n\r\n" + response.second;
      std::size_t sent = 0;
      while (sent < bytes.size()) {
        auto n = send(client, bytes.data() + sent,
                      static_cast<int>(bytes.size() - sent), 0);
        if (n <= 0)
          break;
        sent += n;
      }
      close_socket(client);
    }
  }
  Socket socket_ = invalid;
  std::mutex mutex_;
  std::deque<std::pair<int, std::string>> responses_;
  std::vector<std::string> requests_;
  std::jthread worker_;
};
} // namespace venture::test
