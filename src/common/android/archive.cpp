#include "common/android/archive.h"

#include <miniz.h>

#include <fstream>
#include <mutex>

namespace venture::android {
struct Archive::State {
  std::ifstream file;
  std::uint64_t size = 0;
  mz_zip_archive zip{};
  mutable std::mutex mutex;  // one reader position
  ~State() { mz_zip_reader_end(&zip); }
};
namespace {
size_t read_at(void *opaque, mz_uint64 offset, void *buffer, size_t n) {
  auto &state = *static_cast<Archive::State *>(opaque);
  state.file.clear();
  state.file.seekg(std::streamoff(offset));
  state.file.read(static_cast<char *>(buffer), std::streamsize(n));
  return size_t(state.file.gcount());
}
}  // namespace
Archive::Archive(const fs::path &file) : state_(std::make_unique<State>()) {
  state_->file.open(file, std::ios::binary);
  std::error_code ec;
  state_->size = fs::file_size(file, ec);
  if (!state_->file || ec) throw std::runtime_error("Could not open " + paths::utf8(file.filename()) + ".");
  state_->zip.m_pRead = read_at;
  state_->zip.m_pIO_opaque = state_.get();
  if (!mz_zip_reader_init(&state_->zip, state_->size, 0))
    throw std::runtime_error(paths::utf8(file.filename()) + " is not a readable ZIP archive.");
  const auto count = mz_zip_reader_get_num_files(&state_->zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&state_->zip, i, &stat))
      throw std::runtime_error(paths::utf8(file.filename()) + " has an unreadable entry.");
    if (stat.m_is_directory) continue;
    entries_.push_back({stat.m_filename, stat.m_uncomp_size, i});
  }
}
Archive::~Archive() = default;
const Archive::Entry *Archive::find(std::string_view name) const {
  for (auto &e : entries_)
    if (e.name == name) return &e;
  return nullptr;
}
std::vector<std::uint8_t> Archive::read(const Entry &entry, std::uint64_t limit) const {
  if (entry.size > limit) throw std::runtime_error("Archive member " + entry.name + " is too large.");
  std::vector<std::uint8_t> data(entry.size);
  std::lock_guard lock(state_->mutex);
  if (!mz_zip_reader_extract_to_mem(&state_->zip, entry.index, data.data(), data.size(), 0))
    throw std::runtime_error("Could not read " + entry.name + " from the archive.");
  return data;
}
void Archive::extract(const Entry &entry, const std::function<void(std::span<const std::uint8_t>)> &sink,
                      std::stop_token stop) const {
  std::lock_guard lock(state_->mutex);
  auto *it = mz_zip_reader_extract_iter_new(&state_->zip, entry.index, 0);
  if (!it) throw std::runtime_error("Could not read " + entry.name + " from the archive.");
  std::vector<std::uint8_t> buffer(1 << 20);
  try {
    for (;;) {
      if (stop.stop_requested()) throw std::runtime_error("Transfer paused.");
      const auto n = mz_zip_reader_extract_iter_read(it, buffer.data(), buffer.size());
      if (!n) break;
      sink({buffer.data(), n});
    }
  } catch (...) {
    mz_zip_reader_extract_iter_free(it);
    throw;
  }
  // Frees the iterator and reports a CRC or size mismatch.
  if (!mz_zip_reader_extract_iter_free(it))
    throw std::runtime_error(entry.name + " in the archive is damaged.");
}
bool is_archive(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  char magic[4]{};
  in.read(magic, 4);
  return in.gcount() == 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
}
}  // namespace venture::android
