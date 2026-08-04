#include "strata/series_catalog.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "strata/byteio.hpp"

namespace strata {

std::string CanonicalLabelString(
    std::vector<std::pair<std::string, std::string>> labels) {
  std::sort(labels.begin(), labels.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::string out;
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i > 0) out += ',';
    out += labels[i].first;
    out += '=';
    out += labels[i].second;
  }
  return out;
}

SeriesCatalog::SeriesCatalog(const std::string& path) : path_(path) {
  Replay();
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd_ < 0) {
    throw std::runtime_error("SeriesCatalog: open failed for " + path_ +
                              ": " + std::strerror(errno));
  }
}

SeriesCatalog::~SeriesCatalog() {
  if (fd_ >= 0) ::close(fd_);
}

void SeriesCatalog::Replay() {
  int fd = ::open(path_.c_str(), O_RDWR);
  if (fd < 0) {
    if (errno == ENOENT) return;
    throw std::runtime_error("SeriesCatalog::Replay: open failed for " +
                              path_ + ": " + std::strerror(errno));
  }

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("SeriesCatalog::Replay: fstat failed");
  }

  off_t good_offset = 0;
  off_t size = st.st_size;
  uint8_t header[12];  // series_id(8) + label_len(4)

  while (good_offset + static_cast<off_t>(sizeof(header)) <= size) {
    ssize_t n = ::pread(fd, header, sizeof(header), good_offset);
    if (n != static_cast<ssize_t>(sizeof(header))) break;

    uint64_t id = io::GetU64(header);
    uint32_t label_len = io::GetU32(header + 8);
    off_t record_size = static_cast<off_t>(sizeof(header)) + label_len;
    if (good_offset + record_size > size) break;  // torn write

    std::string label(label_len, '\0');
    if (label_len > 0) {
      ssize_t sn = ::pread(fd, label.data(), label_len,
                            good_offset + static_cast<off_t>(sizeof(header)));
      if (sn != static_cast<ssize_t>(label_len)) break;
    }

    label_to_id_[label] = id;
    if (id >= next_id_) next_id_ = id + 1;

    good_offset += record_size;
  }

  if (good_offset != size) {
    ::ftruncate(fd, good_offset);
  }
  ::close(fd);
}

uint64_t SeriesCatalog::GetOrCreate(const std::string& canonical_labels) {
  auto it = label_to_id_.find(canonical_labels);
  if (it != label_to_id_.end()) return it->second;

  uint64_t id = next_id_++;

  std::vector<uint8_t> buf(12 + canonical_labels.size());
  io::PutU64(buf.data(), id);
  io::PutU32(buf.data() + 8, static_cast<uint32_t>(canonical_labels.size()));
  std::memcpy(buf.data() + 12, canonical_labels.data(),
              canonical_labels.size());

  ssize_t n = ::write(fd_, buf.data(), buf.size());
  if (n != static_cast<ssize_t>(buf.size())) {
    throw std::runtime_error("SeriesCatalog: short write");
  }
  if (::fsync(fd_) != 0) {
    throw std::runtime_error("SeriesCatalog: fsync failed");
  }

  label_to_id_[canonical_labels] = id;
  return id;
}

}  // namespace strata
