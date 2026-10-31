#include "common/ipc.h"
#include "common/platform/pipe.h"
#include <limits>

namespace venture {
PipeChannel::~PipeChannel() { close(); }
void PipeChannel::close() {
  platform::close_pipe(read_.exchange(0));
  platform::close_pipe(write_.exchange(0));
}
std::array<std::shared_ptr<PipeChannel>, 2> PipeChannel::pair() {
  auto handles = platform::make_pipes();
  return {
      std::shared_ptr<PipeChannel>(new PipeChannel(handles[0], handles[1])),
      std::shared_ptr<PipeChannel>(new PipeChannel(handles[2], handles[3]))};
}
std::shared_ptr<PipeChannel> PipeChannel::inherited(std::uintptr_t read,
                                                    std::uintptr_t write) {
  if (!platform::valid_pipe(read) || !platform::valid_pipe(write) ||
      read == write)
    throw std::runtime_error("Invalid inherited credential channel.");
  return std::shared_ptr<PipeChannel>(new PipeChannel(read, write));
}
void PipeChannel::send(const Json &value) {
  auto bytes = value.dump();
  if (bytes.empty() || bytes.size() > 1024 * 1024)
    throw std::runtime_error("Credential channel message exceeds its limit.");
  std::array<unsigned char, 4> length{};
  for (unsigned i = 0; i < 4; ++i)
    length[i] = static_cast<unsigned char>(bytes.size() >> (8 * i));
  platform::write_pipe(write_.load(), length.data(), length.size());
  platform::write_pipe(write_.load(), bytes.data(), bytes.size());
}
Json PipeChannel::receive() {
  std::array<unsigned char, 4> length{};
  platform::read_pipe(read_.load(), length.data(), length.size());
  std::uint32_t size = 0;
  for (unsigned i = 0; i < 4; ++i)
    size |= std::uint32_t(length[i]) << (8 * i);
  if (!size || size > 1024 * 1024)
    throw std::runtime_error("Invalid credential channel message length.");
  std::string bytes(size, '\0');
  platform::read_pipe(read_.load(), bytes.data(), bytes.size());
  auto j = Json::parse(bytes, nullptr, false);
  if (!j.is_object())
    throw std::runtime_error("Invalid credential channel message.");
  return j;
}
Json PipeChannel::call(const Json &request) {
  std::lock_guard lock(mutex_);
  send(request);
  return receive();
}
Json PipeChannel::accept(const Handler &handler) {
  std::lock_guard lock(mutex_);
  auto request = receive();
  Json response;
  try {
    response = handler(request);
  } catch (...) {
    response = Json{{"error", "Native messaging host failed"}};
  }
  send(response);
  return response;
}
void PipeChannel::serve(const Handler &handler, std::stop_token stop) {
  try {
    while (!stop.stop_requested()) {
      auto request = receive();
      Json response;
      try {
        response = handler(request);
      } catch (...) {
        response = Json{{"error", "Credential broker unavailable"}};
      }
      send(response);
    }
  } catch (...) {
  } // EOF/parent or child teardown. Never echo request/exception text.
  close();
}
} // namespace venture
